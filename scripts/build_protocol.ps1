# Side-effect-free plan/check helpers; called only inside the coordinator's
# existing state transaction when changing registration or freezing a plan.
function Get-Field($Object, [string]$Name, $Default = $null) {
    if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}
function Set-Field($Object, [string]$Name, $Value) {
    $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force
}
function Get-VerificationTextDigest([string]$Text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-','').ToLowerInvariant()}
    finally {$sha.Dispose()}
}
# ConvertFrom-Json returns strings in Windows PowerShell and can return typed
# dates in PowerShell 7. Never stringify a typed date: that drops its UTC kind.
function ConvertTo-UtcTimestamp($Value) {
    if ($Value -is [DateTimeOffset]) { return $Value.UtcDateTime }
    if ($Value -is [DateTime]) {
        if ($Value.Kind -eq [DateTimeKind]::Unspecified) { throw 'Timestamp has no time zone.' }
        return $Value.ToUniversalTime()
    }
    if ($Value -isnot [string] -or $Value -notmatch '(Z|[+-][0-9]{2}:[0-9]{2})$') { throw 'Timestamp requires an explicit time zone.' }
    return [DateTimeOffset]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture).UtcDateTime
}
function Get-TimestampAge($Value) {
    return [Math]::Max(0, ([DateTime]::UtcNow - (ConvertTo-UtcTimestamp $Value)).TotalSeconds)
}

