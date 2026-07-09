using CameraOnScreen.Core.FingerControl;

namespace CameraOnScreen.Core.Tests.FingerControl;

public class PalmDecoderTests
{
    private static float[] Row(float score, float x, float y, float size) =>
        new[] { score, x, y, size, 0f, 0f, 0f, 0f };

    [Fact]
    public void EmptyOutput_ReturnsNull() =>
        Assert.Null(PalmDecoder.DecodeBest(ReadOnlySpan<float>.Empty, 0.7f));

    [Fact]
    public void BelowThreshold_ReturnsNull()
    {
        var rows = Row(0.65f, 0.5f, 0.5f, 0.2f);
        Assert.Null(PalmDecoder.DecodeBest(rows, 0.7f));
    }

    [Fact]
    public void PicksHighestScoreRow()
    {
        var rows = Row(0.8f, 0.3f, 0.3f, 0.1f).Concat(Row(0.95f, 0.6f, 0.7f, 0.2f)).ToArray();
        var roi = PalmDecoder.DecodeBest(rows, 0.7f);
        Assert.NotNull(roi);
        Assert.Equal(0.6f, roi!.Value.CenterX, 4);
        Assert.Equal(0.7f, roi.Value.CenterY, 4);
    }

    [Fact]
    public void ExpandsBoxSizeBy2_6()
    {
        var rows = Row(0.9f, 0.5f, 0.5f, 0.2f);
        var roi = PalmDecoder.DecodeBest(rows, 0.7f);
        Assert.Equal(2.6f * 0.2f, roi!.Value.Side, 4);
    }
}
