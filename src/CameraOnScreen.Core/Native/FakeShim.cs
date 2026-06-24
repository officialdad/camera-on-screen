namespace CameraOnScreen.Core.Native;

public sealed class FakeShim : INativeShim
{
    public List<CameraInfo> Cameras { get; } = new();
    public ShimParams? LastParams { get; private set; }
    public bool GreenScreenAvailable { get; set; }
    public bool EyeContactAvailable { get; set; }
    public bool ArtifactReductionAvailable { get; set; }
    public bool SuperResAvailable { get; set; }
    private bool _running;

    public bool Init(IntPtr d3dDevice) => true;
    public IReadOnlyList<CameraInfo> EnumerateCameras() => Cameras;
    public void SetParams(ShimParams p) => LastParams = p;
    public void Start() => _running = true;
    public void Stop() => _running = false;

    public ShimStatus GetStatus() => new(
        Running: _running,
        Fps: _running ? 30 : 0,
        Gaze: GazeState.Unknown,
        GreenScreenActive: _running && (LastParams?.GreenScreenEnabled ?? false),
        EyeContactActive: _running && (LastParams?.EyeContactEnabled ?? false),
        Error: null);

    public bool TryGetFrame(byte[] buffer, out int width, out int height)
    {
        width = 0; height = 0; return false;
    }

    public ShimCapabilities QueryCapabilities() =>
        new(GreenScreenAvailable,
            GreenScreenAvailable ? "fake: available" : "fake: unavailable",
            EyeContactAvailable,
            EyeContactAvailable ? "fake: ec available" : "fake: ec unavailable",
            ArtifactReductionAvailable, SuperResAvailable);

    public bool Disposed { get; private set; }
    public void Dispose() => Disposed = true;
}
