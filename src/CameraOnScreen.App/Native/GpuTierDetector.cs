using CameraOnScreen.Core.Orchestration;
using Vortice.DXGI;

namespace CameraOnScreen.App.Native;

public static class GpuTierDetector
{
    // Heuristic for M1+M2: NVIDIA adapter whose description mentions "RTX".
    // The M3-M5 plan replaces this with a real Maxine-availability probe.
    public static GpuTier Detect()
    {
        if (DXGI.CreateDXGIFactory1(out IDXGIFactory1? factory).Failure || factory is null)
        {
            // Factory creation failed: we fall back to NonRtx (effects off). Log so a wrongly
            // detected NonRtx on an actual RTX box is diagnosable from the trace.
            System.Diagnostics.Debug.WriteLine(
                "GpuTierDetector: CreateDXGIFactory1 failed; falling back to GpuTier.NonRtx.");
            return GpuTier.NonRtx;
        }
        using (factory)
        {
            for (uint i = 0; factory.EnumAdapters1(i, out IDXGIAdapter1? adapter).Success; i++)
            using (adapter)
            {
                var desc = adapter!.Description1.Description ?? "";
                if (desc.Contains("RTX", StringComparison.OrdinalIgnoreCase))
                    return GpuTier.Rtx;
            }
        }
        return GpuTier.NonRtx;
    }
}
