; Inno Setup script for the DECtalk DTC-01 SAPI5 voices (src/sapi; REFERENCE.md s18.21).
;
; Two COM servers (one per bitness) are registered into two registry views, and the uninstall mirrors that. The
; voices speak through the speech library, in-proc at both bitnesses, with no ROMs: DECtalk.dll speaks firmware
; v1.8 and DECtalk20.dll (the same file under a second name, so each firmware has its own engine) firmware v2.0.
; What is ticked here is which of the two is copied; the engine (sapi_main.cpp DllRegisterServer) then registers
; only the voice tokens whose library is beside its own DLL, so the tick boxes and the registered voices cannot
; drift apart. {app}\voices.ini is written for the record only; the engine never reads it.
;
; Build both CMake builds first (presets x64 and x86, REFERENCE.md s17.12), then compile this file:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" src\sapi\installer\DectalkDtc01SAPI.iss
; The installer lands in build\installer.

#define AppName "DECtalk DTC-01 SAPI5 Voices"
#define AppVersion "2.0.0"
#define AppPublisher "DECtalk DTC-01 project"
#define OutputDir "..\..\..\build\installer"
#define Bin64 "..\..\..\build\bin"
#define Bin32 "..\..\..\build\x86\bin"

[Setup]
; The AppId of the releases that ran the ROMs in an emulator (up to 1.2.2), so this one installs over them.
AppId={{035E6ED9-7052-4CEC-98D3-DBB47A4274C3}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\DECtalkDTC01
DefaultGroupName=DECtalk DTC-01
DisableProgramGroupPage=yes
DisableDirPage=no
OutputDir={#OutputDir}
OutputBaseFilename=DectalkDtc01_SAPI_Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; The component page is the point of this installer, so it is always shown -- even when the user picked a
; ready-made type -- and the Ready page lists what was chosen.
AlwaysShowComponentsList=yes
ShowComponentSizes=yes

; Voice tokens and the COM classes live in HKLM, so the installer needs elevation.
PrivilegesRequired=admin

; In 64-bit install mode {sys} is the real System32 and {syswow64} is SysWOW64, which is what lets each COM
; server be registered into the registry view its own hosts read.
ArchitecturesInstallIn64BitMode=x64compatible

UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\DectalkDtc01SAPI.dll

; The installer's own log (Inno's setup log), separate from the engine's dectalk-sapi.log.
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; The stock wording talks about program features; this page is choosing firmware voices.
WizardSelectComponents=Choose which DECtalk firmware voices to install
SelectComponentsDesc=Which DECtalk DTC-01 firmware voices should be installed?
SelectComponentsLabel2=Tick the firmware versions you want. Firmware v2.0 adds 10 voices; firmware v1.8 adds 8 voices (it does not include Doctor Dennis or Whispery Wendy). Both are ticked by default. Clear a tick to leave that firmware's voices out. Click Next when you are ready.

[Types]
Name: "full";   Description: "Everything: both firmware v2.0 and v1.8 voices"
Name: "custom"; Description: "Custom: choose the firmware yourself"; Flags: iscustom

[Components]
Name: "firmware";      Description: "DECtalk firmware voices"; Types: full custom
Name: "firmware\v20";  Description: "Firmware v2.0 (10 voices: Perfect Paul, Beautiful Betty, Huge Harry, Frail Frank, Doctor Dennis, Kit the Kid, Rough Rita, Uppity Ursula, Whispery Wendy, Variable Val)"; Types: full custom
Name: "firmware\v18";  Description: "Firmware v1.8 (8 voices: Perfect Paul, Beautiful Betty, Huge Harry, Frail Frank, Kit the Kid, Rough Rita, Uppity Ursula, Variable Val)"; Types: full custom

[Tasks]
; Optional desktop shortcut to the configuration utility, offered on its own wizard page so the choice is
; reachable with a screen reader.
Name: "desktopicon"; Description: "Create a &desktop icon for the DECtalk configuration utility"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; --- 32-bit engine + utilities, and the library for each ticked firmware ---
Source: "{#Bin32}\DectalkDtc01SAPI.dll";   DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Bin32}\DectalkConfig.exe";      DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Bin32}\DectalkDiagnostics.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Bin32}\DECtalk20.dll";          DestDir: "{app}"; Components: firmware\v20; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Bin32}\DECtalk.dll";            DestDir: "{app}"; Components: firmware\v18; Flags: ignoreversion restartreplace uninsrestartdelete

