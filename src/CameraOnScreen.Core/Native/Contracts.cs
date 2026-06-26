namespace CameraOnScreen.Core.Native;

public readonly record struct CameraInfo(string Id, string Name);

/// <summary>Result of probing the native shim for effect availability. Eye-contact fields default
/// so existing 2-arg call sites keep compiling.</summary>
public sealed record ShimCapabilities(
    bool GreenScreenAvailable, string Detail,
    bool EyeContactAvailable = false, string EyeContactDetail = "",
    bool SuperResAvailable = false,
    bool FrameInterpAvailable = false);

public enum GazeState { Unknown, OnCamera, Redirected, RealEyes }

public readonly record struct ShimStatus(
    bool Running,
    double Fps,
    GazeState Gaze,
    bool GreenScreenActive,
    bool EyeContactActive,
    string? Error,
    bool ExposureSupported = false); // true while a running camera exposes manual exposure (#16)

public sealed record ShimParams(
    string? CameraId,
    bool GreenScreenEnabled,
    double GreenScreenExpand,
    double GreenScreenFeather,
    bool EyeContactEnabled,
    double EyeContactSensitivity,
    double EyeContactLookAwayRange,
    bool SuperResEnabled = false,
    int SuperResScale = 0,             // 0=off, 15=1.5x, 20=2x (upscale modes only)
    int SuperResQualityLevel = 1,      // VSR QualityLevel: 1-4 upscale, 8-11 denoise, 12-15 deblur
    bool ExposureLockEnabled = false,  // lock camera exposure to Manual (#16) so fps holds steady
    double ExposureValue = 0.0,        // 0..1 normalized exposure when locked (native maps to camera range)
    bool FrameInterpEnabled = false);  // FRUC: request frame interpolation (#13)

public interface INativeShim : IDisposable
{
    bool Init(IntPtr d3dDevice);
    IReadOnlyList<CameraInfo> EnumerateCameras();
    void SetParams(ShimParams p);
    void Start();
    void Stop();
    ShimStatus GetStatus();
    // Copies the latest BGRA frame into buffer; returns true when a new frame was written.
    bool TryGetFrame(byte[] buffer, out int width, out int height);
    /// <summary>Probes whether AI Green Screen can run (SDK present + model loads).</summary>
    ShimCapabilities QueryCapabilities();
}
