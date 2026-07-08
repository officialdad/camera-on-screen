namespace CameraOnScreen.Core.FingerControl;

/// <summary>Selects the best palm from the post-processed detector output (rows of 8:
/// score, boxX, boxY, boxSize, kp0x, kp0y, kp2x, kp2y — normalized 0..1 relative to the
/// letterboxed square input; decode + NMS are baked into the ONNX graph and scores are already
/// probabilities, so argmax over rows is all that's left). Single-hand use: no NMS.
/// ponytail: axis-aligned ROI, no palm-keypoint rotation — pointing hands are near-upright; add
/// MediaPipe's rotated-crop math if landmarks degrade on tilted hands.</summary>
public static class PalmDecoder
{
    // ROI expansion palm-box -> hand-crop (MediaPipe uses ~2.6 for the hand crop).
    public const float RoiExpand = 2.6f;
    public const int RowStride = 8;

    public static PalmRoi? DecodeBest(ReadOnlySpan<float> rows, float scoreMin)
    {
        int best = -1;
        float bestScore = scoreMin;
        for (int o = 0; o + RowStride <= rows.Length; o += RowStride)
            if (rows[o] >= bestScore) { bestScore = rows[o]; best = o; }
        if (best < 0) return null;
        return new PalmRoi(rows[best + 1], rows[best + 2], RoiExpand * rows[best + 3]);
    }
}
