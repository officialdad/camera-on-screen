using System.Collections.Generic;
using CameraOnScreen.Core.Config;

namespace CameraOnScreen.Core.Hotkeys;

public static class HotkeyValidator
{
    public static bool HasConflict(
        IReadOnlyList<HotkeyBinding> bindings,
        out (HotkeyBinding a, HotkeyBinding b)? conflict)
    {
        for (int i = 0; i < bindings.Count; i++)
        for (int j = i + 1; j < bindings.Count; j++)
        {
            var a = bindings[i];
            var b = bindings[j];
            if (a.Modifiers == b.Modifiers && a.VirtualKey == b.VirtualKey && a.Action != b.Action)
            {
                conflict = (a, b);
                return true;
            }
        }
        conflict = null;
        return false;
    }
}
