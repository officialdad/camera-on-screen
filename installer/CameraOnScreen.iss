; CameraOnScreen.iss — Inno Setup 6 script. Compiled by scripts\build-installer.ps1.
; The orchestrator passes /DSourceDir=<publish+bundle staging dir> and /DAppVersion=<x.y.z>.
; Defaults below let the script be opened/checked standalone.

#ifndef SourceDir
  #define SourceDir "..\dist\stage"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif

[Setup]
AppId={{6C6D5E07-D334-456C-9E31-2D0C3069BA89}
AppName=Camera on Screen
AppVersion={#AppVersion}
AppPublisher=officialdad
AppPublisherURL=https://github.com/officialdad/camera-on-screen
DefaultDirName={localappdata}\Programs\CameraOnScreen
DefaultGroupName=Camera on Screen
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=CameraOnScreen-Setup-{#AppVersion}-x64
SetupIconFile=..\cos.ico
WizardStyle=modern
; Combined end-user license: the app's MIT terms PLUS the NVIDIA Maxine SDK license
; flow-down for the bundled runtime/models (Maxine SDK License §1.2(v) — end-user terms
; must be consistent with NVIDIA's). NOT the bare MIT LICENSE, which covers our code only.
LicenseFile=COMBINED-LICENSE.txt
UninstallDisplayIcon={app}\CameraOnScreen.App.exe
UninstallDisplayName=Camera on Screen

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
; Ship the consolidated third-party notices into the install dir (repo-root file, path
; relative to this .iss). The per-SDK NVIDIA notice texts already ride inside {#SourceDir}\maxine\.
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Camera on Screen"; Filename: "{app}\CameraOnScreen.App.exe"
Name: "{group}\Uninstall Camera on Screen"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Camera on Screen"; Filename: "{app}\CameraOnScreen.App.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\CameraOnScreen.App.exe"; Description: "{cm:LaunchProgram,Camera on Screen}"; Flags: nowait postinstall skipifsilent

[Code]
// Soft preflight: warn (do not block) when no NVIDIA display adapter is present.
// The app degrades to a plain overlay without RTX, so installation must still proceed.
function HasNvidiaGpu(): Boolean;
var
  Names: TArrayOfString;
  I: Integer;
  Desc: String;
  Key: String;
begin
  Result := False;
  // NOTE: Inno constant-expansion ({app} etc.) does NOT apply inside [Code] string
  // literals, so the single '{' below is correct — do not change it to '{{'.
  Key := 'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}';
  if RegGetSubkeyNames(HKLM, Key, Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      if RegQueryStringValue(HKLM, Key + '\' + Names[I], 'DriverDesc', Desc) then
      begin
        if Pos('NVIDIA', Uppercase(Desc)) > 0 then
        begin
          Result := True;
          Exit;
        end;
      end;
    end;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not HasNvidiaGpu() then
    MsgBox('No NVIDIA GPU was detected.' + #13#10 + #13#10 +
           'The AI effects (green screen, eye contact) require an NVIDIA RTX GPU. ' +
           'The app will still install and run as a plain webcam overlay.',
           mbInformation, MB_OK);
end;
