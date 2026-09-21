#ifndef SourceDir
  #error SourceDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif

#define AppName "VO-VE"
#define AppExeName "vove-ui-qt.exe"

[Setup]
#ifdef InstallerTestIdentity
AppId={{A3DFA875-1E66-4DBD-93C5-488F437F61C1}
#else
AppId={{F2C1B6D9-E1A6-4D94-A27C-CE24F7AE58B0}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=VO-VE contributors
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=VO-VE installer
DefaultDirName={autopf}\VO-VE
DefaultGroupName=VO-VE
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
AllowNoIcons=yes
OutputDir={#OutputDir}
OutputBaseFilename=VO-VE-{#AppVersion}-windows-x64-setup
SetupIconFile={#SourcePath}\vo-ve.ico
SetupArchitecture=x64
PrivilegesRequired=lowest
WizardStyle=modern
ShowLanguageDialog=no
Compression=lzma2/ultra64
SolidCompression=yes
UninstallDisplayIcon={app}\vo-ve.ico
LicenseFile={#SourceDir}\LICENSE
ChangesAssociations=no
ChangesEnvironment=no
CloseApplications=force
RestartApplications=no
RestartIfNeededByRun=no
TerminalServicesAware=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.ComponentsTitle=Additional components
english.ComponentsSubtitle=External tools detected by VO-VE
english.ComponentsIntro=VO-VE does not download these products. Install them separately from their official pages, then click Check again.
english.ComponentInstalled=detected
english.ComponentMissing=not found
english.ComponentRecommended=Recommended
english.ComponentRequired=Required for formats
english.ComponentWindows=Windows
english.ComponentDownload=Download
english.ComponentCheckAgain=Check again
english.LineSeedPurpose=Without this font the interface will work, but will not look like the original design.
english.GhostscriptPurpose=Required for PS, EPS and classic PostScript AI. PDF and PDF-compatible AI do not use it.
english.EverythingPurpose=Recommended for instant global search. Catalog browsing and the local filter work without it.
english.ComponentOpenFailed=The official page could not be opened:
english.RuntimeAccessFailed=Could not prepare the preview handlers. Setup code: %1. Please send the installation log to support.
russian.ComponentsTitle=Дополнительные компоненты
russian.ComponentsSubtitle=Сторонние инструменты, обнаруживаемые VO-VE
russian.ComponentsIntro=VO-VE не загружает эти продукты. Установите их отдельно с официальных страниц, затем нажмите «Проверить снова».
russian.ComponentInstalled=есть
russian.ComponentMissing=отсутствует
russian.ComponentRecommended=Рекомендуется
russian.ComponentRequired=Требуется для форматов
russian.ComponentWindows=Windows
russian.ComponentDownload=Скачать
russian.ComponentCheckAgain=Проверить снова
russian.LineSeedPurpose=Без этого шрифта интерфейс будет работать, но выглядеть не так, как в авторском макете.
russian.GhostscriptPurpose=Требуется для PS, EPS и классического PostScript AI. PDF и PDF-совместимый AI его не используют.
russian.EverythingPurpose=Рекомендуется для мгновенного глобального поиска. Просмотр каталогов и локальный фильтр работают без него.
russian.ComponentOpenFailed=Не удалось открыть официальную страницу:
russian.RuntimeAccessFailed=Не удалось подготовить обработчики просмотра. Код установки: %1. Передайте журнал установки разработчикам.
english.InstallerDesktop=Create a Desktop shortcut
english.InstallerStartMenu=Create a Start menu shortcut
english.InstallerAccept=I accept
english.InstallerDecline=I do not accept
english.InstallerInstall=Install
english.InstallerRussian=Russian
english.InstallerEnglish=English
english.InstallerRestartFailed=Setup could not restart in the selected language:
russian.InstallerDesktop=Создать значок на Рабочем столе
russian.InstallerStartMenu=Создать пункт меню в панели Пуск
russian.InstallerAccept=Принимаю
russian.InstallerDecline=Не принимаю
russian.InstallerInstall=Установить
russian.InstallerRussian=Русский
russian.InstallerEnglish=Английский
russian.InstallerRestartFailed=Не удалось перезапустить установщик на выбранном языке:

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "startmenuicon"; Description: "{cm:InstallerStartMenu}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourcePath}\vo-ve.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\artwork\installer-*.png"; Flags: dontcopy

[Icons]
Name: "{group}\VO-VE"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{code:ShellIconFile}"; Check: ShouldCreateStartMenuShortcut
Name: "{autodesktop}\VO-VE"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{code:ShellIconFile}"; Tasks: desktopicon

[InstallDelete]
Type: files; Name: "{group}\VO-VE.lnk"; Check: RemoveStartMenuShortcut
Type: files; Name: "{autodesktop}\VO-VE.lnk"; Check: RemoveDesktopShortcut

#ifndef InstallerTestIdentity
[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,VO-VE}"; Flags: nowait postinstall skipifsilent
#endif

[UninstallRun]
Filename: "{app}\vove-preview-helper.exe"; Parameters: "--request-installation-shutdown"; RunOnceId: "VO-VE preview helper shutdown"; Flags: runhidden waituntilterminated

[Code]
function ShellIconFile(Param: String): String;
var
  LightTheme: Cardinal;
begin
  LightTheme := 0;
  RegQueryDWordValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Themes\Personalize',
    'SystemUsesLightTheme', LightTheme);
  if LightTheme = 0 then Result := ExpandConstant('{app}\vo-ve-light.ico')
  else Result := ExpandConstant('{app}\vo-ve-dark.ico');
end;

function RemoveDesktopShortcut: Boolean;
begin
  { A silent update has no interactive task selection. Preserve the user's
    existing shortcut unless an interactive install explicitly clears it. }
  Result := (not WizardSilent) and (not WizardIsTaskSelected('desktopicon'));
end;

function RemoveStartMenuShortcut: Boolean;
begin
  Result := (not WizardSilent) and (not WizardIsTaskSelected('startmenuicon'));
end;

procedure LogRuntimePreparation(const S: String; const Error, FirstLine: Boolean);
begin
  Log('VO-VE runtime preparation: ' + Copy(S, 1, 512));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Helper: String;
  ResultCode: Integer;
begin
  Result := '';
  Helper := ExpandConstant('{app}\vove-preview-helper.exe');
  if FileExists(Helper) then
  begin
    if Exec(Helper, '--request-installation-shutdown', '', SW_HIDE,
            ewWaitUntilTerminated, ResultCode) then
      Log(Format('VO-VE existing preview helper shutdown returned %d.', [ResultCode]))
    else
      Log('VO-VE existing preview helper shutdown could not be started; Restart Manager remains available.');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    ResultCode := -1;
    if not ExecAndLogOutput(ExpandConstant('{app}\vove-preview-helper.exe'), '--prepare-worker-runtime',
      ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode,
      @LogRuntimePreparation) or (ResultCode <> 0) then
      RaiseException(FmtMessage(CustomMessage('RuntimeAccessFailed'), [IntToStr(ResultCode)]));
    Log('Prepared read/execute access for the VO-VE decoder profile.');
  end;
end;

const
  InstallerDesignWidth = 540;
  InstallerDesignHeight = 860;
  StartMenuShortcutPolicyVersion = '1';
  LineSeedUrl = 'https://raw.githubusercontent.com/google/fonts/main/ofl/lineseedjp/LINESeedJP-Regular.ttf';
  GhostscriptUrl = 'https://ghostscript.com/releases/gsdnld.html';
  EverythingUrl = 'https://www.voidtools.com/downloads/';

function FindWindow(ClassName, WindowName: String): HWND;
  external 'FindWindowW@user32.dll stdcall';
function GetDriveType(RootPathName: String): LongWord;
  external 'GetDriveTypeW@kernel32.dll stdcall';
function GetSystemMetrics(Index: Integer): Integer;
  external 'GetSystemMetrics@user32.dll stdcall';
function SystemParametersInfo(Action, Param: LongWord; var Data: TRect;
  Update: LongWord): Boolean;
  external 'SystemParametersInfoW@user32.dll stdcall';
function WindowsSendMessage(Window: HWND; Msg: LongWord; WParam: LongInt;
  LParam: LongInt): LongInt;
  external 'SendMessageW@user32.dll stdcall';

var
  InstallerPage: TWizardPage;
  InstallerPanel: TPanel;
  InstallerBackground: TBitmapImage;
  DesktopToggle: TBitmapButton;
  StartMenuToggle: TBitmapButton;
  LineSeedToggle: TBitmapButton;
  GhostscriptToggle: TBitmapButton;
  EverythingToggle: TBitmapButton;
  LineSeedStatus: TBitmapButton;
  GhostscriptStatus: TBitmapButton;
  EverythingStatus: TBitmapButton;
  DownloadButton: TBitmapButton;
  AcceptToggle: TBitmapButton;
  DeclineToggle: TBitmapButton;
  InstallButton: TBitmapButton;
  RussianTarget: TBitmapButton;
  EnglishTarget: TBitmapButton;
  InstallerLicense: TNewMemo;
  OriginalWizardWidth: Integer;
  OriginalWizardHeight: Integer;
  OriginalCancelTabStop: Boolean;
  InstallerCustomMode: Boolean;
  InstallerLanguageSwitching: Boolean;
  LicenseAccepted: Boolean;
  DesktopSelected: Boolean;
  StartMenuSelected: Boolean;
  LineSeedSelected: Boolean;
  GhostscriptSelected: Boolean;
  EverythingSelected: Boolean;
  LineSeedInstalled: Boolean;
  GhostscriptInstalled: Boolean;
  EverythingInstalled: Boolean;
  StartMenuShortcutMigrationNeeded: Boolean;
  StartMenuSelectionTouched: Boolean;

function ShouldCreateStartMenuShortcut: Boolean;
begin
  Result := WizardIsTaskSelected('startmenuicon') or
    (StartMenuShortcutMigrationNeeded and (not StartMenuSelectionTouched) and
     (ExpandConstant('{param:VOVESTARTMENU|}') <> '0'));
end;

#ifdef InstallerTestIdentity
function ForcedComponentState(const VariableName: String; var Forced: Boolean): Boolean;
var
  Value: String;
begin
  Value := Lowercase(GetEnv(VariableName));
  Result := (Value = 'installed') or (Value = 'missing');
  if Result then
    Forced := Value = 'installed';
end;
#endif

function FileExistsInProgramFiles(const RelativePath: String): Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf64}\' + RelativePath)) or
    FileExists(ExpandConstant('{commonpf32}\' + RelativePath));
end;

function IsLocalFixedPath(const Path: String): Boolean;
var
  Drive: String;
begin
  Result := False;
  if (Length(Path) >= 2) and (Copy(Path, 1, 2) = '\\') then
    Exit;
  Drive := ExtractFileDrive(Path);
  Result := (Length(Drive) = 2) and (Drive[2] = ':') and
    (GetDriveType(AddBackslash(Drive)) = 3);
end;

function VersionedBinaryIsPlausible(const Path: String; var Major: Integer): Boolean;
var
  Version: String;
  Separator: Integer;
  Minor: Integer;
begin
  Major := -1;
  Result := IsLocalFixedPath(Path) and FileExists(Path) and
    GetVersionNumbersString(Path, Version);
  if Result then
  begin
    Separator := Pos('.', Version);
    Result := Separator > 1;
    if Result then
    begin
      Major := StrToIntDef(Copy(Version, 1, Separator - 1), -1);
      Delete(Version, 1, Separator);
      Separator := Pos('.', Version);
      if Separator = 0 then
        Separator := Length(Version) + 1;
      Minor := StrToIntDef(Copy(Version, 1, Separator - 1), -1);
      Result := (Major > 0) and (Minor >= 0);
    end;
  end;
end;

function GhostscriptExecutableIsValid(const ExecutablePath: String): Boolean;
var
  ExecutableName: String;
  DllPath: String;
  ExecutableMajor: Integer;
  DllMajor: Integer;
begin
  Result := False;
  if not IsLocalFixedPath(ExecutablePath) then
    Exit;

  if not VersionedBinaryIsPlausible(ExecutablePath, ExecutableMajor) then
    Exit;
  ExecutableName := Lowercase(ExtractFileName(ExecutablePath));
  if ExecutableName = 'gswin64c.exe' then
    DllPath := AddBackslash(ExtractFileDir(ExecutablePath)) + 'gsdll64.dll'
  else if ExecutableName = 'gswin32c.exe' then
    DllPath := AddBackslash(ExtractFileDir(ExecutablePath)) + 'gsdll32.dll'
  else if ExecutableName = 'gs.exe' then
  begin
    DllPath := AddBackslash(ExtractFileDir(ExecutablePath)) + 'gsdll64.dll';
    if VersionedBinaryIsPlausible(DllPath, DllMajor) and
      (ExecutableMajor = DllMajor) then
    begin
      Result := True;
      Exit;
    end;
    DllPath := AddBackslash(ExtractFileDir(ExecutablePath)) + 'gsdll32.dll';
  end
  else
    Exit;

  Result := VersionedBinaryIsPlausible(DllPath, DllMajor) and
    (ExecutableMajor = DllMajor);
end;

function DirectoryContainsGhostscript(const Root: String): Boolean;
var
  FindRec: TFindRec;
  DirectoryPath: String;
begin
  Result := False;
  if not DirExists(Root) then
    Exit;

  if FindFirst(AddBackslash(Root) + 'gs*', FindRec) then
  begin
    try
      repeat
        DirectoryPath := AddBackslash(Root) + FindRec.Name;
        if DirExists(DirectoryPath) and
          (GhostscriptExecutableIsValid(AddBackslash(DirectoryPath) + 'bin\gswin64c.exe') or
           GhostscriptExecutableIsValid(AddBackslash(DirectoryPath) + 'bin\gswin32c.exe')) then
        begin
          Result := True;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function RegistryContainsGhostscript(const RootKey: Integer; const RootPath: String): Boolean;
var
  Versions: TArrayOfString;
  DllPath: String;
  BinPath: String;
  Index: Integer;
begin
  Result := False;
  if not RegGetSubkeyNames(RootKey, RootPath, Versions) then
    Exit;

  for Index := 0 to GetArrayLength(Versions) - 1 do
  begin
    if RegQueryStringValue(RootKey, RootPath + '\' + Versions[Index], 'GS_DLL', DllPath) then
    begin
      BinPath := AddBackslash(ExtractFileDir(DllPath));
      if GhostscriptExecutableIsValid(BinPath + 'gswin64c.exe') or
        GhostscriptExecutableIsValid(BinPath + 'gswin32c.exe') then
      begin
        Result := True;
        Exit;
      end;
    end;
  end;
end;

function GhostscriptExecutableOnPath: Boolean;
var
  Remaining: String;
  DirectoryPath: String;
  Separator: Integer;
begin
  Result := False;
  Remaining := GetEnv('PATH');
  while Remaining <> '' do
  begin
    Separator := Pos(';', Remaining);
    if Separator = 0 then
    begin
      DirectoryPath := Remaining;
      Remaining := '';
    end
    else
    begin
      DirectoryPath := Copy(Remaining, 1, Separator - 1);
      Delete(Remaining, 1, Separator);
    end;
    DirectoryPath := RemoveQuotes(Trim(DirectoryPath));
    if (DirectoryPath <> '') and IsLocalFixedPath(DirectoryPath) and
      (GhostscriptExecutableIsValid(AddBackslash(DirectoryPath) + 'gswin64c.exe') or
       GhostscriptExecutableIsValid(AddBackslash(DirectoryPath) + 'gswin32c.exe') or
       GhostscriptExecutableIsValid(AddBackslash(DirectoryPath) + 'gs.exe')) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function RegistryContainsLineSeed(const RootKey: Integer): Boolean;
var
  ValueNames: TArrayOfString;
  FontPath: String;
  CandidatePath: String;
  Index: Integer;
begin
  Result := False;
  if not RegGetValueNames(RootKey, 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts',
    ValueNames) then
    Exit;

  for Index := 0 to GetArrayLength(ValueNames) - 1 do
  begin
    if Pos('LINE SEED JP', Uppercase(ValueNames[Index])) = 1 then
    begin
      if RegQueryStringValue(RootKey, 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts',
        ValueNames[Index], FontPath) then
      begin
        CandidatePath := FontPath;
        if ExtractFileDrive(CandidatePath) = '' then
          CandidatePath := AddBackslash(ExpandConstant('{fonts}')) + CandidatePath;
        if IsLocalFixedPath(CandidatePath) and FileExists(CandidatePath) then
        begin
          Result := True;
          Exit;
        end;
      end;
    end;
  end;
end;

function DetectLineSeed: Boolean;
#ifdef InstallerTestIdentity
var
  Forced: Boolean;
#endif
begin
#ifdef InstallerTestIdentity
  if ForcedComponentState('VOVE_INSTALLER_TEST_LINE_SEED', Forced) then
  begin
    Result := Forced;
    Exit;
  end;
#endif
  Result := RegistryContainsLineSeed(HKLM64) or RegistryContainsLineSeed(HKLM32) or
    RegistryContainsLineSeed(HKCU);
end;

function DetectGhostscript: Boolean;
#ifdef InstallerTestIdentity
var
  Forced: Boolean;
#endif
begin
#ifdef InstallerTestIdentity
  if GetEnv('VOVE_INSTALLER_TEST_GHOSTSCRIPT_PATH_ONLY') = '1' then
  begin
    Result := GhostscriptExecutableOnPath;
    Exit;
  end;
  if ForcedComponentState('VOVE_INSTALLER_TEST_GHOSTSCRIPT', Forced) then
  begin
    Result := Forced;
    Exit;
  end;
#endif
  Result := GhostscriptExecutableOnPath or
    DirectoryContainsGhostscript(ExpandConstant('{commonpf64}\gs')) or
    DirectoryContainsGhostscript(ExpandConstant('{commonpf32}\gs')) or
    RegistryContainsGhostscript(HKLM64, 'SOFTWARE\GPL Ghostscript') or
    RegistryContainsGhostscript(HKLM64, 'SOFTWARE\Artifex Ghostscript') or
    RegistryContainsGhostscript(HKLM32, 'SOFTWARE\GPL Ghostscript') or
    RegistryContainsGhostscript(HKLM32, 'SOFTWARE\Artifex Ghostscript');
end;

function RegisteredEverythingExists(const RootKey: Integer): Boolean;
var
  RegisteredPath: String;
begin
  Result := RegQueryStringValue(RootKey,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Everything.exe', '', RegisteredPath) and
    IsLocalFixedPath(RemoveQuotes(RegisteredPath)) and
    FileExists(RemoveQuotes(RegisteredPath));
end;

function DirectoryContainsEverything(const Root: String): Boolean;
var
  FindRec: TFindRec;
  DirectoryPath: String;
begin
  Result := False;
  if not DirExists(Root) then
    Exit;

  if FindFirst(AddBackslash(Root) + 'Everything*', FindRec) then
  begin
    try
      repeat
        DirectoryPath := AddBackslash(Root) + FindRec.Name;
        if DirExists(DirectoryPath) and
          (FileExists(AddBackslash(DirectoryPath) + 'Everything.exe') or
           FileExists(AddBackslash(DirectoryPath) + 'Everything64.exe')) then
        begin
          Result := True;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function ExecutableOnLocalFixedPath(const ExecutableName: String): Boolean;
var
  Remaining: String;
  DirectoryPath: String;
  Separator: Integer;
begin
  Result := False;
  Remaining := GetEnv('PATH');
  while Remaining <> '' do
  begin
    Separator := Pos(';', Remaining);
    if Separator = 0 then
    begin
      DirectoryPath := Remaining;
      Remaining := '';
    end
    else
    begin
      DirectoryPath := Copy(Remaining, 1, Separator - 1);
      Delete(Remaining, 1, Separator);
    end;
    DirectoryPath := RemoveQuotes(Trim(DirectoryPath));
    if (DirectoryPath <> '') and IsLocalFixedPath(DirectoryPath) and
      FileExists(AddBackslash(DirectoryPath) + ExecutableName) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function EverythingIpcAvailable: Boolean;
begin
  Result := FindWindow('EVERYTHING_TASKBAR_NOTIFICATION', '') <> 0;
end;

function DetectEverything: Boolean;
#ifdef InstallerTestIdentity
var
  Forced: Boolean;
#endif
begin
#ifdef InstallerTestIdentity
  if GetEnv('VOVE_INSTALLER_TEST_EVERYTHING_PATH_ONLY') = '1' then
  begin
    Result := ExecutableOnLocalFixedPath('Everything.exe') or
      ExecutableOnLocalFixedPath('Everything64.exe');
    Exit;
  end;
  if ForcedComponentState('VOVE_INSTALLER_TEST_EVERYTHING', Forced) then
  begin
    Result := Forced;
    Exit;
  end;
#endif
  Result := EverythingIpcAvailable or ExecutableOnLocalFixedPath('Everything.exe') or
    ExecutableOnLocalFixedPath('Everything64.exe') or
    FileExistsInProgramFiles('Everything\Everything.exe') or
    FileExistsInProgramFiles('Everything\Everything64.exe') or
    DirectoryContainsEverything(ExpandConstant('{commonpf64}')) or
    DirectoryContainsEverything(ExpandConstant('{commonpf32}')) or
    FileExists(ExpandConstant('{localappdata}\Everything\Everything.exe')) or
    FileExists(ExpandConstant('{localappdata}\Everything\Everything64.exe')) or
    RegisteredEverythingExists(HKLM64) or RegisteredEverythingExists(HKLM32) or
    RegisteredEverythingExists(HKCU);
end;

function InstallerLanguageSuffix: String;
begin
  if ActiveLanguage = 'russian' then
    Result := 'ru'
  else
    Result := 'en';
end;

function InstallerArtwork(const Name: String): String;
begin
  Result := ExpandConstant('{tmp}\installer-' + Name + '.png');
end;

procedure LoadButtonArtwork(Button: TBitmapButton; const Name: String);
begin
  Button.PngImage.LoadFromFile(InstallerArtwork(Name));
end;

procedure LoadImageArtwork(Image: TBitmapImage; const Name: String);
begin
  Image.PngImage.LoadFromFile(InstallerArtwork(Name));
end;

function InstallerX(Value: Integer): Integer;
begin
  Result := (Value * InstallerPanel.ClientWidth) div InstallerDesignWidth;
end;

function InstallerY(Value: Integer): Integer;
begin
  Result := (Value * InstallerPanel.ClientHeight) div InstallerDesignHeight;
end;

procedure PlaceInstallerControl(Control: TControl; Left, Top, Width, Height: Integer);
begin
  Control.SetBounds(InstallerX(Left), InstallerY(Top), InstallerX(Width), InstallerY(Height));
end;

procedure SetToggleArtwork(Button: TBitmapButton; Selected: Boolean);
begin
  if Selected then
    LoadButtonArtwork(Button, 'toggle-on')
  else
    LoadButtonArtwork(Button, 'toggle-off');
end;

procedure SetStatusArtwork(Button: TBitmapButton; Installed: Boolean);
var
  State: String;
begin
  if Installed then
  begin
    State := 'present-';
    Button.Caption := CustomMessage('ComponentInstalled');
  end
  else
  begin
    State := 'missing-';
    Button.Caption := CustomMessage('ComponentMissing');
  end;
  LoadButtonArtwork(Button, 'status-' + State + InstallerLanguageSuffix);
end;

procedure RefreshInstallerControls;
begin
  LineSeedInstalled := DetectLineSeed;
  GhostscriptInstalled := DetectGhostscript;
  EverythingInstalled := DetectEverything;
  if LineSeedInstalled then
    LineSeedSelected := False;
  if GhostscriptInstalled then
    GhostscriptSelected := False;
  if EverythingInstalled then
    EverythingSelected := False;
  SetStatusArtwork(LineSeedStatus, LineSeedInstalled);
  SetStatusArtwork(GhostscriptStatus, GhostscriptInstalled);
  SetStatusArtwork(EverythingStatus, EverythingInstalled);
  SetToggleArtwork(DesktopToggle, DesktopSelected);
  SetToggleArtwork(StartMenuToggle, StartMenuSelected);
  SetToggleArtwork(LineSeedToggle, LineSeedSelected);
  SetToggleArtwork(GhostscriptToggle, GhostscriptSelected);
  SetToggleArtwork(EverythingToggle, EverythingSelected);
  LineSeedToggle.Enabled := not LineSeedInstalled;
  GhostscriptToggle.Enabled := not GhostscriptInstalled;
  EverythingToggle.Enabled := not EverythingInstalled;
  LineSeedStatus.Enabled := not LineSeedInstalled;
  GhostscriptStatus.Enabled := not GhostscriptInstalled;
  EverythingStatus.Enabled := not EverythingInstalled;
  if LineSeedInstalled then
  begin
    LineSeedToggle.Cursor := crDefault;
    LineSeedStatus.Cursor := crDefault;
  end
  else
  begin
    LineSeedToggle.Cursor := crHand;
    LineSeedStatus.Cursor := crHand;
  end;
  if GhostscriptInstalled then
  begin
    GhostscriptToggle.Cursor := crDefault;
    GhostscriptStatus.Cursor := crDefault;
  end
  else
  begin
    GhostscriptToggle.Cursor := crHand;
    GhostscriptStatus.Cursor := crHand;
  end;
  if EverythingInstalled then
  begin
    EverythingToggle.Cursor := crDefault;
    EverythingStatus.Cursor := crDefault;
  end
  else
  begin
    EverythingToggle.Cursor := crHand;
    EverythingStatus.Cursor := crHand;
  end;
  SetToggleArtwork(AcceptToggle, LicenseAccepted);
  SetToggleArtwork(DeclineToggle, not LicenseAccepted);
  DownloadButton.Enabled :=
    (LineSeedSelected and not LineSeedInstalled) or
    (GhostscriptSelected and not GhostscriptInstalled) or
    (EverythingSelected and not EverythingInstalled);
  if DownloadButton.Enabled then
    LoadButtonArtwork(DownloadButton, 'download-' + InstallerLanguageSuffix)
  else
    LoadButtonArtwork(DownloadButton, 'download-disabled-' + InstallerLanguageSuffix);
  InstallButton.Enabled := LicenseAccepted;
  if LicenseAccepted then
    LoadButtonArtwork(InstallButton, 'install-' + InstallerLanguageSuffix)
  else
    LoadButtonArtwork(InstallButton, 'install-disabled-' + InstallerLanguageSuffix);
  if InstallerCustomMode and InstallerPanel.Visible then
    WindowsSendMessage(WizardForm.Handle, $0127, $00010001, 0);
end;

procedure RefreshComponentsOnActivate(Sender: TObject);
begin
  if InstallerCustomMode then
    RefreshInstallerControls;
end;

procedure OpenOfficialPage(const Url: String);
var
  ErrorCode: Integer;
begin
  if not ShellExec('open', Url, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode) then
    MsgBox(CustomMessage('ComponentOpenFailed') + #13#10 + Url, mbError, MB_OK);
end;

procedure SelectComponent(Sender: TObject);
var
  SelectedControl: TBitmapButton;
begin
  SelectedControl := TBitmapButton(Sender);
  if (Sender = LineSeedToggle) or (Sender = LineSeedStatus) then
  begin
    if LineSeedInstalled then
      Exit;
    LineSeedSelected := not LineSeedSelected;
  end
  else if (Sender = GhostscriptToggle) or (Sender = GhostscriptStatus) then
  begin
    if GhostscriptInstalled then
      Exit;
    GhostscriptSelected := not GhostscriptSelected;
  end
  else
  begin
    if EverythingInstalled then
      Exit;
    EverythingSelected := not EverythingSelected;
  end;
  RefreshInstallerControls;
  WizardForm.ActiveControl := SelectedControl;
  WindowsSendMessage(WizardForm.Handle, $0127, $00010001, 0);
end;

procedure DownloadSelectedComponent(Sender: TObject);
begin
  RefreshInstallerControls;
  if LineSeedSelected and not LineSeedInstalled then
    OpenOfficialPage(LineSeedUrl);
  if GhostscriptSelected and not GhostscriptInstalled then
    OpenOfficialPage(GhostscriptUrl);
  if EverythingSelected and not EverythingInstalled then
    OpenOfficialPage(EverythingUrl);
end;

function SelectedComponentsMask: Integer;
begin
  Result := 0;
  if LineSeedSelected then
    Result := Result + 1;
  if GhostscriptSelected then
    Result := Result + 2;
  if EverythingSelected then
    Result := Result + 4;
end;

procedure ToggleDesktop(Sender: TObject);
begin
  DesktopSelected := not DesktopSelected;
  if WizardForm.TasksList.Items.Count > 0 then
    WizardForm.TasksList.Checked[0] := DesktopSelected;
  RefreshInstallerControls;
end;

procedure ToggleStartMenu(Sender: TObject);
begin
  StartMenuSelectionTouched := True;
  StartMenuSelected := not StartMenuSelected;
  if WizardForm.TasksList.Items.Count > 1 then
    WizardForm.TasksList.Checked[1] := StartMenuSelected;
  RefreshInstallerControls;
end;

procedure AcceptLicense(Sender: TObject);
begin
  LicenseAccepted := True;
  RefreshInstallerControls;
end;

procedure DeclineLicense(Sender: TObject);
begin
  LicenseAccepted := False;
  RefreshInstallerControls;
end;

procedure LeaveInstallerMode;
begin
  if not InstallerCustomMode then
    Exit;
  InstallerCustomMode := False;
  InstallerPanel.Visible := False;
  WizardForm.Width := OriginalWizardWidth;
  WizardForm.Height := OriginalWizardHeight;
  WizardForm.Position := poScreenCenter;
  WizardForm.OuterNotebook.Visible := True;
  WizardForm.MainPanel.Visible := True;
  WizardForm.Bevel.Visible := True;
  WizardForm.BackButton.Visible := True;
  WizardForm.NextButton.Visible := True;
  WizardForm.CancelButton.Visible := True;
  WizardForm.CancelButton.TabStop := OriginalCancelTabStop;
end;

procedure InstallNow(Sender: TObject);
begin
  if not LicenseAccepted then
    Exit;
  LeaveInstallerMode;
  WizardForm.NextButton.OnClick(WizardForm.NextButton);
end;

procedure RestartWithLanguage(const Language: String);
var
  ErrorCode: Integer;
  RelaunchCommand: String;
  Parameters: String;
  SelectedTasks: String;
begin
  if ActiveLanguage = Language then
    Exit;
  SelectedTasks := '';
  if DesktopSelected then
    SelectedTasks := 'desktopicon';
  if StartMenuSelected then
  begin
    if SelectedTasks <> '' then
      SelectedTasks := SelectedTasks + ',';
    SelectedTasks := SelectedTasks + 'startmenuicon';
  end;
  Parameters := '/CURRENTUSER /LANG=' + Language + ' /DIR=' + AddQuotes(WizardDirValue) +
    ' /TASKS=' + AddQuotes(SelectedTasks) +
    ' /VOVECOMPONENTS=' + IntToStr(SelectedComponentsMask);
  if StartMenuSelected then
    Parameters := Parameters + ' /VOVESTARTMENU=1'
  else
    Parameters := Parameters + ' /VOVESTARTMENU=0';
  if LicenseAccepted then
    Parameters := Parameters + ' /VOVEACCEPTED=1';
  { Setup cannot directly execute itself before installation has started. The native shell waits
    until this instance is gone and then performs the language-preserving relaunch. }
  RelaunchCommand := '/D /S /C "ping.exe -n 2 127.0.0.1 >NUL & start "" ' +
    AddQuotes(ExpandConstant('{srcexe}')) + ' ' + Parameters + '"';
  if Exec(ExpandConstant('{cmd}'), RelaunchCommand, ExpandConstant('{src}'),
      SW_HIDE, ewNoWait, ErrorCode) then
  begin
    InstallerLanguageSwitching := True;
    WizardForm.Close;
  end
  else
    MsgBox(CustomMessage('InstallerRestartFailed') + #13#10 + Language + #13#10 +
      IntToStr(ErrorCode) + ': ' + SysErrorMessage(ErrorCode), mbError, MB_OK);
end;

procedure SelectRussian(Sender: TObject);
begin
  RestartWithLanguage('russian');
end;

procedure SelectEnglish(Sender: TObject);
begin
  RestartWithLanguage('english');
end;

function CreateArtworkButton(const Caption: String; Handler: TNotifyEvent): TBitmapButton;
begin
  Result := TBitmapButton.Create(InstallerPage);
  Result.Parent := InstallerPanel;
  Result.BackColor := clNone;
  Result.Stretch := True;
  Result.Caption := Caption;
  Result.Cursor := crHand;
  Result.OnClick := Handler;
end;

procedure LayoutInstaller;
begin
  InstallerPanel.SetBounds(0, 0, WizardForm.ClientWidth, WizardForm.ClientHeight);
  InstallerBackground.SetBounds(0, 0, InstallerPanel.ClientWidth, InstallerPanel.ClientHeight);
  PlaceInstallerControl(DesktopToggle, 34, 198, 34, 34);
  PlaceInstallerControl(StartMenuToggle, 34, 233, 34, 34);
  PlaceInstallerControl(GhostscriptToggle, 34, 352, 34, 34);
  PlaceInstallerControl(LineSeedToggle, 34, 458, 34, 34);
  PlaceInstallerControl(EverythingToggle, 34, 518, 34, 34);
  PlaceInstallerControl(GhostscriptStatus, 156, 358, 110, 22);
  PlaceInstallerControl(LineSeedStatus, 226, 464, 110, 22);
  PlaceInstallerControl(EverythingStatus, 157, 524, 110, 22);
  PlaceInstallerControl(DownloadButton, 412, 586, 92, 30);
  PlaceInstallerControl(InstallerLicense, 38, 683, 464, 85);
  PlaceInstallerControl(AcceptToggle, 34, 774, 34, 34);
  PlaceInstallerControl(DeclineToggle, 34, 806, 34, 34);
  PlaceInstallerControl(InstallButton, 412, 800, 92, 30);
  PlaceInstallerControl(RussianTarget, 451, 8, 33, 28);
  PlaceInstallerControl(EnglishTarget, 486, 8, 45, 28);
end;

procedure EnterInstallerMode;
var
  AvailableHeight: Integer;
  AvailableWidth: Integer;
  NonClientHeight: Integer;
  NonClientWidth: Integer;
  TargetHeight: Integer;
  TargetWidth: Integer;
  WorkArea: TRect;
begin
  if InstallerCustomMode then
    Exit;
  InstallerCustomMode := True;
  if not SystemParametersInfo(48, 0, WorkArea, 0) then
  begin
    WorkArea.Left := 0;
    WorkArea.Top := 0;
    WorkArea.Right := GetSystemMetrics(0);
    WorkArea.Bottom := GetSystemMetrics(1);
  end;
  NonClientHeight := WizardForm.Height - WizardForm.ClientHeight;
  NonClientWidth := WizardForm.Width - WizardForm.ClientWidth;
  AvailableHeight := WorkArea.Bottom - WorkArea.Top - NonClientHeight - ScaleY(8);
  AvailableWidth := WorkArea.Right - WorkArea.Left - NonClientWidth - ScaleX(8);
  TargetHeight := ScaleY(InstallerDesignHeight);
  if TargetHeight > AvailableHeight then
    TargetHeight := AvailableHeight;
  TargetWidth := (TargetHeight * InstallerDesignWidth) div InstallerDesignHeight;
  if TargetWidth > AvailableWidth then
  begin
    TargetWidth := AvailableWidth;
    TargetHeight := (TargetWidth * InstallerDesignHeight) div InstallerDesignWidth;
  end;
  WizardForm.ClientWidth := TargetWidth;
  WizardForm.ClientHeight := TargetHeight;
  WizardForm.Left := WorkArea.Left +
    (WorkArea.Right - WorkArea.Left - WizardForm.Width) div 2;
  WizardForm.Top := WorkArea.Top +
    (WorkArea.Bottom - WorkArea.Top - WizardForm.Height) div 2;
  WizardForm.OuterNotebook.Visible := False;
  WizardForm.MainPanel.Visible := False;
  WizardForm.Bevel.Visible := False;
  WizardForm.BackButton.Visible := False;
  WizardForm.NextButton.Visible := False;
  { Keep the native Cancel button logically visible behind the opaque custom panel. Inno Setup
    ignores the title-bar Close command when its Cancel button is hidden. }
  WizardForm.CancelButton.Visible := True;
  WizardForm.CancelButton.TabStop := False;
  LayoutInstaller;
  LoadImageArtwork(InstallerBackground, 'background-' + InstallerLanguageSuffix);
  LoadButtonArtwork(DownloadButton, 'download-' + InstallerLanguageSuffix);
  RefreshInstallerControls;
  InstallerPanel.Visible := True;
  InstallerPanel.BringToFront;
  WindowsSendMessage(WizardForm.Handle, $0127, $00010001, 0);
end;

procedure InitializeWizard;
var
  ComponentMask: Integer;
  StartMenuChoice: String;
begin
  OriginalWizardWidth := WizardForm.Width;
  OriginalWizardHeight := WizardForm.Height;
  OriginalCancelTabStop := WizardForm.CancelButton.TabStop;
  StartMenuShortcutMigrationNeeded :=
    GetPreviousData('StartMenuShortcutPolicy', '') <> StartMenuShortcutPolicyVersion;
  StartMenuSelectionTouched := False;
  StartMenuChoice := ExpandConstant('{param:VOVESTARTMENU|}');
  if WizardForm.TasksList.Items.Count > 1 then
  begin
    if StartMenuChoice = '1' then
      WizardForm.TasksList.Checked[1] := True
    else if StartMenuChoice = '0' then
      WizardForm.TasksList.Checked[1] := False
    else if StartMenuShortcutMigrationNeeded then
      { Migrate installations made while the Start menu task defaulted to off. }
      WizardForm.TasksList.Checked[1] := True;
  end;
  if WizardSilent then
    Exit;
  ExtractTemporaryFile('installer-background-ru.png');
  ExtractTemporaryFile('installer-background-en.png');
  ExtractTemporaryFile('installer-toggle-on.png');
  ExtractTemporaryFile('installer-toggle-off.png');
  ExtractTemporaryFile('installer-status-present-ru.png');
  ExtractTemporaryFile('installer-status-missing-ru.png');
  ExtractTemporaryFile('installer-status-present-en.png');
  ExtractTemporaryFile('installer-status-missing-en.png');
  ExtractTemporaryFile('installer-download-ru.png');
  ExtractTemporaryFile('installer-download-en.png');
  ExtractTemporaryFile('installer-download-disabled-ru.png');
  ExtractTemporaryFile('installer-download-disabled-en.png');
  ExtractTemporaryFile('installer-install-ru.png');
  ExtractTemporaryFile('installer-install-en.png');
  ExtractTemporaryFile('installer-install-disabled-ru.png');
  ExtractTemporaryFile('installer-install-disabled-en.png');

  InstallerPage := CreateCustomPage(wpWelcome, 'VO-VE', 'View only - view everything');
  InstallerPanel := TPanel.Create(WizardForm);
  InstallerPanel.Parent := WizardForm;
  InstallerPanel.BevelOuter := bvNone;
  InstallerPanel.Color := $00BBE4F1;
  InstallerPanel.Visible := False;

  InstallerBackground := TBitmapImage.Create(InstallerPage);
  InstallerBackground.Parent := InstallerPanel;
  InstallerBackground.Stretch := True;
  InstallerBackground.BackColor := $00BBE4F1;

  DesktopToggle := CreateArtworkButton(CustomMessage('InstallerDesktop'), @ToggleDesktop);
  StartMenuToggle := CreateArtworkButton(CustomMessage('InstallerStartMenu'), @ToggleStartMenu);
  LineSeedToggle := CreateArtworkButton('LINE Seed JP', @SelectComponent);
  GhostscriptToggle := CreateArtworkButton('Ghostscript', @SelectComponent);
  EverythingToggle := CreateArtworkButton('Everything', @SelectComponent);
  LineSeedStatus := CreateArtworkButton('', @SelectComponent);
  GhostscriptStatus := CreateArtworkButton('', @SelectComponent);
  EverythingStatus := CreateArtworkButton('', @SelectComponent);
  DownloadButton := CreateArtworkButton(CustomMessage('ComponentDownload'),
    @DownloadSelectedComponent);
  AcceptToggle := CreateArtworkButton(CustomMessage('InstallerAccept'), @AcceptLicense);
  DeclineToggle := CreateArtworkButton(CustomMessage('InstallerDecline'), @DeclineLicense);
  InstallButton := CreateArtworkButton(CustomMessage('InstallerInstall'), @InstallNow);

  RussianTarget := CreateArtworkButton(CustomMessage('InstallerRussian'), @SelectRussian);
  RussianTarget.Hint := CustomMessage('InstallerRussian');
  RussianTarget.ShowHint := True;
  EnglishTarget := CreateArtworkButton(CustomMessage('InstallerEnglish'), @SelectEnglish);
  EnglishTarget.Hint := CustomMessage('InstallerEnglish');
  EnglishTarget.ShowHint := True;

  InstallerLicense := TNewMemo.Create(InstallerPage);
  InstallerLicense.Parent := InstallerPanel;
  InstallerLicense.ReadOnly := True;
  InstallerLicense.ScrollBars := ssVertical;
  InstallerLicense.Color := $00EAF9FF;
  InstallerLicense.Font.Name := 'Segoe UI';
  InstallerLicense.Font.Size := 9;
  InstallerLicense.Text := WizardForm.LicenseMemo.Text;

  DesktopSelected := (WizardForm.TasksList.Items.Count > 0) and
    WizardForm.TasksList.Checked[0];
  StartMenuSelected := (WizardForm.TasksList.Items.Count > 1) and
    WizardForm.TasksList.Checked[1];
  ComponentMask := StrToIntDef(ExpandConstant('{param:VOVECOMPONENTS|2}'), 2);
  if (ComponentMask < 0) or (ComponentMask > 7) then
    ComponentMask := 2;
  LineSeedSelected := (ComponentMask mod 2) = 1;
  GhostscriptSelected := ((ComponentMask div 2) mod 2) = 1;
  EverythingSelected := ((ComponentMask div 4) mod 2) = 1;
  LicenseAccepted := ExpandConstant('{param:VOVEACCEPTED|0}') = '1';

  WizardForm.OnActivate := @RefreshComponentsOnActivate;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'StartMenuShortcutPolicy',
    StartMenuShortcutPolicyVersion);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpLicense) or (PageID = wpSelectDir) or
    (PageID = wpSelectProgramGroup) or (PageID = wpSelectTasks) or (PageID = wpReady);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := (InstallerPage = nil) or (CurPageID <> InstallerPage.ID) or LicenseAccepted;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  Cancel := True;
  if InstallerLanguageSwitching or InstallerCustomMode or
      ((InstallerPage <> nil) and (CurPageID = InstallerPage.ID)) then
    Confirm := False;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (InstallerPage <> nil) and (CurPageID = InstallerPage.ID) then
    EnterInstallerMode
  else
    LeaveInstallerMode;
end;
