using CameraOnScreen.Core.Orchestration;

namespace CameraOnScreen.App.Avalonia.Native;

public static class GpuTierDetector
{
    // Display-only heuristic (the native capability probe is the real effect gate).
    // Linux: the NVIDIA kernel driver exposes /proc/driver/nvidia when loaded.
    public static GpuTier Detect() =>
        File.Exists("/proc/driver/nvidia/version") ? GpuTier.Rtx : GpuTier.NonRtx;
}
