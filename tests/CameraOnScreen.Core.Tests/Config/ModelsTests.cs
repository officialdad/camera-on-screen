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
        var config = new AppConfig(); // defaults include Control|Alt on all hotkeys
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
        Assert.Equal(2, c.Hotkeys.Count); // ToggleOverlayVisible, ToggleRunning (Lock/ClickThrough are dead)
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

    [Fact]
    public void MinimizeToTray_defaults_on_and_round_trips_when_disabled()
    {
        Assert.True(new AppConfig().MinimizeToTray);

        var json = ConfigSerializer.Serialize(new AppConfig { MinimizeToTray = false });
        Assert.False(ConfigSerializer.Deserialize(json).MinimizeToTray);
    }

    [Fact]
    public void MinimizeToTray_absent_from_json_stays_enabled()
    {
        // A config.json written before the feature existed has no key at all: System.Text.Json
        // leaves the property at its initializer, so upgrading users get the tray by default.
        var back = ConfigSerializer.Deserialize("{ \"CameraId\": \"cam\" }");
        Assert.True(back.MinimizeToTray);
    }

    [Fact]
    public void MinimizeToTray_participates_in_equality()
    {
        // AppConfig.Equals is hand-written — a field left out of it silently makes the
        // "did anything change?" comparison lie.
        Assert.NotEqual(new AppConfig(), new AppConfig { MinimizeToTray = false });
    }
}
