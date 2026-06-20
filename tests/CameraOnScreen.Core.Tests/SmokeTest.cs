using CameraOnScreen.Core;
using Xunit;

namespace CameraOnScreen.Core.Tests;

public class SmokeTest
{
    [Fact]
    public void Core_is_referenced()
    {
        Assert.Equal("CameraOnScreen.Core", BuildMarker.Name);
    }
}
