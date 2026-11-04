; Installer for Muntin. Build the Release exe first, then:
;   ISCC.exe installer\Muntin.iss
; It installs for the current user only, without administrator rights, into the folder that also holds the
; settings and layouts. Uninstalling removes the program and its shortcuts and keeps the settings and layouts.

#define AppExe AddBackslash(SourcePath) + "..\x64\Release\Muntin.exe"
#define AppVersion GetVersionNumbersString(AppExe)
#if AppVersion == ""
  #error Build the Release Muntin.exe first
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

[Icons]
Name: "{userprograms}\Muntin\Muntin"; Filename: "{app}\Muntin.exe"
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

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usUninstall) and not QuitMuntin() then
    MsgBox('Muntin is still running, so it can''t be removed completely. Quit it from its tray icon and uninstall again.', mbError, MB_OK);
end;
