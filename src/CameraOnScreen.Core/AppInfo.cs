namespace CameraOnScreen.Core;

/// <summary>
/// Product / attribution strings surfaced in the UI.
///
/// <para><see cref="MaxineAttribution"/> satisfies the NVIDIA Maxine SDK License Supplement
/// §3.1 — a desktop app that integrates the SDK must attribute that use per NVIDIA's branding
/// guidelines. It follows NVIDIA's documented trademark-attribution format (® on the first
/// NVIDIA reference, ™ on the product mark, no "debranding" / composite marks).</para>
///
/// <para>The Maxine branding portal (nvidia.com/maxine-sdk-guidelines → brand.nvidia.com) is
/// interactive + login-gated and could not be read to confirm an exact mandated string; this
/// wording is a good-faith fit to the verifiable format. If the portal prescribes different
/// exact wording/placement, update this single constant. Confirm via maxinesdk-support@nvidia.com.
/// See docs/superpowers/specs/2026-06-22-camera-on-screen-m5-license-compliance.md.</para>
/// </summary>
public static class AppInfo
{
    public const string MaxineAttribution = "AI effects powered by NVIDIA® Maxine™";

    public const string MaxineTrademarkNotice =
        "NVIDIA, Maxine, RTX, GeForce, TensorRT, and CUDA are trademarks and/or " +
        "registered trademarks of NVIDIA Corporation.";
}
