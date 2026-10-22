[CmdletBinding()]
param(
    [ValidateSet("full", "fast", "core", "tools", "label", "name", "list", "plan", "verify")]
    [string]$Mode = "list",
    [string]$Filter = "",
    [ValidatePattern("^[a-f0-9]{32}$")][string]$PlanBatch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location -LiteralPath $repositoryRoot

# Direct PowerShell, batch, IDE and CI callers all enter the same private Job.
# A coordinator child already has live delegated authority and does not recurse.
if(-not [Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')) {
    $entryOptions=@{Action='tests';Mode=$Mode;Filter=$Filter}
    if($PlanBatch){$entryOptions.PlanBatch=$PlanBatch}
    & (Join-Path $PSScriptRoot 'build_entry.ps1') @entryOptions
    exit $LASTEXITCODE
}
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_telemetry.ps1')

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string[]]$ExpectedTests = @()
    )

    $reportPath = $null
    if ($FilePath -eq "ctest") {
        $reportRoot = Join-Path $repositoryRoot ".build\Testing"
        [void][IO.Directory]::CreateDirectory($reportRoot)
        $script:lastReportPath = $null
        $reportPath = Join-Path $reportRoot ("test-run-" + [Guid]::NewGuid().ToString("N") + ".xml")
        $script:lastReportPath = $reportPath
        $Arguments += @("--output-junit", $reportPath)
    }
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    $stageName=if($FilePath -eq 'ctest'){'test-execute'}elseif($FilePath -eq 'cmake' -and $Arguments -contains '--build'){'test-target-build'}elseif($FilePath -eq 'cmake'){'configure'}else{'arrange'}
    $commandExit=2
    $script:lastCommandExit=2
    Invoke-BuildStageCommand $repositoryRoot $stageName $FilePath $Arguments ([ref]$commandExit)
    $script:lastCommandExit=$commandExit
    $elapsed.Stop()
    Write-Host ("{0}: {1:N2}s, exit {2}" -f $FilePath, $elapsed.Elapsed.TotalSeconds, $commandExit)
    if ($reportPath) { Write-Host "CTest report: $reportPath" }
    if ($commandExit -ne 0) {
        throw "$FilePath exited with code $commandExit."
    }
    if ($reportPath) {
        Assert-CompleteTestReport -Report ([xml](Get-Content -LiteralPath $reportPath -Raw)) -ExpectedTests $ExpectedTests
    }
}

function Assert-CompleteTestReport {
    param([xml]$Report, [string[]]$ExpectedTests = @())
    $cases = @($Report.SelectNodes("//testcase"))
    if ($cases.Count -eq 0) { throw "CTest report contains no executed test cases." }
    $incomplete = @($Report.SelectNodes("//testcase[skipped or failure or error or @status='notrun' or @status='disabled']"))
    if ($incomplete.Count -gt 0) {
        throw ("CTest run is not fully verified (failed, skipped or not run): " +
            (($incomplete | ForEach-Object { $_.GetAttribute("name") }) -join ", "))
    }
    if ($ExpectedTests.Count -gt 0) {
        $actual = @($cases | ForEach-Object { $_.GetAttribute("name") })
        if ($actual.Count -ne $ExpectedTests.Count -or
            @(Compare-Object ($ExpectedTests | Sort-Object) ($actual | Sort-Object)).Count -gt 0) {
            throw "CTest report does not match the selected test inventory."
        }
    }
}

function Get-TestSelection {
    param([string[]]$CTestFilterArguments = @(), [string]$TestPreset = "tests")

    $arguments = @(
        "--preset", $TestPreset,
        "--show-only=json-v1"
    ) + $CTestFilterArguments
    $output = & ctest @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to query the configured CTest inventory."
    }

    $manifest = ($output -join [Environment]::NewLine) |
        ConvertFrom-Json
    $tests = @($manifest.tests)
    $targets = foreach ($test in $tests) {
        $commandProperty = $test.PSObject.Properties["command"]
        if ($null -ne $commandProperty -and $commandProperty.Value.Count -gt 0) {
            $executable = [IO.Path]::GetFileName([string]$commandProperty.Value[0])
        }
        else {
            # CTest omits command when the executable has not been built yet.
            # CMake still publishes its generated path in REQUIRED_FILES.
            $requiredExecutables = @($test.properties |
                Where-Object name -eq "REQUIRED_FILES" |
                ForEach-Object { $_.value } |
                Where-Object { [IO.Path]::GetFileName([string]$_) -match "^SnowDesktop.+Tests\.exe$" })
            if ($requiredExecutables.Count -ne 1) {
                throw "Cannot resolve the build target for test '$($test.name)' from CTest metadata."
            }
            $executable = [IO.Path]::GetFileName([string]$requiredExecutables[0])
        }
        if($executable -eq 'SnowDesktopTestRuntimeObserver.exe'){
            $observedExecutables=@($test.properties|Where-Object name -eq 'REQUIRED_FILES'|ForEach-Object value|Where-Object {[IO.Path]::GetFileName([string]$_) -match '^SnowDesktop.+Tests\.exe$'})
            if($observedExecutables.Count -ne 1){throw "Cannot resolve observed native test target: $($test.name)"}
            $executable=[IO.Path]::GetFileName([string]$observedExecutables[0])
        }
        if ($executable -match "^SnowDesktop.+Tests\.exe$") {
            [IO.Path]::GetFileNameWithoutExtension($executable)
        }
    }

    [pscustomobject]@{
        Tests = $tests
        Targets = @($targets | Sort-Object -Unique)
    }
}

