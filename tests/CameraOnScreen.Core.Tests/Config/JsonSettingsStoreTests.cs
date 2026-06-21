using System.IO;
using CameraOnScreen.Core.Config;
using Xunit;

namespace CameraOnScreen.Core.Tests.Config;

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

    [Fact]
    public void Save_then_Load_round_trips_geometry_and_flags()
    {
        var path = TempFile();
        try
        {
            var store = new JsonSettingsStore(path);
            var cfg = new AppConfig
            {
                Overlay = new OverlaySettings
                {
                    X = 12, Y = 34, Width = 567, Height = 890,
                    Locked = true, ClickThrough = true
                },
                Effects = new EffectSettings
                {
                    GreenScreenEnabled = false, GreenScreenExpand = 0.25, GreenScreenFeather = 0.4,
                    EyeContactEnabled = true, EyeContactSensitivity = 0.75,
                    EyeContactLookAwayRange = 0.9
                }
            };
            store.Save(cfg);

            var loaded = store.Load();

            Assert.Equal(12, loaded.Overlay.X);
            Assert.Equal(34, loaded.Overlay.Y);
            Assert.Equal(567, loaded.Overlay.Width);
            Assert.Equal(890, loaded.Overlay.Height);
            Assert.True(loaded.Overlay.Locked);
            Assert.True(loaded.Overlay.ClickThrough);
            Assert.False(loaded.Effects.GreenScreenEnabled);
            Assert.Equal(0.25, loaded.Effects.GreenScreenExpand);
            Assert.Equal(0.4, loaded.Effects.GreenScreenFeather);
            Assert.True(loaded.Effects.EyeContactEnabled);
            Assert.Equal(0.75, loaded.Effects.EyeContactSensitivity);
            Assert.Equal(0.9, loaded.Effects.EyeContactLookAwayRange);
        }
        finally
        {
            var dir = Path.GetDirectoryName(path);
            if (dir is not null && Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void Save_then_load_round_trips_overlay_mirror_and_zoom()
    {
        var path = Path.Combine(Path.GetTempPath(), $"cos-cfg-{Guid.NewGuid():N}.json");
        try
        {
            var store = new JsonSettingsStore(path);
            store.Save(new AppConfig
            {
                Overlay = new OverlaySettings { Mirror = true, Zoom = 2.5 }
            });

            var loaded = store.Load();

            Assert.True(loaded.Overlay.Mirror);
            Assert.Equal(2.5, loaded.Overlay.Zoom);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
