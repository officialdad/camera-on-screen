using System.IO;

namespace CameraOnScreen.Core.Config;

public sealed class JsonSettingsStore : ISettingsStore
{
    private readonly string _filePath;

    public JsonSettingsStore(string filePath) => _filePath = filePath;

    // Windows: %LOCALAPPDATA% (existing contract, WinUI app unchanged). Elsewhere:
    // SpecialFolder.ApplicationData maps to $XDG_CONFIG_HOME (default ~/.config) on Unix,
    // which is where Linux config belongs (issue #29).
    public static string DefaultPath() => Path.Combine(
        Environment.GetFolderPath(OperatingSystem.IsWindows()
            ? Environment.SpecialFolder.LocalApplicationData
            : Environment.SpecialFolder.ApplicationData),
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
