using System.Text.Json;
using System.Text.Json.Serialization;

namespace CameraOnScreen.Core.Config;

public static class ConfigSerializer
{
    public static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    public static string Serialize(AppConfig config) => JsonSerializer.Serialize(config, Options);

    public static AppConfig Deserialize(string json) =>
        JsonSerializer.Deserialize<AppConfig>(json, Options)
        ?? throw new JsonException("Deserialized AppConfig was null.");
}
