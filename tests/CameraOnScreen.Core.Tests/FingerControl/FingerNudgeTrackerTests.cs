using CameraOnScreen.Core.FingerControl;
namespace CameraOnScreen.Core.Tests.FingerControl;

public class FingerNudgeTrackerTests
{
    private const double W = 1000, H = 1000;

    private static FingerNudgeTracker Armed(out NudgeResult last)
    {
        var t = new FingerNudgeTracker();
        last = default;
        for (int i = 0; i < 3; i++) last = t.Update(HandPose.Fist, 0.5f, 0.5f, W, H);
        return t;
    }

    [Fact] public void ArmsAfterThreeConsecutivePointingFrames()
    {
        var t = new FingerNudgeTracker();
        Assert.False(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
        Assert.False(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
        Assert.True(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
    }

    [Fact] public void OtherPoseResetsArmingStreak()
    {
        var t = new FingerNudgeTracker();
        t.Update(HandPose.Fist, 0.5f, 0.5f, W, H);
        t.Update(HandPose.Fist, 0.5f, 0.5f, W, H);
        t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H);
        Assert.False(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
    }

    [Fact] public void PointingNeverArms()
    {
        var t = new FingerNudgeTracker();
        for (int i = 0; i < 10; i++)
            Assert.False(t.Update(HandPose.Pointing, 0.5f, 0.5f, W, H).Armed);
    }

    [Fact] public void ArmingFrameProducesZeroDelta()
    {
        Armed(out var last);
        Assert.Equal(0, last.DxPx);
        Assert.Equal(0, last.DyPx);
    }

    [Fact] public void MovingRightInCameraMovesLeftOnScreen_MirrorConvention()
    {
        var t = Armed(out _);
        // Big move so EMA(0.5) output clears the 2px deadzone: 0.1 * 1.5 * 1000 * 0.5 = 75px.
        var r = t.Update(HandPose.Fist, 0.6f, 0.5f, W, H);
        Assert.True(r.Armed);
        Assert.True(r.DxPx < -10);      // camera +x -> screen -x
        Assert.Equal(0, r.DyPx, 3);
    }

    [Fact] public void MovingDownInCameraMovesDownOnScreen()
    {
        var t = Armed(out _);
        var r = t.Update(HandPose.Fist, 0.5f, 0.6f, W, H);
        Assert.True(r.DyPx > 10);       // same sign vertically
    }

    [Fact] public void StillFingerYieldsZeroAfterDeadzone()
    {
        var t = Armed(out _);
        var r = t.Update(HandPose.Fist, 0.5001f, 0.5f, W, H); // 0.15px raw -> deadzone
        Assert.Equal(0, r.DxPx);
        Assert.Equal(0, r.DyPx);
    }

    [Fact] public void GainScalesDelta()
    {
        var t = Armed(out _);
        t.Gain = 3.0;
        var r3 = t.Update(HandPose.Fist, 0.6f, 0.5f, W, H);
        var t2 = Armed(out _);
        t2.Gain = 1.5;
        var r15 = t2.Update(HandPose.Fist, 0.6f, 0.5f, W, H);
        Assert.Equal(r15.DxPx * 2, r3.DxPx, 1);
    }

    [Fact] public void StaysArmedThroughFourLostFrames_DisarmsOnFifth()
    {
        var t = Armed(out _);
        for (int i = 0; i < 4; i++)
            Assert.True(t.Update(HandPose.NoHand, 0, 0, W, H).Armed);
        Assert.False(t.Update(HandPose.NoHand, 0, 0, W, H).Armed);
    }

    [Fact] public void LostFramesProduceZeroDelta()
    {
        var t = Armed(out _);
        var r = t.Update(HandPose.NoHand, 0.9f, 0.9f, W, H);
        Assert.Equal(0, r.DxPx);
        Assert.Equal(0, r.DyPx);
    }

    [Fact] public void RearmAfterDisarmDoesNotJump()
    {
        var t = Armed(out _);
        for (int i = 0; i < 5; i++) t.Update(HandPose.NoHand, 0, 0, W, H);
        // re-arm at a totally different position: no teleport delta on the arming frame
        NudgeResult last = default;
        for (int i = 0; i < 3; i++) last = t.Update(HandPose.Fist, 0.1f, 0.1f, W, H);
        Assert.True(last.Armed);
        Assert.Equal(0, last.DxPx);
        Assert.Equal(0, last.DyPx);
    }

    [Fact] public void ResetDisarmsAndRequiresFreshArmingStreak()
    {
        var t = Armed(out var armedResult);
        Assert.True(armedResult.Armed);

        t.Reset();

        // Immediately after Reset, the tracker reports disarmed and a single Fist frame is not
        // enough to re-arm (the arming streak was cleared, not just the armed flag).
        Assert.False(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
        Assert.False(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
        Assert.True(t.Update(HandPose.Fist, 0.5f, 0.5f, W, H).Armed);
    }

    [Fact] public void ResetThenRearmAtNewPositionDoesNotJump()
    {
        var t = Armed(out _);
        // Move while armed so the tracker accumulates EMA state that must not leak past Reset.
        t.Update(HandPose.Fist, 0.6f, 0.6f, W, H);

        t.Reset();

        // Re-arm at a totally different tip position: the first armed frame after Reset must be a
        // zero delta (no teleport from stale EMA/prevX/prevY state).
        NudgeResult last = default;
        for (int i = 0; i < 3; i++) last = t.Update(HandPose.Fist, 0.9f, 0.1f, W, H);
        Assert.True(last.Armed);
        Assert.Equal(0, last.DxPx);
        Assert.Equal(0, last.DyPx);
    }
}
