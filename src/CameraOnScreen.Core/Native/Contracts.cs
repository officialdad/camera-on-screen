namespace CameraOnScreen.Core.Native;

public readonly record struct CameraInfo(string Id, string Name);

/// <summary>Result of probing the native shim for effect availability.</summary>
public sealed record ShimCapabilities(bool GreenScreenAvailable, string Detail);

public enum GazeState { Unknown, OnCamera, Redirected, RealEyes }

public readonly record struct ShimStatus(
    bool Running,
    double Fps,
    GazeState Gaze,
    bool GreenScreenActive,
    bool EyeContactActive,
    string? Error);

public sealed record ShimParams(
    string? CameraId,
    bool GreenScreenEnabled,
    double GreenScreenExpand,
    double GreenScreenFeather,
    bool EyeContactEnabled,
    double EyeContactSensitivity,
    double EyeContactLookAwayRange);

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
