param([ValidateSet('dashboard','watch','resource')][string]$RuntimeCommand='dashboard',
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$RuntimeArguments=@())
# Optional Python enhancements never decide whether required tests disappear.
function Get-BuildPython {
    $candidate=Get-Command python.exe -ErrorAction SilentlyContinue
    if(-not $candidate){return [pscustomobject]@{available=$false;path=$null;reason='Python 3.8+ is not installed/on PATH.'}}
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

if($MyInvocation.InvocationName -ne '.') {
    $python=Get-BuildPython
    if(-not $python.available){
        if($RuntimeCommand -eq 'dashboard'){exit 0}
        [Console]::Error.WriteLine('Python enhancement unavailable: '+$python.reason+' Use local PowerShell begin/claim/ready-and-wait/status; no dependency was installed.')
        exit 2
    }
    if($RuntimeCommand -eq 'dashboard') {
        $process=$null
        try {
            $info=New-Object Diagnostics.ProcessStartInfo
            $info.FileName=$python.path;$info.Arguments='"'+(Join-Path $PSScriptRoot '../tools/build-dashboard/manage.py')+'" start'
            $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
            $process=[Diagnostics.Process]::Start($info)
            if(-not $process.WaitForExit(12000)){$process.Kill()}
        } catch { [Console]::Error.WriteLine('Optional local dashboard unavailable: '+$_.Exception.Message) }
        finally {if($process){$process.Dispose()}}
        exit 0
    }
    $scriptName=if($RuntimeCommand -eq 'watch'){'build_wait_tasks.py'}else{'build_shared_resources.py'}
    & $python.path (Join-Path $PSScriptRoot $scriptName) @RuntimeArguments
    exit $LASTEXITCODE
}
