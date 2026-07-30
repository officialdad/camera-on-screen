using CameraOnScreen.Core.Native;
using Xunit;

namespace CameraOnScreen.Core.Tests.Native;

public class ContractsTests
{
    [Fact]
    public void ShimParams_CarriesGreenScreenBackend()
    {
        var p = new ShimParams(null, true, 0, 0, false, 0.5, 0.5, GreenScreenBackend: 2);
        Assert.Equal(2, p.GreenScreenBackend);
        // Default stays Auto so existing call sites keep their behavior.
        var d = new ShimParams(null, true, 0, 0, false, 0.5, 0.5);
        Assert.Equal(0, d.GreenScreenBackend);
    }

    [Fact]
    public void FakeShim_ReportsOnnxAvailability()
    {
        var shim = new FakeShim { GreenScreenOnnxAvailable = true };
        var caps = shim.QueryCapabilities();
        Assert.True(caps.GreenScreenOnnxAvailable);
        Assert.False(caps.GreenScreenAvailable);
    }
}
