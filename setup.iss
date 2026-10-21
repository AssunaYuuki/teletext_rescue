; Установщик Teletext Rescue (Inno Setup 6).
; Сначала: ninja в cppuild, затем strip TeletextRescue.exe и trcli.exe в dist\Teletext Rescue <версия>\n; Потом:   ISCC.exe setup.iss  -> dist\TeletextRescue-Setup-<версия>.exe

#define AppName "Teletext Rescue"
#define AppVersion "1.3"
; ISCC /DArch=x86 setup.iss: the 32-bit build (dist\Teletext Rescue 1.3 x86)
#ifndef Arch
#define Arch "x64"
#endif
#define AppPublisher "AssunaYuuki"
#define AppURL "https://github.com/AssunaYuuki/teletext_rescue"
#define AppExe "Teletext Rescue.exe"

[Setup]
AppId={{6E2B1D4A-7C3F-4B8E-9A51-2F0D3C7E8B91}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
; для текущего пользователя, без прав администратора (можно выбрать «для всех»)
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE
OutputDir=dist
#if Arch == "x86"
OutputBaseFilename=TeletextRescue-Setup-v{#AppVersion}-x86
#else
OutputBaseFilename=TeletextRescue-Setup-v{#AppVersion}
#endif
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
; оформление как у программы: тёмный мастер, янтарное табло, значок TR
WizardStyle=modern dark includetitlebar hidebevels
WizardBackColor=#1b1c1f
WizardImageFile=installer\wizard_100.png,installer\wizard_150.png,installer\wizard_200.png
WizardSmallImageFile=installer\small_100.png,installer\small_150.png,installer\small_200.png
WizardImageBackColor=#1b1c1f
WizardSmallImageBackColor=#1b1c1f
WizardImageStretch=no
DisableWelcomePage=no
#if Arch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
#if Arch == "x86"
Source: "dist\{#AppName} {#AppVersion} x86\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#else
Source: "dist\{#AppName} {#AppVersion}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[InstallDelete]
; файлы старой версии 1.0
Type: filesandordirs; Name: "{app}\_internal"
Type: files; Name: "{app}\README.txt"

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
