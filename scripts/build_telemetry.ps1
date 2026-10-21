# Execution observations are append-only and written by one process per file.
# They do not grant build authority and never infer unobserved compiler phases.
function Write-BuildStageEvent([string]$Root,$Event) {
    $id=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
    if(-not $id){return}
    if($id -notmatch '^[a-f0-9]{32}$' -or -not (Test-EntryDelegation $Root)){throw 'Stage telemetry requires a live Run execution credential.'}
    $directory=Join-Path $Root '.build/collaboration/runs'
    $request=Read-EntryJson (Join-Path $directory ($id+'.request.json'))
    $record=Read-EntryJson (Join-Path $directory ($id+'.json'))
    if(-not $request -or -not $record -or $request.repositoryRoot -ne $Root -or $record.nonce -ne $request.nonce -or
        $record.status -ne 'running' -or $record.owner.pid -ne (Read-EntryJson (Join-Path $Root '.build/collaboration/execution.json')).owner.pid){
        throw 'Stage telemetry has no matching live Run owner.'
    }
    $Event | Add-Member NoteProperty runId $id
    $Event | Add-Member NoteProperty pid $PID
    $Event | Add-Member NoteProperty utc ([DateTime]::UtcNow.ToString('o'))
    $ticks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()
    $path=Join-Path $directory ($id+'.stages.'+$PID+'.'+$ticks+'.jsonl')
    $bytes=[Text.Encoding]::UTF8.GetBytes(($Event|ConvertTo-Json -Depth 8 -Compress)+"`n")
    $stream=[IO.File]::Open($path,'Append','Write','Read')
    try {$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
}
function Start-BuildStage([string]$Root,[string]$Name,[string]$LogPath='') {
    if(-not [Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')){return $null}
    $stage=[pscustomobject]@{id=[Guid]::NewGuid().ToString('N');name=$Name;clock=[Diagnostics.Stopwatch]::StartNew()}
    Write-BuildStageEvent $Root ([pscustomobject]@{schemaVersion=1;kind='start';id=$stage.id;name=$Name;
        parentId=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT','Process');logPath=$LogPath})
    return $stage
}
function Complete-BuildStage([string]$Root,$Stage,[int]$Code) {
    if(-not $Stage){return}
    Write-BuildStageEvent $Root ([pscustomobject]@{schemaVersion=1;kind='end';id=$Stage.id;name=$Stage.name;
        elapsedSeconds=$Stage.clock.Elapsed.TotalSeconds;exitCode=$Code})
}
function Read-BuildStageEvents([string]$Root,[string]$Id) {
    $events=@();$bytes=0L
    $directory=Join-Path $Root '.build/collaboration/runs'
    $files=@(Get-ChildItem -LiteralPath $directory -Filter ($Id+'.stages.*.jsonl') -ErrorAction Stop)+@(Get-ChildItem -LiteralPath $directory -Filter ($Id+'.msbuild.*.jsonl') -ErrorAction Stop)
    foreach($file in $files){
        $bytes+=$file.Length
        if($bytes -gt 4MB){throw 'Run stage observations exceed the 4 MiB read budget; raw observations are retained.'}
        $stream=[IO.File]::Open($file.FullName,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
        try {$content=$reader.ReadToEnd()}finally{$reader.Dispose()}
        # Ignore only the final line while its writer is still appending it.
        $lines=$content.Split("`n")
        for($line=0;$line -lt $lines.Length-1;$line++){
            if(-not $lines[$line]){continue}
            $event=$lines[$line]|ConvertFrom-Json
            if($event.runId -ne $Id -or $event.schemaVersion -ne 1){throw 'Run stage observation identity mismatch.'}
            $events+=@($event)
        }
    }
    return @($events|Sort-Object utc,id,kind)
}

function Get-BuildLoggerArgument([string]$Root,[string[]]$Arguments){
    $id=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
    if(-not $id -or $Arguments -notcontains '--build'){return $null}
    if(-not (Test-EntryDelegation $Root)){throw 'Build observations require a live output credential.'}
    $presetIndex=[Array]::IndexOf($Arguments,'--preset')
    if($presetIndex -ge 0){
        $configuration=Get-Content -LiteralPath (Join-Path $Root 'CMakePresets.json') -Raw|ConvertFrom-Json
        if(-not $configuration.PSObject.Properties['buildPresets']){return $null}
        $buildPreset=@($configuration.buildPresets|Where-Object name -eq $Arguments[$presetIndex+1])[0]
        $configurePreset=@($configuration.configurePresets|Where-Object name -eq $buildPreset.configurePreset)[0]
        $seen=@{}
        while(-not $configurePreset.PSObject.Properties['binaryDir']){
            if($seen.ContainsKey($configurePreset.name) -or -not $configurePreset.PSObject.Properties['inherits'] -or @($configurePreset.inherits).Count -ne 1){throw 'Cannot resolve the configured build output for observations.'}
            $seen[$configurePreset.name]=$true
            $configurePreset=@($configuration.configurePresets|Where-Object name -eq $configurePreset.inherits)[0]
        }
        $output=$configurePreset.binaryDir.Replace('${sourceDir}',$Root)
    }else{$output=[IO.Path]::GetFullPath((Join-Path $Root $Arguments[1]))}
    $cache=Join-Path $output 'CMakeCache.txt'
    if(-not [IO.File]::Exists($cache)){return $null}
    $generator='';$instance=''
    foreach($line in [IO.File]::ReadLines($cache)){
        if($line -match '^CMAKE_GENERATOR:INTERNAL=(.*)$'){$generator=$Matches[1]}
        if($line -match '^CMAKE_GENERATOR_INSTANCE:INTERNAL=(.*)$'){$instance=$Matches[1]}
    }
    if($generator -notlike 'Visual Studio *'){return $null}
    $framework=Join-Path $instance 'MSBuild/Current/Bin/Microsoft.Build.Framework.dll'
    $compiler=Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    if(-not [IO.File]::Exists($framework) -or -not [IO.File]::Exists($compiler)){throw 'Configured MSBuild logger compiler/reference is unavailable; preserve this Run for diagnosis.'}
    $source=Join-Path $PSScriptRoot 'build_msbuild_logger.cs'
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$identity=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes((Get-FileHash -LiteralPath $source).Hash+(Get-FileHash -LiteralPath $framework).Hash+(Get-FileHash -LiteralPath $compiler).Hash)))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
    $directory=Join-Path $Root '.build/collaboration/instrumentation'
    [void][IO.Directory]::CreateDirectory($directory)
    $assembly=Join-Path $directory ('msbuild-'+$identity+'.dll')
    $receipt=Read-EntryJson ($assembly+'.json')
    if(-not [IO.File]::Exists($assembly) -or -not $receipt -or $receipt.sha256 -ne (Get-FileHash -LiteralPath $assembly).Hash){
        & $compiler /nologo /target:library /optimize+ /warn:4 /warnaserror+ ('/reference:'+$framework) ('/out:'+$assembly) $source | ForEach-Object {[Console]::Out.WriteLine($_)}
        if($LASTEXITCODE -ne 0){throw 'MSBuild observation logger compilation failed.'}
        [IO.File]::WriteAllText($assembly+'.json',(@{sha256=(Get-FileHash -LiteralPath $assembly).Hash;sourceIdentity=$identity}|ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
    }
    $eventFile=Join-Path $Root ('.build/collaboration/runs/'+$id+'.msbuild.'+[Guid]::NewGuid().ToString('N')+'.jsonl')
    $parameters=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($eventFile+"`n"+$id+"`n"+[Environment]::GetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT','Process')))
    return '-logger:SnowDesktop.Build.PhaseLogger,'+$assembly+';'+$parameters
}

function Invoke-BuildStageCommand([string]$Root,[string]$Name,[string]$File,[string[]]$Arguments,[ref]$ExitCode){
    $stage=Start-BuildStage $Root $Name
    $previous=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT','Process')
    $code=2
    try{
        if($stage){[Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$stage.id,'Process')}
        if($File -eq 'cmake' -and $Arguments -contains '--build'){
            $logger=Get-BuildLoggerArgument $Root $Arguments
            if($logger){if($Arguments -notcontains '--'){$Arguments+=@('--')};$Arguments+=@($logger)}
        }
        & $File @Arguments
        $code=$LASTEXITCODE
    }finally{
        [Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$previous,'Process')
        Complete-BuildStage $Root $stage $code
        $ExitCode.Value=$code
    }
}
