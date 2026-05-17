; Inno Setup script — produces Neovere-Setup.exe (a real Windows installer)
; from the deploy directory created by package_windows.ps1.
;
; Build steps (after running package_windows.ps1):
;   1. Install Inno Setup 6+: https://jrsoftware.org/isdl.php
;   2. Compile from the GUI, or from a developer prompt:
;       "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer_windows.iss
;   3. Output lands in dist\Neovere-Setup.exe
;
; The installer is unsigned. Windows SmartScreen will prompt the user once
; ("Windows protected your PC -> More info -> Run anyway"). Code-signing
; requires an Authenticode certificate from a CA (~$200-300/year); the option
; is left out so this works for indie distribution.

#define MyAppName       "Neovere"
#define MyAppVersion    "1.0"
#define MyAppPublisher  "Neovere"
#define MyAppExeName    "Neovere.exe"
#define DeployDir       "build-windows\deploy"

[Setup]
AppId={{8C9DEDE0-2B5C-4A8C-9D9B-NEOVERE000001}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename={#MyAppName}-Setup
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Files]
; Recursively pull in everything package_windows.ps1 staged into the deploy dir.
Source: "{#DeployDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "&Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; First-launch artifacts go in %USERPROFILE%\Documents\Neovere\ and ~\neovere_venv\.
; We leave those alone on uninstall so users don't lose their projects/venvs.
; (Uncomment below to nuke them too.)
; Type: filesandordirs; Name: "{userdocs}\Neovere"
; Type: filesandordirs; Name: "{userprofile}\neovere_venv"
