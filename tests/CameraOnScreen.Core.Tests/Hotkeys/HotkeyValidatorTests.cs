using CameraOnScreen.Core.Config;
using CameraOnScreen.Core.Hotkeys;
using Xunit;

public class HotkeyValidatorTests
{
    [Fact]
    public void Default_hotkeys_have_no_conflicts()
    {
        Assert.False(HotkeyValidator.HasConflict(AppConfig.DefaultHotkeys(), out _));
    }

    [Fact]
    public void Same_chord_for_two_actions_conflicts()
    {
        var bindings = new[]
        {
            new HotkeyBinding { Action = HotkeyAction.ToggleLock, Modifiers = HotkeyModifiers.Alt, VirtualKey = 0x77 },
            new HotkeyBinding { Action = HotkeyAction.ToggleRunning, Modifiers = HotkeyModifiers.Alt, VirtualKey = 0x77 },
        };
        Assert.True(HotkeyValidator.HasConflict(bindings, out var conflict));
        Assert.NotNull(conflict);
    }
}
