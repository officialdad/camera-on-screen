namespace CameraOnScreen.Core.Config;

public interface ISettingsStore
{
    AppConfig Load();
    void Save(AppConfig config);
}
