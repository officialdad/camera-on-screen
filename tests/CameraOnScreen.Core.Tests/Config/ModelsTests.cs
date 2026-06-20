using CameraOnScreen.Core.Config;
using Xunit;
using Xunit.Abstractions;

namespace CameraOnScreen.Core.Tests.Config;

public class ModelsTests
{
    private readonly ITestOutputHelper _output;
    public ModelsTests(ITestOutputHelper output) => _output = output;

    [Fact]
    public void Flags_modifiers_round_trip_as_names()
    {
        var config = new AppConfig(); // defaults include Control|Alt on all 4 hotkeys
        var json = ConfigSerializer.Serialize(config);
        _output.WriteLine("Serialized JSON snippet (Modifiers):");
        _output.WriteLine(json);

        // Must NOT contain a bare numeric modifier (e.g. "Modifiers": 3)
        Assert.DoesNotContain("\"Modifiers\": 3", json);
        Assert.DoesNotContain("\"Modifiers\":3", json);

        // Must contain name-based representation — "Alt" appears in "Alt, Control" or "Control, Alt"
        Assert.Contains("Alt", json);

        // Round-trip: deserialized modifier must equal the composite flags value
        var back = ConfigSerializer.Deserialize(json);
        Assert.Equal(HotkeyModifiers.Control | HotkeyModifiers.Alt, back.Hotkeys[0].Modifiers);
    }

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
