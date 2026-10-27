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

function Test-BuildDashboardHealthy([string]$Root,[int]$Port) {
    $response=$null;$client=$null;$handler=$null
    try {
        $directory=Join-Path $Root '.build/dashboard'
        $receiptPath=Join-Path $directory ('server-'+$Port+'.json')
        foreach($part in @((Join-Path $Root '.build'),$directory,$receiptPath)){
            if(([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0){return $false}
        }
        if((New-Object IO.FileInfo($receiptPath)).Length -gt 4096){return $false}
        $receipt=[IO.File]::ReadAllText($receiptPath)|ConvertFrom-Json
        if($receipt.instance -cnotmatch '^[a-f0-9]{32}$' -or $receipt.port -ne $Port -or $receipt.fixture -ne $false){return $false}
        if(-not ('System.Net.Http.HttpClient' -as [type])){Add-Type -AssemblyName System.Net.Http}
        $handler=New-Object Net.Http.HttpClientHandler
        $handler.UseProxy=$false;$handler.AllowAutoRedirect=$false;$handler.UseDefaultCredentials=$false;$handler.UseCookies=$false;$handler.MaxResponseHeadersLength=8
        $client=New-Object Net.Http.HttpClient($handler)
        $client.Timeout=[TimeSpan]::FromMilliseconds(750);$client.MaxResponseContentBufferSize=4096
        # Default completion buffers the bounded body under the same total
        # timeout, so a slowly trickled response cannot extend each read.
        $response=$client.GetAsync('http://127.0.0.1:'+$Port+'/api/health').GetAwaiter().GetResult()
        if($response.StatusCode -ne [Net.HttpStatusCode]::OK){return $false}
        $health=$response.Content.ReadAsStringAsync().GetAwaiter().GetResult()|ConvertFrom-Json
        return $health.service -ceq 'SnowDesktop build dashboard' -and $health.instance -ceq $receipt.instance -and $health.fixture -eq $false
    } catch {return $false}
    finally {if($response){$response.Dispose()};if($client){$client.Dispose()};if($handler){$handler.Dispose()}}
}
function Ensure-BuildDashboard([string]$Root=(Split-Path $PSScriptRoot -Parent),[ValidateRange(0,65535)][int]$Port=0) {
    $manager=Join-Path $Root 'tools/build-dashboard/manage.py'
    if(-not [IO.File]::Exists($manager)){return}
    if($Port -eq 0){
        $Port=8765;$preferred=Join-Path $Root '.build/dashboard/preferred-port.json'
        try {
            $present=$false
            try{[void][IO.File]::GetAttributes($preferred);$present=$true}catch [IO.FileNotFoundException]{}catch [IO.DirectoryNotFoundException]{}
            if($present){
                foreach($part in @((Join-Path $Root '.build'),(Join-Path $Root '.build/dashboard'),$preferred)){
                    if(([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Linked dashboard port preference.'}
                }
                if((New-Object IO.FileInfo($preferred)).Length -gt 1024){throw 'Oversized dashboard port preference.'}
                $value=[IO.File]::ReadAllText($preferred)|ConvertFrom-Json
                if($value.schemaVersion -ne 1 -or ($value.port -isnot [int] -and $value.port -isnot [long])){throw 'Invalid dashboard port preference.'}
                $Port=$value.port
            }
            if($Port -lt 1024 -or $Port -gt 65535){throw 'Dashboard port must be 1024..65535.'}
        }catch{[Console]::Error.WriteLine('Optional local dashboard unavailable: '+$_.Exception.Message);return}
    }
    # A fresh receipt/health comparison avoids starting two Python processes on
    # every coordination write. Unknown listeners still go through manage.py.
    if(Test-BuildDashboardHealthy $Root $Port){return}
    $python=Get-BuildPython
    if(-not $python.available){return}
    $process=$null
    try {
        $info=New-Object Diagnostics.ProcessStartInfo
        $escapedRoot=$Root -replace '(\\+)$','$1$1'
        $info.FileName=$python.path;$info.Arguments='"'+$manager+'" start --port '+$Port+' --root "'+$escapedRoot+'"'
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