# Investigation presence and durable messages share the coordinator's atomic
# state transaction. Neither grants editing authority nor joins its barrier.
function Get-TaskLedger($State) {
    $ledger = Get-Field $State 'taskLedger'
    if (-not $ledger) {
        $ledger = [pscustomobject]@{schemaVersion=1; sequence=[long]0; tasks=@(); events=@()}
        Set-Field $State 'taskLedger' $ledger
    }
    if ($ledger.schemaVersion -ne 1) { throw 'Unknown task ledger version; preserve state.' }
    return $ledger
}
function Find-VisibleTask($Ledger, [string]$Id) {
    $found = @($Ledger.tasks | Where-Object id -eq $Id)
    if ($found.Count -ne 1) { throw 'Task is not registered. Use task-open before investigation.' }
    return $found[0]
}
function Assert-TaskRevision($Task, [long]$Expected) {
    if ($Expected -lt 0 -or $Task.revision -ne $Expected) { throw "Stale/missing task revision; current revision is $($Task.revision). Read task-list and re-evaluate." }
}
function Open-VisibleTask($Ledger, [string]$Id, [string]$Name, [string]$Adapter, [string]$Origin) {
    $found = @($Ledger.tasks | Where-Object id -eq $Id)
    if ($found.Count) {
        if ($found[0].phase -in 'completed','cancelled') { throw 'Task ID is closed; use a new ID for a new task.' }
        return $found[0]
    }
    if (@($Ledger.tasks).Count -ge 2000) { throw 'Task ledger is full; archive through a reviewed migration. No task was removed.' }
    if ([string]::IsNullOrWhiteSpace($Name)) { $Name=$Id }
    $now=[DateTime]::UtcNow.ToString('o')
    $task=[pscustomobject]@{id=$Id; displayName=$Name; phase='investigating'; revision=[long]0; requirementsRevision=[long]0;
        openedUtc=$now; lastHeartbeatUtc=$now; lastProgressUtc=$now; progress=''; blocker=''; closedUtc=$null;
        registrationOrigin=$Origin; adapter=$Adapter; hostParticipation='host';
        capabilities=[pscustomobject]@{taskRegistration=$true; beforeWriteGate=$false; boundedWait=$false; liveProgress=$false; activeTurnSteering=$false; idleWake=$false}}
    $Ledger.tasks=@($Ledger.tasks)+@($task)
    return $task
}
function Set-TaskHostParticipation($State,$Task,[string]$Participation) {
    if((Get-Field $Task 'hostParticipation' 'host') -eq $Participation){return}
    $batches=@($(if($State.current){$State.current});@(Get-Field $State 'activeCandidates' @()))
    if(@($batches|Where-Object {@($_.participants|Where-Object {$_.id -eq $Task.id -and $_.state -ne 'withdrawn'}).Count}).Count){
        throw 'Host participation cannot change while the task belongs to an active batch. Finish/retire it or recover only your stopped editing registration first.'
    }
    Set-Field $Task 'hostParticipation' $Participation
}
function Assert-IndependentTaskPath([string]$Relative) {
    $path=Normalize-OwnedPath $Relative
    if(@($path.Split('/')|Where-Object {$_ -ieq '.git'}).Count){throw 'Independent task paths cannot target Git metadata.'}
    $top=$path.Split('/')[0]
    $external=$top -in 'website','SnowDesktop_Website','SnowDesktop_SteamAssets','SnowDesktop_Marketing','SnowDesktop-Marketing'
    $community=$path -ieq 'developer_assets/workshop_widgets' -or $path.StartsWith('developer_assets/workshop_widgets/',[StringComparison]::OrdinalIgnoreCase)
    $document=-not $path.Contains('/') -and $path -ine 'AGENTS.md' -and [IO.Path]::GetExtension($path) -in '.md','.txt','.csv','.pdf','.docx','.xlsx','.pptx' -and -not (Test-BuildInputPath $path)
    if(-not ($external -or $community -or $document)){
        throw "Independent tasks cannot write host/shared inputs: $path. Select host participation, then begin and claim before writing."
    }
    return $path
}
function Set-TaskCapabilities($Task,[string]$Names) {
    $supported=@('beforeWriteGate','boundedWait','liveProgress','activeTurnSteering','idleWake')
    $declared=@($Names.Split(',')|Where-Object {$_}|Sort-Object -Unique)
    if(-not $declared.Count -or ($declared -contains 'none' -and $declared.Count -ne 1) -or
        @($declared|Where-Object {$_ -ne 'none' -and $_ -cnotin $supported}).Count){throw 'Capabilities must be supported literal names or none.'}
    $value=[ordered]@{taskRegistration=$true}
    foreach($name in $supported){$value[$name]=$declared -ccontains $name}
    # Adapter declarations are metadata, never an edit grant or message receipt.
    $Task.capabilities=[pscustomobject]$value
}
function New-VisibleTask($Ledger,[string]$PreviousId,[string]$Name) {
    $previous=Find-VisibleTask $Ledger $PreviousId
    if($previous.phase -notin 'completed','cancelled'){throw 'Finish the current task before task-new; use task-stage to rename an active task.'}
    if([string]::IsNullOrWhiteSpace($Name) -or $Name -match '[\r\n]'){throw 'task-new requires a specific, nonempty single-line TaskName.'}
    $root=Get-Field $previous 'continuationRoot' $previous.id
    $generation=[long](Get-Field $previous 'generation' 1)
    if(@($Ledger.tasks|Where-Object {(Get-Field $_ 'continuationRoot') -eq $root -and (Get-Field $_ 'generation' 1) -gt $generation}).Count){throw 'A later task already exists in this task series; read task-list and use its current ID.'}
    $task=Open-VisibleTask $Ledger ('task-'+[Guid]::NewGuid().ToString('N')) $Name.Trim() $previous.adapter 'task-new'
    Set-Field $task 'previousTask' $previous.id
    Set-Field $task 'continuationRoot' $root
    Set-Field $task 'generation' ($generation+1)
    return $task
}
function Get-CompactBatch($Batch,[string]$Participant) {
    if(-not $Batch){return $null}
    $members=@($Batch.participants)
    $own=@($members|Where-Object id -eq $Participant|Select-Object -First 1)
    $member=$null
    if($own.Count){
        $entry=$own[0];$files=@(Get-Field $entry 'ownedFiles' @())
        $member=[pscustomobject]@{id=$entry.id;state=$entry.state;editRevision=(Get-EditRevision $entry);
            ownedFiles=@($files|Select-Object -First 32);ownedFileCount=$files.Count;ownedFilesTruncated=($files.Count -gt 32);
            checkStatus=(Get-Field (Get-Field $entry 'check') 'status');waiterStatus=(Get-Field (Get-Field $entry 'waiter') 'status')}
    }
    return [pscustomobject]@{id=$Batch.id;phase=$Batch.phase;runId=(Get-Field $Batch 'runId');participant=$member;
        participantCount=$members.Count;participants=@($members|Select-Object -First 32|ForEach-Object {[pscustomobject]@{id=$_.id;state=$_.state;editRevision=(Get-EditRevision $_)}});
        participantsTruncated=($members.Count -gt 32)}
}
function Get-CompactCollaborationStatus($State,[string]$Participant,$Current,$Result) {
    $ledger=Get-Field $State 'taskLedger';$tasks=@(Get-Field $ledger 'tasks' @())
    $own=@($tasks|Where-Object id -eq $Participant|Select-Object -First 1)
    $candidates=@(Get-Field $State 'activeCandidates' @())
    $events=@(Get-TaskEvents $ledger $Participant)
    $recent=@($events|Select-Object -Last 8|ForEach-Object {
        [pscustomobject]@{sequence=$_.sequence;sender=$_.sender;kind=$_.kind;importance=$_.importance;
            text=$_.text.Substring(0,[Math]::Min(400,$_.text.Length));textTruncated=($_.text.Length -gt 400);createdUtc=$_.createdUtc;
            agentAcknowledged=(@($_.receipts|Where-Object {$_.task -eq $Participant -and $_.level -eq 'agent'}).Count -gt 0)}
    })
    $task=$null
    if($own.Count){
        $entry=$own[0]
        $task=[pscustomobject]@{id=$entry.id;displayName=$entry.displayName;phase=$entry.phase;revision=$entry.revision;requirementsRevision=$entry.requirementsRevision;hostParticipation=(Get-Field $entry 'hostParticipation' 'host');
            lastHeartbeatUtc=$entry.lastHeartbeatUtc;lastProgressUtc=$entry.lastProgressUtc;progress=$entry.progress;blocker=$entry.blocker;closedUtc=$entry.closedUtc;
            previousTask=(Get-Field $entry 'previousTask');continuationRoot=(Get-Field $entry 'continuationRoot');generation=(Get-Field $entry 'generation' 1);
            deferredBatchIds=@(Get-Field $entry 'deferredBatchIds' @()|Select-Object -First 32)}
    }
    return [pscustomobject]@{schemaVersion=1;detail='compact';taskId=$Participant;task=$task;
        current=(Get-CompactBatch $Current $Participant);activeCandidates=@($candidates|Select-Object -Last 8|ForEach-Object {Get-CompactBatch $_ $Participant});
        activeCandidateCount=$candidates.Count;activeCandidatesTruncated=($candidates.Count -gt 8);recentEvents=$recent;recentEventsTruncated=($events.Count -gt 8);
        result=$(if($Result){[pscustomobject]@{batchId=$Result.batchId;outcome=$Result.outcome;exitCode=$Result.exitCode;runId=(Get-Field $Result 'runId');recovery=(Get-Field $Result 'recovery')}}else{$null});
        observation='Atomic read-only summary; use status without -Compact for full details, event-read for event text. Reading does not acknowledge events.'}
}
function Get-CompactBatchResult($Result,[string]$ResultPath) {
    $coverage=Get-Field $Result 'coverage';$completed=@(Get-Field $coverage 'completed' @())
    $failures=@($completed|Where-Object {$_.status -in 'failed','invalidated','interrupted','cancelled'}|ForEach-Object name)
    $selectedCount=@(Get-Field $coverage 'selected' @()).Count
    $referencesCount=@(Get-Field $coverage 'references' @()).Count
    return [pscustomobject]@{schemaVersion=1;detail='compact';batchId=$Result.batchId;runId=(Get-Field $Result 'runId');
        outcome=$Result.outcome;exitCode=$Result.exitCode;error=(Get-Field $Result 'error' '');resultPath=$ResultPath;
        inputCheck=(Get-Field $Result 'inputCheck');recovery=(Get-Field $Result 'recovery');
        selectedCount=$selectedCount;completedCount=$completed.Count;
        passedCount=@($completed|Where-Object status -eq 'passed').Count;failedCount=$failures.Count;
        notRunCount=(@($completed|Where-Object status -eq 'not-run').Count+[Math]::Max(0,$selectedCount-$completed.Count-$referencesCount));
        failedTests=@($failures|Select-Object -First 16);failedTestsTruncated=($failures.Count -gt 16);
        referencesCount=$referencesCount;
        observation='Full immutable plan, evidence and logs are retained at resultPath. -FullResult opts into full CLI output; compact output does not change execution or evidence.'}
}
function Get-TaskEvents($Ledger, [string]$Id, [long]$After=0, [switch]$UrgentUnacknowledged) {
    if(-not $Ledger){return @()}
    return @($Ledger.events | Where-Object {
        $_.sequence -gt $After -and $_.recipients -contains $Id -and
        (-not $UrgentUnacknowledged -or ($_.importance -eq 'urgent' -and @($_.receipts | Where-Object { $_.task -eq $Id -and $_.level -eq 'agent' }).Count -eq 0))
    })
}
function Get-EditRevision($Entry) { return [long](Get-Field $Entry 'editRevision' 0) }
function Assert-EditRevision($Entry, [long]$Expected) {
    $actual = Get-EditRevision $Entry
    if (($Expected -lt 0 -and $actual -gt 0) -or ($Expected -ge 0 -and $Expected -ne $actual)) {
        throw "Stale/missing edit revision. Current revision is $actual; use the revision returned by begin."
    }
}
function Reopen-Entry($Entry) {
    Set-Field $Entry 'editRevision' ((Get-EditRevision $Entry) + 1)
    $Entry.state = 'editing'; $Entry.finishedUtc = $null
    Set-Field $Entry 'reopenedUtc' ([DateTime]::UtcNow.ToString('o'))
    $check = Get-Field $Entry 'check'
    if ($check) { $check.status = 'invalidated'; Set-Field $check 'reason' 'Registration reopened; evidence belongs to an older edit revision.' }
}
function Get-RepairSources($Batch) {
    $sources=@(Get-Field $Batch 'repairSources' @())
    if($sources.Count){return $sources}
    $parent=Get-Field $Batch 'repairOf'
    if($parent){return @([pscustomobject]@{parentBatchId=$parent;logicalBatchId=(Get-Field $Batch 'logicalBatchId' $parent);attempt=(Get-Field $Batch 'attempt' 2);reason=(Get-Field $Batch 'repairReason' '')})}
    return @()
}
function Find-RepairBatches($State,[string]$Parent) {
    return @(@($State.current)+@(Get-Field $State 'activeCandidates' @())|Where-Object {
        $_ -and @(Get-RepairSources $_|Where-Object parentBatchId -eq $Parent).Count
    })
}
function Get-EffectiveTaskPlan($Entry) {
    $primary=Get-Field $Entry 'testPlan'
    if(-not $primary){$primary=Default-TaskPlan $Entry}
    $extra=@(Get-Field $Entry 'repairRequirements' @())
    if(-not $extra.Count){return $primary}
    # Keep the live registration/claim/revision intact. Other parents add
    # obligations rather than replacing that member's own declared plan.
    $plan=$primary|ConvertTo-Json -Depth 20|ConvertFrom-Json
    $requirements=@($primary)+@($extra|ForEach-Object {$_.requirement})
    $plan.tests=@($requirements|ForEach-Object {$_.tests}|Sort-Object -Unique)
    $plan.suites=@($requirements|ForEach-Object {$_.suites}|Sort-Object -Unique)
    if($plan.suites.Count -gt 1){$plan.suites=@($plan.suites|Where-Object {$_ -ne 'none'})}
    $plan.inputs=@(if(-not @($requirements|Where-Object {@($_.inputs).Count -eq 0}).Count){$requirements|ForEach-Object {$_.inputs}|Sort-Object -Unique})
    $plan.scope=if(@($requirements|Where-Object {$_.scope -notin 'docs','component','tool','test'}).Count){'module'}elseif(@($requirements|Where-Object scope -eq 'tool').Count){'tool'}elseif(@($requirements|Where-Object scope -eq 'test').Count){'test'}else{'component'}
    $plan.source='repair-carried';$plan.requiredFull=$plan.suites -contains 'full'
    Set-Field $plan 'mergedRepairRequirements' $extra
    $resources=@($requirements|Where-Object {$_.suites -notcontains 'none'}|ForEach-Object {Get-Field $_ 'projectResources'}|Where-Object {$_})
    $selected=@($requirements|Where-Object {$_.suites -notcontains 'none'})
    if($resources.Count -ne $selected.Count){$plan.PSObject.Properties.Remove('projectResources')}
    elseif($resources.Count){
        $first=$resources[0]
        if(@($resources|Where-Object {$_.policy -ne $first.policy -or $_.policyHash -ne $first.policyHash -or $_.commonGitDirectory -ne $first.commonGitDirectory}).Count){throw 'Merged repair resource policies differ; update the affected plans.'}
        $binding=$first|ConvertTo-Json -Depth 8|ConvertFrom-Json
        $binding.cpuSlots=[int](($resources|Measure-Object cpuSlots -Maximum).Maximum)
        $binding.memoryMiB=[int](($resources|Measure-Object memoryMiB -Maximum).Maximum)
        $binding.reservedMemoryMiB=[int]([Math]::Ceiling($binding.memoryMiB/256.0)*256)
        if(@($resources|Where-Object compatibilityMode -ne 'shared').Count){$binding.compatibilityMode='exclusive'}
        Set-Field $plan 'projectResources' $binding
    }
    return $plan
}
function Add-RepairSource($Current,$Result,[string]$Parent,[string]$Requester,[string]$Reason) {
    $known=@(Get-RepairSources $Current)
    if(@($known|Where-Object parentBatchId -eq $Parent).Count){return}
    if($Current.phase -ne 'editing'){throw 'Repair attempt is frozen. Observe it; no edit permission granted.'}
    if((Get-Field $Current 'protocolVersion' 1) -lt (Get-Field $Result 'protocolVersion' 1)){throw 'An older active protocol cannot import a newer failed batch; wait for its editing window to close.'}
    if(@($Current.participants|Where-Object id -eq $Requester).Count){throw 'This requester already has another active registration; use its current batch/revision instead of reopening it from another parent.'}
    $now=[DateTime]::UtcNow.ToString('o')
    foreach($old in @($Result.participants|Where-Object state -ne 'withdrawn')){
        $previous=Get-EffectiveTaskPlan $old
        $plan=$previous|ConvertTo-Json -Depth 20|ConvertFrom-Json
        $existing=@($Current.participants|Where-Object id -eq $old.id)
        if($existing.Count){
            $entry=$existing[0]
            if($entry.state -eq 'withdrawn'){throw 'A withdrawn registration cannot be silently restored by another repair source.'}
            $requirements=@(Get-Field $entry 'repairRequirements' @())+@([pscustomobject]@{parentBatchId=$Parent;editRevision=(Get-EditRevision $old);requirement=$plan})
            Set-Field $entry 'repairRequirements' $requirements
            # Supersede any in-flight check by replacing its operation record.
            # Its late completion cannot publish against the new obligations.
            if($entry.state -eq 'finished'){
                Set-Field $entry.testPlan 'inputIdentity' $null
                Set-Field $entry 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=(Get-EditRevision $entry);reason='Another repair source added requirements; check the merged inputs.'})
            }
        }else{
            $entry=[pscustomobject]@{id=$old.id;state='finished';editRevision=((Get-EditRevision $old)+1);registeredUtc=$now;finishedUtc=$now;withdrawalReason=$null;carriedFrom=$Parent;repairRequested=$false}
            $plan.source='repair-carried';$plan.editRevision=Get-EditRevision $entry;$plan.inputIdentity=$null
            $plan.requiredFull=$plan.suites -contains 'full'
            Set-Field $plan 'originalRequirement' (Get-Field $previous 'originalRequirement' $previous)
            Set-Field $entry 'testPlan' $plan
            Set-Field $entry 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=(Get-EditRevision $entry);reason='Prior ready evidence is not reused for repaired inputs.'})
            $Current.participants=@($Current.participants)+@($entry)
        }
    }
    $source=[pscustomobject]@{parentBatchId=$Parent;logicalBatchId=(Get-Field $Result 'logicalBatchId' $Parent);attempt=([int](Get-Field $Result 'attempt' 1)+1);reason=$Reason}
    Set-Field $Current 'repairSources' @($known+@($source))
    if(-not $known.Count){
        Set-Field $Current 'repairOf' $Parent;Set-Field $Current 'logicalBatchId' $source.logicalBatchId
        Set-Field $Current 'attempt' $source.attempt;Set-Field $Current 'repairReason' $Reason
    }
}
function Assert-TestOnlyPlan($Plan) {
    if((Get-Field $Plan 'scope' 'unknown') -ne 'test'){return}
    $suites=@(Get-Field $Plan 'suites' @())
    $inputs=@(Get-Field $Plan 'inputs' @())
    if($suites.Count -ne 1 -or $suites[0] -ne 'selected' -or -not @(Get-Field $Plan 'tests' @()).Count){
        throw 'Test-only plans require an explicit nonempty selected test set.'
    }
    if(-not $inputs.Count -or @($inputs|Where-Object {$_ -cne 'tests' -and -not $_.StartsWith('tests/',[StringComparison]::Ordinal)}).Count){
        throw 'Test-only plans require reviewed inputs within tests/; product or build input changes keep their normal scope.'
    }
}
function New-TaskPlan($Entry, [string]$Scope, [string]$Suites, [string]$Tests, [string]$Inputs, [string]$Reason) {
    $suiteList = @($Suites.Split(',') | Where-Object { $_ } | Sort-Object -Unique)
    if ($suiteList.Count -eq 0 -or @($suiteList | Where-Object { $_ -notin 'full','core','fast','tools','selected','none' }).Count) { throw 'Suites must be full/core/fast/tools/selected/none.' }
    if ($suiteList -contains 'tools') { throw 'Tool tests have been removed; use task-scoped temporary verification instead.' }
    $names = @($Tests.Split(',') | Where-Object { $_ } | Sort-Object -Unique)
    foreach ($name in $names) { if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$') { throw 'Tests must be literal CTest names; shell syntax and regex are not accepted.' } }
    $paths = @($Inputs.Split(',') | Where-Object { $_ } | ForEach-Object {$_.Replace('\','/').TrimEnd('/')} | Sort-Object -Unique)
    foreach ($path in $paths) {
        if ($path -eq '.' -or [IO.Path]::IsPathRooted($path) -or $path -match '(^|[\\/])\.\.([\\/]|$)|[:*?"<>|]' -or $path -match '^(\.git|\.build|\.codex-probes)([\\/]|$)') { throw 'Inputs must be literal repository-relative paths, outside generated/state directories.' }
    }
    if ($suiteList -contains 'none' -and ($suiteList.Count -ne 1 -or $names.Count -gt 0 -or $Scope -notin 'docs','component','tool')) { throw 'Explicit no-host-tests is limited to docs/component/tool and cannot be mixed with tests.' }
    if (($suiteList -contains 'selected') -and $names.Count -eq 0) { throw 'Selected tests must not be empty.' }
    if ([string]::IsNullOrWhiteSpace($Reason) -or $paths.Count -eq 0) { throw 'A plan requires Inputs and a reason/dependency mapping (or exemption basis); full also requires the concrete need for whole-product coverage.' }
    # Scope describes impact, not permission to expand the selected tests.
    # Whole-product coverage must be requested explicitly with its necessity
    # reviewed in Reason; unknown impact is resolved before declaring a plan.
    $requiredFull = $suiteList -contains 'full'
    $plan=[pscustomobject]@{ schemaVersion=1; scope=$Scope; suites=$suiteList; tests=$names; inputs=$paths;
        reason=$Reason; editRevision=(Get-EditRevision $Entry); source='task-declared'; lightChecks=@('builtin-basic'); requiredFull=$requiredFull;
        escalationReason=$(if($requiredFull){$Reason}else{''}); inputIdentity=$null }
    Assert-TestOnlyPlan $plan
    return $plan
}
function Default-TaskPlan($Entry) {
    throw "Missing test plan for $($Entry.id); declare inputs, impact and selected tests with plan before ready/finish. No build or full suite was started."
}
function Get-TaskIdentity($Plan) {
    $paths = @(Get-Field $Plan 'inputs' @())
    if ($paths.Count -eq 0) { return Get-BuildInputIdentity $repositoryRoot }
    $listing = Invoke-InputGit $repositoryRoot 'ls-files -z --cached --others --exclude-standard'
    if ($listing.code -ne 0) { throw 'Cannot enumerate declared task inputs.' }
    $selected = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($declared in $paths) {
        $declared = $declared.Replace('\','/').TrimEnd('/')
        [void]$selected.Add($declared) # also records missing/deleted files
        foreach ($file in $listing.output.Split([char]0)) {
            if ($file -eq $declared -or $file.StartsWith($declared + '/', [StringComparison]::Ordinal)) { [void]$selected.Add($file) }
        }
    }
    $sha = [Security.Cryptography.SHA256]::Create(); $text = New-Object Text.StringBuilder
    try {
        foreach ($path in @($selected | Sort-Object)) {
            $absolute = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $path))
            if (-not $absolute.StartsWith($repositoryRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Task input escaped repository.' }
            $digest='missing'
            if ([IO.File]::Exists($absolute)) {
                $stream=[IO.File]::Open($absolute,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                try { $digest=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant() } finally { $stream.Dispose() }
            } elseif ([IO.Directory]::Exists($absolute)) { $digest='directory' }
            [void]$text.Append($path).Append([char]0).Append($digest).Append("`n")
        }
        $digest=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToString()))).Replace('-','').ToLowerInvariant()
    } finally { $sha.Dispose() }
    return [pscustomobject]@{ algorithm='sha256-declared-paths-v1'; digest=$digest; fileCount=$selected.Count }
}
function Invoke-LightCheck($Plan) {
    # No build, shared artifacts, host startup or arbitrary user command.
    $issues=New-Object 'System.Collections.Generic.List[string]'
    $diff=Invoke-InputGit $repositoryRoot 'diff --check'
    if ($diff.code -ne 0) { $issues.Add(('git diff --check: ' + $diff.output + $diff.error).Trim()) }
    $changed=Invoke-InputGit $repositoryRoot 'diff --name-only -z HEAD'
    $untracked=Invoke-InputGit $repositoryRoot 'ls-files -z --others --exclude-standard'
    if ($changed.code -ne 0 -or $untracked.code -ne 0) { throw 'Cannot determine lightweight check scope.' }
    $candidates=@(($changed.output + $untracked.output).Split([char]0) | Where-Object { $_ } | Sort-Object -Unique)
    $checked=New-Object 'System.Collections.Generic.List[string]'
    foreach ($relative in $candidates) {
        $allowed=@(Get-Field $Plan 'inputs' @())
        if ($allowed.Count -gt 0 -and @($allowed | Where-Object { $relative -eq $_ -or $relative.StartsWith($_.TrimEnd('/') + '/') }).Count -eq 0) { continue }
        $absolute=Join-Path $repositoryRoot $relative
        if (-not [IO.File]::Exists($absolute)) { continue }
        if ($relative.EndsWith('.ps1',[StringComparison]::OrdinalIgnoreCase)) {
            $tokens=$null; $parseErrors=$null
            [void][Management.Automation.Language.Parser]::ParseFile($absolute,[ref]$tokens,[ref]$parseErrors)
            $checked.Add($relative)
            foreach ($errorItem in @($parseErrors)) { $issues.Add("${relative}:$($errorItem.Extent.StartLineNumber): $($errorItem.Message)") }
        } elseif ($relative -in 'CMakePresets.json','CMakeUserPresets.json','version.json','packaging/steam-identity.json') {
            $checked.Add($relative)
            try { [IO.File]::ReadAllText($absolute) | ConvertFrom-Json | Out-Null } catch { $issues.Add("${relative}: invalid JSON") }
        }
    }
    return [pscustomobject]@{ source='builtin-basic'; description='Read-only diff whitespace, changed PowerShell syntax and selected build JSON syntax; not native compilation or semantic review.'; files=@($checked); issues=@($issues); passed=($issues.Count -eq 0) }
}
function Checks-BlockFreeze($Current) {
    return @($Current.participants | Where-Object {
        $check=Get-Field $_ 'check'
        $_.state -eq 'finished' -and $check -and $check.status -in 'pending','running','failed','invalidated','interrupted'
    }).Count -gt 0
}
function New-FrozenPlan($Current) {
    $tasks=@(); $full=$false; $build=$false; $testing=$false;$resources=@();$exclusive=$false
    foreach ($entry in $Current.participants) {
        if ($entry.state -eq 'withdrawn') { continue }
        # Recheck original obligations, not the merged repair input union:
        # docs/tool exemptions can join a test-only repair, while any product
        # obligation must still require its normal complete product build.
        $declared=@((Get-Field $entry 'testPlan'))+@(Get-Field $entry 'repairRequirements' @()|ForEach-Object {$_.requirement})
        foreach($requirement in $declared){Assert-TestOnlyPlan $requirement}
        $plan=Get-EffectiveTaskPlan $entry
        if ($plan.editRevision -ne (Get-EditRevision $entry)) { throw "Plan is stale for $($entry.id); reopen/update plan before ready." }
        if ($plan.source -eq 'task-declared' -and $plan.inputIdentity -and (Get-TaskIdentity $plan).digest -ne $plan.inputIdentity.digest) { throw "Declared inputs changed after ready for $($entry.id); reopen and update plan." }
        $check=Get-Field $entry 'check'
        if($check -and $check.status -eq 'passed' -and (Get-TaskIdentity $plan).digest -ne $check.inputEnd.digest){throw "Check evidence is stale for $($entry.id); repeat check or reopen before edits."}
        # Old plans may carry a scope-derived requiredFull flag. Only an
        # explicit full suite can select whole-product coverage now.
        $plan.requiredFull = $plan.suites -contains 'full'
        if ($plan.requiredFull) { $full=$true }
        if ($plan.suites -notcontains 'none') {
            $testing=$true
            $request=Get-Field $plan 'projectResources'
            if($request){$resources+=@($request)}else{$exclusive=$true}
            # Test selection and a complete host build are distinct needs.
            # Targeted test builds still use the inventory's real dependencies.
            if((Get-Field $plan 'scope' 'unknown') -notin 'docs','component','tool','test'){$build=$true}
        }
        $tasks += [pscustomobject]@{ participant=$entry.id; editRevision=(Get-EditRevision $entry); requirement=$plan }
    }
    $names=@($tasks | ForEach-Object { $_.requirement.tests } | Sort-Object -Unique)
    $suites=@($tasks | ForEach-Object { $_.requirement.suites } | Where-Object { $_ -ne 'none' } | Sort-Object -Unique)
    $result=[pscustomobject]@{ schemaVersion=1; batchId=$Current.id; configuration='Release'; tasks=$tasks; tests=$names; suites=$suites; buildRequired=$build;
        mode=$(if($full){'full'}elseif($testing){'selected'}else{'skipped'}); createdUtc=[DateTime]::UtcNow.ToString('o') }
    Set-Field $result 'repairSources' @(Get-RepairSources $Current)
    if($resources.Count -and -not $exclusive){
        $first=$resources[0]
        if(@($resources|Where-Object {$_.policy -ne 'project-weighted-v1' -or $_.policyHash -ne $first.policyHash -or $_.commonGitDirectory -ne $first.commonGitDirectory}).Count){throw 'Participant resource policies differ; reopen and update the affected plans.'}
        # The aggregate pipeline has one CPU ceiling and one committed-memory
        # ceiling. Use the largest member need, not additive nested budgets.
        $binding=($first|ConvertTo-Json -Depth 8)|ConvertFrom-Json
        if($build -or @($resources|Where-Object compatibilityMode -ne 'shared').Count){$binding.compatibilityMode='exclusive'}
        $binding.cpuSlots=[int](($resources|Measure-Object cpuSlots -Maximum).Maximum)
        $binding.memoryMiB=[int](($resources|Measure-Object memoryMiB -Maximum).Maximum)
        $binding.reservedMemoryMiB=[int]([Math]::Ceiling($binding.memoryMiB/256.0)*256)
        Set-Field $result 'projectResources' $binding
    }
    return $result
}
function Resolve-PlanTests($Plan, $Inventory) {
    $selected=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $available=@($Inventory | ForEach-Object { $_.name })
    foreach ($name in @($Plan.tests)) {
        if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$' -or $available -notcontains $name) { throw "Declared test not in configured inventory: $name" }
        [void]$selected.Add($name)
    }
    foreach ($test in $Inventory) {
        $labels=@($test.properties | Where-Object name -eq 'LABELS' | ForEach-Object { $_.value })
        if ($labels -contains 'manual' -or $labels -contains 'temporary') { continue } # explicit names above remain
        if ($labels -contains 'tools') {
            if ($Plan.suites -contains 'tools') { [void]$selected.Add($test.name) }
            continue
        }
        if ($Plan.mode -eq 'full' -or ($Plan.suites -contains 'core' -and $labels -contains 'core') -or ($Plan.suites -contains 'fast' -and $labels -notcontains 'integration')) { [void]$selected.Add($test.name) }
    }
    if ($Plan.mode -ne 'skipped' -and $selected.Count -eq 0) { throw 'Required test plan matched zero tests.' }
    foreach ($task in @($Plan.tasks)) {
        $request=$task.requirement
        if ($request.suites -contains 'none') { continue }
        $single=[pscustomobject]@{ tests=$request.tests; suites=$request.suites; mode=$(if($request.requiredFull -or $request.suites -contains 'full'){'full'}else{'selected'}); tasks=@() }
        if (@(Resolve-PlanTests $single $Inventory).Count -eq 0) { throw "Empty test requirement for $($task.participant)." }
    }
    return @($selected | Sort-Object)
}

function Get-FileDigest([string]$Path) {
    $sha=[Security.Cryptography.SHA256]::Create();$stream=$null
    try {
        $stream=[IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant()
    } finally {if($stream){$stream.Dispose()};$sha.Dispose()}
}
