using CameraOnScreen.Core.Overlay;
using Xunit;

namespace CameraOnScreen.Core.Tests.Overlay;

public class OverlaySizerTests
{
    // A monitor large enough that no clamp engages, for the basic grow/shrink cases.
    private static readonly Rect BigWorkArea = new(0, 0, 3840, 2160);

    [Fact]
    public void Zero_notches_returns_current_unchanged()
    {
        var cur = new Rect(200, 200, 320, 240);
        Assert.Equal(cur, OverlaySizer.Resize(cur, 0, BigWorkArea));
    }

    [Fact]
    public void One_notch_up_grows_about_8_percent_and_keeps_center()
    {
        // 320x240 @ (200,200): center (360,320). factor 1.08 -> H=round(259.2)=259,
        // W=round(259*4/3)=345. newX=360-345/2=188, newY=320-259/2=191.
        var result = OverlaySizer.Resize(new Rect(200, 200, 320, 240), 1, BigWorkArea);
        Assert.Equal(345, result.W);
        Assert.Equal(259, result.H);
        Assert.Equal(188, result.X);
        Assert.Equal(191, result.Y);
    }

    [Fact]
    public void One_notch_down_shrinks()
    {
        // factor 1/1.08=0.9259 -> H=round(222.2)=222, W=round(222*4/3)=296.
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), -1, BigWorkArea);
        Assert.Equal(296, result.W);
        Assert.Equal(222, result.H);
    }

    [Fact]
    public void Aspect_ratio_is_preserved_within_one_pixel()
    {
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), 3, BigWorkArea);
        Assert.True(System.Math.Abs((double)result.W / result.H - 320.0 / 240.0) < 0.02);
    }

    [Fact]
    public void Shrinking_past_min_height_floors_at_min_keeping_aspect()
    {
        // Huge negative notch count collapses toward zero; floored to MinHeight=120,
        // W=round(120*4/3)=160.
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), -50, BigWorkArea);
        Assert.Equal(OverlaySizer.MinHeight, result.H);
        Assert.Equal(160, result.W);
    }

    [Fact]
    public void Growth_is_clamped_to_work_area_height()
    {
        // 320x240 in a 400x300 work area, big zoom: height capped at 300, W=round(300*4/3)=400.
        var result = OverlaySizer.Resize(new Rect(0, 0, 320, 240), 10, new Rect(0, 0, 400, 300));
        Assert.Equal(300, result.H);
        Assert.Equal(400, result.W);
    }

    [Fact]
    public void Growth_is_clamped_to_work_area_width_for_wide_overlays()
    {
        // 400x200 (2:1) in a 500x500 work area: width caps at 500, H=round(500/2)=250.
        var result = OverlaySizer.Resize(new Rect(0, 0, 400, 200), 10, new Rect(0, 0, 500, 500));
        Assert.Equal(500, result.W);
        Assert.Equal(250, result.H);
    }
}
