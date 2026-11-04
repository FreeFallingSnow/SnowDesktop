[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('native','settings','localization','help')][string]$Kind='help',
    [switch]$Help,[string]$Executable='', [string]$Out='', [switch]$DryRun,
    [string]$Component='control-panel',[string]$Page='all',[string]$Locale='zh-CN',
    [string]$Appearance='dark',[ValidateRange(96,480)][int]$Dpi=144,[string]$Background='',
    [ValidateRange(320,8192)][int]$Width=1440,[ValidateRange(240,8192)][int]$Height=1000,
    [ValidateRange(0,4096)][int]$Padding=32,[switch]$Transparent,[switch]$ContentOnly,
    [string]$ThemePackage='',[string]$ThemeId='',[string]$IconPreset='none',[switch]$IconOnly,
    [ValidateSet('top','bottom')][string]$TitleBar='top',[string]$State='collapsed',[string]$Focus=''
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if($Help -or $Kind -eq 'help'){
    @'
Usage: scripts/preview.bat native -Out DIR [-Component control-panel] [-Appearance dark] [-Locale zh-CN] [-Dpi 144] [-Width 1440] [-Height 1000] [-Padding 32] [-Background FILE] [-Transparent] [-ContentOnly] [-ThemePackage FILE -ThemeId ID] [-IconPreset none] [-IconOnly] [-TitleBar top]
       scripts/preview.bat settings -Out DIR [-Page all] [-Appearance dark] [-Locale zh-CN] [-Width 1440] [-Height 1000] [-State collapsed] [-Focus ID]
       scripts/preview.bat localization [-Executable TEST_EXE]
Common: -Executable EXE selects an existing build; -DryRun prints the exact argv without starting it; -Help displays this text. This wrapper never compiles.
Native components: collection, collection-group, file-group, file-categories, folder-mapping, calendar-panel, control-panel, tray-panel, resource-panel, status-bar, quick-navigation, dock, taskbar, popup, popup-group, demo-scene, all, storage-hover.
Native appearances: dark, light, glass-dark, glass-light, glass-transparent, acrylic-dark, acrylic-light.
Settings supports light/dark; states: expanded, collapsed, custom, disabled, scenarios, searching, restore, secret-input, notices, general-notices, calendar-dates, calendar-weekly, calendar-monthly, calendar-occurrence.
Settings size: width 840-2400, height 520-1800 DIP. Native output is PNG/result.json; settings output includes result.txt. Isolated preview data does not constitute desktop interaction acceptance.
'@
    exit 0
}
if(-not $Executable){$Executable=Join-Path $root $(if($Kind -eq 'localization'){'.build/Release/tests/SnowDesktopLocalizationContractTests.exe'}else{'.build/Release/SnowDesktop.exe'})}
$Executable=[IO.Path]::GetFullPath($Executable)
$argv=@()
if($Kind -eq 'localization'){$argv=@($root)}else{
    if(-not $Out){throw '-Out is required for preview output.'}
    $Out=[IO.Path]::GetFullPath($Out)
    if($Kind -eq 'settings'){
        if($Appearance -notin 'light','dark' -or $Width -lt 840 -or $Width -gt 2400 -or $Height -lt 520 -or $Height -gt 1800){throw 'Unsupported settings appearance/size; see -Help.'}
        $argv=@('--settings-visual-preview',$Out,$Page,$Appearance,$Locale,[string]$Width,[string]$Height,$State)
        if($Focus){$argv+=$Focus}
    }else{
        if(($Transparent -or $ContentOnly) -and $Background){throw 'Transparent/content-only previews require an empty background.'}
        if(($ThemePackage -and -not $ThemeId) -or ($ThemeId -and -not $ThemePackage)){throw 'ThemePackage and ThemeId must be supplied together.'}
        if($Padding*2 -ge $Width -or $Padding*2 -ge $Height){throw 'Padding must fit inside the canvas.'}
        if($Background){$Background=[IO.Path]::GetFullPath($Background)}
        if($ThemePackage){$ThemePackage=[IO.Path]::GetFullPath($ThemePackage)}
        $argv=@('--native-component-preview',$Component,$Out,[string]$Dpi,$Locale,$Appearance,$Background,[string]$Width,[string]$Height,[string]$Padding,
            [string][int][bool]$Transparent,[string][int][bool]$ContentOnly,(Join-Path $Out 'result.json'),$ThemePackage,$ThemeId,$IconPreset,[string][int][bool]$IconOnly,$TitleBar)
    }
}
if($DryRun){[pscustomobject]@{executable=$Executable;arguments=$argv;argumentCount=$argv.Count;startsProcess=$false}|ConvertTo-Json -Depth 4;exit 0}
if(-not [IO.File]::Exists($Executable)){throw 'Executable not found. Select an existing matching build with -Executable; this wrapper does not build it.'}
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName=$Executable;$info.WorkingDirectory=$root;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
if($info.PSObject.Properties['ArgumentList']){foreach($argument in $argv){$info.ArgumentList.Add($argument)}}else{
    # CommandLineToArgvW escaping for WinPS 5.1/.NET Framework, including empty strings.
    $info.Arguments=(@($argv|ForEach-Object {'"'+([regex]::Replace([regex]::Replace($_,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'}) -join ' ')
}
$process=New-Object Diagnostics.Process;$process.StartInfo=$info
try{[void]$process.Start();$process.WaitForExit();exit $process.ExitCode}finally{$process.Dispose()}
