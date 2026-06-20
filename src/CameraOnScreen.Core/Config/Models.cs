namespace CameraOnScreen.Core.Config;

public enum OverlayShape { Full, RoundedRect, Circle }

[System.Flags]
public enum HotkeyModifiers { None = 0, Alt = 1, Control = 2, Shift = 4, Win = 8 }

public enum HotkeyAction { ToggleLock, ToggleClickThrough, ToggleOverlayVisible, ToggleRunning }

public sealed record OverlaySettings
{
    public double X { get; init; } = 100;
    public double Y { get; init; } = 100;
    public double Width { get; init; } = 320;
    public double Height { get; init; } = 240;
    public double Opacity { get; init; } = 1.0;
    public OverlayShape Shape { get; init; } = OverlayShape.Full;
    public bool Mirror { get; init; }
    public bool Locked { get; init; }
    public bool ClickThrough { get; init; }
}

public sealed record EffectSettings
{
    public bool GreenScreenEnabled { get; init; } = true;
    public double GreenScreenStrength { get; init; } = 1.0;
    public bool EyeContactEnabled { get; init; }
    public double EyeContactSensitivity { get; init; } = 0.5;
    public double EyeContactLookAwayRange { get; init; } = 0.5;
}

public sealed record HotkeyBinding
{
    public HotkeyAction Action { get; init; }
    public HotkeyModifiers Modifiers { get; init; }
    public uint VirtualKey { get; init; }
}

public sealed record AppConfig
{
    public string? CameraId { get; init; }
    public OverlaySettings Overlay { get; init; } = new();
    public EffectSettings Effects { get; init; } = new();
    public IReadOnlyList<HotkeyBinding> Hotkeys { get; init; } = DefaultHotkeys();

    // VK codes: F8=0x77, F9=0x78, F10=0x79, F11=0x7A
    public static IReadOnlyList<HotkeyBinding> DefaultHotkeys() => new[]
    {
        new HotkeyBinding { Action = HotkeyAction.ToggleLock,          Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x77 },
        new HotkeyBinding { Action = HotkeyAction.ToggleClickThrough,  Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x78 },
        new HotkeyBinding { Action = HotkeyAction.ToggleOverlayVisible,Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x79 },
        new HotkeyBinding { Action = HotkeyAction.ToggleRunning,       Modifiers = HotkeyModifiers.Control | HotkeyModifiers.Alt, VirtualKey = 0x7A },
    };
}
