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
    foreach($other in $Current.participants) {
        if($other.id -eq $Entry.id -or $other.state -eq 'withdrawn'){continue}
        foreach($mine in $paths){foreach($theirs in @(Get-Field $other 'ownedFiles' @())){
            if($mine.Equals($theirs,[StringComparison]::OrdinalIgnoreCase) -or $mine.StartsWith($theirs+'/',[StringComparison]::OrdinalIgnoreCase) -or $theirs.StartsWith($mine+'/',[StringComparison]::OrdinalIgnoreCase)){
                throw "File claim conflicts with $($other.id): $mine / $theirs. Coordinate the shared file; no ownership was changed."
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
    Set-Field $Entry 'ownedFiles' $paths
    Set-Field $Entry 'ownershipUtc' ([DateTime]::UtcNow.ToString('o'))
    Set-Field $Entry 'unclaimedPeers' @($Current.participants | Where-Object {$_.id -ne $Entry.id -and $_.state -ne 'withdrawn' -and @(Get-Field $_ 'ownedFiles' @()).Count -eq 0} | ForEach-Object {$_.id})
}
function Invoke-OwnedCommit($Selection,[string]$PathText,[string]$MessageFile) {
    $paths=@($PathText.Split(',') | Where-Object {$_} | ForEach-Object {Normalize-OwnedPath $_} | Sort-Object -Unique)
    if($paths.Count -eq 0 -or -not [IO.File]::Exists($MessageFile)){throw 'Commit requires Paths and an existing UTF-8 MessageFile.'}
    foreach($path in $paths){if(@($Selection.ownedFiles | Where-Object {$path.Equals($_,[StringComparison]::OrdinalIgnoreCase) -or $path.StartsWith($_+'/',[StringComparison]::OrdinalIgnoreCase)}).Count -eq 0){throw "Unclaimed commit path: $path"}}
    $before=(& git.exe rev-parse HEAD) -join ''
    if($LASTEXITCODE -ne 0){throw 'Cannot determine commit parent.'}
    $indexBefore=@(& git.exe ls-files --stage)
    if($LASTEXITCODE -ne 0){throw 'Cannot snapshot shared index.'}
    # --only commits these paths rather than the shared staged set. Existing
    # staged files belonging to other tasks remain in the index. The lease
    # covers the entire semantic transaction, including these inspections.
    foreach($path in $paths){if([IO.File]::Exists((Join-Path $repositoryRoot $path))){ & git.exe add --intent-to-add -- $path | ForEach-Object {[Console]::Error.WriteLine($_)}; if($LASTEXITCODE -ne 0){throw "Cannot prepare claimed path: $path"} }}
    & git.exe diff --check -- @paths
    if($LASTEXITCODE -ne 0){throw 'Claimed patch has whitespace errors.'}
    & git.exe commit --only -F $MessageFile -- @paths | ForEach-Object {[Console]::Error.WriteLine($_)}
    if($LASTEXITCODE -ne 0){throw 'Commit failed; no index reset or rollback was attempted.'}
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
