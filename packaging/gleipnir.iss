; Inno Setup script for Gleipnir -- builds gleipnir-1.0.0-setup.exe
;
; Copyright 2026 ValisSowilo.  GPL-3.0-or-later; see LICENSE.md.
;
; Per-user install (PrivilegesRequired=lowest), so it needs no administrator
; rights and touches nothing outside the user's profile.  A command-line
; archiver has no business asking for elevation, and requiring it is a good way
; to have people run the whole thing as admin out of habit.
;
; Build:
;   "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" packaging\gleipnir.iss

#define AppName    "Gleipnir"
#define AppVer     "1.0.0"
#define AppExe     "gleipnir.exe"
#define AppPublish "ValisSowilo"

[Setup]
AppId={{7A3C1E64-9B22-4E5D-8F71-2C6D0A9B4E13}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher={#AppPublish}
AppPublisherURL=https://github.com/ValisSowilo
AppSupportURL=https://github.com/ValisSowilo
AppCopyright=Copyright 2026 ValisSowilo - GPL-3.0-or-later
DefaultDirName={localappdata}\Programs\Gleipnir
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no
; Per-user: no UAC prompt, installs under the user's own profile.
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=gleipnir-{#AppVer}-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The installer alters PATH, so tell Windows to broadcast the change rather
; than leaving new terminals to pick it up by luck.
ChangesEnvironment=yes
WizardStyle=modern
LicenseFile=..\dist\LICENSE.md
InfoBeforeFile=
UninstallDisplayName={#AppName} {#AppVer}
UninstallDisplayIcon={app}\{#AppExe}
VersionInfoVersion={#AppVer}.0
VersionInfoDescription=Context-mixing archiver
VersionInfoProductName={#AppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath"; Description: "Add Gleipnir to my PATH (lets you run 'gleipnir' from any terminal)"; GroupDescription: "Command line:"
Name: "contextmenu"; Description: "Add Gleipnir to the right-click menu in File Explorer"; GroupDescription: "Explorer:"

; End-user files only.  ARCHITECTURE.md and EXPERIENCES.md are development
; notes -- why the engine is built the way it is, and what went wrong while
; building it.  They are useful to someone working on Gleipnir and pure clutter in
; an install directory, so they stay with the source rather than shipping.
[Files]
Source: "..\dist\gleipnir.exe";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\README.txt";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\USAGE.txt";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\LICENSE.txt";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\LICENSE.md";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\gleipnir-shell.cmd"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Gleipnir README"; Filename: "{app}\README.txt"
Name: "{group}\Gleipnir usage reference"; Filename: "{app}\USAGE.txt"
Name: "{group}\Uninstall Gleipnir"; Filename: "{uninstallexe}"

; Explorer integration, all under HKCU so no elevation is needed and nothing
; is written outside this user's profile.  Every key carries uninsdeletekey,
; so removing Gleipnir removes the menu entries with it.
;
; On Windows 11 these appear under "Show more options" (or Shift+F10), not on
; the short menu that opens by default.  Getting onto the short menu requires
; shipping a packaged IExplorerCommand COM handler, which is a great deal of
; machinery for two entries.
[Registry]
; --- right-click a folder: compress it
Root: HKCU; Subkey: "Software\Classes\Directory\shell\gleipnir_compress"; ValueType: string; ValueName: ""; ValueData: "Compress with Gleipnir"; Tasks: contextmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\shell\gleipnir_compress"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\gleipnir.exe,0"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\Directory\shell\gleipnir_compress\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gleipnir-shell.cmd"" compress ""%1"""; Tasks: contextmenu

; --- right-click any file: compress it
Root: HKCU; Subkey: "Software\Classes\*\shell\gleipnir_compress"; ValueType: string; ValueName: ""; ValueData: "Compress with Gleipnir"; Tasks: contextmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\*\shell\gleipnir_compress"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\gleipnir.exe,0"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\*\shell\gleipnir_compress\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gleipnir-shell.cmd"" compress ""%1"""; Tasks: contextmenu

; --- .gl files get their own type, with extract / verify / list
Root: HKCU; Subkey: "Software\Classes\.gl"; ValueType: string; ValueName: ""; ValueData: "Gleipnir.archive"; Tasks: contextmenu; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive"; ValueType: string; ValueName: ""; ValueData: "Gleipnir archive"; Tasks: contextmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\gleipnir.exe,0"; Tasks: contextmenu

Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\shell\extract"; ValueType: string; ValueName: ""; ValueData: "Extract here"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\shell\extract\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gleipnir-shell.cmd"" extract ""%1"""; Tasks: contextmenu

Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\shell\verify"; ValueType: string; ValueName: ""; ValueData: "Verify (fast check)"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\shell\verify\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gleipnir-shell.cmd"" verify ""%1"""; Tasks: contextmenu

Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\shell\list"; ValueType: string; ValueName: ""; ValueData: "List contents"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\Gleipnir.archive\shell\list\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gleipnir-shell.cmd"" list ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\README.txt"; Description: "Open the README (it explains why this is slow)"; Flags: shellexec postinstall skipifsilent unchecked

[Code]
// PATH handling.
//
// Deliberately not done with a [Registry] append.  That writes the old value
// and the app dir concatenated as a literal, which duplicates the entry on
// reinstall and leaves it behind forever on uninstall.  Reading, checking and
// rewriting the value is the only way to be idempotent in both directions.
//
// Note: these comments use // rather than braces because Pascal brace comments
// do not nest, and the Inno constants written in braces would close them
// early -- which is exactly how the first build of this script failed.

const
  EnvKey = 'Environment';

function PathList(): String;
var
  V: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', V) then
    V := '';
  Result := V;
end;

// Case-insensitive containment test on whole entries, so that a directory
// whose name merely contains another one is not mistaken for it.
function HasPathEntry(const Dir: String): Boolean;
var
  Cur: String;
begin
  Cur := ';' + Uppercase(PathList()) + ';';
  Result := Pos(';' + Uppercase(Dir) + ';', Cur) > 0;
end;

procedure AddPathEntry(const Dir: String);
var
  Cur: String;
begin
  if HasPathEntry(Dir) then
    Exit;
  Cur := PathList();
  if (Cur <> '') and (Cur[Length(Cur)] <> ';') then
    Cur := Cur + ';';
  RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Cur + Dir);
end;

procedure RemovePathEntry(const Dir: String);
var
  Cur, Res, Part: String;
  P: Integer;
begin
  Cur := PathList();
  if Cur = '' then
    Exit;
  Res := '';
  Cur := Cur + ';';
  repeat
    P := Pos(';', Cur);
    Part := Copy(Cur, 1, P - 1);
    Cur := Copy(Cur, P + 1, Length(Cur));
    // Keep everything that is not our directory.  Rewriting the whole
    // variable from scratch would be a fine way to destroy someone's
    // toolchain, so each surviving entry is copied across verbatim.
    if (Part <> '') and (Uppercase(Part) <> Uppercase(Dir)) then
    begin
      if Res <> '' then
        Res := Res + ';';
      Res := Res + Part;
    end;
  until Cur = '';
  RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvKey, 'Path', Res);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if WizardIsTaskSelected('addtopath') then
      AddPathEntry(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Before the files go, so the app dir still expands to what was installed.
  if CurUninstallStep = usUninstall then
    RemovePathEntry(ExpandConstant('{app}'));
end;
