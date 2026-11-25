; Inno Setup script for CAM by COTERIA (Windows)
; Build: iscc installer\windows\WebcamViewer.iss  (run from the repository root after the CMake build)

#define MyAppName "CAM"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "COTERIA"
#define ArtefactsDir "..\..\build\WebcamVST_artefacts\Release"

[Setup]
; Новый идентификатор: CAM считается новым продуктом (прежний "Webcam Viewer" остаётся отдельным)
AppId={{D3A6F1B2-8C47-4E9A-B5D0-6F2E1C7A9B38}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir=..\..\installer_out
OutputBaseFilename=CAM-Setup-Windows
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
; Логотип: иконка установщика и картинки мастера (пути относительно этого файла)
SetupIconFile=WebcamViewer.ico
WizardImageFile=wizard-164.bmp,wizard-328.bmp
WizardSmallImageFile=wizard-small-55.bmp,wizard-small-110.bmp

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation (VST3 plugin + standalone app)"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (C:\Program Files\Common Files\VST3)"; Types: full plugin custom; Flags: fixed
Name: "standalone"; Description: "Standalone app (for testing the camera without a DAW)"; Types: full

[InstallDelete]
; Убираем старую версию плагина под прежним именем, чтобы в DAW не было двух копий
Type: filesandordirs; Name: "{commoncf64}\VST3\Webcam Viewer.vst3"

[Files]
Source: "{#ArtefactsDir}\VST3\CAM.vst3\*"; DestDir: "{commoncf64}\VST3\CAM.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#ArtefactsDir}\Standalone\CAM.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\CAM.exe"; Components: standalone

[Run]
Filename: "{app}\CAM.exe"; Description: "Launch CAM (standalone)"; Flags: nowait postinstall skipifsilent unchecked; Components: standalone
