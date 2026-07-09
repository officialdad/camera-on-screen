using CameraOnScreen.App.Overlay;
using CameraOnScreen.Core.FingerControl;
using CameraOnScreen.Core.Native;
using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace CameraOnScreen.App.FingerControl;

/// <summary>Runs MediaPipe palm-detection + hand-landmark ONNX (CPU) on its own thread at ~15 Hz,
/// classifies the pointing pose, and emits smoothed nudge deltas. Never touches UI or D3D.</summary>
public sealed class HandInference : IDisposable
{
    // ==== constants from the RECORDED MODEL CONTRACT (Assets/models/hand/README.md) — do not guess ====
    private const int PalmInput = 192;
    private const int LandmarkInput = 224;
    // Both models: NCHW, RGB, 0..1 value range (identical preprocess) — fixed by the recorded
    // contract, so tensors below are built NCHW-only (no runtime layout branch).
    private const string PalmInName = "input";
    // Single post-processed output: decode + NMS are baked into the graph (the "_inf_post_" export),
    // so there are NO raw SSD anchors and NO separate scores tensor to sigmoid — see PalmDecoder.
    private const string PalmOutName = "pdscore_boxx_boxy_boxsize_kp0x_kp0y_kp2x_kp2y"; // [N,8]
    private const string LmInName = "input";     // dynamic batch; we feed batch size 1
    private const string LmPointsOut = "xyz_x21";    // [N,63], x,y,z interleaved, PIXEL units of the 224 crop
    private const string LmPresenceOut = "hand_score"; // [N,1] — see the comment at its use site below
    // ======================================================================================================

    private readonly INativeShim _shim;
    private readonly InferenceSession _palm, _landmark;
    // Owned exclusively by the inference thread's RunLoop; sized to match the UI-thread frame buffer
    // convention (4K BGRA) so cos_get_frame never rejects it as too small.
    private readonly byte[] _frame = new byte[3840 * 2160 * 4];
    public FingerNudgeTracker Tracker { get; } = new();
    /// <summary>Raised on the inference thread, once per processed frame.</summary>
    public event Action<NudgeResult>? Nudge;
    private CancellationTokenSource? _cts;
    private Task? _loop;

    private HandInference(INativeShim shim, InferenceSession palm, InferenceSession landmark)
    {
        _shim = shim; _palm = palm; _landmark = landmark;
    }

    public static HandInference? TryCreate(INativeShim shim, out string detail)
    {
        var dir = Path.Combine(AppContext.BaseDirectory, "models", "hand");
        var palmPath = Path.Combine(dir, "palm_detection.onnx");
        var lmPath = Path.Combine(dir, "hand_landmark.onnx");
        if (!File.Exists(palmPath) || !File.Exists(lmPath))
        {
            detail = "Finger control unavailable: hand models not found beside the app.";
            return null;
        }
        InferenceSession? palm = null;
        try
        {
            palm = new InferenceSession(palmPath);
            var landmark = new InferenceSession(lmPath);
            detail = "";
            return new HandInference(shim, palm, landmark);
        }
        catch (Exception ex)
        {
            // If the palm session loaded but the landmark session then threw, dispose it here —
            // it never made it into a HandInference for the caller to Dispose.
            palm?.Dispose();
            detail = $"Finger control unavailable: {ex.Message}";
            return null;
        }
    }

    public void Start()
    {
        if (_loop is not null) return;
        _cts = new CancellationTokenSource();
        _loop = Task.Run(() => RunLoop(_cts.Token));
    }

    public void Stop()
    {
        _cts?.Cancel();
        try { _loop?.Wait(); } catch (AggregateException) { /* cancellation */ }
        _loop = null;
        _cts?.Dispose();
        _cts = null;
    }

