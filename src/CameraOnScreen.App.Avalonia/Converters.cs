using System;
using System.Collections.Generic;
using System.Globalization;
using Avalonia.Data.Converters;

namespace CameraOnScreen.App.Avalonia;

// Avalonia has no x:Bind function bindings, so the WinUI code-behind helpers
// (StatusLine, ExposureSliderEnabled, QualityEnabled) become MultiBinding converters.
// The `!available` visibilities use Avalonia's built-in `{Binding !Prop}` negation instead.

/// <summary>[IsRunning(bool), Fps(double), FrameWidth(int), FrameHeight(int)] -> status string.</summary>
public sealed class StatusLineConverter : IMultiValueConverter
{
    public object Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
    {
        bool running = values.Count > 0 && values[0] is bool b && b;
        double fps = values.Count > 1 && values[1] is double d ? d : 0;
        int w = values.Count > 2 && values[2] is int iw ? iw : 0;
        int h = values.Count > 3 && values[3] is int ih ? ih : 0;
        if (!running) return "Stopped";
        // Size stays hidden until the first frame lands, so it never shows a stale 0×0.
        return w > 0 && h > 0 ? $"Running — {fps:F0} fps — {w}×{h}" : $"Running — {fps:F0} fps";
    }
}

/// <summary>All bound values must be true. Mirrors ExposureSliderEnabled(supported, locked).</summary>
public sealed class AllTrueConverter : IMultiValueConverter
{
    public object Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
    {
        foreach (var v in values)
            if (v is not bool b || !b) return false;
        return true;
    }
}

/// <summary>[SuperResAvailable(bool), SuperResModeIndex(int)] -> available AND mode != 0.
/// Mirrors WinUI MainWindow.QualityEnabled.</summary>
public sealed class QualityEnabledConverter : IMultiValueConverter
{
    public object Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
    {
        bool available = values.Count > 0 && values[0] is bool b && b;
        int mode = values.Count > 1 && values[1] is int i ? i : 0;
        return available && mode != 0;
    }
}
