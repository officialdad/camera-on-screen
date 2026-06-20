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
        var p = new ShimParams("cam-1", true, 0.8, false, 0.5, 0.5);
        shim.SetParams(p);
        Assert.Equal(p, shim.LastParams);
    }

    [Fact]
    public void EnumerateCameras_returns_seeded_list()
    {
        var shim = new FakeShim { Cameras = { new CameraInfo("a", "Cam A") } };
        Assert.Single(shim.EnumerateCameras());
    }
}
