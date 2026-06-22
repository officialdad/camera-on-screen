using System;
using CameraOnScreen.Core;
using Xunit;

namespace CameraOnScreen.Core.Tests;

public class AppInfoTests
{
    // NVIDIA Maxine SDK License Supplement §3.1 requires the app to attribute SDK use per
    // NVIDIA's branding/trademark rules: credit the company + product, ® on first NVIDIA use,
    // ™ on the product, and never "debrand". These assertions encode that format requirement.
    [Fact]
    public void Maxine_attribution_credits_nvidia_and_maxine_with_trademark_symbols()
    {
        var a = AppInfo.MaxineAttribution;
        Assert.Contains("NVIDIA", a);
        Assert.Contains("Maxine", a);
        Assert.Contains("®", a); // ® registration symbol (first NVIDIA reference)
        Assert.Contains("™", a); // ™ on the Maxine product mark
        Assert.Contains("powered by", a, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Trademark_notice_attributes_marks_to_nvidia_corporation()
    {
        var t = AppInfo.MaxineTrademarkNotice;
        Assert.Contains("trademark", t, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("NVIDIA Corporation", t);
    }
}
