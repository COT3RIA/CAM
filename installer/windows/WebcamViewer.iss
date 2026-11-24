; Inno Setup script for Webcam Viewer (Windows)
; Build: iscc installer\windows\WebcamViewer.iss  (run from the repository root after the CMake build)

#define MyAppName "Webcam Viewer"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "WebcamViewer"
#define ArtefactsDir "..\..\build\WebcamVST_artefacts\Release"

[Setup]
AppId={{7B1E6C3A-4D52-4F0E-9A6B-3C5E8D2F1A47}
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
OutputBaseFilename=WebcamViewer-Setup-Windows
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

[Files]
Source: "{#ArtefactsDir}\VST3\Webcam Viewer.vst3\*"; DestDir: "{commoncf64}\VST3\Webcam Viewer.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#ArtefactsDir}\Standalone\Webcam Viewer.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\Webcam Viewer.exe"; Components: standalone

[Run]
Filename: "{app}\Webcam Viewer.exe"; Description: "Launch Webcam Viewer (standalone)"; Flags: nowait postinstall skipifsilent unchecked; Components: standalone