; --- 64-bit engine + diagnostics + library, for Narrator and other 64-bit hosts ---
Source: "{#Bin64}\DectalkDtc01SAPI.dll";   DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "{#Bin64}\DectalkDiagnostics.exe"; DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "{#Bin64}\DECtalk20.dll";          DestDir: "{app}\x64"; Components: firmware\v20; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "{#Bin64}\DECtalk.dll";            DestDir: "{app}\x64"; Components: firmware\v18; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode

[Icons]
Name: "{group}\DECtalk configuration"; Filename: "{app}\DectalkConfig.exe"
Name: "{group}\Check DECtalk voices"; Filename: "{app}\DectalkDiagnostics.exe"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\DECtalk configuration"; Filename: "{app}\DectalkConfig.exe"; Tasks: desktopicon

[UninstallDelete]
; Written from [Code] rather than copied, so the uninstaller is told about it by hand.
Type: files; Name: "{app}\voices.ini"

[Code]
// Each COM server must be registered by a regsvr32 of its own bitness: the 32-bit one writes the voice tokens
// under WOW6432Node where 32-bit SAPI hosts look, and the 64-bit one writes them to the native view where
// Narrator and other 64-bit hosts look.
function RegisterServer(const Dll: String; Use64: Boolean; Unregister: Boolean): Boolean;
var
  Exe, Args: String;
  ResultCode: Integer;
