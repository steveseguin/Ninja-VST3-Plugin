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
Source: "..\..\scripts\configure_windows_firewall.ps1"; DestDir: "{app}\VDO.Ninja Tools"; Flags: ignoreversion
Source: "..\..\scripts\collect_windows_diagnostics.ps1"; DestDir: "{app}\VDO.Ninja Tools"; Flags: ignoreversion

[Tasks]
Name: "hostfirewall"; Description: "Allow a DAW or plugin host through Windows Firewall on Private networks (optional; requires administrator permission)"; Flags: unchecked

[Run]
Filename: "{#GettingStartedUrl}"; Description: "Open Getting Started guide"; Flags: postinstall shellexec skipifsilent

[Code]
var
  FirewallHostPage: TInputFileWizardPage;

procedure InitializeWizard;
begin
  FirewallHostPage := CreateInputFilePage(wpSelectTasks, 'Choose your audio host',
    'Select the executable that runs the VST3 plugin.',
    'For REAPER, select reaper.exe. If plugins run in a separate process, select that host instead. This allows inbound UDP for the entire selected application on Private networks only. Public networks are unchanged.');
  FirewallHostPage.Add('DAW or plugin host:', 'Programs (*.exe)|*.exe', '.exe');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = FirewallHostPage.ID) and not WizardIsTaskSelected('hostfirewall');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = FirewallHostPage.ID) and WizardIsTaskSelected('hostfirewall') then
  begin
    Result := FileExists(FirewallHostPage.Values[0]) and
      (CompareText(ExtractFileExt(FirewallHostPage.Values[0]), '.exe') = 0);
    if not Result then
      MsgBox('Select an existing .exe that hosts your plugin.', mbError, MB_OK);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ExitCode: Integer;
  Started: Boolean;
  Params: String;
begin
  { Silent installs never elevate or change firewall rules. }
  if (CurStep = ssPostInstall) and not WizardSilent and
      WizardIsTaskSelected('hostfirewall') then
  begin
    Params := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
      ExpandConstant('{app}\VDO.Ninja Tools\configure_windows_firewall.ps1') +
      '" -HostPath "' + FirewallHostPage.Values[0] + '" -Profile Private';
    Started := ShellExec('runas', ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      Params, '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
    if not Started or (ExitCode <> 0) then
      MsgBox('The plugin was installed, but firewall access could not be configured. You can run configure_windows_firewall.ps1 from the VDO.Ninja Tools folder later as administrator.', mbInformation, MB_OK);
  end;
end;
