using System;

namespace CameraOnScreen.Core.Overlay;

/// <summary>Screen rectangle in pixels. Win32-free so it stays unit-testable in Core.</summary>
public readonly record struct Rect(int X, int Y, int W, int H);

/// <summary>
/// Pure geometry for mouse-wheel overlay resize. One wheel notch scales the overlay by
/// <see cref="StepPerNotch"/>; aspect ratio is preserved (driven by height), height is floored at
/// <see cref="MinHeight"/>, the result is clamped to the monitor work area, and the overlay's
/// center stays fixed (center-anchored resize).
/// </summary>
public static class OverlaySizer
{
    /// <summary>Fractional size change per wheel notch (one WHEEL_DELTA = 120 units).</summary>
    public const double StepPerNotch = 0.08;

    /// <summary>Smallest overlay height (px); a shrink never goes below this.</summary>
    public const int MinHeight = 120;

    public static Rect Resize(Rect current, int notches, Rect workArea)
    {
        if (notches == 0) return current;

        double aspect = current.W / (double)current.H;
        double factor = Math.Pow(1.0 + StepPerNotch, notches);

        int newH = (int)Math.Round(current.H * factor);
        if (newH < MinHeight) newH = MinHeight;
        if (newH > workArea.H) newH = workArea.H;

        int newW = (int)Math.Round(newH * aspect);
        if (newW > workArea.W)
        {
            newW = workArea.W;
            newH = (int)Math.Round(newW / aspect);
        }

        // Center-anchored: keep the current center point fixed.
        int cx = current.X + current.W / 2;
        int cy = current.Y + current.H / 2;
        return new Rect(cx - newW / 2, cy - newH / 2, newW, newH);
    }
}
