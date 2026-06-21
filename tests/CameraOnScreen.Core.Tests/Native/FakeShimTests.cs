using System;
using CameraOnScreen.Core.Native;
using Xunit;

namespace CameraOnScreen.Core.Tests.Native;

public class FakeShimTests
{
    [Fact]
    public void Start_then_status_reports_running()
    {
        var shim = new FakeShim();
        Assert.True(shim.Init(IntPtr.Zero));
        Assert.False(shim.GetStatus().Running);
        shim.Start();
        Assert.True(shim.GetStatus().Running);
        shim.Stop();
        Assert.False(shim.GetStatus().Running);
    }

    [Fact]
    public void SetParams_is_recorded()
    {
        var shim = new FakeShim();
        var p = new ShimParams("cam-1", GreenScreenEnabled: true, GreenScreenExpand: 0.8, GreenScreenFeather: 0.0,
            EyeContactEnabled: false, EyeContactSensitivity: 0.5, EyeContactLookAwayRange: 0.5);
        shim.SetParams(p);
        Assert.Equal(p, shim.LastParams);
    }

    [Fact]
    public void EnumerateCameras_returns_seeded_list()
    {
        var shim = new FakeShim { Cameras = { new CameraInfo("a", "Cam A") } };
        Assert.Single(shim.EnumerateCameras());
    }

    [Fact]
    public void FakeShim_QueryCapabilities_ReportsConfiguredValue()
    {
        var shim = new FakeShim { GreenScreenAvailable = true };
        var caps = shim.QueryCapabilities();
        Assert.True(caps.GreenScreenAvailable);
        Assert.False(string.IsNullOrEmpty(caps.Detail));
    }

    [Fact]
    public void FakeShim_QueryCapabilities_DefaultsUnavailable()
    {
        var caps = new FakeShim().QueryCapabilities();
        Assert.False(caps.GreenScreenAvailable);
    }
}
