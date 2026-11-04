; Installer for FanciestZones. Build the Release exe first, then:
;   ISCC.exe installer\FanciestZones.iss
; It installs for the current user only, without administrator rights, into the folder that also holds the
; settings and layouts. Uninstalling removes the program and its shortcuts and keeps the settings and layouts.

#define AppExe AddBackslash(SourcePath) + "..\x64\Release\FanciestZones.exe"
#define AppVersion GetVersionNumbersString(AppExe)
#if AppVersion == ""
  #error Build the Release FanciestZones.exe first
#endif

[Setup]
AppId={{6F3B1C2A-8E4D-4F7A-9C1E-2B5D7A9E0F14}
AppName=FanciestZones
AppVersion={#AppVersion}
AppPublisher=FanciestZones contributors
AppPublisherURL=https://github.com/c-hurtado/FanciestZones
AppSupportURL=https://github.com/c-hurtado/FanciestZones/issues
LicenseFile=..\LICENSE
PrivilegesRequired=lowest
DefaultDirName={localappdata}\FanciestZones
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\FanciestZones.exe
SetupIconFile=..\src\modules\fancyzones\FancyZonesLib\icon.ico
OutputDir=..\x64\Release\installer
OutputBaseFilename=FanciestZones-{#AppVersion}-setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; [Code] asks a running FanciestZones to quit first; this only catches what that missed, and never forces it
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "startup"; Description: "Start FanciestZones when I sign in"

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "..\NOTICE.md"; DestDir: "{app}"

[Icons]
Name: "{userprograms}\FanciestZones\FanciestZones"; Filename: "{app}\FanciestZones.exe"
; The same shortcut the tray menu's "Start at sign-in" item writes
Name: "{userstartup}\FanciestZones"; Filename: "{app}\FanciestZones.exe"; Tasks: startup

[Run]
Filename: "{app}\FanciestZones.exe"; Description: "Start FanciestZones now"; Flags: nowait postinstall

[UninstallDelete]
; Start menu entries of the named layouts, which FanciestZones makes, and the sign-in shortcut if the tray made it
Type: files; Name: "{userprograms}\FanciestZones\*.lnk"
Type: dirifempty; Name: "{userprograms}\FanciestZones"
Type: files; Name: "{userstartup}\FanciestZones.lnk"

[Code]
const
  WM_ENDSESSION = $0016;

// Asks a running FanciestZones to quit the way it does when Windows shuts down, which brings scrolled windows back.
// Killing it would leave them parked off screen. False when it is still running, for example as administrator.
function QuitFanciestZones(): Boolean;
var
  Window: HWND;
  Tries: Integer;
begin
  Window := FindWindowByClassName('FanciestZones');
  if Window <> 0 then
    PostMessage(Window, WM_ENDSESSION, 1, 0);
  Tries := 0;
  while (FindWindowByClassName('FanciestZones') <> 0) and (Tries < 50) do
  begin
    Sleep(100);
    Tries := Tries + 1;
  end;
  Result := FindWindowByClassName('FanciestZones') = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not QuitFanciestZones() then
    Result := 'FanciestZones is still running. Quit it from its tray icon, then run Setup again.';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usUninstall) and not QuitFanciestZones() then
    MsgBox('FanciestZones is still running, so it can''t be removed completely. Quit it from its tray icon and uninstall again.', mbError, MB_OK);
end;
