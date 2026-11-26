; Installer for Muntin. Build the Release exe and publish the settings window into an empty folder first
; (everything in it is packaged), then:
;   Remove-Item -Recurse -Force x64\Release\MuntinSettings -ErrorAction SilentlyContinue
;   dotnet publish src\modules\fancyzones\MuntinSettings\Muntin.Settings\Muntin.Settings.csproj -c Release -r win-x64 -o x64\Release\MuntinSettings
;   ISCC.exe installer\Muntin.iss
; It installs for the current user only, without administrator rights, into the folder that also holds the
; settings and layouts. Uninstalling removes the program and its shortcuts and keeps the settings and layouts.
; The settings window (Muntin.Settings.exe, in {app}\Settings where the tray's "Settings..." looks first) is
; framework-dependent: it needs the .NET 9 Runtime and Windows App Runtime 2.2 or later.

#define AppExe AddBackslash(SourcePath) + "..\x64\Release\Muntin.exe"
#define AppVersion GetVersionNumbersString(AppExe)
#if AppVersion == ""
  #error Build the Release Muntin.exe first
#endif
#define SettingsDir AddBackslash(SourcePath) + "..\x64\Release\MuntinSettings"
#if !FileExists(SettingsDir + "\Muntin.Settings.exe")
  #error Publish Muntin.Settings to x64\Release\MuntinSettings first (see the top of this file)
#endif

[Setup]
AppId={{04BEEECC-6BE4-488B-B335-89696E1B2207}
AppName=Muntin
AppVersion={#AppVersion}
AppPublisher=Muntin contributors
AppPublisherURL=https://github.com/c-hurtado/Muntin
AppSupportURL=https://github.com/c-hurtado/Muntin/issues
LicenseFile=..\LICENSE
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Muntin
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\Muntin.exe
SetupIconFile=..\src\modules\fancyzones\FancyZonesLib\icon.ico
OutputDir=..\x64\Release\installer
OutputBaseFilename=Muntin-{#AppVersion}-setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; [Code] asks a running Muntin to quit first; this only catches what that missed, and never forces it
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "startup"; Description: "Start Muntin when I sign in"

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "..\NOTICE.md"; DestDir: "{app}"
Source: "{#SettingsDir}\*"; DestDir: "{app}\Settings"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Files an older settings window left behind (an update replaces the whole folder)
Type: filesandordirs; Name: "{app}\Settings"

[Icons]
Name: "{userprograms}\Muntin\Muntin"; Filename: "{app}\Muntin.exe"
Name: "{userprograms}\Muntin\Muntin Settings"; Filename: "{app}\Settings\Muntin.Settings.exe"; Parameters: "--muntin ""{app}\Muntin.exe"""; IconFilename: "{app}\Muntin.exe"
; The same shortcut the tray menu's "Start at sign-in" item writes
Name: "{userstartup}\Muntin"; Filename: "{app}\Muntin.exe"; Tasks: startup

[Run]
Filename: "{app}\Muntin.exe"; Description: "Start Muntin now"; Flags: nowait postinstall

[UninstallDelete]
; Start menu entries of the named layouts, which Muntin makes, and the sign-in shortcut if the tray made it
Type: files; Name: "{userprograms}\Muntin\*.lnk"
Type: dirifempty; Name: "{userprograms}\Muntin"
Type: files; Name: "{userstartup}\Muntin.lnk"

[Code]
const
  WM_ENDSESSION = $0016;
  WM_CLOSE = $0010;

// Asks a running Muntin to quit the way it does when Windows shuts down, which brings scrolled windows back.
// Killing it would leave them parked off screen. False when it is still running, for example as administrator.
function QuitMuntin(): Boolean;
var
  Window: HWND;
  Tries: Integer;
begin
  Window := FindWindowByClassName('Muntin');
  if Window <> 0 then
    PostMessage(Window, WM_ENDSESSION, 1, 0);
  Tries := 0;
  while (FindWindowByClassName('Muntin') <> 0) and (Tries < 50) do
  begin
    Sleep(100);
    Tries := Tries + 1;
  end;
  Result := FindWindowByClassName('Muntin') = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not QuitMuntin() then
    Result := 'Muntin is still running. Quit it from its tray icon, then run Setup again.';
end;

function HasDir(const Pattern: String): Boolean;
var
  Found: TFindRec;
begin
  Result := FindFirst(ExpandConstant(Pattern), Found);
  if Result then
    FindClose(Found);
end;

// The settings window runs on the shared .NET 9 runtime (Microsoft.NETCore.App 9.x only: the default roll-forward
// stays on 9). On ARM64 the x64 runtime lives under dotnet\x64. Windows App Runtime is not checked here: the app's
// bootstrapper offers to install it on first start.
function HasDotNet9(): Boolean;
begin
  Result := HasDir('{commonpf64}\dotnet\shared\Microsoft.NETCore.App\9.*') or
    HasDir('{commonpf64}\dotnet\x64\shared\Microsoft.NETCore.App\9.*');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not HasDotNet9() then
    SuppressibleMsgBox('The Muntin settings window needs the .NET 9 Runtime, which is not installed. Muntin itself works without it.' + #13#10#13#10 +
      'Get it from https://dotnet.microsoft.com/download/dotnet/9.0 (".NET Runtime", x64).', mbInformation, MB_OK, IDOK);
end;

// The settings window keeps its files locked, and the uninstaller does not close it on its own
procedure CloseSettingsWindow();
var
  Window: HWND;
  Tries: Integer;
begin
  Window := FindWindowByWindowName('Muntin Settings');
  if Window <> 0 then
    PostMessage(Window, WM_CLOSE, 0, 0);
  Tries := 0;
  while (FindWindowByWindowName('Muntin Settings') <> 0) and (Tries < 30) do
  begin
    Sleep(100);
    Tries := Tries + 1;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    CloseSettingsWindow();
  if (CurUninstallStep = usUninstall) and not QuitMuntin() then
    MsgBox('Muntin is still running, so it can''t be removed completely. Quit it from its tray icon and uninstall again.', mbError, MB_OK);
end;
