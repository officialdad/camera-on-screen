# Finger-Pointing Overlay Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Point ☝ at the webcam and nudge the overlay around the screen; hold still to stop; relax hand to disarm.

**Architecture:** MediaPipe Hands (palm detection + 21-landmark) ONNX models run on a dedicated App background thread via ONNX Runtime **CPU** at ~15 Hz, pulling frames through the existing thread-safe `ShimRef.TryGetFrame`. All decision logic (anchor decode, pose classify, arming state machine, delta smoothing) is pure `CameraOnScreen.Core` code, fully unit-tested. Deltas post to the UI thread and move the overlay through the existing `SetBounds` path. **No shim changes, no Maxine/NVIDIA dependency, works on non-RTX.**

**Tech Stack:** .NET 8, `Microsoft.ML.OnnxRuntime` (CPU EP), xUnit, WinUI 3 (one card edit).

**Spec:** `docs/superpowers/specs/2026-07-08-finger-pointing-overlay-control-design.md` — read it first.

## Global Constraints

- Builds and tests must be **pristine (0 warnings)** — CI enforces `/warnaserror` + `TreatWarningsAsErrors`.
- Core stays pure .NET 8: **no WinUI/Win32/OnnxRuntime types in `CameraOnScreen.Core`**.
- Model files must be freely redistributable (Apache-2.0/MIT). **Do NOT commit a model whose license restricts redistribution** (Qualcomm AI Hub exports carry their own license — check it; fall back to PINTO zoo / Google's Apache-2.0 originals).
- Defaults locked by spec: arm after **3** consecutive pointing frames, disarm after **5** lost frames, EMA α **0.5**, deadzone **2 px/tick**, gain default **1.5** (slider 0.5–3.0), palm score min **0.7**, presence min **0.6**, inference cadence **~66 ms**.
- Camera frames are **unmirrored**; horizontal screen delta sign is **negative** (`dxScreen = −dxCam`), vertical positive. Single constant, flip if the human gate proves it backwards.
- Finger control is **not** a shim param: it must NOT touch `ShimParams`/`BuildParams`/`ApplyLiveParams`.
- Build shim SDK config **last** before any manual run (deploy-the-right-shim gotcha).

---

### Task 1: Model acquisition + I/O verification gate

Nothing downstream is coded until the real tensor shapes, dtypes, layouts, and value ranges are recorded — the repo's "verify the real API before coding" rule applied to models.

**Files:**
- Create: `src/CameraOnScreen.App/Assets/models/hand/palm_detection.onnx` (downloaded)
- Create: `src/CameraOnScreen.App/Assets/models/hand/hand_landmark.onnx` (downloaded)
- Create: `src/CameraOnScreen.App/Assets/models/hand/LICENSE.txt` (copied from model source)
- Create: `src/CameraOnScreen.App/Assets/models/hand/README.md` (recorded metadata — see step 4)

**Interfaces:**
- Produces: the two `.onnx` files at the exact paths above, plus `README.md` whose recorded constants (input sizes, layout, value range, output tensor names/shapes/order) later tasks copy into code.

- [ ] **Step 1: Locate candidate ONNX exports and check licenses**

Primary: Qualcomm AI Hub HF repos. List files via the HF API:

```powershell
curl.exe -s https://huggingface.co/api/models/qualcomm/MediaPipe-Hand-Detection | ConvertFrom-Json | Select-Object -ExpandProperty siblings
```

Also check its `README.md`/license field on the repo page. **Gate:** if the license is not Apache-2.0/MIT (Qualcomm AI Hub Model License restricts redistribution), REJECT and use the fallback: PINTO model zoo `033_Hand_Detection_and_Tracking` (https://github.com/PINTO0309/PINTO_model_zoo — MIT/Apache conversions of Google's Apache-2.0 MediaPipe models): `palm_detection_full` (192×192) + `hand_landmark_full` (224×224) ONNX variants. Prefer a palm-detection export **with post-processing baked in** (outputs = boxes+scores) if one exists; otherwise the raw-regressor variant is fine (Task 2 decodes it).

- [ ] **Step 2: Download both models + the source license into the assets dir**

```powershell
New-Item -ItemType Directory -Force src/CameraOnScreen.App/Assets/models/hand
curl.exe -L -o src/CameraOnScreen.App/Assets/models/hand/palm_detection.onnx "<chosen-palm-url>"
curl.exe -L -o src/CameraOnScreen.App/Assets/models/hand/hand_landmark.onnx "<chosen-landmark-url>"
# plus the license file from the same source -> LICENSE.txt
```

Sanity: each file is 1–15 MB, starts with ONNX magic (`Format-Hex -Count 8` shows `08 ..`), not an HTML error page.

- [ ] **Step 3: Inspect real tensor I/O with a throwaway console app (scratchpad, not repo)**

```powershell
dotnet new console -o $env:TEMP\onnx-probe; cd $env:TEMP\onnx-probe
dotnet add package Microsoft.ML.OnnxRuntime
```

```csharp
using Microsoft.ML.OnnxRuntime;
foreach (var path in args)
{
    using var s = new InferenceSession(path);
    Console.WriteLine($"== {path}");
    foreach (var kv in s.InputMetadata)
        Console.WriteLine($"  IN  {kv.Key}: {kv.Value.ElementDataType} [{string.Join(",", kv.Value.Dimensions)}]");
    foreach (var kv in s.OutputMetadata)
        Console.WriteLine($"  OUT {kv.Key}: {kv.Value.ElementDataType} [{string.Join(",", kv.Value.Dimensions)}]");
}
```

Run: `dotnet run -- <abs-path>\palm_detection.onnx <abs-path>\hand_landmark.onnx`
Expected shape families (verify which): palm det input `[1,3,192,192]` or `[1,192,192,3]` (or 256); raw outputs `[1,2016,18]`+`[1,2016,1]` (192) or `[1,2944,18]`+`[1,2944,1]` (256), OR decoded boxes if post-processed. Landmark input `[1,3,224,224]`-ish; outputs `[1,63]` landmarks + `[1,1]` presence (+ optional handedness).

- [ ] **Step 4: Record everything in the assets README**

Write `src/CameraOnScreen.App/Assets/models/hand/README.md` containing: source URLs + license name, exact input tensor name/shape/layout (NCHW vs NHWC) per model, input value range (0..1 vs −1..1 — from the source repo's docs/demo code), every output tensor name/shape and its meaning/order, and whether palm decode is raw-anchor or baked-in. Later tasks copy constants from this file verbatim.

- [ ] **Step 5: Commit**

```powershell
git add src/CameraOnScreen.App/Assets/models/hand
git commit -m "feat(finger-control): vendor MediaPipe hand ONNX models + recorded tensor I/O"
```

---

### Task 2: Core — hand types + palm anchor decoder (TDD)

Skip this task's decoder (types still needed) if Task 1 landed a post-processed palm model; then `PalmDecoder.DecodeBest` is instead a trivial argmax over the baked-in boxes — same public signature.

**Files:**
- Create: `src/CameraOnScreen.Core/FingerControl/HandTypes.cs`
- Create: `src/CameraOnScreen.Core/FingerControl/PalmDecoder.cs`
- Test: `tests/CameraOnScreen.Core.Tests/FingerControl/PalmDecoderTests.cs`

**Interfaces:**
- Produces:
  - `readonly record struct PalmRoi(float CenterX, float CenterY, float Side)` — normalized 0..1 frame coords.
  - `static float[] PalmDecoder.GenerateAnchors(int inputSize, (int stride, int anchorsPerCell)[] layers)` — flat `[cx0,cy0,cx1,cy1,…]`, normalized.
  - `static PalmRoi? PalmDecoder.DecodeBest(ReadOnlySpan<float> rawBoxes, ReadOnlySpan<float> rawScores, float[] anchors, int inputSize, float scoreMin)` — sigmoid best score; null below `scoreMin`. Single hand → argmax, **no NMS**.
  - Anchor configs (copy the one matching Task 1's README): 192 → `[(8,2),(16,2),(16,2),(16,2)]` = **2016** anchors; 256 → `[(8,2),(16,2),(32,2),(32,2),(32,2)]` = **2944** anchors.

- [ ] **Step 1: Write failing tests**

```csharp
using CameraOnScreen.Core.FingerControl;
namespace CameraOnScreen.Core.Tests.FingerControl;

public class PalmDecoderTests
{
    private static readonly (int, int)[] Layers192 = { (8, 2), (16, 2), (16, 2), (16, 2) };
    private static readonly (int, int)[] Layers256 = { (8, 2), (16, 2), (32, 2), (32, 2), (32, 2) };

    [Fact] public void AnchorCount_192_Is2016() =>
        Assert.Equal(2016 * 2, PalmDecoder.GenerateAnchors(192, Layers192).Length);

    [Fact] public void AnchorCount_256_Is2944() =>
        Assert.Equal(2944 * 2, PalmDecoder.GenerateAnchors(256, Layers256).Length);

    [Fact] public void Anchors_AreCellCenters_Normalized()
    {
        var a = PalmDecoder.GenerateAnchors(192, Layers192);
        // first cell of the stride-8 layer (24x24 grid): center (0.5/24, 0.5/24)
        Assert.Equal(0.5f / 24, a[0], 5);
        Assert.Equal(0.5f / 24, a[1], 5);
    }

    [Fact] public void DecodeBest_BelowThreshold_ReturnsNull()
    {
        var anchors = PalmDecoder.GenerateAnchors(192, Layers192);
        var boxes = new float[2016 * 18];
        var scores = new float[2016]; // raw 0 -> sigmoid 0.5 < 0.7
        Assert.Null(PalmDecoder.DecodeBest(boxes, scores, anchors, 192, 0.7f));
    }

    [Fact] public void DecodeBest_PicksArgmax_AndOffsetsByAnchor()
    {
        var anchors = PalmDecoder.GenerateAnchors(192, Layers192);
        var boxes = new float[2016 * 18];
        var scores = new float[2016];
        scores[100] = 5f;                        // sigmoid(5) ≈ 0.993
        boxes[100 * 18 + 0] = 19.2f;             // dx: +0.1 normalized
        boxes[100 * 18 + 2] = 38.4f;             // w: 0.2 normalized
        boxes[100 * 18 + 3] = 38.4f;             // h: 0.2
        var roi = PalmDecoder.DecodeBest(boxes, scores, anchors, 192, 0.7f);
        Assert.NotNull(roi);
        Assert.Equal(anchors[200] + 0.1f, roi!.Value.CenterX, 4);
        Assert.Equal(2.6f * 0.2f, roi.Value.Side, 4);   // 2.6x expansion of max(w,h)
    }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~PalmDecoder"`
Expected: FAIL — namespace `CameraOnScreen.Core.FingerControl` does not exist.

- [ ] **Step 3: Implement**

`HandTypes.cs`:

```csharp
namespace CameraOnScreen.Core.FingerControl;

public enum HandPose { NoHand, Other, Pointing }

/// <summary>Palm ROI in normalized (0..1) camera-frame coordinates, axis-aligned square.</summary>
public readonly record struct PalmRoi(float CenterX, float CenterY, float Side);
```

`PalmDecoder.cs`:

```csharp
namespace CameraOnScreen.Core.FingerControl;

/// <summary>Decodes MediaPipe BlazePalm SSD outputs. Single-hand use: argmax over scores, no NMS.
/// ponytail: axis-aligned ROI, no palm-keypoint rotation — pointing hands are near-upright; add
/// MediaPipe's rotated-crop math if landmarks degrade on tilted hands.</summary>
public static class PalmDecoder
{
    // ROI expansion palm-box -> hand-crop (MediaPipe uses ~2.6 for the hand crop).
    public const float RoiExpand = 2.6f;

    public static float[] GenerateAnchors(int inputSize, (int stride, int anchorsPerCell)[] layers)
    {
        var list = new List<float>();
        foreach (var (stride, perCell) in layers)
        {
            int grid = inputSize / stride;
            for (int y = 0; y < grid; y++)
                for (int x = 0; x < grid; x++)
                    for (int a = 0; a < perCell; a++)
                    {
                        list.Add((x + 0.5f) / grid);
                        list.Add((y + 0.5f) / grid);
                    }
        }
        return list.ToArray();
    }

    /// <param name="rawBoxes">[anchors*18]: dx,dy,w,h then 7 keypoint (x,y) pairs, in input px.</param>
    /// <param name="rawScores">[anchors] raw logits.</param>
    public static PalmRoi? DecodeBest(ReadOnlySpan<float> rawBoxes, ReadOnlySpan<float> rawScores,
        float[] anchors, int inputSize, float scoreMin)
    {
        int best = -1; float bestRaw = float.MinValue;
        for (int i = 0; i < rawScores.Length; i++)
            if (rawScores[i] > bestRaw) { bestRaw = rawScores[i]; best = i; }
        if (best < 0) return null;
        float score = 1f / (1f + MathF.Exp(-Math.Clamp(bestRaw, -50f, 50f)));
        if (score < scoreMin) return null;

        int o = best * 18;
        float cx = rawBoxes[o + 0] / inputSize + anchors[best * 2];
        float cy = rawBoxes[o + 1] / inputSize + anchors[best * 2 + 1];
        float w = rawBoxes[o + 2] / inputSize;
        float h = rawBoxes[o + 3] / inputSize;
        return new PalmRoi(cx, cy, RoiExpand * MathF.Max(w, h));
    }
}
```

- [ ] **Step 4: Run tests to verify pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~PalmDecoder"`
Expected: 5 PASS, 0 warnings.

- [ ] **Step 5: Commit**

```powershell
git add src/CameraOnScreen.Core/FingerControl tests/CameraOnScreen.Core.Tests/FingerControl
git commit -m "feat(finger-control): palm anchor generation + argmax box decode (Core, tested)"
```

---

### Task 3: Core — pointing-pose classifier (TDD)

**Files:**
- Create: `src/CameraOnScreen.Core/FingerControl/HandPoseClassifier.cs`
- Test: `tests/CameraOnScreen.Core.Tests/FingerControl/HandPoseClassifierTests.cs`

**Interfaces:**
- Consumes: `HandPose` enum (Task 2).
- Produces: `static HandPose HandPoseClassifier.Classify(ReadOnlySpan<float> xs, ReadOnlySpan<float> ys, float palmScore, float presenceScore)` — 21 landmarks in normalized frame coords (MediaPipe index order: 0 wrist; 5/6/8 index MCP/PIP/TIP; 10/12 middle PIP/TIP; 14/16 ring; 18/20 pinky). Constants `PalmScoreMin = 0.7f`, `PresenceMin = 0.6f`, `IndexTip = 8`.

- [ ] **Step 1: Write failing tests**

```csharp
using CameraOnScreen.Core.FingerControl;
namespace CameraOnScreen.Core.Tests.FingerControl;

public class HandPoseClassifierTests
{
    // Synthetic hand, wrist at (0.5, 0.9), fingers up the frame (y decreases upward).
    private static (float[] xs, float[] ys) Hand(bool indexExtended, bool othersExtended)
    {
        var xs = new float[21]; var ys = new float[21];
        for (int i = 0; i < 21; i++) { xs[i] = 0.5f; ys[i] = 0.85f; } // default: near palm
        xs[0] = 0.5f; ys[0] = 0.9f;                                   // wrist
        xs[5] = 0.50f; ys[5] = 0.70f;                                 // index MCP
        xs[6] = 0.50f; ys[6] = indexExtended ? 0.60f : 0.78f;         // index PIP
        xs[8] = 0.50f; ys[8] = indexExtended ? 0.40f : 0.84f;         // index TIP
        (int pip, int tip)[] others = { (10, 12), (14, 16), (18, 20) };
        float xo = 0.55f;
        foreach (var (pip, tip) in others)
        {
            xs[pip] = xo; ys[pip] = 0.68f;
            xs[tip] = xo; ys[tip] = othersExtended ? 0.42f : 0.80f;   // extended: far; curled: tip below pip
            xo += 0.04f;
        }
        return (xs, ys);
    }

    [Fact] public void PointingHand_IsPointing()
    {
        var (xs, ys) = Hand(indexExtended: true, othersExtended: false);
        Assert.Equal(HandPose.Pointing, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.9f));
    }

    [Fact] public void OpenPalm_IsOther()
    {
        var (xs, ys) = Hand(indexExtended: true, othersExtended: true);
        Assert.Equal(HandPose.Other, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.9f));
    }

    [Fact] public void Fist_IsOther()
    {
        var (xs, ys) = Hand(indexExtended: false, othersExtended: false);
        Assert.Equal(HandPose.Other, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.9f));
    }

    [Fact] public void LowPalmScore_IsNoHand()
    {
        var (xs, ys) = Hand(true, false);
        Assert.Equal(HandPose.NoHand, HandPoseClassifier.Classify(xs, ys, 0.5f, 0.9f));
    }

    [Fact] public void LowPresence_IsNoHand()
    {
        var (xs, ys) = Hand(true, false);
        Assert.Equal(HandPose.NoHand, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.4f));
    }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~HandPoseClassifier"`
Expected: FAIL — `HandPoseClassifier` not defined.

- [ ] **Step 3: Implement**

```csharp
namespace CameraOnScreen.Core.FingerControl;

/// <summary>☝ detector on 21 MediaPipe hand landmarks: index finger extended (tip clearly farther
/// from the wrist than its PIP), middle/ring/pinky curled (tip not farther than PIP). Thumb ignored.
/// ponytail: wrist-distance ratios only — no joint angles; revisit if sideways-pointing misfires.</summary>
public static class HandPoseClassifier
{
    public const float PalmScoreMin = 0.7f;
    public const float PresenceMin = 0.6f;
    public const int IndexTip = 8;
    private const float ExtendMargin = 1.15f; // tip must beat PIP distance by 15% to count extended

    public static HandPose Classify(ReadOnlySpan<float> xs, ReadOnlySpan<float> ys,
        float palmScore, float presenceScore)
    {
        if (palmScore < PalmScoreMin || presenceScore < PresenceMin) return HandPose.NoHand;

        // Static local taking the spans as parameters — a non-static local capturing a
        // ReadOnlySpan is CS9108 (ref-like types cannot be captured).
        static float D(ReadOnlySpan<float> xs, ReadOnlySpan<float> ys, int i)
        {
            float dx = xs[i] - xs[0], dy = ys[i] - ys[0];
            return MathF.Sqrt(dx * dx + dy * dy);
        }
        bool indexExtended = D(xs, ys, 8) > D(xs, ys, 6) * ExtendMargin;
        bool middleCurled = D(xs, ys, 12) <= D(xs, ys, 10) * ExtendMargin;
        bool ringCurled = D(xs, ys, 16) <= D(xs, ys, 14) * ExtendMargin;
        bool pinkyCurled = D(xs, ys, 20) <= D(xs, ys, 18) * ExtendMargin;
        return indexExtended && middleCurled && ringCurled && pinkyCurled
            ? HandPose.Pointing : HandPose.Other;
    }
}
```

- [ ] **Step 4: Run tests to verify pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~HandPoseClassifier"`
Expected: 5 PASS.

- [ ] **Step 5: Commit**

```powershell
git add src/CameraOnScreen.Core/FingerControl/HandPoseClassifier.cs tests/CameraOnScreen.Core.Tests/FingerControl/HandPoseClassifierTests.cs
git commit -m "feat(finger-control): pointing-pose classifier (Core, tested)"
```

---

### Task 4: Core — FingerNudgeTracker state machine (TDD)

**Files:**
- Create: `src/CameraOnScreen.Core/FingerControl/FingerNudgeTracker.cs`
- Test: `tests/CameraOnScreen.Core.Tests/FingerControl/FingerNudgeTrackerTests.cs`

**Interfaces:**
- Consumes: `HandPose` (Task 2).
- Produces:
  - `readonly record struct NudgeResult(double DxPx, double DyPx, bool Armed)`
  - `sealed class FingerNudgeTracker` — `double Gain { get; set; } = 1.5;` (live slider writes this), `NudgeResult Update(HandPose pose, float tipX, float tipY, double screenWpx, double screenHpx)` called once per inference frame with the index tip in normalized frame coords (ignored unless Pointing).

- [ ] **Step 1: Write failing tests**

```csharp
using CameraOnScreen.Core.FingerControl;
namespace CameraOnScreen.Core.Tests.FingerControl;

public class FingerNudgeTrackerTests
{
    private const double W = 1000, H = 1000;

    private static FingerNudgeTracker Armed(out NudgeResult last)
    {
        var t = new FingerNudgeTracker();
        last = default;
        for (int i = 0; i < 3; i++) last = t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H);
        return t;
    }

    [Fact] public void ArmsAfterThreeConsecutivePointingFrames()
    {
        var t = new FingerNudgeTracker();
        Assert.False(t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H).Armed);
        Assert.False(t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H).Armed);
        Assert.True(t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H).Armed);
    }

    [Fact] public void OtherPoseResetsArmingStreak()
    {
        var t = new FingerNudgeTracker();
        t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H);
        t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H);
        t.Update(HandPose.Other, 0.5f, 0.5f, W, H);
        Assert.False(t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H).Armed);
    }

    [Fact] public void ArmingFrameProducesZeroDelta()
    {
        Armed(out var last);
        Assert.Equal(0, last.DxPx);
        Assert.Equal(0, last.DyPx);
    }

    [Fact] public void MovingRightInCameraMovesLeftOnScreen_MirrorConvention()
    {
        var t = Armed(out _);
        // Big move so EMA(0.5) output clears the 2px deadzone: 0.1 * 1.5 * 1000 * 0.5 = 75px.
        var r = t.Update(HandPose.Pointing, 0.6f, 0.5f, W, H);
        Assert.True(r.Armed);
        Assert.True(r.DxPx < -10);      // camera +x -> screen -x
        Assert.Equal(0, r.DyPx, 3);
    }

    [Fact] public void MovingDownInCameraMovesDownOnScreen()
    {
        var t = Armed(out _);
        var r = t.Update(HandPose.Pointing, 0.5f, 0.6f, W, H);
        Assert.True(r.DyPx > 10);       // same sign vertically
    }

    [Fact] public void StillFingerYieldsZeroAfterDeadzone()
    {
        var t = Armed(out _);
        var r = t.Update(HandPose.Pointing, 0.5001f, 0.5f, W, H); // 0.15px raw -> deadzone
        Assert.Equal(0, r.DxPx);
        Assert.Equal(0, r.DyPx);
    }

    [Fact] public void GainScalesDelta()
    {
        var t = Armed(out _);
        t.Gain = 3.0;
        var r3 = t.Update(HandPose.Pointing, 0.6f, 0.5f, W, H);
        var t2 = Armed(out _);
        t2.Gain = 1.5;
        var r15 = t2.Update(HandPose.Pointing, 0.6f, 0.5f, W, H);
        Assert.Equal(r15.DxPx * 2, r3.DxPx, 1);
    }

    [Fact] public void StaysArmedThroughFourLostFrames_DisarmsOnFifth()
    {
        var t = Armed(out _);
        for (int i = 0; i < 4; i++)
            Assert.True(t.Update(HandPose.NoHand, 0, 0, W, H).Armed);
        Assert.False(t.Update(HandPose.NoHand, 0, 0, W, H).Armed);
    }

    [Fact] public void LostFramesProduceZeroDelta()
    {
        var t = Armed(out _);
        var r = t.Update(HandPose.NoHand, 0.9f, 0.9f, W, H);
        Assert.Equal(0, r.DxPx);
        Assert.Equal(0, r.DyPx);
    }

    [Fact] public void RearmAfterDisarmDoesNotJump()
    {
        var t = Armed(out _);
        for (int i = 0; i < 5; i++) t.Update(HandPose.NoHand, 0, 0, W, H);
        // re-arm at a totally different position: no teleport delta on the arming frame
        NudgeResult last = default;
        for (int i = 0; i < 3; i++) last = t.Update(HandPose.Pointing, 0.1f, 0.1f, W, H);
        Assert.True(last.Armed);
        Assert.Equal(0, last.DxPx);
        Assert.Equal(0, last.DyPx);
    }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~FingerNudgeTracker"`
Expected: FAIL — `FingerNudgeTracker` not defined.

- [ ] **Step 3: Implement**

```csharp
namespace CameraOnScreen.Core.FingerControl;

public readonly record struct NudgeResult(double DxPx, double DyPx, bool Armed);

/// <summary>Air-trackpad state machine: ☝ held for ArmFrames arms; fingertip deltas (EMA-smoothed,
/// deadzoned, gain-scaled) drive the overlay; pose lost for DisarmFrames disarms. Pure logic —
/// the caller owns threading, clamping, and drag suppression.</summary>
public sealed class FingerNudgeTracker
{
    public const int ArmFrames = 3;
    public const int DisarmFrames = 5;
    public const double EmaAlpha = 0.5;
    public const double DeadzonePx = 2.0;
    // Camera frames are unmirrored (user-left = image-right), so horizontal flips; vertical doesn't.
    private const double HorizontalSign = -1.0;

    public double Gain { get; set; } = 1.5;

    private int _pointingStreak, _lostStreak;
    private bool _armed;
    private float _prevX, _prevY;
    private double _emaDx, _emaDy;

    public NudgeResult Update(HandPose pose, float tipX, float tipY, double screenWpx, double screenHpx)
    {
        if (pose != HandPose.Pointing)
        {
            _pointingStreak = 0;
            if (_armed && ++_lostStreak >= DisarmFrames) Disarm();
            return new NudgeResult(0, 0, _armed);
        }

        _lostStreak = 0;
        if (!_armed)
        {
            if (++_pointingStreak >= ArmFrames)
            {
                _armed = true;
                _prevX = tipX; _prevY = tipY;   // baseline here: the arming frame never jumps
                _emaDx = _emaDy = 0;
            }
            return new NudgeResult(0, 0, _armed);
        }

        double rawDx = HorizontalSign * (tipX - _prevX) * Gain * screenWpx;
        double rawDy = (tipY - _prevY) * Gain * screenHpx;
        _prevX = tipX; _prevY = tipY;
        _emaDx = EmaAlpha * rawDx + (1 - EmaAlpha) * _emaDx;
        _emaDy = EmaAlpha * rawDy + (1 - EmaAlpha) * _emaDy;
        return Math.Sqrt(_emaDx * _emaDx + _emaDy * _emaDy) < DeadzonePx
            ? new NudgeResult(0, 0, true)
            : new NudgeResult(_emaDx, _emaDy, true);
    }

    private void Disarm()
    {
        _armed = false;
        _pointingStreak = _lostStreak = 0;
        _emaDx = _emaDy = 0;
    }
}
```

- [ ] **Step 4: Run tests to verify pass**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~FingerNudgeTracker"`
Expected: 10 PASS.

- [ ] **Step 5: Commit**

```powershell
git add src/CameraOnScreen.Core/FingerControl/FingerNudgeTracker.cs tests/CameraOnScreen.Core.Tests/FingerControl/FingerNudgeTrackerTests.cs
git commit -m "feat(finger-control): arming state machine + smoothed nudge deltas (Core, tested)"
```

---

### Task 5: Config + ViewModel plumbing (TDD)

**Files:**
- Modify: `src/CameraOnScreen.Core/Config/Models.cs` (EffectSettings, ~line 36)
- Modify: `src/CameraOnScreen.Core/ViewModels/MainViewModel.cs` (props ~line 79, LoadFrom ~line 107, ToAppConfig ~line 137)
- Test: `tests/CameraOnScreen.Core.Tests/ViewModels/MainViewModelTests.cs`, `tests/CameraOnScreen.Core.Tests/Config/ModelsTests.cs`

**Interfaces:**
- Produces (VM observable props Task 6/7 bind to): `bool FingerControlEnabled` (default false), `double FingerControlSensitivity` (default 1.5), `bool FingerControlAvailable` (default false, set by App after ORT init), `string FingerControlDetail` (default `"Checking finger-control availability…"`). Config: `EffectSettings.FingerControlEnabled`, `EffectSettings.FingerControlSensitivity` (default 1.5).
- **Not** a shim param: no `ShimParams`/`BuildParams`/`ApplyLiveParams`/`On…Changed` partial changes.

- [ ] **Step 1: Write failing round-trip tests**

Append to `MainViewModelTests.cs` (match the file's existing construction helpers):

```csharp
[Fact]
public void FingerControl_RoundTrips_Through_Config()
{
    var vm = CreateVm(); // use the file's existing VM factory helper
    vm.FingerControlEnabled = true;
    vm.FingerControlSensitivity = 2.5;
    var cfg = vm.ToAppConfig(0, 0, 100, 100);
    Assert.True(cfg.Effects.FingerControlEnabled);
    Assert.Equal(2.5, cfg.Effects.FingerControlSensitivity);

    var vm2 = CreateVm();
    vm2.LoadFrom(cfg);
    Assert.True(vm2.FingerControlEnabled);
    Assert.Equal(2.5, vm2.FingerControlSensitivity);
}

[Fact]
public void FingerControl_Defaults_OffAndGain15()
{
    var vm = CreateVm();
    Assert.False(vm.FingerControlEnabled);
    Assert.Equal(1.5, vm.FingerControlSensitivity);
    Assert.False(vm.FingerControlAvailable);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj --filter "FullyQualifiedName~FingerControl"`
Expected: FAIL — properties do not exist (compile error).

- [ ] **Step 3: Implement**

`Models.cs` — add to `EffectSettings`:

```csharp
    public bool FingerControlEnabled { get; init; }               // point ☝ to nudge the overlay
    public double FingerControlSensitivity { get; init; } = 1.5;  // nudge gain, 0.5..3.0
```

`MainViewModel.cs` — add observable props (near line 79):

```csharp
    [ObservableProperty] private bool fingerControlEnabled;
    [ObservableProperty] private double fingerControlSensitivity = 1.5;
    [ObservableProperty] private bool fingerControlAvailable;    // set by the App after ORT init
    [ObservableProperty] private string fingerControlDetail = "Checking finger-control availability…";
```

`LoadFrom` — add:

```csharp
        FingerControlEnabled = config.Effects.FingerControlEnabled;
        FingerControlSensitivity = Math.Clamp(config.Effects.FingerControlSensitivity, 0.5, 3.0);
```

`ToAppConfig` `Effects` initializer — add:

```csharp
            FingerControlEnabled = FingerControlEnabled,
            FingerControlSensitivity = FingerControlSensitivity,
```

- [ ] **Step 4: Run the full Core suite (round-trip + no regressions)**

Run: `dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj`
Expected: all PASS, 0 warnings.

- [ ] **Step 5: Commit**

```powershell
git add src/CameraOnScreen.Core tests/CameraOnScreen.Core.Tests
git commit -m "feat(finger-control): config + viewmodel plumbing with persistence round-trip"
```

---

### Task 6: App — HandInference (ORT sessions, preprocessing, ~15 Hz loop)

**Files:**
- Create: `src/CameraOnScreen.App/FingerControl/HandInference.cs`
- Modify: `src/CameraOnScreen.App/CameraOnScreen.App.csproj` (NuGet + model copy)

**Interfaces:**
- Consumes: `INativeShim.TryGetFrame` (thread-safe), Core `PalmDecoder`/`HandPoseClassifier`/`FingerNudgeTracker`, Task 1's recorded tensor metadata.
- Produces: `sealed class HandInference : IDisposable` with
  - `static HandInference? TryCreate(INativeShim shim, out string detail)` — null + reason when models missing / ORT init fails.
  - `FingerNudgeTracker Tracker { get; }` (UI thread writes `Tracker.Gain`; benign double race).
  - `event Action<NudgeResult>? Nudge;` — raised **on the inference thread** per processed frame.
  - `void Start()` / `void Stop()` — idempotent; Stop joins the loop.

- [ ] **Step 1: Add the NuGet + model copy to the csproj**

```powershell
dotnet add src/CameraOnScreen.App/CameraOnScreen.App.csproj package Microsoft.ML.OnnxRuntime
```

Add to the csproj (new ItemGroup, mirroring the shim-DLL copy pattern):

```xml
  <ItemGroup>
    <!-- MediaPipe hand models (Apache-2.0, see Assets/models/hand/README.md) -> <out>\models\hand\ -->
    <None Include="Assets\models\hand\*.onnx;Assets\models\hand\LICENSE.txt">
      <Link>models\hand\%(Filename)%(Extension)</Link>
      <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
    </None>
  </ItemGroup>
```

- [ ] **Step 2: Implement HandInference**

Copy constants (input sizes, layout, value range, tensor names, anchor layer config) from `Assets/models/hand/README.md` — do NOT guess them. Skeleton (adjust the marked constants to the README):

```csharp
using CameraOnScreen.Core.FingerControl;
using CameraOnScreen.Core.Native;
using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace CameraOnScreen.App.FingerControl;

/// <summary>Runs MediaPipe palm-detection + hand-landmark ONNX (CPU) on its own thread at ~15 Hz,
/// classifies the pointing pose, and emits smoothed nudge deltas. Never touches UI or D3D.</summary>
public sealed class HandInference : IDisposable
{
    // ==== constants from Assets/models/hand/README.md (Task 1) — verify before changing ====
    private const int PalmInput = 192;                    // or 256 per README
    private const int LandmarkInput = 224;
    private const bool Nchw = true;                       // per README
    private const string PalmInName = "input";            // per README
    private const string PalmBoxesOut = "regressors";     // per README
    private const string PalmScoresOut = "classificators";// per README
    private const string LmInName = "input";              // per README
    private const string LmPointsOut = "landmarks";       // [1,63] x,y,z in crop px, per README
    private const string LmPresenceOut = "presence";      // per README
    private static readonly (int, int)[] AnchorLayers = { (8, 2), (16, 2), (16, 2), (16, 2) };
    // ========================================================================================

    private readonly INativeShim _shim;
    private readonly InferenceSession _palm, _landmark;
    private readonly float[] _anchors;
    private readonly byte[] _frame = new byte[3840 * 2160 * 4];
    public FingerNudgeTracker Tracker { get; } = new();
    public event Action<NudgeResult>? Nudge;
    private CancellationTokenSource? _cts;
    private Task? _loop;

    private HandInference(INativeShim shim, InferenceSession palm, InferenceSession landmark)
    {
        _shim = shim; _palm = palm; _landmark = landmark;
        _anchors = PalmDecoder.GenerateAnchors(PalmInput, AnchorLayers);
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
        try
        {
            var inference = new HandInference(shim, new InferenceSession(palmPath), new InferenceSession(lmPath));
            detail = "";
            return inference;
        }
        catch (Exception ex)
        {
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
        _loop = null; _cts?.Dispose(); _cts = null;
    }

    private async Task RunLoop(CancellationToken ct)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(66));
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
        var boxes = palmOut.First(v => v.Name == PalmBoxesOut).AsEnumerable<float>().ToArray();
        var scores = palmOut.First(v => v.Name == PalmScoresOut).AsEnumerable<float>().ToArray();
        var roiIn = PalmDecoder.DecodeBest(boxes, scores, _anchors, PalmInput, HandPoseClassifier.PalmScoreMin);
        if (roiIn is not { } roiInput)
            return Tracker.Update(HandPose.NoHand, 0, 0, ScreenW, ScreenH);
        // Map ROI from letterboxed-input space back to normalized frame space.
        var roi = new PalmRoi(
            (roiInput.CenterX * PalmInput - padX) / (w * scale),
            (roiInput.CenterY * PalmInput - padY) / (h * scale),
            roiInput.Side * PalmInput / (Math.Min(w, h) * scale));

        // Stage 2: landmarks on the square hand crop.
        var (lmTensor, cropX, cropY, cropSide) = CropTensor(_frame, w, h, roi, LandmarkInput);
        using var lmOut = _landmark.Run(new[] { NamedOnnxValue.CreateFromTensor(LmInName, lmTensor) });
        var pts = lmOut.First(v => v.Name == LmPointsOut).AsEnumerable<float>().ToArray(); // 21 x (x,y,z)
        float presence = Sigmoid(lmOut.First(v => v.Name == LmPresenceOut).AsEnumerable<float>().First());

        // Crop px -> normalized frame coords.
        var xs = new float[21]; var ys = new float[21];
        for (int i = 0; i < 21; i++)
        {
            xs[i] = (cropX + pts[i * 3] / LandmarkInput * cropSide) / w;
            ys[i] = (cropY + pts[i * 3 + 1] / LandmarkInput * cropSide) / h;
        }
        var pose = HandPoseClassifier.Classify(xs, ys, 1.0f /* palm gate already applied */, presence);
        return Tracker.Update(pose, xs[HandPoseClassifier.IndexTip], ys[HandPoseClassifier.IndexTip],
            ScreenW, ScreenH);
    }

    // Primary-monitor pixel size for gain scaling; coarse is fine (gain is a feel knob).
    private static double ScreenW => Interop.GetSystemMetrics(0 /* SM_CXSCREEN */);
    private static double ScreenH => Interop.GetSystemMetrics(1 /* SM_CYSCREEN */);

    private static float Sigmoid(float v) => 1f / (1f + MathF.Exp(-v));

    // Letterbox the BGRA frame into a square model input (nearest-neighbor; value range per README).
    private static (DenseTensor<float> t, float scale, float padX, float padY) Letterbox(
        byte[] bgra, int w, int h, int size)
    {
        float scale = (float)size / Math.Max(w, h);
        int outW = (int)(w * scale), outH = (int)(h * scale);
        float padX = (size - outW) / 2f, padY = (size - outH) / 2f;
        var t = new DenseTensor<float>(Nchw ? new[] { 1, 3, size, size } : new[] { 1, size, size, 3 });
        for (int y = 0; y < outH; y++)
        {
            int sy = Math.Min(h - 1, (int)(y / scale));
            for (int x = 0; x < outW; x++)
            {
                int sx = Math.Min(w - 1, (int)(x / scale));
                int o = (sy * w + sx) * 4;                    // BGRA
                int dx = x + (int)padX, dy = y + (int)padY;
                WritePx(t, dx, dy, size, bgra[o + 2], bgra[o + 1], bgra[o]); // ->RGB
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
        var t = new DenseTensor<float>(Nchw ? new[] { 1, 3, size, size } : new[] { 1, size, size, 3 });
        for (int y = 0; y < size; y++)
        {
            int sy = Math.Min(h - 1, (int)(y0 + y * side / size));
            for (int x = 0; x < size; x++)
            {
                int sx = Math.Min(w - 1, (int)(x0 + x * side / size));
                int o = (sy * w + sx) * 4;
                WritePx(t, x, y, size, bgra[o + 2], bgra[o + 1], bgra[o]);
            }
        }
        return (t, x0, y0, side);
    }

    private static void WritePx(DenseTensor<float> t, int x, int y, int size, byte r, byte g, byte b)
    {
        // 0..1 range; switch to (v/127.5 - 1) if the README records -1..1.
        if (Nchw)
        {
            t[0, 0, y, x] = r / 255f; t[0, 1, y, x] = g / 255f; t[0, 2, y, x] = b / 255f;
        }
        else
        {
            t[0, y, x, 0] = r / 255f; t[0, y, x, 1] = g / 255f; t[0, y, x, 2] = b / 255f;
        }
    }

    public void Dispose()
    {
        Stop();
        _palm.Dispose();
        _landmark.Dispose();
    }
}
```

`Interop.GetSystemMetrics` may not exist yet — add to `src/CameraOnScreen.App/Overlay/Interop.cs`:

```csharp
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
```

- [ ] **Step 3: Build pristine**

Run: `dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild`
Expected: 0 warnings, 0 errors. Verify `models\hand\palm_detection.onnx` + `hand_landmark.onnx` landed in `bin/Debug/net8.0-windows10.0.19041.0/win-x64/models/hand/`.

- [ ] **Step 4: Commit**

```powershell
git add src/CameraOnScreen.App
git commit -m "feat(finger-control): ONNX hand inference loop (palm det + landmarks, CPU, 15 Hz)"
```

---

### Task 7: App wiring — UI card, start/stop, delta application, armed indicator

**Files:**
- Modify: `src/CameraOnScreen.App/MainWindow.xaml` (AI Effects card, after the FRUC toggle ~line 95)
- Modify: `src/CameraOnScreen.App/MainWindow.xaml.cs` (ctor, OnVmPropertyChanged, OnWindowClosed, OnMouse)
- Modify: `THIRD-PARTY-NOTICES.md`

**Interfaces:**
- Consumes: `HandInference.TryCreate/Start/Stop/Tracker/Nudge` (Task 6), VM props (Task 5), `_overlay.GetBounds/SetBounds/SetHandleVisible`, `_dragging`.

- [ ] **Step 1: XAML — toggle + slider + availability note in the AI Effects card**

Insert after the "Smooth 60 fps (AI)" ToggleSwitch (before the CapabilityDetail note):

```xml
                            <ToggleSwitch Header="Finger control (point ☝ to move overlay)"
                                          IsEnabled="{x:Bind Vm.FingerControlAvailable, Mode=OneWay}"
                                          IsOn="{x:Bind Vm.FingerControlEnabled, Mode=TwoWay}"/>
                            <Slider Header="Finger sensitivity" Minimum="0.5" Maximum="3" StepFrequency="0.1"
                                    IsEnabled="{x:Bind Vm.FingerControlEnabled, Mode=OneWay}"
                                    Value="{x:Bind Vm.FingerControlSensitivity, Mode=TwoWay}"/>
                            <TextBlock Text="{x:Bind Vm.FingerControlDetail, Mode=OneWay}"
                                       Visibility="{x:Bind FingerControlNotAvailableVisibility, Mode=OneWay}"
                                       TextWrapping="Wrap"
                                       Foreground="{ThemeResource SystemFillColorCautionBrush}"/>
```

- [ ] **Step 2: Code-behind wiring**

Add field + visibility helper to `MainWindow`:

```csharp
    private FingerControl.HandInference? _handInference;
    private bool _fingerArmed; // last armed state from the inference thread (UI-thread copy)

    public Visibility FingerControlNotAvailableVisibility =>
        Vm.FingerControlAvailable ? Visibility.Collapsed : Visibility.Visible;
```

In the ctor, after `_mouseHook = new Overlay.OverlayMouseHook(OnMouse);`:

```csharp
        // Finger control: CPU ONNX hand tracking, independent of the Maxine probe (works non-RTX).
        _handInference = FingerControl.HandInference.TryCreate(Vm.ShimRef, out var fingerDetail);
        Vm.FingerControlAvailable = _handInference is not null;
        Vm.FingerControlDetail = _handInference is null ? fingerDetail : "";
        if (_handInference is not null)
            _handInference.Nudge += OnFingerNudge;
```

New handler + start/stop sync (add method; called from `OnVmPropertyChanged`):

```csharp
    // Runs on the inference thread — marshal to the UI thread before touching the overlay.
    private void OnFingerNudge(Core.FingerControl.NudgeResult r)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            bool armedChanged = _fingerArmed != r.Armed;
            _fingerArmed = r.Armed;
            if (armedChanged) _overlay.SetHandleVisible(_fingerArmed); // armed indicator: handle shown
            if (_dragging || !r.Armed || (r.DxPx == 0 && r.DyPx == 0)) return; // manual drag wins
            var (x, y, w, h) = _overlay.GetBounds();
            var wa = GetWorkArea(_overlay.Hwnd);
            int nx = (int)Math.Clamp(x + r.DxPx, wa.X - w + 48, wa.X + wa.W - 48);
            int ny = (int)Math.Clamp(y + r.DyPx, wa.Y - h + 48, wa.Y + wa.H - 48);
            _overlay.SetBounds(nx, ny, w, h);
            _saveTimer?.Stop(); _saveTimer?.Start(); // debounced persist, same as wheel-resize
        });
    }

    // ponytail: 48px of the overlay always stays inside the work area — cheap anti-lost clamp,
    // full virtual-screen multi-monitor clamping if anyone drags across monitors by finger.
    private void SyncFingerControl()
    {
        if (_handInference is null) return;
        _handInference.Tracker.Gain = Vm.FingerControlSensitivity;
        if (Vm.IsRunning && Vm.FingerControlEnabled) _handInference.Start();
        else _handInference.Stop();
    }
```

In `OnVmPropertyChanged`, add branches:

```csharp
        else if (e.PropertyName is nameof(MainViewModel.FingerControlEnabled)
                 or nameof(MainViewModel.FingerControlSensitivity)
                 or nameof(MainViewModel.IsRunning))
            SyncFingerControl();
        else if (e.PropertyName == nameof(MainViewModel.FingerControlAvailable))
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(FingerControlNotAvailableVisibility)));
```

NOTE: `IsRunning` already has a branch (StatusLine) using `is nameof(...) or nameof(...)` — keep both effects: the StatusLine branch must still fire. Restructure to independent `if`s rather than `else if` for `IsRunning`, or call `SyncFingerControl()` inside the existing IsRunning branch. Pick one; keep StatusLine behavior intact.

In `OnWindowClosed`, before `Vm.Dispose()`:

```csharp
        if (_handInference is not null)
        {
            _handInference.Nudge -= OnFingerNudge;
            _handInference.Dispose();
            _handInference = null;
        }
```

In `OnMouse` `Move` branch: the hover logic overwrites handle visibility on every mouse move, which
would blink the armed indicator off. Change the existing line to OR in the armed state:

```csharp
                _overlay.SetHandleVisible(over || _fingerArmed);
```

In `OnMouse` `LeftUp` (drag end) nothing changes — finger deltas already yield to `_dragging`.

- [ ] **Step 3: THIRD-PARTY-NOTICES.md**

Append a section (adjust names to what Task 1 actually shipped):

```markdown
## MediaPipe hand models (palm detection + hand landmark)

Bundled at `models\hand\`. Derived from Google MediaPipe (https://github.com/google-ai-edge/mediapipe),
Apache License 2.0. ONNX conversion source and full license text: `models\hand\LICENSE.txt`.
Used for the optional Finger Control feature (on-device inference only; no data leaves the machine).
```

- [ ] **Step 4: Build + full test suite pristine**

```powershell
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
```

Expected: 0 warnings, all tests PASS.

- [ ] **Step 5: Commit**

```powershell
git add src/CameraOnScreen.App THIRD-PARTY-NOTICES.md
git commit -m "feat(finger-control): UI toggle + sensitivity, nudge application, armed indicator"
```

---

### Task 8: End-to-end verification (human gate) + docs

**Files:**
- Modify: `README.md` (feature list)
- Modify: `docs/superpowers/specs/2026-07-08-finger-pointing-overlay-control-design.md` (only if reality diverged — e.g. sign flip)

- [ ] **Step 1: Build the shim SDK config LAST, then run the app**

```powershell
$env:COS_VFX_SDK_DIR  = "C:\actions-runner\_sdk\VideoFX"
$env:COS_AR_SDK_DIR   = "C:\actions-runner\_sdk\Maxine-AR-SDK-1.1.1.0"
$env:COS_FRUC_SDK_DIR = "C:\actions-runner\_sdk\Optical_Flow_SDK_5.0.7"
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" native/shim/shim.vcxproj /p:Configuration=Debug /p:Platform=x64
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
src/CameraOnScreen.App/bin/Debug/net8.0-windows10.0.19041.0/win-x64/CameraOnScreen.App.exe
```

- [ ] **Step 2: Human gate checklist (visual verification is inherent — see docs/superpowers/verification/)**

1. Finger control toggle is enabled (not greyed); note text absent.
2. Start camera, enable finger control. Raise ☝ steadily → the centre handle appears (armed indicator) within ~half a second.
3. Nudge finger left → overlay moves LEFT on screen. **If it moves right, flip `HorizontalSign` in `FingerNudgeTracker` to `+1` and record the flip in the spec.**
4. Hold the finger still while pointing → overlay stays put (no drift).
5. Open palm / drop hand → handle hides after ~⅓ s; waving hands while talking does NOT move the overlay.
6. Drag by handle while pointing → manual drag wins, no fighting.
7. Toggle Mirror on: overlay image mirrors, finger direction behavior unchanged.
8. Stop camera → no crash; close app → clean exit (no abort dialog).
9. Task Manager: CPU delta with finger control on is acceptable (< ~10% of one core equivalent).

- [ ] **Step 3: README feature bullet**

Add to the feature list: `- **Finger control** — point ☝ at the camera and nudge your finger to move the overlay; hold still to stop, relax your hand to release. Runs on-device (CPU, MediaPipe hand models); works on any GPU.`

- [ ] **Step 4: Full pristine verification + installer pickup + commit**

```powershell
dotnet build src/CameraOnScreen.App/CameraOnScreen.App.csproj -t:Rebuild
dotnet test tests/CameraOnScreen.Core.Tests/CameraOnScreen.Core.Tests.csproj
# Installer stages the App build output, so models\hand\ rides along automatically — confirm:
scripts/build-installer.ps1 -DryRun
```

Expected: dry-run plan shows the App build-output staging (which now contains `models\hand\`); no installer script change needed (spec §6).

```powershell
git add README.md docs
git commit -m "docs(finger-control): README feature entry + verified human gate"
```

- [ ] **Step 5: Branch/PR** — this work should be on a feature branch (`feat/finger-control`); open a PR so self-hosted CI runs the export-verify + full build. Use superpowers:finishing-a-development-branch.
