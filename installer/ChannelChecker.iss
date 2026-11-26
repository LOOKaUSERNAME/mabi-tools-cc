#ifndef VerifiedPayload
  #error Pass the complete verified Qt payload directory with /DVerifiedPayload.
#endif
#ifndef PackageOutput
  #error Pass the output directory with /DPackageOutput.
#endif

#ifndef RuntimeRedist
  #error Pass the pinned Microsoft x64 redistributable with /DRuntimeRedist.
#endif
#ifndef RuntimeSha256
  #error Pass its pinned SHA-256 with /DRuntimeSha256.
#endif

#define ProductName "MabiTools Channel Checker"
#define DisplayName "Channel Checker"
#define ProductDir "{localappdata}\Programs\MabiTools\Channel Checker"
#define ProductShortcut "Channel Checker"
#define ProductExe "MabiTools.ChannelChecker.exe"
#ifndef AppVersion
  #define AppVersion "2026.11.26"
#endif
#ifndef ResourceVersion
  #define ResourceVersion "2026.11.26.0"
#endif

[Setup]
AppId={{B81A179F-E1D0-471B-B274-B521D71C8088}
AppName={#DisplayName}
AppVersion={#AppVersion}
VersionInfoVersion={#ResourceVersion}
VersionInfoProductName={#ProductName}
SetupIconFile=..\assets\Assets\channel-checker.ico
AppPublisher=MabiTools
UninstallDisplayName={#DisplayName}
UninstallDisplayIcon={app}\{#ProductExe}
DefaultDirName={#ProductDir}
DefaultGroupName={#ProductShortcut}
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir={#PackageOutput}
OutputBaseFilename=MabiTools-Channel-Checker-{#AppVersion}-Setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Restart Manager detects processes holding this installation's files and asks
; the user to close them before replacement. Do not force termination/restart.
CloseApplications=yes
CloseApplicationsFilter={#ProductExe},*.dll
RestartApplications=no
SetupMutex=MabiToolsChannelCheckerSetup

; List the exact tested payload. No updater or profile cleanup,
; no wildcard copying. Launch after installation is an explicit finish-page option.
[Files]
Source: "{#RuntimeRedist}"; DestName: "VC_redist.x64.exe"; Flags: dontcopy
Source: "{#VerifiedPayload}\MabiTools.ChannelChecker.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\Qt6Gui.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\Qt6Widgets.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\Qt6Network.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\qt.conf"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\platforms\qwindows.dll"; DestDir: "{app}\platforms"; Flags: ignoreversion
Source: "{#VerifiedPayload}\tls\qschannelbackend.dll"; DestDir: "{app}\tls"; Flags: ignoreversion
Source: "{#VerifiedPayload}\manifest.json"; DestDir: "{app}"; Flags: ignoreversion

Source: "{#VerifiedPayload}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VerifiedPayload}\notices\*"; DestDir: "{app}\notices"; Flags: ignoreversion recursesubdirs

Source: "{#VerifiedPayload}\sources\*"; DestDir: "{app}\sources"; Flags: ignoreversion recursesubdirs


[Icons]
; The legacy installer owns the top-level name even when installed later.
; Keep the short label in a stable native group in either installation order.
Name: "{userprograms}\MabiTools\{#ProductShortcut}"; Filename: "{app}\{#ProductExe}"; WorkingDir: "{app}"

[INI]
Filename: "{app}\install-kind.ini"; Section: "Package"; Key: "Kind"; String: "setup"
Filename: "{app}\install-kind.ini"; Section: "Package"; Key: "Product"; String: "cc-native"

[Run]
Filename: "{app}\{#ProductExe}"; WorkingDir: "{app}"; Description: "Launch Channel Checker"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\install-kind.ini"

[InstallDelete]
Type: files; Name: "{userprograms}\Channel Checker (Qt).lnk"; Check: IsInstalledAppShortcut('{userprograms}\Channel Checker (Qt).lnk')
Type: files; Name: "{userprograms}\MabiTools Channel Checker.lnk"; Check: IsInstalledAppShortcut('{userprograms}\MabiTools Channel Checker.lnk')
Type: files; Name: "{userprograms}\Channel Checker.lnk"; Check: IsInstalledAppShortcut('{userprograms}\Channel Checker.lnk')

[Code]
var
  RuntimeRestartRequired: Boolean;
  PreservedShortcutPaths: array[0..3] of String;
  PreservedShortcutCopies: array[0..3] of String;

// Shortcut cleanup is target-based, not name-based: another product may own
// the short display name. An unreadable shortcut is preserved conservatively.
function IsInstalledAppShortcut(ShortcutPath: String): Boolean;
var
  Shell, Shortcut: Variant;
  Path: String;
begin
  Result := False;
  Path := ExpandConstant(ShortcutPath);
  if not FileExists(Path) then Exit;
  try
    // Resolve the link without invoking it. A matching display name alone is
    // insufficient: the legacy application can own the same historical path.
    Shell := CreateOleObject('WScript.Shell');
    Shortcut := Shell.CreateShortcut(Path);
    Result := (CompareText(Shortcut.TargetPath, ExpandConstant('{app}\{#ProductExe}')) = 0)
      and (Shortcut.Arguments = '');
  except
    Log('Kept the shortcut because its target could not be verified.');
  end;
end;

// Inno retains shortcut entries from earlier native installs in its uninstall
// log. Another installer may since have taken ownership of one of those paths.
// Save such links before uninstall starts and restore only paths that the log
// actually removed. Never rewrite a surviving link or launch its target.
function InitializeUninstall: Boolean;
var
  Index: Integer;
  Path: String;
begin
  Result := False;
  PreservedShortcutPaths[0] := ExpandConstant('{userprograms}\Channel Checker.lnk');
  PreservedShortcutPaths[1] := ExpandConstant('{userprograms}\MabiTools Channel Checker.lnk');
  PreservedShortcutPaths[2] := ExpandConstant('{userprograms}\Channel Checker (Qt).lnk');
  PreservedShortcutPaths[3] := ExpandConstant('{userprograms}\MabiTools\Channel Checker.lnk');
  // Snapshot foreign links before Inno replays its accumulated uninstall log.
  // Copies live in this uninstaller's temporary folder, not in either product.
  for Index := 0 to 3 do begin
    Path := PreservedShortcutPaths[Index];
    if FileExists(Path) and not IsInstalledAppShortcut(Path) then begin
      PreservedShortcutCopies[Index] := ExpandConstant('{tmp}\preserved-shortcut-') + IntToStr(Index) + '.lnk';
      if not FileCopy(Path, PreservedShortcutCopies[Index], False) then begin
        MsgBox('A shortcut owned by another application could not be preserved. Uninstallation was cancelled.', mbError, MB_OK);
        Exit;
      end;
    end;
  end;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Index: Integer;
  Path, Backup: String;
begin
  // Restore only after Inno has finished removing its recorded files. Leave a
  // surviving link untouched, even if another installer changed it meanwhile.
  if CurUninstallStep <> usPostUninstall then Exit;
  for Index := 0 to 3 do begin
    Path := PreservedShortcutPaths[Index];
    Backup := PreservedShortcutCopies[Index];
    if (Backup <> '') and not FileExists(Path) then begin
      if not ForceDirectories(ExtractFileDir(Path)) or not FileCopy(Backup, Path, True) then
        MsgBox('Could not restore another application''s Start shortcut: ' + Path, mbError, MB_OK);
    end;
  end;
end;

function RuntimeVersionCompatible(Major, Minor, Build: Cardinal): Boolean;
begin
  Result := (Major > 14) or ((Major = 14) and
    ((Minor > 50) or ((Minor = 50) and (Build >= 35719))));
end;

function RuntimeRegistered(RootKey: Integer): Boolean;
var
  Installed, Major, Minor, Build: Cardinal;
begin
  Result := RegQueryDWordValue(RootKey, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and
    RegQueryDWordValue(RootKey, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', Major) and
    RegQueryDWordValue(RootKey, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Minor', Minor) and
    RegQueryDWordValue(RootKey, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Bld', Build) and
    (Installed = 1) and RuntimeVersionCompatible(Major, Minor, Build);
end;

// Require both registry compatibility and DLL presence. The prerequisite is
// shared machine state; only its installer elevates, and uninstall leaves it.
function RuntimeReady: Boolean;
begin
  Result := (RuntimeRegistered(HKLM64) or RuntimeRegistered(HKLM32)) and
    FileExists(ExpandConstant('{sys}\vcruntime140.dll')) and
    FileExists(ExpandConstant('{sys}\msvcp140.dll'));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  RuntimePath, Arguments: String;
  Code: Integer;
begin
  Result := '';
  if CompareText(ExpandConstant('{app}'), ExpandConstant('{#ProductDir}')) <> 0 then begin
    Result := 'Channel Checker must use its own default installation folder.';
    Log(Result);
    Exit;
  end;
  // Refuse a foreign owner at the new stable path; do not overwrite it or
  // choose a different shortcut location based on installation order.
  if FileExists(ExpandConstant('{userprograms}\MabiTools\Channel Checker.lnk')) and
    not IsInstalledAppShortcut('{userprograms}\MabiTools\Channel Checker.lnk') then begin
    Result := 'The MabiTools Channel Checker shortcut belongs to another application. Move that shortcut before continuing.';
    Exit;
  end;
  if RuntimeReady then begin
    Log('VC++ x64 runtime is compatible; prerequisite installation skipped.');
    Exit;
  end;
  Log('VC++ x64 runtime is missing or too old; installing bundled redistributable.');
  ExtractTemporaryFile('VC_redist.x64.exe');
  RuntimePath := ExpandConstant('{tmp}\VC_redist.x64.exe');
  if CompareText(GetSHA256OfFile(RuntimePath), '{#RuntimeSha256}') <> 0 then begin
    Result := 'The bundled Microsoft runtime failed verification. Run Setup again from a fresh download.';
    Exit;
  end;
  Arguments := '/install /norestart /log "' + ExpandConstant('{tmp}\VC-Runtime.log') + '"';
  if WizardSilent then Arguments := Arguments + ' /quiet'
  else Arguments := Arguments + ' /passive';
  if not ShellExec('runas', RuntimePath, Arguments, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then begin
    Log(Format('VC++ runtime could not start: %d', [Code]));
    Result := 'Microsoft Visual C++ runtime installation was not approved or could not start. Run Setup again and allow the Windows permission prompt.';
    Exit;
  end;
  Log(Format('VC++ redistributable exit code: %d', [Code]));
  if (Code = 3010) or (Code = 1641) then RuntimeRestartRequired := True;
  if ((Code <> 0) and (Code <> 3010) and (Code <> 1641) and (Code <> 1638)) or not RuntimeReady then begin
    Result := Format('Microsoft Visual C++ runtime installation did not complete (code %d). Restart Windows and run Setup again.', [Code]);
    NeedsRestart := RuntimeRestartRequired;
    Exit;
  end;
  Log('VC++ x64 runtime installation verified.');
end;

function NeedRestart: Boolean;
begin
  Result := RuntimeRestartRequired;
end;
