using CameraOnScreen.Core.FingerControl;
namespace CameraOnScreen.Core.Tests.FingerControl;

public class HandPoseClassifierTests
{
    // Synthetic hand, wrist at (0.5, 0.9), fingers up the frame (y decreases upward).
    private static (float[] xs, float[] ys) Hand(bool indexExtended, bool othersExtended)
    {
        var xs = new float[21]; var ys = new float[21];
        for (int i = 0; i < 21; i++) { xs[i] = 0.5f; ys[i] = 0.85f; } // default: near palm
        xs[0] = 0.5f; ys[0] = 0.9f;                                   // wrist
        xs[5] = 0.50f; ys[5] = 0.70f;                                 // index MCP
        xs[6] = 0.50f; ys[6] = indexExtended ? 0.60f : 0.78f;         // index PIP
        xs[8] = 0.50f; ys[8] = indexExtended ? 0.40f : 0.84f;         // index TIP
        (int pip, int tip)[] others = { (10, 12), (14, 16), (18, 20) };
        float xo = 0.55f;
        foreach (var (pip, tip) in others)
        {
            xs[pip] = xo; ys[pip] = 0.68f;
            xs[tip] = xo; ys[tip] = othersExtended ? 0.42f : 0.80f;   // extended: far; curled: tip below pip
            xo += 0.04f;
        }
        return (xs, ys);
    }

    [Fact] public void PointingHand_IsPointing()
    {
        var (xs, ys) = Hand(indexExtended: true, othersExtended: false);
        Assert.Equal(HandPose.Pointing, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.9f));
    }

    [Fact] public void OpenPalm_IsOther()
    {
        var (xs, ys) = Hand(indexExtended: true, othersExtended: true);
        Assert.Equal(HandPose.Other, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.9f));
    }

    [Fact] public void Fist_IsOther()
    {
        var (xs, ys) = Hand(indexExtended: false, othersExtended: false);
        Assert.Equal(HandPose.Other, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.9f));
    }

    [Fact] public void LowPalmScore_IsNoHand()
    {
        var (xs, ys) = Hand(true, false);
        Assert.Equal(HandPose.NoHand, HandPoseClassifier.Classify(xs, ys, 0.5f, 0.9f));
    }

    [Fact] public void LowPresence_IsNoHand()
    {
        var (xs, ys) = Hand(true, false);
        Assert.Equal(HandPose.NoHand, HandPoseClassifier.Classify(xs, ys, 0.9f, 0.4f));
    }
}
