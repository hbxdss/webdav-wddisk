; Wddisk Windows 安装包脚本
#define MyAppName "Wddisk"
#define MyAppVersion "1.0"
#define MyAppPublisher "Wddisk"
#define MyAppExeName "wddisk.exe"

[Setup]
AppId={{8E5C2A1F-6B7D-4E3A-9C21-WDDISK1000001}
AppName={#MyAppName} · WebDAV 网络磁盘
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Wddisk
DefaultGroupName={#MyAppName}
OutputDir=.
OutputBaseFilename=wddisk-setup-x64
SetupIconFile=..\windows\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\build\win\wddisk.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "wddisk-portable\winfsp-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "deps\winfsp.msi"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Dirs]
; 允许普通用户写入安装目录，账号配置 config.json 直接存放在安装目录（便携式）
Name: "{app}"; Permissions: users-modify

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Run]
; 安装 WinFsp 驱动（若已安装会跳过升级提示）
Filename: "msiexec.exe"; Parameters: "/i ""{tmp}\winfsp.msi"" /qn /norestart"; Flags: runhidden; StatusMsg: "正在安装 WinFsp 驱动..."
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 只清理缓存；保留 config.json（账号配置属于用户数据）
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{userappdata}\..\Local\Wddisk\cache"
