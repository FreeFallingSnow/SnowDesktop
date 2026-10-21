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
    foreach($file in @(Get-ChildItem -LiteralPath $directory -Filter ($Id+'.stages.*.jsonl') -ErrorAction Stop)){
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