function Get-TestExecutionContext {
    return Get-VerificationEnvironment $repositoryRoot
}

function Get-TestBuildRoots([string]$Root,[string[]]$Targets) {
    $original=@($Targets|Sort-Object -Unique)
    $fallback=[pscustomobject]@{targets=$original;omitted=@();reason='original-targets'}
    if($original.Count -lt 2 -or -not [Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process') -or
       -not (Get-Command Read-BuildStageEvents -ErrorAction SilentlyContinue)){return $fallback}
    try{
        $context=Get-ConfiguredBuildContext $Root @('--build','--preset','tests')
        if(-not $context -or $context.generator -notlike 'Visual Studio *'){return $fallback}
        $directory=[IO.Path]::GetFullPath($context.directory).TrimEnd('\','/')
        $pending=New-Object 'System.Collections.Generic.Queue[string]';$graph=@{};$selected=@{}
        foreach($target in $original){
            if($target -notmatch '^[A-Za-z0-9_]+$'){return $fallback}
            $path=Join-Path $directory ($target+'.vcxproj');$selected[$target]=$path;$pending.Enqueue($path)
        }
        while($pending.Count){
            $path=$pending.Dequeue();if($graph.ContainsKey($path)){continue}
            if($graph.Count -ge 2048 -or -not $path.StartsWith($directory+'\',[StringComparison]::OrdinalIgnoreCase) -or
               -not [IO.File]::Exists($path) -or (Get-Item -LiteralPath $path).Length -gt 8MB){return $fallback}
            $settings=New-Object Xml.XmlReaderSettings;$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
            $reader=[Xml.XmlReader]::Create($path,$settings)
            try{$document=New-Object Xml.XmlDocument;$document.XmlResolver=$null;$document.Load($reader)}finally{$reader.Dispose()}
            $edges=@()
            foreach($reference in $document.SelectNodes('//*[local-name()="ProjectReference"][@Include]')){
                $conditional=$false;$parent=$reference
                while($parent -is [Xml.XmlElement]){if($parent.HasAttribute('Condition')){$conditional=$true;break};$parent=$parent.ParentNode}
                if($conditional -or @($reference.SelectNodes('*[local-name()="BuildReference"]')|Where-Object InnerText -ne 'true').Count){continue}
                $include=$reference.GetAttribute('Include')
                $child=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($include)){$include}else{Join-Path ([IO.Path]::GetDirectoryName($path)) $include}))
                $edges+=@($child);$pending.Enqueue($child)
            }
            $graph[$path]=$edges
        }
        $reach=@{}
        foreach($target in $original){
            $seen=@{};$pending.Enqueue($selected[$target])
            while($pending.Count){$path=$pending.Dequeue();if($seen.ContainsKey($path)){continue};$seen[$path]=$true;foreach($child in $graph[$path]){$pending.Enqueue($child)}}
            $reach[$target]=$seen
        }
        $roots=@();$omitted=@()
        foreach($target in $original){
            $parents=@($original|Where-Object {$_ -ne $target -and $reach[$_].ContainsKey($selected[$target])})
            if(@($parents|Where-Object {$reach[$target].ContainsKey($selected[$_])}).Count){return $fallback}
            if($parents.Count){$omitted+=@($target)}else{$roots+=@($target)}
        }
        if(-not $roots.Count){return $fallback}
        return [pscustomobject]@{targets=$roots;omitted=$omitted;reason='generated-project-reference-roots'}
    }catch{$fallback.reason='project-reference-graph-unavailable';return $fallback}
}
function Get-TestConfirmedBuildTargets([string]$Root,[string[]]$Targets) {
    # A declared edge is only a scheduling hint. Confirm actual successful
    # Release/x64 Build targets in this immutable Run before omitting a call.
    try{
        $id=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
        if($id -notmatch '^[a-f0-9]{32}$'){return @()}
        $context=Get-ConfiguredBuildContext $Root @('--build','--preset','tests')
        $events=@(Read-BuildStageEvents (Get-EntryRunRoot $Root) $id|Where-Object {
            (Get-Field $_ 'kind') -eq 'project-build' -and (Get-Field $_ 'exitCode') -eq 0 -and
            (Get-Field $_ 'configuration') -ceq 'Release' -and (Get-Field $_ 'platform') -ceq 'x64'})
        $directories=@($context.directory);$candidate=Get-CandidateEntryContext $Root
        if($candidate){$directories+=@($candidate.output)}
        $built=@($events|ForEach-Object {Get-Field $_ 'projectFile'})
        return @($Targets|Where-Object {$target=$_;@($directories|Where-Object {(Join-Path $_ ($target+'.vcxproj')) -in $built}).Count -gt 0})
    }catch{return @()}
}

