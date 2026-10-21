# Manual acceptance shares the coordinator transaction but has no editing or
# execution authority. Its outcome never changes a build/test result.
function Get-AcceptanceClock {
    $ticks=[Diagnostics.Stopwatch]::GetTimestamp().ToString();$utc=[DateTime]::UtcNow.ToString('o')
    try {
        $boot=(Get-CimInstance -ClassName Win32_OperatingSystem -Property LastBootUpTime -ErrorAction Stop).LastBootUpTime
        return [pscustomobject]@{ticks=$ticks;utc=$utc;frequency=[Diagnostics.Stopwatch]::Frequency.ToString();
            bootUtc=(ConvertTo-UtcTimestamp $boot).ToString('o');available=$true}
    } catch {return [pscustomobject]@{ticks=$null;utc=$utc;frequency=$null;bootUtc=$null;available=$false}}
}
function Get-AcceptanceRecords($Ledger) {
    if(-not $Ledger.PSObject.Properties['acceptances']){Set-Field $Ledger 'acceptances' @()}
    return @($Ledger.acceptances)
}
function Get-AcceptanceArtifacts([string]$Root,[string]$Files) {
    $paths=@($Files.Split(',')|Where-Object {$_}|Sort-Object -Unique)
    if($paths.Count -eq 0 -or $paths.Count -gt 64){throw 'Acceptance requires1 to64 explicitly identified runtime artifact files.'}
    $records=@()
    foreach($relative in $paths){
        if([IO.Path]::IsPathRooted($relative)){throw 'Acceptance artifacts must be repository-relative files.'}
        $absolute=[IO.Path]::GetFullPath((Join-Path $Root $relative))
        if(-not $absolute.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) -or -not [IO.File]::Exists($absolute)){throw 'Acceptance artifact is missing or outside the checkout.'}
        for($part=$absolute;$part.Length -gt $Root.Length;$part=[IO.Path]::GetDirectoryName($part)){
            if(([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Linked acceptance artifacts are not supported.'}
        }
        $records+=@([pscustomobject]@{file=$absolute.Substring($Root.Length+1).Replace('\','/');sha256=(Get-FileDigest $absolute)})
    }
    return $records
}
function Start-TaskAcceptance([string]$Root,$State,$Task,$Result,[long]$EditRevision,[string]$Criterion,[string]$Files) {
    if([string]::IsNullOrWhiteSpace($Criterion)){throw 'Acceptance requires an observable criterion.'}
    if($Result.outcome -ne 'passed' -or $Result.inputCheck -ne 'stable'){throw 'Acceptance requires a passing stable candidate, independently of its eventual manual outcome.'}
    $member=@($Result.participants|Where-Object id -eq $Task.id)
    if($member.Count -ne 1){throw 'Acceptance task does not belong to this candidate.'}
    Assert-EditRevision $member[0] $EditRevision
    $requirement=Get-Field (Get-Field $member[0] 'testPlan') 'requirementsRevision'
    if($null -eq $requirement -or $requirement -ne $Task.requirementsRevision){throw 'Candidate requirements are missing or stale; do not infer manual acceptance.'}
    $current=Get-BuildInputIdentity $Root
    if($current.digest -ne $Result.inputEnd.digest){throw 'Candidate inputs differ from current inputs.'}
    $ledger=Get-TaskLedger $State;$records=@(Get-AcceptanceRecords $ledger)
    if($records.Count -ge 4096){throw 'Acceptance ledger is full; preserve its history.'}
    if(@($records|Where-Object {$_.task -eq $Task.id -and $_.status -eq 'running'}).Count){throw 'This task already has an open acceptance interval.'}
    $artifacts=@(Get-AcceptanceArtifacts $Root $Files)
    $listing=Invoke-InputGit $Root 'ls-files -z --cached --others --exclude-standard'
    if($listing.code -ne 0){throw 'Cannot verify acceptance artifact association.'}
    $inputs=@($listing.output.Split([char]0)|ForEach-Object {$_.Replace('\','/')})
    foreach($artifact in $artifacts){
        $association=$null
        if($inputs -contains $artifact.file -and (Test-BuildInputPath $artifact.file)){$association='candidate input'}
        $built=Get-Field $Result 'binaryEvidence'
        if((Get-Field $built 'buildStagePassed' $false) -and @((Get-Field $built 'executables' @())|Where-Object {('.build/Release/'+$_.file).Equals($artifact.file,[StringComparison]::OrdinalIgnoreCase) -and $_.sha256 -eq $artifact.sha256}).Count){$association='candidate build output'}
        if(@((Get-Field (Get-Field $Result 'coverage') 'testExecutablesBeforeRun' @())|Where-Object {$_.executable.Replace('\','/').Equals($artifact.file,[StringComparison]::OrdinalIgnoreCase) -and $_.sha256 -eq $artifact.sha256}).Count){$association='candidate test output'}
        if(-not $association){throw 'Runtime artifact has no verified association with this candidate; existing output alone is insufficient.'}
        Set-Field $artifact 'sourceAssociation' $association
    }
    $clock=Get-AcceptanceClock
    $entry=[pscustomobject]@{id=[Guid]::NewGuid().ToString('N');task=$Task.id;requirementsRevision=$Task.requirementsRevision;editRevision=$EditRevision;
        batchId=$Result.batchId;runId=(Get-Field $Result 'runId');sourceDigest=$Result.inputEnd.digest;criterion=$Criterion;
        artifacts=$artifacts;status='running';startedUtc=$clock.utc;completedUtc=$null;
        clock=$clock;elapsedSeconds=$null;durationStatus='running';reporter=$null;reference=$null;notes=$null}
    Set-Field $ledger 'acceptances' (@($records)+@($entry))
    return $entry
}
function Complete-TaskAcceptance([string]$Root,$State,$Task,[string]$Id,[string]$Outcome,[string]$Reporter,[string]$Reference,[string]$Notes) {
    if([string]::IsNullOrWhiteSpace($Reference) -or [string]::IsNullOrWhiteSpace($Notes)){throw 'Acceptance completion requires the actual observation/reference and notes.'}
    $records=@(Get-AcceptanceRecords (Get-TaskLedger $State));$found=@($records|Where-Object id -eq $Id)
    if($found.Count -ne 1 -or $found[0].task -ne $Task.id){throw 'Acceptance record does not belong to this task.'}
    $entry=$found[0]
    if($entry.status -ne 'running'){throw 'Acceptance is already closed; its outcome is immutable.'}
    if($Outcome -eq 'passed'){
        if($entry.requirementsRevision -ne $Task.requirementsRevision){throw 'Requirements changed during acceptance; open a new acceptance for the new candidate.'}
        if((Get-BuildInputIdentity $Root).digest -ne $entry.sourceDigest){throw 'Inputs changed during acceptance; a pass cannot verify the changed candidate.'}
        foreach($artifact in $entry.artifacts){
            $current=@(Get-AcceptanceArtifacts $Root $artifact.file)[0]
            if($current.sha256 -ne $artifact.sha256){throw 'Acceptance runtime artifact changed; the pass would refer to a different program.'}
        }
    }
    $end=Get-AcceptanceClock;$entry.durationStatus='unavailable'
    if($entry.clock.available -and $end.available -and $entry.clock.frequency -eq $end.frequency -and
        (ConvertTo-UtcTimestamp $entry.clock.bootUtc) -eq (ConvertTo-UtcTimestamp $end.bootUtc) -and [long]$end.ticks -ge [long]$entry.clock.ticks){
        $entry.elapsedSeconds=([long]$end.ticks-[long]$entry.clock.ticks)/[double]$end.frequency;$entry.durationStatus='monotonic'
    }
    $entry.status=$Outcome;$entry.completedUtc=$end.utc;$entry.reporter=$Reporter;$entry.reference=$Reference;$entry.notes=$Notes
    return $entry
}
