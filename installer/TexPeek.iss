; TexPeek installer script (Inno Setup 6)
; Build:  build-release.bat  (it compiles the program into release\TexPeek first, then runs this script)
; The version comes from the VERSION file in the project root (build-release.bat also passes it as /DAppVer).
#ifndef AppVer
  #define VerFile FileOpen(AddBackslash(SourcePath) + "..\VERSION")
  #define AppVer Trim(FileRead(VerFile))
  #expr FileClose(VerFile)
  #undef VerFile
#endif

[Setup]
AppId={{62A7896D-9F79-4A7D-9A8C-66F3F2668AE1}
AppName=TexPeek
AppVersion={#AppVer}
AppVerName=TexPeek {#AppVer}
AppPublisher=TexPeek
VersionInfoVersion={#AppVer}
DefaultDirName={autopf}\TexPeek
DefaultGroupName=TexPeek
UninstallDisplayIcon={app}\texpeek.exe
SetupIconFile=..\res\texpeek.ico
OutputDir=..\release
OutputBaseFilename=TexPeek-Setup-{#AppVer}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Always a per-user install (%LOCALAPPDATA%\Programs\TexPeek): no admin rights, and the Windows integration
; (written to HKCU by texpeek.exe --register) belongs to the same user.
PrivilegesRequired=lowest
ChangesAssociations=yes
DisableProgramGroupPage=yes
WizardStyle=modern
LicenseFile=..\LICENSE

[Languages]
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "shell"; Description: "{cm:ShellTask}"; GroupDescription: "{cm:ShellGroup}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[CustomMessages]
turkish.ShellTask=DDS dosyalarını çift tıklayınca TexPeek ile aç ve Gezgin'de küçük resimlerini göster
turkish.ShellGroup=Windows bütünleşmesi:
english.ShellTask=Open DDS files with TexPeek on double-click and show their thumbnails in Explorer
english.ShellGroup=Windows integration:

[Registry]
; The language picked in this installer becomes the program's interface language (read by src\Lang.cpp;
; it can be changed later with the TR | EN switch in the toolbar). The key is removed on uninstall.
Root: HKCU; Subkey: "Software\TexPeek"; ValueType: string; ValueName: "Language"; ValueData: "tr"; Languages: turkish; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\TexPeek"; ValueType: string; ValueName: "Language"; ValueData: "en"; Languages: english; Flags: uninsdeletekey

[Files]
; release\TexPeek is prepared by build-release.bat: texpeek.exe, TexPeekThumb.dll, LICENSE, THIRD-PARTY-NOTICES.txt, licenses\
Source: "..\release\TexPeek\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\TexPeek"; Filename: "{app}\texpeek.exe"
Name: "{autodesktop}\TexPeek"; Filename: "{app}\texpeek.exe"; Tasks: desktopicon

[Run]
; The registry work is done by the program itself (src\ShellIntegration.cpp), the same code as its "Windows > Install" button.
Filename: "{app}\texpeek.exe"; Parameters: "--register"; Flags: runhidden; Tasks: shell
Filename: "{app}\texpeek.exe"; Description: "{cm:LaunchProgram,TexPeek}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Removes only what TexPeek wrote; a .dds association that now belongs to another program is left alone.
Filename: "{app}\texpeek.exe"; Parameters: "--unregister"; Flags: runhidden; RunOnceId: "TexPeekUnregister"
