namespace CameraOnScreen.Core.FingerControl;

public readonly record struct NudgeResult(double DxPx, double DyPx, bool Armed);

/// <summary>Air-trackpad state machine: ☝ held for ArmFrames arms; fingertip deltas (EMA-smoothed,
/// deadzoned, gain-scaled) drive the overlay; pose lost for DisarmFrames disarms. Pure logic —
/// the caller owns threading, clamping, and drag suppression.</summary>
public sealed class FingerNudgeTracker
{
    public const int ArmFrames = 3;
    public const int DisarmFrames = 5;
    public const double EmaAlpha = 0.5;
    public const double DeadzonePx = 2.0;
    // Camera frames are unmirrored (user-left = image-right), so horizontal flips; vertical doesn't.
    private const double HorizontalSign = -1.0;

    public double Gain { get; set; } = 1.5;

    private int _pointingStreak, _lostStreak;
    private bool _armed;
    private float _prevX, _prevY;
    private double _emaDx, _emaDy;

    public NudgeResult Update(HandPose pose, float tipX, float tipY, double screenWpx, double screenHpx)
    {
        if (pose != HandPose.Pointing)
        {
            _pointingStreak = 0;
            if (_armed && ++_lostStreak >= DisarmFrames) Disarm();
            return new NudgeResult(0, 0, _armed);
        }

        _lostStreak = 0;
        if (!_armed)
        {
            if (++_pointingStreak >= ArmFrames)
            {
                _armed = true;
                _prevX = tipX; _prevY = tipY;   // baseline here: the arming frame never jumps
                _emaDx = _emaDy = 0;
            }
            return new NudgeResult(0, 0, _armed);
        }

        double rawDx = HorizontalSign * (tipX - _prevX) * Gain * screenWpx;
        double rawDy = (tipY - _prevY) * Gain * screenHpx;
        _prevX = tipX; _prevY = tipY;
        _emaDx = EmaAlpha * rawDx + (1 - EmaAlpha) * _emaDx;
        _emaDy = EmaAlpha * rawDy + (1 - EmaAlpha) * _emaDy;
        return Math.Sqrt(_emaDx * _emaDx + _emaDy * _emaDy) < DeadzonePx
            ? new NudgeResult(0, 0, true)
            : new NudgeResult(_emaDx, _emaDy, true);
    }

    /// <summary>Force the tracker back to the disarmed, un-smoothed baseline (e.g. on Stop/Start of
    /// the inference loop) — a fresh ArmFrames streak of Pointing is required to re-arm, and the
    /// first armed frame after that yields a zero delta (no stale-EMA teleport).</summary>
    public void Reset() => Disarm();

    private void Disarm()
    {
        _armed = false;
        _pointingStreak = _lostStreak = 0;
        _emaDx = _emaDy = 0;
    }
}
