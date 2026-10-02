# Side-effect-free plan/check helpers; called only inside the coordinator's
# existing state transaction when changing registration or freezing a plan.
function Get-Field($Object, [string]$Name, $Default = $null) {
    if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}
function Set-Field($Object, [string]$Name, $Value) {
    $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force
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
function New-TaskPlan($Entry, [string]$Scope, [string]$Suites, [string]$Tests, [string]$Inputs, [string]$Reason) {
    $suiteList = @($Suites.Split(',') | Where-Object { $_ } | Sort-Object -Unique)
    if ($suiteList.Count -eq 0 -or @($suiteList | Where-Object { $_ -notin 'full','core','fast','selected','none' }).Count) { throw 'Suites must be full/core/fast/selected/none.' }
    $names = @($Tests.Split(',') | Where-Object { $_ } | Sort-Object -Unique)
    foreach ($name in $names) { if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$') { throw 'Tests must be literal CTest names; shell syntax and regex are not accepted.' } }
    $paths = @($Inputs.Split(',') | Where-Object { $_ } | ForEach-Object {$_.Replace('\','/').TrimEnd('/')} | Sort-Object -Unique)
    foreach ($path in $paths) {
        if ($path -eq '.' -or [IO.Path]::IsPathRooted($path) -or $path -match '(^|[\\/])\.\.([\\/]|$)|[:*?"<>|]' -or $path -match '^(\.git|\.build|\.codex-probes)([\\/]|$)') { throw 'Inputs must be literal repository-relative paths, outside generated/state directories.' }
    }
    if ($suiteList -contains 'none' -and ($suiteList.Count -ne 1 -or $names.Count -gt 0 -or $Scope -notin 'docs','component','tool')) { throw 'Explicit no-host-tests is limited to docs/component/tool and cannot be mixed with tests.' }
    if (($suiteList -contains 'selected') -and $names.Count -eq 0) { throw 'Selected tests must not be empty.' }
    if ($Scope -ne 'unknown' -and ([string]::IsNullOrWhiteSpace($Reason) -or $paths.Count -eq 0)) { throw 'A scoped plan requires Inputs and a reason/dependency mapping (or exemption basis).' }
    # These are broad/shared inputs even if a caller labels them local. This
    # is a conservative guard, not an automatic dependency selector.
    $broad = @($paths | Where-Object { $_ -match '^(CMakeLists\.txt|CMakePresets\.json|scripts/(build|test)|src/(widget_engine|widget_api_registry|core/)|src/winui/)' }).Count -gt 0
    $requiredFull = $Scope -in 'unknown','public','infrastructure' -or $broad
    if ($requiredFull -and $suiteList -contains 'none') { throw 'Shared/public/build inputs cannot claim the no-host-tests exemption.' }
    return [pscustomobject]@{ schemaVersion=1; scope=$Scope; suites=$suiteList; tests=$names; inputs=$paths;
        reason=$Reason; editRevision=(Get-EditRevision $Entry); source='task-declared'; requiredFull=$requiredFull;
        escalationReason=$(if($requiredFull){'Unknown impact or shared/public infrastructure requires full automatic coverage.'}else{''}); inputIdentity=$null }
}
function Default-TaskPlan($Entry) {
    return [pscustomobject]@{ schemaVersion=1; scope='unknown'; suites=@('full'); tests=@(); inputs=@();
        reason='Legacy registration has no reviewed scope; conservative full automatic coverage.';
        editRevision=(Get-EditRevision $Entry); source='legacy-default'; requiredFull=$true; escalationReason='Undeclared impact'; inputIdentity=$null }
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
    $tasks=@(); $full=$false; $build=$false
    foreach ($entry in $Current.participants) {
        if ($entry.state -eq 'withdrawn') { continue }
        $plan=Get-Field $entry 'testPlan'
        if (-not $plan) { $plan=Default-TaskPlan $entry }
        if ($plan.editRevision -ne (Get-EditRevision $entry)) { throw "Plan is stale for $($entry.id); reopen/update plan before ready." }
        if ($plan.source -eq 'task-declared' -and $plan.inputIdentity -and (Get-TaskIdentity $plan).digest -ne $plan.inputIdentity.digest) { throw "Declared inputs changed after ready for $($entry.id); reopen and update plan." }
        $check=Get-Field $entry 'check'
        if($check -and $check.status -eq 'passed' -and (Get-TaskIdentity $plan).digest -ne $check.inputEnd.digest){throw "Check evidence is stale for $($entry.id); repeat check or reopen before edits."}
        if ($plan.requiredFull -or $plan.suites -contains 'full') { $full=$true }
        if ($plan.suites -notcontains 'none') { $build=$true }
        $tasks += [pscustomobject]@{ participant=$entry.id; editRevision=(Get-EditRevision $entry); requirement=$plan }
    }
    $names=@($tasks | ForEach-Object { $_.requirement.tests } | Sort-Object -Unique)
    $suites=@($tasks | ForEach-Object { $_.requirement.suites } | Where-Object { $_ -ne 'none' } | Sort-Object -Unique)
    return [pscustomobject]@{ schemaVersion=1; batchId=$Current.id; configuration='Release'; tasks=$tasks; tests=$names; suites=$suites; buildRequired=$build;
        mode=$(if($full){'full'}elseif($build){'selected'}else{'skipped'}); createdUtc=[DateTime]::UtcNow.ToString('o') }
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
        if ($labels -contains 'manual') { continue } # explicit names above remain
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
