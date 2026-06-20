using CameraOnScreen.Core;
using Xunit;

public class SmokeTest
{
    [Fact]
    public void Core_is_referenced()
    {
        Assert.Equal("CameraOnScreen.Core", BuildMarker.Name);
    }
}
