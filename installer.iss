; Inno Setup script (free) - packages Zunimi Studio. Built by .github/workflows/installer.yml:
;   ISCC /DAppVersion=0.1.1 /DStageDir=<unzipped release zip> installer.iss
; Installs for the current user only, so no admin prompt. Updates run it with /SILENT and it reopens the app.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "build\stage"
#endif
#define AppExe "bin\64bit\zunimi-studio.exe"

[Setup]
AppId={{5A1B95BB-D280-417D-9BA8-56D1E15C974E}
AppName=Zunimi Studio
AppVersion={#AppVersion}
AppVerName=Zunimi Studio {#AppVersion}
AppPublisher=ZunimiLabs
AppPublisherURL=https://zunimilabs.online
AppSupportURL=https://github.com/Deba-dev/zunimi-studio/issues
AppCopyright=ZunimiLabs. GPL-2.0-or-later.
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=ZunimiLabs
VersionInfoDescription=Zunimi Studio installer
DefaultDirName={autopf}\Zunimi Studio
DefaultGroupName=Zunimi Studio
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=LICENSE
OutputDir=installer
OutputBaseFilename=ZunimiStudio-Setup
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName=Zunimi Studio
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; The [Code] section below closes the app itself; Windows must not reopen it as well as [Run]
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "*.pdb,Start Zunimi Studio.cmd"; Flags: recursesubdirs ignoreversion

[InstallDelete]
; Files from the previous version must not mix with the new ones (an old module DLL could load against a newer libobs).
; Settings and recordings are safe: they live in %LOCALAPPDATA%\ZunimiStudio and your Videos folder.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\core"
Type: filesandordirs; Name: "{app}\data"

[UninstallDelete]
; The app's own logs and update log. Settings (with the encrypted stream key) stay unless the user deletes them.
Type: filesandordirs; Name: "{localappdata}\ZunimiStudio\logs"
Type: files; Name: "{localappdata}\ZunimiStudio\update.log"

[Icons]
Name: "{autoprograms}\Zunimi Studio"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"
Name: "{autodesktop}\Zunimi Studio"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Description: "Start Zunimi Studio"; Flags: nowait postinstall skipifsilent
; An update runs silently; open the app again when it is done
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Flags: nowait; Check: WizardSilent

[Code]
// Waits for the running app to close before any file is touched. A file still in use would leave half an update behind.
function AppIsRunning(Args: String): Boolean;
var
  Code: Integer;
begin
  Result := False;
  if Exec(ExpandConstant('{sys}\taskkill.exe'), Args, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := Code <> 128;      // 128 = no such program running
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to 20 do begin
    if not AppIsRunning('/IM zunimi-studio.exe') then begin
      Sleep(500);               // Windows needs a moment to let go of the files
      exit;
    end;
    Sleep(500);
  end;
  AppIsRunning('/F /T /IM zunimi-studio.exe');
  Sleep(1500);
end;
