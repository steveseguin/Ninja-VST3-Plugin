#define MyAppName "WebRTC VST for VDO.Ninja"

#ifndef MyAppVersion
  #error MyAppVersion must be defined
#endif

#ifndef SourceVstBundle
  #error SourceVstBundle must be defined
#endif

#ifndef OutputDir
  #define OutputDir "."
#endif

#ifndef OutputBaseFilename
  #define OutputBaseFilename "webrtc_vst-windows-setup"
#endif

#ifndef GettingStartedUrl
  #define GettingStartedUrl "https://steveseguin.github.io/Ninja-VST3-Plugin/getting-started.html"
#endif

[Setup]
AppId={{A6DE2F66-0E6A-4C59-9FE8-2E7B0D7AA5BE}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Steve Seguin
DefaultDirName={localappdata}\Programs\Common\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible
RedirectionGuard=no
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
Uninstallable=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#SourceVstBundle}\*"; DestDir: "{app}\webrtc_vst.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "desktop.ini"

[Run]
Filename: "{#GettingStartedUrl}"; Description: "Open Getting Started guide"; Flags: postinstall shellexec skipifsilent
