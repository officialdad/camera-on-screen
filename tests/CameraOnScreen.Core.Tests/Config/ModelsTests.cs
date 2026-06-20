using CameraOnScreen.Core.Config;
using Xunit;

public class ModelsTests
{
    [Fact]
    public void AppConfig_defaults_are_sane()
    {
        var c = new AppConfig();
        Assert.Null(c.CameraId);
        Assert.Equal(OverlayShape.Full, c.Overlay.Shape);
        Assert.True(c.Effects.GreenScreenEnabled);
        Assert.False(c.Effects.EyeContactEnabled);
        Assert.Equal(4, c.Hotkeys.Count); // one per HotkeyAction
    }

    [Fact]
    public void Round_trips_through_json_with_enum_names()
    {
        var c = new AppConfig
        {
            CameraId = "cam-1",
            Overlay = new OverlaySettings { Shape = OverlayShape.Circle, Mirror = true, X = 50 }
        };
        var json = ConfigSerializer.Serialize(c);
        Assert.Contains("\"Circle\"", json); // enum serialized as name, not number
        var back = ConfigSerializer.Deserialize(json);
        Assert.Equal(c.CameraId, back.CameraId);
        Assert.Equal(c.Overlay, back.Overlay);
        Assert.Equal(c.Effects, back.Effects);
        Assert.True(back.Hotkeys.SequenceEqual(c.Hotkeys));
    }
}