    private async Task RunLoop(CancellationToken ct)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(66)); // ~15 Hz
        try
        {
            while (await timer.WaitForNextTickAsync(ct))
            {
                if (!_shim.TryGetFrame(_frame, out int w, out int h) || w <= 0) continue;
                var result = ProcessFrame(w, h);
                Nudge?.Invoke(result);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception)
        {
            // Fail-safe: report a permanent NoHand -> tracker disarms; feature goes quiet, app lives.
            Nudge?.Invoke(Tracker.Update(HandPose.NoHand, 0, 0, 1, 1));
        }
    }

    private NudgeResult ProcessFrame(int w, int h)
    {
        // Stage 1: palm detection on the letterboxed full frame.
        var (palmTensor, scale, padX, padY) = Letterbox(_frame, w, h, PalmInput);
        using var palmOut = _palm.Run(new[] { NamedOnnxValue.CreateFromTensor(PalmInName, palmTensor) });
        var rows = palmOut.First(v => v.Name == PalmOutName).AsEnumerable<float>().ToArray();
        var roiIn = PalmDecoder.DecodeBest(rows, HandPoseClassifier.PalmScoreMin);
        if (roiIn is not { } roiInput)
            return Tracker.Update(HandPose.NoHand, 0, 0, ScreenW, ScreenH);

        // Map ROI from letterboxed-input space (normalized 0..1 of the PalmInput square) back to
        // normalized frame space.
        var roi = new PalmRoi(
            (roiInput.CenterX * PalmInput - padX) / (w * scale),
            (roiInput.CenterY * PalmInput - padY) / (h * scale),
            roiInput.Side * PalmInput / (Math.Min(w, h) * scale));

        // Stage 2: landmarks on the square hand crop.
        var (lmTensor, cropX, cropY, cropSide) = CropTensor(_frame, w, h, roi, LandmarkInput);
        using var lmOut = _landmark.Run(new[] { NamedOnnxValue.CreateFromTensor(LmInName, lmTensor) });
        var pts = lmOut.First(v => v.Name == LmPointsOut).AsEnumerable<float>().ToArray(); // 21 x (x,y,z), px of the 224 crop

        // hand_score is ALREADY a probability — do NOT apply sigmoid. Verified against the source
        // repo's own consumption of this exact output (PINTO0309/hand-gesture-recognition-using-onnx,
        // commit e2deb2e0, model/hand_landmark/hand_landmark.py:255):
        //   keep = hand_scores[:, 0] > self.class_score_th   # hand_score > self.class_score_th
        // — a direct threshold comparison (default class_score_th = 0.50); no sigmoid/expit call
        // appears anywhere in that file. Our runtime gate mirrors that: HandPoseClassifier.Classify
        // compares this raw value directly against PresenceMin (0.6).
        float presence = lmOut.First(v => v.Name == LmPresenceOut).AsEnumerable<float>().First();

        // Crop px -> normalized frame coords.
        var xs = new float[21]; var ys = new float[21];
        for (int i = 0; i < 21; i++)
        {
            xs[i] = (cropX + pts[i * 3] / LandmarkInput * cropSide) / w;
            ys[i] = (cropY + pts[i * 3 + 1] / LandmarkInput * cropSide) / h;
        }
        var pose = HandPoseClassifier.Classify(xs, ys, 1.0f /* palm gate already applied via DecodeBest */, presence);
        return Tracker.Update(pose, xs[HandPoseClassifier.IndexTip], ys[HandPoseClassifier.IndexTip],
            ScreenW, ScreenH);
    }

    // Primary-monitor pixel size for gain scaling; coarse is fine (gain is a feel knob).
    private static double ScreenW => Interop.GetSystemMetrics(Interop.SM_CXSCREEN);
    private static double ScreenH => Interop.GetSystemMetrics(Interop.SM_CYSCREEN);

    // Letterbox the BGRA frame into a square model input (nearest-neighbor; 0..1 RGB per the contract).
    private static (DenseTensor<float> t, float scale, float padX, float padY) Letterbox(
        byte[] bgra, int w, int h, int size)
    {
        float scale = (float)size / Math.Max(w, h);
        int outW = (int)(w * scale), outH = (int)(h * scale);
        float padX = (size - outW) / 2f, padY = (size - outH) / 2f;
        var t = new DenseTensor<float>(new[] { 1, 3, size, size });
        for (int y = 0; y < outH; y++)
        {
            int sy = Math.Min(h - 1, (int)(y / scale));
            for (int x = 0; x < outW; x++)
            {
                int sx = Math.Min(w - 1, (int)(x / scale));
                int o = (sy * w + sx) * 4;                    // BGRA
                int dx = x + (int)padX, dy = y + (int)padY;
                WritePx(t, dx, dy, bgra[o + 2], bgra[o + 1], bgra[o]); // -> RGB
            }
        }
        return (t, scale, padX, padY);
    }

    // Square crop around the ROI (clamped to frame), resampled to the landmark input size.
    private static (DenseTensor<float> t, float cropX, float cropY, float cropSide) CropTensor(
        byte[] bgra, int w, int h, PalmRoi roi, int size)
    {
        float side = Math.Clamp(roi.Side * Math.Min(w, h), 32, Math.Min(w, h));
        float cx = Math.Clamp(roi.CenterX * w, side / 2, w - side / 2);
        float cy = Math.Clamp(roi.CenterY * h, side / 2, h - side / 2);
        float x0 = cx - side / 2, y0 = cy - side / 2;
        var t = new DenseTensor<float>(new[] { 1, 3, size, size });
        for (int y = 0; y < size; y++)
        {
            int sy = Math.Min(h - 1, (int)(y0 + y * side / size));
            for (int x = 0; x < size; x++)
            {
                int sx = Math.Min(w - 1, (int)(x0 + x * side / size));
                int o = (sy * w + sx) * 4;
                WritePx(t, x, y, bgra[o + 2], bgra[o + 1], bgra[o]);
            }
        }
        return (t, x0, y0, side);
    }

    private static void WritePx(DenseTensor<float> t, int x, int y, byte r, byte g, byte b)
    {
        // 0..1 range, RGB, NCHW — the recorded model contract (Assets/models/hand/README.md); both
        // models share identical preprocessing (/255.0, BGR->RGB, HWC->CHW). Not -1..1.
        t[0, 0, y, x] = r / 255f; t[0, 1, y, x] = g / 255f; t[0, 2, y, x] = b / 255f;
    }

    public void Dispose()
    {
        Stop();
        _palm.Dispose();
        _landmark.Dispose();
    }
}
