namespace CameraOnScreen.Core.FingerControl;

/// <summary>☝ detector on 21 MediaPipe hand landmarks: index finger extended (tip clearly farther
/// from the wrist than its PIP), middle/ring/pinky curled (tip not farther than PIP). Thumb ignored.
/// ponytail: wrist-distance ratios only — no joint angles; revisit if sideways-pointing misfires.</summary>
public static class HandPoseClassifier
{
    public const float PalmScoreMin = 0.7f;
    public const float PresenceMin = 0.6f;
    public const int IndexTip = 8;
    private const float ExtendMargin = 1.15f; // tip must beat PIP distance by 15% to count extended

    public static HandPose Classify(ReadOnlySpan<float> xs, ReadOnlySpan<float> ys,
        float palmScore, float presenceScore)
    {
        if (palmScore < PalmScoreMin || presenceScore < PresenceMin) return HandPose.NoHand;

        static float D(ReadOnlySpan<float> xs, ReadOnlySpan<float> ys, int i)
        {
            float dx = xs[i] - xs[0], dy = ys[i] - ys[0];
            return MathF.Sqrt(dx * dx + dy * dy);
        }
        bool indexExtended = D(xs, ys, 8) > D(xs, ys, 6) * ExtendMargin;
        bool middleCurled = D(xs, ys, 12) <= D(xs, ys, 10) * ExtendMargin;
        bool ringCurled = D(xs, ys, 16) <= D(xs, ys, 14) * ExtendMargin;
        bool pinkyCurled = D(xs, ys, 20) <= D(xs, ys, 18) * ExtendMargin;
        return indexExtended && middleCurled && ringCurled && pinkyCurled
            ? HandPose.Pointing : HandPose.Other;
    }
}