begin
  if Use64 then
    Exe := ExpandConstant('{sys}\regsvr32.exe')
  else if Is64BitInstallMode then
    Exe := ExpandConstant('{syswow64}\regsvr32.exe')
  else
    Exe := ExpandConstant('{sys}\regsvr32.exe');

  if Unregister then
    Args := '/s /u "' + Dll + '"'
  else
    Args := '/s "' + Dll + '"';

  Result := Exec(Exe, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// The record of which firmwares were ticked, so a user can see what they chose. The engine does not read it:
// which voices get registered is decided by which library files are on disk (sapi_main.cpp DllRegisterServer).
procedure WriteSelectionFile;
var
  Contents, Firmware: String;
begin
  Firmware := '';
  if WizardIsComponentSelected('firmware\v20') then
    Firmware := 'v20';
  if WizardIsComponentSelected('firmware\v18') then
  begin
    if Firmware <> '' then
      Firmware := Firmware + ',';
    Firmware := Firmware + 'v18';
  end;

  Contents :=
    '; Which DECtalk firmware was selected in the installer.' + #13#10 +
    '; This file is a record only -- the engine does NOT read it back. It' + #13#10 +
    '; registers voices by which library files it finds beside its own DLL' + #13#10 +
    '; (DECtalk.dll: v1.8, DECtalk20.dll: v2.0).' + #13#10 +
    '[Selection]' + #13#10 +
    'Firmware=' + Firmware + #13#10;

  if not SaveStringToFile(ExpandConstant('{app}\voices.ini'), Contents, False) then
    MsgBox('The list of installed firmware could not be written to ' +
           ExpandConstant('{app}\voices.ini') + '.' + #13#10 + #13#10 +
           'This does not affect which voices are registered.', mbInformation, MB_OK);
end;

// Cosmetic count for the Ready-page memo only. v20 -> 10 voices, v18 -> 8 voices, both -> 18. Kept in step BY
// HAND with src/sapi/voices.hpp's VOICES table (10 v20 + 8 v18; v18 has no Doctor Dennis or Whispery Wendy).
function PickedVoiceCount(): Integer;
begin
  Result := 0;
  if WizardIsComponentSelected('firmware\v20') then
    Result := Result + 10;
  if WizardIsComponentSelected('firmware\v18') then
    Result := Result + 8;
end;

// No firmware ticked would register no voices at all, so it is caught here where it can still be fixed.
function NextButtonClick(PageID: Integer): Boolean;
begin
  Result := True;
  if PageID <> wpSelectComponents then
    Exit;

  if (not WizardIsComponentSelected('firmware\v20')) and
     (not WizardIsComponentSelected('firmware\v18')) then
  begin
    MsgBox('Please tick at least one firmware version.' + #13#10 + #13#10 +
           'Without a firmware, DECtalk DTC-01 would add no voices at all.', mbError, MB_OK);
    Result := False;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
                         MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + MemoTypeInfo + NewLine + NewLine +
            MemoComponentsInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + 'Voices that will appear in your speech settings:' + NewLine +
            Space + IntToStr(PickedVoiceCount) + ' voices';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Failed: String;
begin
  if CurStep = ssInstall then
  begin
    // ssInstall fires BEFORE [Files] copies anything, so this is where a library left by an earlier run is
    // removed: [Files] only ever adds files, so an unticked firmware's DLL would stay, and DllRegisterServer
    // would register its voices again. The ROMs and emulator cores of the releases before 2.0 go too.
    DeleteFile(ExpandConstant('{app}\DECtalk.dll'));
    DeleteFile(ExpandConstant('{app}\DECtalk20.dll'));
    DeleteFile(ExpandConstant('{app}\x64\DECtalk.dll'));
    DeleteFile(ExpandConstant('{app}\x64\DECtalk20.dll'));
    DeleteFile(ExpandConstant('{app}\dtc01_x86.dll'));
    DeleteFile(ExpandConstant('{app}\x64\dtc01_x64.dll'));
    DelTree(ExpandConstant('{app}\roms'), True, True, True);
    DelTree(ExpandConstant('{app}\x64\roms'), True, True, True);

    // Drop any previous registration first so a firmware unticked on this run -- or a rename/removal of a voice
    // between versions -- cannot leave an orphaned token pointing at this engine.
    RegisterServer(ExpandConstant('{app}\DectalkDtc01SAPI.dll'), False, True);
    if Is64BitInstallMode then
      RegisterServer(ExpandConstant('{app}\x64\DectalkDtc01SAPI.dll'), True, True);
  end
  else if CurStep = ssPostInstall then
  begin
    // Order matters: the library files have to be on disk before regsvr32 runs, because DllRegisterServer
    // decides which voices to register by which of them it finds beside its own DLL.
    WriteSelectionFile;

    Failed := '';
    if not RegisterServer(ExpandConstant('{app}\DectalkDtc01SAPI.dll'), False, False) then
      Failed := '32-bit';

    if Is64BitInstallMode then
    begin
      if not RegisterServer(ExpandConstant('{app}\x64\DectalkDtc01SAPI.dll'), True, False) then
      begin
        if Failed <> '' then
          Failed := Failed + ' and 64-bit'
        else
          Failed := '64-bit';
      end;
    end;

    if Failed <> '' then
      MsgBox('The ' + Failed + ' speech engine could not be registered. ' +
             'The DECtalk DTC-01 voices may not appear in your applications.' + #13#10 + #13#10 +
             'Try running the installer again as an administrator.', mbError, MB_OK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    if Is64BitInstallMode then
      RegisterServer(ExpandConstant('{app}\x64\DectalkDtc01SAPI.dll'), True, True);
    RegisterServer(ExpandConstant('{app}\DectalkDtc01SAPI.dll'), False, True);
  end;
end;