function Invoke-FilteredTests {
    param(
        [string[]]$CTestFilterArguments,
        [string]$BuildPreset = "",
        [string]$TestPreset = "tests",
        [switch]$ExecutionOnly, $ExistingSelection=$null, $ResourceBudget=$null
    )

    $selection = if($ExecutionOnly){$ExistingSelection}else{Get-TestSelection -CTestFilterArguments $CTestFilterArguments -TestPreset $TestPreset}
    if ($selection.Tests.Count -eq 0) {
        throw "The requested filter did not match any configured tests."
    }

    $blocked=@($selection.Tests|Where-Object {@($_.properties|Where-Object name -eq 'LABELS'|ForEach-Object {$_.value}) -contains 'environment-blocked'}|ForEach-Object {$_.name})
    if($blocked.Count){throw ('Environment blocked; required tests not executed: '+($blocked -join ', '))}
    $hostExecutable=[IO.Path]::GetFullPath((Join-Path $repositoryRoot '.build\Release\SnowDesktop.exe'))
    $needsHostRuntime = @($selection.Tests | Where-Object {
        $labels=@($_.properties | Where-Object name -eq 'LABELS' | ForEach-Object {$_.value})
        # Metadata works before the first build, when CTest omits command.
        # Existing configured inventories remain usable through host arguments.
        $paths=@(Get-Field $_ 'command' @())+@($_.properties | Where-Object name -eq 'REQUIRED_FILES' | ForEach-Object {$_.value})
        ($labels -contains 'host-runtime') -or @($paths | Where-Object {
            [string]$_ -and [IO.Path]::IsPathRooted([string]$_) -and
                [IO.Path]::GetFullPath([string]$_).Equals($hostExecutable,[StringComparison]::OrdinalIgnoreCase)
        }).Count -gt 0
    }).Count -gt 0
    if ($needsHostRuntime -or $BuildPreset -in "tests", "all-tests") {
        Assert-HostRuntimeAvailable
    }

    if ($ExecutionOnly) {
        Write-Host '=== Existing artifacts verified; configure, build and arrangement are omitted ==='
    }
    elseif (-not [string]::IsNullOrWhiteSpace($BuildPreset)) {
        Write-Host "=== Building the aggregate target for $($selection.Tests.Count) selected test(s) ==="
        Invoke-Checked -FilePath "cmake" -Arguments @(
            "--build", "--preset", $BuildPreset)
    }
    elseif ($selection.Targets.Count -gt 0) {
        $buildRoots=Get-TestBuildRoots $repositoryRoot $selection.Targets
        Write-Host "=== Building $($buildRoots.targets.Count) root target(s) for $($selection.Targets.Count) target(s) / $($selection.Tests.Count) selected test(s) ==="
        $buildArguments = @(
            "--build", "--preset", "tests", "--target"
        ) + $buildRoots.targets
        Invoke-Checked -FilePath "cmake" -Arguments $buildArguments
        if($buildRoots.omitted.Count){
            $confirmed=@(Get-TestConfirmedBuildTargets $repositoryRoot $buildRoots.omitted)
            $remaining=@($buildRoots.omitted|Where-Object {$_ -notin $confirmed})
            if($remaining.Count){
                # A project can disable reference builds. After explicit
                # dependency calls, its already-built parent may need relinking.
                # Restore the entire original order instead of only children.
                Write-Host "=== Restoring $($selection.Targets.Count) original target calls: $($remaining.Count) dependencies lack confirmed Build completion ==="
                Invoke-Checked -FilePath 'cmake' -Arguments (@('--build','--preset','tests','--target')+$selection.Targets)
            }
        }
    }

    # The full aggregate already arranges its output in CMake.
    if (-not $ExecutionOnly -and $needsHostRuntime -and $BuildPreset -notin "tests", "all-tests") {
        Invoke-Checked -FilePath (Get-BuildPowerShell) -Arguments @(
            "-NoProfile",
            "-File", (Join-Path $PSScriptRoot "arrange_build_output.ps1"),
            "-BuildOutput", (Join-Path $repositoryRoot ".build\Release"),
            "-AllowMissingFirstPartyRuntime"
        )
    }

    Write-Host ""
    if($Mode -eq 'plan'){
        # A first-build CTest inventory can omit command until its executable
        # exists. Re-query declarations after building; never guess its argv.
        $executed=Get-TestSelection -CTestFilterArguments $CTestFilterArguments -TestPreset $TestPreset
        if(@(Compare-Object @($selection.Tests.name|Sort-Object) @($executed.Tests.name|Sort-Object)).Count){throw 'Configured test selection changed before execution.'}
        $selection=$executed
        $artifacts=@()
        $definitions=@(Get-VerificationDefinitions $selection.Tests)
        foreach($test in $selection.Tests){
            $cmd=@(Get-Field $test 'command' @());if($cmd.Count -eq 0){continue}
            $path=[IO.Path]::GetFullPath([string]$cmd[0])
            if($path.StartsWith($repositoryRoot+'\',[StringComparison]::OrdinalIgnoreCase) -and [IO.File]::Exists($path)){
                $artifacts += [pscustomobject]@{test=$test.name;executable=$path.Substring($repositoryRoot.Length+1);sha256=(Get-FileDigest $path);bytes=(Get-Item -LiteralPath $path).Length}
            }
        }
        Set-Field $coverage 'testExecutablesBeforeRun' $artifacts
        Set-Field $coverage 'testDefinitionsBeforeRun' $definitions
        Set-Field $coverage 'testRuntimeFilesBeforeRun' @(Get-VerificationRuntimeFiles $repositoryRoot $selection.Tests)
        Set-Field $coverage 'executionContext' (Get-TestExecutionContext)
    }
    if($Mode -in 'plan','verify'){
        # Verification already bound invocation/binaries at admission. Observe
        # this execution's inputs instead of borrowing the baseline's reads.
        $dependencyScript=Join-Path $PSScriptRoot 'build_test_dependencies.ps1'
        if([IO.File]::Exists($dependencyScript)){
            . $dependencyScript
            Set-Field $coverage 'dependencyManifest' (Save-TestDependencyManifest $repositoryRoot $selection.Tests)
        }else{Set-Field $coverage 'dependencyManifest' ([pscustomobject]@{status='unavailable';reason='Dependency observer is absent; result reuse remains disabled.'})}
        $coverage | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $coveragePath -Encoding UTF8
    }
    Write-Host "=== Running $($selection.Tests.Count) selected test(s) ==="
    $testArguments=if($ExecutionOnly){@('--preset','all-tests','--test-dir',(Join-Path $repositoryRoot '.build'),'-C','Release','--no-tests=error','--output-on-failure')}else{@('--preset',$TestPreset)}
    $budget=$ResourceBudget
    if($budget){
        foreach($test in $selection.Tests){
            $declared=@($test.properties|Where-Object name -eq 'PROCESSORS')
            $slots=if($declared.Count){[int]$declared[0].value}else{1}
            if($slots -lt 1 -or $slots -gt $budget.cpuSlots){throw ('Weighted CPU budget is below or incompatible with the declared CTest PROCESSORS for '+$test.name+'; no test execution started.')}
            $shared=@($test.properties|Where-Object {$_.name -in 'RESOURCE_LOCK','RESOURCE_GROUPS','RUN_SERIAL' -and $_.value})
            if($shared.Count -and $budget.compatibilityMode -ne 'exclusive'){throw 'Shared test resources require exclusive project admission; no test execution started.'}
        }
        $testArguments+=@('--parallel',[string]$budget.cpuSlots)
    }
    $candidate=Get-CandidateEntryContext $repositoryRoot
    if($candidate){
        . (Join-Path $PSScriptRoot 'build_candidate.ps1')
        . (Join-Path $PSScriptRoot 'build_candidate_execution.ps1')
        # Query commands after the first build; CTest may omit them beforehand.
        $executed=Get-TestSelection -CTestFilterArguments $CTestFilterArguments -TestPreset $(if($ExecutionOnly){'all-tests'}else{$TestPreset})
        if($executed.Tests.Count -ne $selection.Tests.Count -or @(Compare-Object @($selection.Tests.name|Sort-Object) @($executed.Tests.name|Sort-Object)).Count){throw 'Candidate test selection changed before execution.'}
        $selection=$executed
    }
    $moduleDirectory=Join-Path $repositoryRoot ('.build/Testing/runtime-modules-'+[Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($moduleDirectory)
    $previousModuleRoot=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT','Process')
    [Environment]::SetEnvironmentVariable('SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT',$moduleDirectory,'Process')
    $script:lastReportPath='';$script:lastCommandExit=2;$testCode=2
    try {
        Invoke-Checked -FilePath "ctest" -Arguments ($testArguments + $CTestFilterArguments) -ExpectedTests @($selection.Tests.name)
        $testCode=0
    }finally{
        [Environment]::SetEnvironmentVariable('SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT',$previousModuleRoot,'Process')
        if($Mode -in 'plan','verify'){
            $runtimeCheck=if(Get-Command Compare-TestRuntimeInputs -ErrorAction SilentlyContinue){Compare-TestRuntimeInputs $repositoryRoot $selection.Tests (Get-Field $coverage 'dependencyManifest')}else{[pscustomobject]@{status='unavailable';tests=@();reason='Runtime input observer is unavailable.';reuseAllowed=$false}}
            Set-Field $coverage 'runtimeInputCheck' $runtimeCheck
            $moduleScript=Join-Path $PSScriptRoot 'build_test_runtime_modules.ps1'
            $moduleCheck=if([IO.File]::Exists($moduleScript)){. $moduleScript;Get-TestRuntimeModuleObservations $repositoryRoot $selection.Tests $moduleDirectory}else{[pscustomobject]@{status='unavailable';tests=@();reason='Runtime module observer is unavailable.'}}
            Set-Field $coverage 'runtimeModules' $moduleCheck
            $coverage|ConvertTo-Json -Depth 15|Set-Content -LiteralPath $coveragePath -Encoding UTF8
        }
        if($candidate){Write-CandidateTestCoverage $repositoryRoot $selection $script:lastReportPath $(if($testCode -eq 0){0}elseif($script:lastCommandExit -ne 0){$script:lastCommandExit}else{2})}
    }
}

function Get-TestRunOptions {
    param([string]$Mode, [string]$Filter = "")
    switch ($Mode) {
        "full" { return @{ TestPreset = "tests"; BuildPreset = "tests"; CTestFilterArguments = @() } }
        "core" { return @{ TestPreset = "core-tests"; BuildPreset = "core-tests"; CTestFilterArguments = @() } }
        "fast" { return @{ TestPreset = "fast-tests"; BuildPreset = "fast-tests"; CTestFilterArguments = @() } }
        "tools" { return @{ TestPreset = "tools-tests"; BuildPreset = "tools-tests"; CTestFilterArguments = @() } }
        { $_ -in "name", "label" } {
            if ([string]::IsNullOrWhiteSpace($Filter)) {
                throw "$Mode mode requires a non-empty regular expression."
            }
            $flag = if ($Mode -eq "name") { "-R" } else { "-L" }
            # Explicit selection can opt into manual diagnostics. The same
            # preset must be used to query inventory and execute that selection.
            return @{ TestPreset = "all-tests"; CTestFilterArguments = @($flag, $Filter) }
        }
        default { throw "Unknown execution mode: $Mode" }
    }
}

function Get-HostRuntimeLocks {
    $releaseRoots = @(Get-BuildOutputDirectories $repositoryRoot 'Release')
    $runtimePrefixes = @($releaseRoots|ForEach-Object {(Join-Path $_ 'SnowDesktop.Runtime')+[IO.Path]::DirectorySeparatorChar})
    $hooks = @($releaseRoots|ForEach-Object {Join-Path $_ 'SnowDesktopTaskbarHook.dll'})
    foreach ($process in @(Get-Process -Name SnowDesktop, snowwidget,
            SnowDesktopWorkshopManager, SnowDesktopSteamBridge, SnowDesktopLauncher,
            SnowDesktopWallpaperInjector32 -ErrorAction SilentlyContinue)) {
        $path = $process.Path
        if ([string]::IsNullOrEmpty($path) -or
            @($releaseRoots|Where-Object {$path.StartsWith($_+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}).Count) {
            "$($process.ProcessName) (PID $($process.Id))"
        }
    }
    foreach ($process in @(Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        try {
            foreach ($module in $process.Modules) {
                if (@($runtimePrefixes|Where-Object {$module.FileName.StartsWith($_,[StringComparison]::OrdinalIgnoreCase)}).Count -or $hooks -contains $module.FileName) {
                    "Explorer (PID $($process.Id)): $($module.FileName)"
                }
            }
        }
        catch { throw "Cannot inspect Explorer runtime ownership before building: $_" }
    }
}

function Assert-HostRuntimeAvailable {
    $locks = @(Get-HostRuntimeLocks)
    if ($locks.Count -gt 0) {
        throw ("Host runtime is in use; no build or output arrangement was started. Close the owning application first; " +
            "if a Shell reload is intended, use scripts/build.bat --reload-shell. Owners: " + ($locks -join "; "))
    }
}

. (Join-Path $PSScriptRoot 'test_output.ps1')

. (Join-Path $PSScriptRoot 'build_entry.ps1')
$entryLease=$null
try {
    try {$entryLease=Enter-BuildEntry $repositoryRoot}
    catch {[Console]::Error.WriteLine($_.Exception.Message);exit 2}
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
. (Join-Path $PSScriptRoot 'build_verification.ps1')
$script:lastReportPath=$null
if($Mode -eq 'verify'){
    $runId=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
    if($runId -notmatch '^[a-f0-9]{32}$'){throw 'Verification must execute inside its admitted durable Run.'}
    $originRoot=Get-EntryRunRoot $repositoryRoot
    $runRoot=Join-Path $originRoot '.build/collaboration/runs'
    $request=Read-EntryJson (Join-Path $runRoot ($runId+'.request.json'))
    $coveragePath=Join-Path $runRoot ($runId+'.verification.json')
    $coverage=[pscustomobject]@{schemaVersion=1;runId=$runId;baseline=$PlanBatch;mode='verify';status='not-run';selected=@();completed=@();tasks=@();report=$null;error='';noBuild=$true}
    $code=5;$admitted=$false
    try {
        if(-not $request -or $request.id -ne $runId -or $request.repositoryRoot -ne $originRoot -or $request.mode -ne 'verify' -or $request.planBatch -ne $PlanBatch -or $request.filter -ne $Filter){throw 'Verification Run request identity mismatch.'}
        $verificationBudget=Get-EntryResourceBudget $repositoryRoot
        $snapshot=Get-VerificationSnapshot $repositoryRoot $PlanBatch $Filter $verificationBudget
        Set-Field $coverage 'observedExecutionContext' $snapshot.executionContext
        Set-Field $coverage 'observedReviewContext' (Get-Field $snapshot.binding 'reviewedReuse')
        if($snapshot.key -ne $request.verification.key){
            $before=Get-Field $request.verification.executionContext 'environmentFields'
            $after=$snapshot.executionContext.environmentFields
            $keys=@(@($before.PSObject.Properties.Name)+@($after.Keys)|Sort-Object -Unique)
            $changed=@($keys|Where-Object {(Get-Field $before $_) -ne $after[$_]})
            throw ('Verification inputs or execution environment changed while queued. Changed environment fields (values withheld): '+($changed -join ', '))
        }
        $admitted=$true;$code=1
        $coverage.selected=@($snapshot.binding.names)
        $coverage.tasks=@([pscustomobject]@{participant=$request.task;requested=$coverage.selected})
        Set-Field $coverage 'testDefinitionsBeforeRun' $snapshot.binding.definitions
        Set-Field $coverage 'testExecutablesBeforeRun' $snapshot.binding.artifacts
        Set-Field $coverage 'testRuntimeFilesBeforeRun' $snapshot.binding.runtimeFiles
        Set-Field $coverage 'executionContext' $snapshot.executionContext
        $coverage.status='running'
        $coverage|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $coveragePath -Encoding UTF8
        Invoke-FilteredTests -CTestFilterArguments @('-R',$snapshot.pattern) -ExecutionOnly -ExistingSelection ([pscustomobject]@{Tests=$snapshot.tests;Targets=@()}) -ResourceBudget $verificationBudget
        $coverage.status='passed';$code=0
    } catch {$coverage.status=if($admitted){'failed'}else{'invalidated'};$coverage.error=$_.Exception.Message}
    finally {
        $coverage.report=$script:lastReportPath
        if($script:lastReportPath -and [IO.File]::Exists($script:lastReportPath)){
            $report=[xml][IO.File]::ReadAllText($script:lastReportPath)
            $coverage.completed=@($report.SelectNodes('//testcase')|ForEach-Object {
                [pscustomobject]@{name=$_.GetAttribute('name');status=$(if($_.SelectSingleNode('failure|error')){'failed'}elseif($_.SelectSingleNode('skipped') -or $_.GetAttribute('status') -in 'notrun','disabled'){'not-run'}else{'passed'});seconds=$_.GetAttribute('time')}
            })
        }
        if($admitted){
            try {$after=Get-VerificationSnapshot $repositoryRoot $PlanBatch $Filter $verificationBudget;if($after.key -ne $snapshot.key){throw 'Verification context changed during execution.'}}
            catch {$coverage.status='invalidated';$coverage.error=$_.Exception.Message;$code=5}
            $changedRuntime=@(Get-Field (Get-Field $coverage 'runtimeInputCheck') 'tests' @()|Where-Object status -eq 'invalidated')
            if($changedRuntime.Count){
                $coverage.status='invalidated';$code=5
                $coverage.error='Reviewed runtime file inputs changed during verification: '+(($changedRuntime|ForEach-Object test) -join ', ')
            }
        }
        $coverage|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $coveragePath -Encoding UTF8
    }
    if($coverage.error){[Console]::Error.WriteLine($coverage.error)}
    exit $code
}
if($Mode -eq 'plan') {
    if(-not $PlanBatch){throw 'Plan mode requires a frozen batch ID.'}
    $initialPlanRoot=Join-Path $repositoryRoot '.build/collaboration'
    $initialPlan=Read-EntryJson (Join-Path $initialPlanRoot ($PlanBatch+'.plan.json'))
    if(-not $initialPlan -or $initialPlan.schemaVersion -ne 1 -or $initialPlan.batchId -ne $PlanBatch -or $initialPlan.configuration -ne 'Release'){throw 'Invalid frozen testing plan.'}
    # Publish a new not-run receipt before configure/selection. A failure there
    # cannot leave an earlier passing coverage record masquerading as this run.
    [pscustomobject]@{schemaVersion=1;batchId=$PlanBatch;mode=$initialPlan.mode;status='not-run';selectionStatus='pending';
        selected=@();completed=@();report=$null;error='Configuration/selection has not completed.';
        postChecks=$(if($initialPlan.mode -eq 'full'){@{outputIsolation=(New-OutputIsolationCheck)}}else{@{}});
        tasks=@($initialPlan.tasks|ForEach-Object {[pscustomobject]@{participant=$_.participant;requested=@($_.requirement.tests);suites=@($_.requirement.suites);status='not-run';reason=$_.requirement.reason;failed=@();requiresOutputIsolation=($_.requirement.requiredFull -or $_.requirement.suites -contains 'full')}})} |
        ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $initialPlanRoot ($PlanBatch+'.coverage.json')) -Encoding UTF8
}
Write-Host "=== Configuring tests ==="
Invoke-Checked -FilePath "cmake" -Arguments @("--preset", "tests")

if ($Mode -eq "plan") {
    if (-not $PlanBatch) { throw 'Plan mode requires a frozen batch ID.' }
    $planRoot=Join-Path $repositoryRoot '.build\collaboration'
    $plan=[IO.File]::ReadAllText((Join-Path $planRoot ($PlanBatch+'.plan.json'))) | ConvertFrom-Json
    if ($plan.schemaVersion -ne 1 -or $plan.batchId -ne $PlanBatch -or $plan.configuration -ne 'Release') { throw 'Invalid frozen testing plan.' }
    $inventory=Get-TestSelection -TestPreset 'all-tests'
    $names=@(Resolve-PlanTests $plan $inventory.Tests)
    $coverage=[pscustomobject]@{schemaVersion=1;batchId=$PlanBatch;mode=$plan.mode;status='running';selected=$names;completed=@();tasks=@();report=$null;error='';postChecks=$(if($plan.mode -eq 'full'){@{outputIsolation=(New-OutputIsolationCheck)}}else{@{}})}
    foreach($task in $plan.tasks) {
        $request=$task.requirement
        $single=[pscustomobject]@{tests=$request.tests;suites=$request.suites;mode=$(if($request.suites -contains 'none'){'skipped'}elseif($request.requiredFull -or $request.suites -contains 'full'){'full'}else{'selected'});tasks=@()}
        $coverage.tasks += [pscustomobject]@{participant=$task.participant;requested=@(Resolve-PlanTests $single $inventory.Tests);status=$(if($single.mode -eq 'skipped'){'not-required'}else{'not-run'});reason=$request.reason;failed=@();requiresOutputIsolation=($single.mode -eq 'full')}
    }
    $coveragePath=Join-Path $planRoot ($PlanBatch+'.coverage.json')
    $coverage | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $coveragePath -Encoding UTF8
    try {
        $blocked=@($inventory.Tests|Where-Object {$names -contains $_.name -and @($_.properties|Where-Object name -eq 'LABELS'|ForEach-Object {$_.value}) -contains 'environment-blocked'}|ForEach-Object {$_.name})
        if($blocked.Count){
            $coverage.status='environment-blocked'
            Set-Field $coverage 'blocked' $blocked
            foreach($task in $coverage.tasks){if(@($task.requested|Where-Object {$_ -in $blocked}).Count){$task.status='environment-blocked'}}
            throw ('Required tests not executed: '+($blocked -join ', ')+'. Install/repair Python separately; no automatic installation.')
        }
        # CTest uses the CMake regex engine, which does not support (?:...).
        $pattern='^('+ (($names | ForEach-Object {[regex]::Escape($_)}) -join '|') +')$'
        $arguments=@('-R',$pattern)
        if ($plan.mode -eq 'full') {
            $includesTools=@($inventory.Tests | Where-Object {$names -contains $_.name -and @($_.properties | Where-Object name -eq 'LABELS' | ForEach-Object {$_.value}) -contains 'tools'}).Count -gt 0
            $fullBuildPreset=if($includesTools){'all-tests'}else{'tests'}
            Invoke-FilteredTests -CTestFilterArguments $arguments -BuildPreset $fullBuildPreset -TestPreset 'all-tests' -ResourceBudget (Get-EntryResourceBudget $repositoryRoot)
            Invoke-OutputIsolationCheck $repositoryRoot $coverage.postChecks.outputIsolation
        }
        else { Invoke-FilteredTests -CTestFilterArguments $arguments -TestPreset 'all-tests' -ResourceBudget (Get-EntryResourceBudget $repositoryRoot) }
        $coverage.status='passed'
    } catch { if($coverage.status -ne 'environment-blocked'){$coverage.status='failed'};$coverage.error=$_.Exception.Message; throw }
    finally {
        $coverage.report=$script:lastReportPath
        if ($script:lastReportPath -and [IO.File]::Exists($script:lastReportPath)) {
            $report=[xml][IO.File]::ReadAllText($script:lastReportPath)
            $coverage.completed=@($report.SelectNodes('//testcase') | ForEach-Object {
                [pscustomobject]@{name=$_.GetAttribute('name');status=$(if($_.SelectSingleNode('failure|error')){'failed'}elseif($_.SelectSingleNode('skipped') -or $_.GetAttribute('status') -in 'notrun','disabled'){'not-run'}else{'passed'});seconds=$_.GetAttribute('time')}
            })
            foreach($task in $coverage.tasks) {
                if($task.status -eq 'not-required'){continue}
                $actual=@($coverage.completed | Where-Object {$task.requested -contains $_.name})
                $task.failed=@($actual | Where-Object status -eq 'failed' | ForEach-Object {$_.name})
                $task.status=if($task.failed.Count){'failed'}elseif($actual.Count -eq $task.requested.Count -and @($actual | Where-Object status -ne 'passed').Count -eq 0){'passed'}else{'not-run'}
                if($task.requiresOutputIsolation -and $task.status -eq 'passed' -and $coverage.postChecks.outputIsolation.status -ne 'passed'){
                    $task.status=if($coverage.postChecks.outputIsolation.status -eq 'failed'){'failed'}else{'not-run'}
                }
            }
        }
        foreach($artifact in @(Get-Field $coverage 'testExecutablesBeforeRun' @())){
            $path=Join-Path $repositoryRoot $artifact.executable
            if(-not [IO.File]::Exists($path) -or (Get-FileDigest $path) -ne $artifact.sha256){$coverage.status='invalidated';$coverage.error='Test executable changed during run.'}
        }
        foreach($artifact in @(Get-Field $coverage 'testRuntimeFilesBeforeRun' @())){
            $path=if([IO.Path]::IsPathRooted($artifact.file)){$artifact.file}else{Join-Path $repositoryRoot $artifact.file}
            if(-not [IO.File]::Exists($path) -or (Get-FileDigest $path) -ne $artifact.sha256){$coverage.status='invalidated';$coverage.error='Explicit test runtime file changed during run.'}
        }
        $coverage | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $coveragePath -Encoding UTF8
        if($coverage.status -eq 'invalidated'){throw $coverage.error}
    }
}
elseif ($Mode -eq "list") {
        $selection = Get-TestSelection -TestPreset "all-tests"
        foreach ($test in $selection.Tests) {
            $labelProperty = @($test.properties |
                Where-Object name -eq "LABELS")
            $labels = if ($labelProperty.Count -gt 0) {
                @($labelProperty[0].value) -join ","
            }
            else {
                "-"
            }
            "{0,-42} {1}" -f $test.name, $labels
        }
        Write-Host ""
        Write-Host "$($selection.Tests.Count) test(s) configured."
}
else {
    $options = Get-TestRunOptions -Mode $Mode -Filter $Filter
    if ($Mode -in "full", "core", "fast") {
        Write-Host "Build tool regressions and manual diagnostics are excluded; select tools, name or label explicitly."
    }
    Invoke-FilteredTests @options -ResourceBudget (Get-EntryResourceBudget $repositoryRoot)
    if ($Mode -eq "full") {
        Write-Host ""
        Write-Host "=== Verifying isolated test output ==="
        Test-IsolatedOutput $repositoryRoot
    }
}

Write-Host ""
Write-Host "=== Tests complete ($Mode) ==="

} finally {Exit-BuildEntry $entryLease}
