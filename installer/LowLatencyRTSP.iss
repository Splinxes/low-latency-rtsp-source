#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#define MyAppName "Low Latency RTSP"
#define MyAppPublisher "Splinxes"
#define MyAppURL "https://github.com/Splinxes/low-latency-rtsp-source"

[Setup]
AppId={{8E77351D-ED6B-447F-945F-1474F1B84284}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} v{#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
AppComments=Third-party RTSP source plugin for OBS Studio
DefaultDirName={autopf}\Low Latency RTSP
DisableDirPage=yes
DisableProgramGroupPage=yes
DirExistsWarning=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir=..\dist
OutputBaseFilename=Low-Latency-RTSP-Setup-v{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
UninstallDisplayName={#MyAppName}
CloseApplications=no
RestartApplications=no
SetupLogging=yes

[Files]
Source: "..\dist\low-latency-rtsp.dll"; DestDir: "{commonappdata}\obs-studio\plugins\low-latency-rtsp\bin\64bit"; Flags: ignoreversion
Source: "..\data\locale\en-US.ini"; DestDir: "{commonappdata}\obs-studio\plugins\low-latency-rtsp\data\locale"; Flags: ignoreversion
Source: "..\data\updater\Install-Update.ps1"; DestDir: "{commonappdata}\obs-studio\plugins\low-latency-rtsp\data\updater"; Flags: ignoreversion
Source: "..\dist\runtime\*"; DestDir: "{commonappdata}\obs-studio\plugins\low-latency-rtsp\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE-NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{pf}\obs-studio\bin\64bit\obs64.exe"; Description: "Launch OBS Studio"; Flags: postinstall nowait skipifsilent; Check: FileExists(ExpandConstant('{pf}\obs-studio\bin\64bit\obs64.exe'))

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\low-latency-rtsp"

[Code]
function IsProcessRunning(const ProcessName: String): Boolean;
var
  Locator: Variant;
  Services: Variant;
  Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Services.ExecQuery(
      Format('SELECT * FROM Win32_Process WHERE Name="%s"', [ProcessName])
    );
    Result := Processes.Count > 0;
  except
    Result := False;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;

  if IsProcessRunning('obs64.exe') then
  begin
    MsgBox(
      'OBS Studio is currently running.' + #13#10 + #13#10 +
      'Close OBS completely, then run this installer again.',
      mbInformation,
      MB_OK
    );
    Result := False;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;

  if IsProcessRunning('obs64.exe') then
  begin
    MsgBox(
      'OBS Studio is currently running.' + #13#10 + #13#10 +
      'Close OBS completely before uninstalling Low Latency RTSP.',
      mbInformation,
      MB_OK
    );
    Result := False;
  end;
end;
