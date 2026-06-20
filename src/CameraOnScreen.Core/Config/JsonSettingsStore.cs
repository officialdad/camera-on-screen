using System.IO;

namespace CameraOnScreen.Core.Config;

public sealed class JsonSettingsStore : ISettingsStore
{
    private readonly string _filePath;

    public JsonSettingsStore(string filePath) => _filePath = filePath;

    public static string DefaultPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CameraOnScreen", "config.json");

    public AppConfig Load()
    {
        try
        {
            if (!File.Exists(_filePath)) return new AppConfig();
            return ConfigSerializer.Deserialize(File.ReadAllText(_filePath));
        }
        catch
        {
            return new AppConfig(); // missing/corrupt => safe defaults
        }
    }

    public void Save(AppConfig config)
    {
        var dir = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(_filePath, ConfigSerializer.Serialize(config));
    }
}
