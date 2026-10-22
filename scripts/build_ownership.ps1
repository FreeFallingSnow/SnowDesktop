function Get-SharedResourcePaths {
    $manifest=Join-Path $PSScriptRoot 'shared_resources.json'
    if(-not [IO.File]::Exists($manifest)){return @()}
    return @((Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json).languageFiles)
}
function Request-HandoffCheck($Current,$Other,[string]$Actor,[string]$File) {
    $plan=Get-Field $Other 'testPlan'
    if(-not $plan){return}
    $check=Get-Field $Other 'check'
    if($check -and $check.status -in 'running','failed','interrupted'){throw 'Ready check is active/failed; diagnose it before a file handoff.'}
    if(-not (Get-Field $plan 'handoffCarried' $false)){Set-Field $plan 'sourceBeforeHandoff' $plan.source}
    # Reuse the existing deferred-check mode, including already-running ready workers.
    Set-Field $plan 'handoffCarried' $true
    Set-Field $plan 'source' 'repair-carried';Set-Field $plan 'inputIdentity' $null
    if($check){Set-Field $Other 'handoffPreviousCheck' $check;Set-Field $Other 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=(Get-EditRevision $Other);reason='Ownership handed off; recheck final inputs after peer editors close.'})}
    $events=@(Get-Field $Other 'handoffs' @())+@([pscustomobject]@{file=$File;from=$Other.id;to=$Actor;utc=[DateTime]::UtcNow.ToString('o')})
    Set-Field $Other 'handoffs' @($events | Select-Object -Last 100)
    if($check){Set-Field $Other 'waiter' (Start-ReadyWorker $Current $Other)}
}
function Normalize-OwnedPath([string]$Path) {
    $path=$Path.Replace('\','/').TrimEnd('/')
    if (-not $path -or $path -eq '.' -or [IO.Path]::IsPathRooted($path) -or $path -match '(^|/)\.\.(/|$)|[:*?"<>|]' -or $path -match '^(\.git|\.build|\.codex-probes)(/|$)') { throw 'Ownership paths must be literal source paths inside this repository.' }
    $absolute=[IO.Path]::GetFullPath((Join-Path $repositoryRoot $path))
    if (-not $absolute.StartsWith($repositoryRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Ownership path escaped repository.' }
    $cursor=$absolute
    while($cursor -ne $repositoryRoot) {
        if(Test-Path -LiteralPath $cursor) { if(((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Ownership does not follow reparse points.'} }
        $cursor=[IO.Path]::GetDirectoryName($cursor)
    }
    return $absolute.Substring($repositoryRoot.Length+1).Replace('\','/')
}
function Set-Ownership($Current,$Entry,[string]$Files,[bool]$Adopt) {
    $paths=@($Files.Split(',') | Where-Object {$_} | ForEach-Object {Normalize-OwnedPath $_} | Sort-Object -Unique)
    if($paths.Count -eq 0){throw 'Claim requires at least one explicit path.'}
    $shared=@(Get-SharedResourcePaths)
    $handoffs=@()
    foreach($other in $Current.participants) {
        if($other.id -eq $Entry.id -or $other.state -eq 'withdrawn'){continue}
        foreach($mine in $paths){foreach($theirs in @(Get-Field $other 'ownedFiles' @())){
            if($mine.Equals($theirs,[StringComparison]::OrdinalIgnoreCase) -or $mine.StartsWith($theirs+'/',[StringComparison]::OrdinalIgnoreCase) -or $theirs.StartsWith($mine+'/',[StringComparison]::OrdinalIgnoreCase)){
                if($mine -in $shared -and $theirs -in $shared){continue}
                if($other.state -eq 'finished'){$handoffs+=@([pscustomobject]@{other=$other;file=$mine});continue}
                throw "Active file edit conflicts with $($other.id): $mine / $theirs. Queue a structural edit; shared language keys use resource prepare/apply."
            }
        }}
    }
    if(-not $Adopt){
        Push-Location $repositoryRoot
        try {
            $dirty=@(& git.exe status --porcelain -- @paths)
            if($LASTEXITCODE -ne 0){throw 'Cannot inspect claimed paths.'}
            if($dirty.Count -and @(Get-Field $Entry 'ownedFiles' @()).Count -eq 0){throw 'Claimed paths already have changes; review/adopt them explicitly with -AdoptExistingChanges. No claim was changed.'}
        } finally {Pop-Location}
    }
    foreach($handoff in $handoffs){Request-HandoffCheck $Current $handoff.other $Entry.id $handoff.file}
    Set-Field $Entry 'ownedFiles' $paths
    Set-Field $Entry 'ownershipUtc' ([DateTime]::UtcNow.ToString('o'))
    Set-Field $Entry 'unclaimedPeers' @($Current.participants | Where-Object {$_.id -ne $Entry.id -and $_.state -eq 'editing' -and @(Get-Field $_ 'ownedFiles' @()).Count -eq 0} | ForEach-Object {$_.id})
}
function Register-SourceOwnership($State,$Entry,[string]$BatchId) {
    $ledger=Get-TaskLedger $State
    $sequence=[long](Get-Field $ledger 'ownershipSequence' 0)+1
    Set-Field $ledger 'ownershipSequence' $sequence
    Set-Field $Entry 'ownershipSequence' $sequence
    $records=@(Get-Field $ledger 'sourceOwnership' @())
    foreach($path in @(Get-Field $Entry 'ownedFiles' @())) {
        $records=@($records|Where-Object {$_.path -ine $path})+@([pscustomobject]@{
            path=$path;task=$Entry.id;sequence=$sequence;batchId=$BatchId;editRevision=(Get-EditRevision $Entry)})
    }
    Set-Field $ledger 'sourceOwnership' $records
}
function Get-SourceChangeAudit($State) {
    # NUL records retain spaces, Unicode and rename endpoints without relying
    # on Git's quoted display names. This is observation, never staging.
    $status=Invoke-InputGit $repositoryRoot 'status --porcelain=v1 -z --untracked-files=all'
    if($status.code -ne 0){throw 'Cannot inspect outstanding source changes; no task was closed.'}
    $ownership=@(Get-Field (Get-Field $State 'taskLedger') 'sourceOwnership' @())
    $changes=@();$records=$status.output.Split([char]0)
    for($i=0;$i -lt $records.Length;$i++) {
        $record=$records[$i];if(-not $record){continue}
        $kind=$record.Substring(0,2);$paths=@($record.Substring(3))
        if($kind -match '[RC]'){$i++;$paths+= $records[$i]}
        foreach($path in $paths) {
            $owner=@($ownership|Where-Object {$path -ieq $_.path -or $path.StartsWith($_.path+'/',[StringComparison]::OrdinalIgnoreCase)}|Sort-Object sequence -Descending|Select-Object -First 1)
            $changes+= [pscustomobject]@{path=$path;kind=$kind;task=$(if($owner.Count){$owner[0].task}else{$null})}
        }
    }
    return [pscustomobject]@{checkedUtc=[DateTime]::UtcNow.ToString('o');changes=$changes;limitation='Ownership follows recorded claims; pre-upgrade and unregistered edits remain unassigned.'}
}
function Get-OwnedCommitIdentity($Entry) {
    $scopes=@(Get-Field $Entry 'ownedFiles' @())
    $listing=Invoke-InputGit $repositoryRoot 'ls-files -z --cached --others --exclude-standard'
    if($listing.code -ne 0){throw 'Cannot enumerate Git checkpoint inputs.'}
    $paths=@($scopes)+@($listing.output.Split([char]0)|Where-Object {
        $file=$_;$file -and @($scopes|Where-Object {$file -ieq $_ -or $file.StartsWith($_+'/',[StringComparison]::OrdinalIgnoreCase)}).Count
    })
    $paths=@($paths|Sort-Object -Unique)
    $identity=Get-TaskIdentity ([pscustomobject]@{inputs=$paths})
    # Keep explicit missing/deleted inputs even after Git updates the index.
    # New files under the owned scopes still enter the fresh identity check.
    Set-Field $identity 'paths' $paths
    return $identity
}
function Assert-CommitIdentity($Entry) {
    $expected=Get-Field $Entry 'commitIdentity'
    if(-not $expected){throw 'This frozen registration predates Git checkpoints; begin/claim reviewed changes in a new editing window.'}
    $actual=Get-TaskIdentity ([pscustomobject]@{inputs=@(Get-Field $Entry 'ownedFiles' @())+@(Get-Field $expected 'paths' @())})
    if($actual.digest -ne $expected.digest){throw 'Claimed source changed after ready; frozen Git checkpoint refused. Begin/claim the new revision.'}
}
function Invoke-OwnedGitChange([string[]]$Arguments) {
    # WinPS 5.1 turns stderr warnings into ErrorRecords. Capture both streams
    # without treating an advisory as failure; the native exit code decides.
    $previous=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $messages=@(& git.exe @Arguments 2>&1)
        $code=$LASTEXITCODE
    } finally {$ErrorActionPreference=$previous}
    foreach($message in $messages){[Console]::Error.WriteLine([string]$message)}
    return $code
}
function Invoke-OwnedCommit($Selection,[string]$PathText,[string]$MessageFile) {
    $paths=@($PathText.Split(',') | Where-Object {$_} | ForEach-Object {Normalize-OwnedPath $_} | Sort-Object -Unique)
    if($paths.Count -eq 0 -or -not [IO.File]::Exists($MessageFile)){throw 'Commit requires Paths and an existing UTF-8 MessageFile.'}
    foreach($path in $paths){if(@($Selection.ownedFiles | Where-Object {$path.Equals($_,[StringComparison]::OrdinalIgnoreCase) -or $path.StartsWith($_+'/',[StringComparison]::OrdinalIgnoreCase)}).Count -eq 0){throw "Unclaimed commit path: $path"}}
    if(Get-Field $Selection 'frozen' $false){Assert-CommitIdentity $Selection}
    $before=(& git.exe rev-parse HEAD) -join ''
    if($LASTEXITCODE -ne 0){throw 'Cannot determine commit parent.'}
    $indexBefore=@(& git.exe ls-files --stage)
    if($LASTEXITCODE -ne 0){throw 'Cannot snapshot shared index.'}
    # --only commits these paths rather than the shared staged set. Existing
    # staged files belonging to other tasks remain in the index. The lease
    # covers the entire semantic transaction, including these inspections.
    foreach($path in $paths){if(Test-Path -LiteralPath (Join-Path $repositoryRoot $path)){if((Invoke-OwnedGitChange (@('--literal-pathspecs','add','--intent-to-add','--',$path))) -ne 0){throw "Cannot prepare claimed path: $path"}}}
    if((Invoke-OwnedGitChange (@('--literal-pathspecs','diff','--check','--')+$paths)) -ne 0){throw 'Claimed patch has whitespace errors.'}
    if(Get-Field $Selection 'frozen' $false){Assert-CommitIdentity $Selection}
    if((Invoke-OwnedGitChange (@('--literal-pathspecs','commit','--only','-F',$MessageFile,'--')+$paths)) -ne 0){throw 'Commit failed; no index reset or rollback was attempted.'}
    $after=(& git.exe rev-parse HEAD) -join ''
    $committed=@(& git.exe diff-tree --no-commit-id --name-only -r $after)
    foreach($file in $committed){if(@($paths|Where-Object {$file -eq $_ -or $file.StartsWith($_+'/')}).Count -eq 0){throw 'Unexpected committed paths; inspect manual Git activity.'}}
    $otherBefore=@($indexBefore | Where-Object { $name=($_ -split "`t",2)[-1];@($paths|Where-Object {$name -eq $_ -or $name.StartsWith($_+'/')}).Count -eq 0 })
    $otherAfter=@(& git.exe ls-files --stage | Where-Object { $name=($_ -split "`t",2)[-1];@($paths|Where-Object {$name -eq $_ -or $name.StartsWith($_+'/')}).Count -eq 0 })
    $indexStable=@(Compare-Object $otherBefore $otherAfter).Count -eq 0
    return [pscustomobject]@{parent=$before;commit=$after;paths=$paths;otherIndexEntriesUnchanged=$indexStable;utc=[DateTime]::UtcNow.ToString('o');limitation='Only cooperating Git callers honor the transaction lease; same-file edits still require reviewed ownership.'}
}
function Get-BinaryEvidence($Current,[bool]$BuildPassed) {
    $files=@('SnowDesktop.exe','snowwidget.exe','SnowDesktopLauncher.exe','SnowDesktopSteamBridge.exe','SnowDesktopWorkshopManager.exe')
    $root=Join-Path $repositoryRoot '.build\Release'
    $evidence=@()
    foreach($relative in $files){
        $path=Join-Path $root $relative
        if([IO.File]::Exists($path)){
            $file=Get-Item -LiteralPath $path
            $evidence += [pscustomobject]@{file=$relative;sha256=(Get-FileDigest $path);bytes=$file.Length;modifiedUtc=$file.LastWriteTimeUtc.ToString('o');sourceAssociation=$(if($BuildPassed){'batch build completed'}else{'unverified existing output'})}
        }
    }
    return [pscustomobject]@{capturedUtc=[DateTime]::UtcNow.ToString('o');buildStagePassed=$BuildPassed;executables=$evidence;limitation='File identities bind observed outputs; they do not establish runtime/visual acceptance or external SDK identity.'}
}

function Assert-ResultBinaries($Result) {
    $evidence=Get-Field $Result 'binaryEvidence'
    if($evidence -and $evidence.buildStagePassed){
        foreach($file in $evidence.executables){
            $path=Join-Path $repositoryRoot ('.build/Release/'+$file.file)
            if(-not [IO.File]::Exists($path) -or (Get-FileDigest $path) -ne $file.sha256){throw 'Output binary differs from this historical result; use status for history and begin for current verification.'}
        }
    }
    $coverage=Get-Field $Result 'coverage'
    foreach($file in @(Get-Field $coverage 'testExecutablesBeforeRun' @())){
        $path=Join-Path $repositoryRoot $file.executable
        if(-not [IO.File]::Exists($path) -or (Get-FileDigest $path) -ne $file.sha256){throw 'Test executable differs from recorded coverage; begin a new verification batch.'}
    }
}
