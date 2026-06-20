using System.IO;
using CameraOnScreen.Core.Config;
using Xunit;

public class JsonSettingsStoreTests
{
    private static string TempFile() =>
        Path.Combine(Path.GetTempPath(), "cos-" + Path.GetRandomFileName(), "config.json");

    [Fact]
    public void Load_returns_defaults_when_file_missing()
    {
        var store = new JsonSettingsStore(TempFile());
        Assert.Equal(new AppConfig(), store.Load());
    }

    [Fact]
    public void Save_then_Load_round_trips()
    {
        var path = TempFile();
        var store = new JsonSettingsStore(path);
        var cfg = new AppConfig { CameraId = "cam-7" };
        store.Save(cfg);
        Assert.True(File.Exists(path));
        Assert.Equal("cam-7", store.Load().CameraId);
    }

    [Fact]
    public void Load_returns_defaults_when_file_corrupt()
    {
        var path = TempFile();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "{ this is not json ");
        var store = new JsonSettingsStore(path);
        Assert.Equal(new AppConfig(), store.Load());
    }
}
