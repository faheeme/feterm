; ---------------------------------------------------------------------------
;  Inno Setup script for the Windows installer. build.sh runs it as
;
;      ISCC /DAppVersion=X.Y.Z /DStageDir=<staged folder> /O<output dir> feterm.iss
;
;  Installs per user (no administrator rights, %LOCALAPPDATA%\Programs) by
;  default; the installer offers "install for all users" too, which winget
;  uses for --scope machine. Upgrades replace the previous version in place;
;  the user's settings and sessions in %APPDATA%\feterm are never touched.
; ---------------------------------------------------------------------------
#ifndef AppVersion
  #error Pass /DAppVersion=X.Y.Z
#endif
#ifndef StageDir
  #error Pass /DStageDir=<folder made by stage.sh>
#endif

#define AppName    "FeTerm"
#define AppExe     "feterm.exe"
#define AppURL     "https://gitlab.com/faheeme/feterm"
#define AppHome    "https://faheeme.gitlab.io/feterm-site/"

[Setup]
; Never change AppId: Windows recognises installed versions by it.
AppId={{6F0D3C9E-4B1A-4E57-9C2B-8A7E5D1F3B64}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Faheem Essack
AppPublisherURL={#AppHome}
AppSupportURL={#AppURL}/-/issues
AppUpdatesURL={#AppURL}/-/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809: the first version with ConPTY.
MinVersion=10.0.17763
LicenseFile={#StageDir}\LICENSE.txt
SetupIconFile=..\..\data\windows\feterm.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
OutputBaseFilename=feterm-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
ChangesAssociations=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "contextmenu"; Description: "Add ""Open &FeTerm here"" to folder right-click menus"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; "Open FeTerm here" on a folder and on a folder's background. "%V\." keeps a
; drive root ("C:\") from ending in \" , which would escape the quote.
Root: HKA; Subkey: "Software\Classes\Directory\shell\FeTerm"; ValueType: string; ValueData: "Open FeTerm here"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\FeTerm"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExe}"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\FeTerm\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" --directory ""%V\."""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\FeTerm"; ValueType: string; ValueData: "Open FeTerm here"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\FeTerm"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExe}"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\FeTerm\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" --directory ""%V\."""; Tasks: contextmenu

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent
