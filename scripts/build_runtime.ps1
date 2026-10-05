param([ValidateSet('dashboard','watch','resource')][string]$RuntimeCommand='dashboard',
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$RuntimeArguments=@())
# Optional Python enhancements never decide whether required tests disappear.
function Test-BuildNativeExecutable([string]$Path) {
    $reader=$null
    try {
        $reader=New-Object IO.BinaryReader([IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)))
        if($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d){return $false}
        $reader.BaseStream.Position=0x3c;$offset=$reader.ReadInt32()
        if($offset -lt 64 -or $offset -gt $reader.BaseStream.Length-24){return $false}
        $reader.BaseStream.Position=$offset
        return $reader.ReadUInt32() -eq 0x4550
    } catch {return $false}
    finally {if($reader){$reader.Dispose()}}
}
function Get-BuildPython {
    $candidate=Get-Command python.exe -ErrorAction SilentlyContinue
    if(-not $candidate){return [pscustomobject]@{available=$false;path=$null;reason='Python 3.8+ is not installed/on PATH.'}}
    if(-not (Test-BuildNativeExecutable $candidate.Source)){return [pscustomobject]@{available=$false;path=$candidate.Source;reason='Python command is not a readable Windows PE executable.'}}
    $process=$null
    try {
        $info=New-Object Diagnostics.ProcessStartInfo
        $info.FileName=$candidate.Source;$info.Arguments='-c "import sys;sys.exit(0 if sys.version_info >= (3,8) else 78)"'
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $process=[Diagnostics.Process]::Start($info)
        if(-not $process.WaitForExit(5000)){$process.Kill();throw 'Python probe exceeded five seconds.'}
        if($process.ExitCode -ne 0){throw ('Python probe failed/version below 3.8 (exit '+$process.ExitCode+').')}
        return [pscustomobject]@{available=$true;path=$candidate.Source;reason=''}
    } catch {return [pscustomobject]@{available=$false;path=$candidate.Source;reason=$_.Exception.Message}}
    finally {if($process){$process.Dispose()}}
}

function Ensure-BuildDashboard {
    # Run inside the existing entry process instead of starting another shell.
    $python=Get-BuildPython
    if(-not $python.available){return}
    $process=$null
    try {
        $info=New-Object Diagnostics.ProcessStartInfo
        $info.FileName=$python.path;$info.Arguments='"'+(Join-Path $PSScriptRoot '../tools/build-dashboard/manage.py')+'" start'
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $process=[Diagnostics.Process]::Start($info)
        if(-not $process.WaitForExit(12000)){$process.Kill()}
    } catch { [Console]::Error.WriteLine('Optional local dashboard unavailable: '+$_.Exception.Message) }
    finally {if($process){$process.Dispose()}}
}

if($MyInvocation.InvocationName -ne '.') {
    if($RuntimeCommand -eq 'dashboard'){Ensure-BuildDashboard;exit 0}
    if($RuntimeCommand -eq 'watch'){Ensure-BuildDashboard}
    $python=Get-BuildPython
    if(-not $python.available){
        if($RuntimeCommand -eq 'dashboard'){exit 0}
        [Console]::Error.WriteLine('Python enhancement unavailable: '+$python.reason+' Use local PowerShell begin/claim/ready-and-wait/status; no dependency was installed.')
        exit 2
    }
    $scriptName=if($RuntimeCommand -eq 'watch'){'build_wait_tasks.py'}else{'build_shared_resources.py'}
    & $python.path (Join-Path $PSScriptRoot $scriptName) @RuntimeArguments
    exit $LASTEXITCODE
}
