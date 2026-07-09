namespace CameraOnScreen.Core.FingerControl;

public enum HandPose { NoHand, Other, Pointing, Fist }

/// <summary>Palm ROI in normalized (0..1) coordinates of the letterboxed square model input,
/// axis-aligned square.</summary>
public readonly record struct PalmRoi(float CenterX, float CenterY, float Side);
