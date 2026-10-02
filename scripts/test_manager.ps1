[CmdletBinding()]
param(
    [ValidateSet("full", "fast", "core", "label", "name", "list", "plan")]
    [string]$Mode = "full",
    [string]$Filter = "",
    [ValidatePattern("^[a-f0-9]{32}$")][string]$PlanBatch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location -LiteralPath $repositoryRoot

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
    & $FilePath @Arguments
    $commandExit = $LASTEXITCODE
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
        if ($executable -match "^SnowDesktop.+Tests\.exe$") {
            [IO.Path]::GetFileNameWithoutExtension($executable)
        }
    }

    [pscustomobject]@{
        Tests = $tests
        Targets = @($targets | Sort-Object -Unique)
    }
}

function Invoke-FilteredTests {
    param(
        [string[]]$CTestFilterArguments,
        [string]$BuildPreset = "",
        [string]$TestPreset = "tests"
    )

    $selection = Get-TestSelection -CTestFilterArguments $CTestFilterArguments -TestPreset $TestPreset
    if ($selection.Tests.Count -eq 0) {
        throw "The requested filter did not match any configured tests."
    }

    $needsHostRuntime = $selection.Targets -contains "SnowDesktopWidgetAuthorPreviewCliTests"
    if ($needsHostRuntime -or $BuildPreset -eq "tests") {
        Assert-HostRuntimeAvailable
    }

    if (-not [string]::IsNullOrWhiteSpace($BuildPreset)) {
        Write-Host "=== Building the aggregate target for $($selection.Tests.Count) selected test(s) ==="
        Invoke-Checked -FilePath "cmake" -Arguments @(
            "--build", "--preset", $BuildPreset)
    }
    elseif ($selection.Targets.Count -gt 0) {
        Write-Host "=== Building $($selection.Targets.Count) target(s) for $($selection.Tests.Count) selected test(s) ==="
        $buildArguments = @(
            "--build", "--preset", "tests", "--target"
        ) + $selection.Targets
        Invoke-Checked -FilePath "cmake" -Arguments $buildArguments
    }

    # The full aggregate already arranges its output in CMake.
    if ($needsHostRuntime -and $BuildPreset -ne "tests") {
        Invoke-Checked -FilePath "powershell.exe" -Arguments @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $PSScriptRoot "arrange_build_output.ps1"),
            "-BuildOutput", (Join-Path $repositoryRoot ".build\Release"),
            "-AllowMissingFirstPartyRuntime"
        )
    }

    Write-Host ""
    if($Mode -eq 'plan'){
        $artifacts=@()
        foreach($test in $selection.Tests){
            $cmd=@(Get-Field $test 'command' @());if($cmd.Count -eq 0){continue}
            $path=[IO.Path]::GetFullPath([string]$cmd[0])
            if($path.StartsWith($repositoryRoot+'\',[StringComparison]::OrdinalIgnoreCase) -and [IO.File]::Exists($path)){
                $artifacts += [pscustomobject]@{test=$test.name;executable=$path.Substring($repositoryRoot.Length+1);sha256=(Get-FileDigest $path);bytes=(Get-Item -LiteralPath $path).Length}
            }
        }
        Set-Field $coverage 'testExecutablesBeforeRun' $artifacts
        $coverage | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $coveragePath -Encoding UTF8
    }
    Write-Host "=== Running $($selection.Tests.Count) selected test(s) ==="
    Invoke-Checked -FilePath "ctest" -Arguments (
        @("--preset", $TestPreset) + $CTestFilterArguments) -ExpectedTests @($selection.Tests.name)
}

function Get-TestRunOptions {
    param([string]$Mode, [string]$Filter = "")
    switch ($Mode) {
        "full" { return @{ TestPreset = "tests"; BuildPreset = "tests"; CTestFilterArguments = @() } }
        "core" { return @{ TestPreset = "core-tests"; BuildPreset = "core-tests"; CTestFilterArguments = @() } }
        "fast" { return @{ TestPreset = "fast-tests"; BuildPreset = "fast-tests"; CTestFilterArguments = @() } }
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
    $releaseRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot ".build\Release"))
    $runtimePrefix = (Join-Path $releaseRoot "SnowDesktop.Runtime") + [IO.Path]::DirectorySeparatorChar
    foreach ($process in @(Get-Process -Name SnowDesktop, snowwidget,
            SnowDesktopWorkshopManager, SnowDesktopSteamBridge, SnowDesktopLauncher,
            SnowDesktopWallpaperInjector32 -ErrorAction SilentlyContinue)) {
        $path = $process.Path
        if ([string]::IsNullOrEmpty($path) -or
            $path.StartsWith($releaseRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            "$($process.ProcessName) (PID $($process.Id))"
        }
    }
    foreach ($process in @(Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        try {
            foreach ($module in $process.Modules) {
                if ($module.FileName.StartsWith($runtimePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                    $module.FileName.Equals((Join-Path $releaseRoot "SnowDesktopTaskbarHook.dll"),
                        [StringComparison]::OrdinalIgnoreCase)) {
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

function Test-IsolatedOutput {
    $releaseRoot = [IO.Path]::GetFullPath(".build\Release")
    $testRoot = Join-Path $releaseRoot "tests"
    $runtimeRoot = Join-Path $releaseRoot "SnowDesktop.Runtime"
    $tests = @(Get-ChildItem -LiteralPath $testRoot -File -Filter "SnowDesktop*Tests.exe" -ErrorAction Stop)
    $rootTests = @(Get-ChildItem -LiteralPath $releaseRoot -File -Filter "SnowDesktop*Tests.exe" -ErrorAction Stop)
    $rootDlls = @(Get-ChildItem -LiteralPath $releaseRoot -File -Filter "*.dll" -ErrorAction Stop)
    $runtimeDirectoryNames = @(Get-ChildItem -LiteralPath $runtimeRoot -Directory -ErrorAction Stop | ForEach-Object Name)
    $emptyRuntimeDirs = @(Get-ChildItem -LiteralPath $releaseRoot -Directory -ErrorAction Stop | Where-Object {
            $runtimeDirectoryNames -contains $_.Name -and
                [IO.Directory]::GetFileSystemEntries($_.FullName).Count -eq 0
        })
    if ($tests.Count -eq 0 -or $rootTests.Count -ne 0 -or
        $rootDlls.Count -ne 0 -or $emptyRuntimeDirs.Count -ne 0) {
        throw "Build or CTest output escaped its dedicated runtime/test directory."
    }
}

. (Join-Path $PSScriptRoot 'build_protocol.ps1')
$script:lastReportPath=$null
Write-Host "=== Configuring tests ==="
Invoke-Checked -FilePath "cmake" -Arguments @("--preset", "tests")

if ($Mode -eq "plan") {
    if (-not $PlanBatch) { throw 'Plan mode requires a frozen batch ID.' }
    $planRoot=Join-Path $repositoryRoot '.build\collaboration'
    $plan=[IO.File]::ReadAllText((Join-Path $planRoot ($PlanBatch+'.plan.json'))) | ConvertFrom-Json
    if ($plan.schemaVersion -ne 1 -or $plan.batchId -ne $PlanBatch -or $plan.configuration -ne 'Release') { throw 'Invalid frozen testing plan.' }
    $inventory=Get-TestSelection -TestPreset 'all-tests'
    $names=@(Resolve-PlanTests $plan $inventory.Tests)
    $coverage=[pscustomobject]@{schemaVersion=1;batchId=$PlanBatch;mode=$plan.mode;status='running';selected=$names;completed=@();tasks=@();report=$null;error=''}
    foreach($task in $plan.tasks) {
        $request=$task.requirement
        $single=[pscustomobject]@{tests=$request.tests;suites=$request.suites;mode=$(if($request.suites -contains 'none'){'skipped'}elseif($request.requiredFull -or $request.suites -contains 'full'){'full'}else{'selected'});tasks=@()}
        $coverage.tasks += [pscustomobject]@{participant=$task.participant;requested=@(Resolve-PlanTests $single $inventory.Tests);status=$(if($single.mode -eq 'skipped'){'not-required'}else{'not-run'});reason=$request.reason;failed=@()}
    }
    $coveragePath=Join-Path $planRoot ($PlanBatch+'.coverage.json')
    $coverage | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $coveragePath -Encoding UTF8
    try {
        # CTest uses the CMake regex engine, which does not support (?:...).
        $pattern='^('+ (($names | ForEach-Object {[regex]::Escape($_)}) -join '|') +')$'
        $arguments=@('-R',$pattern)
        if ($plan.mode -eq 'full') { Invoke-FilteredTests -CTestFilterArguments $arguments -BuildPreset 'tests' -TestPreset 'all-tests'; Test-IsolatedOutput }
        else { Invoke-FilteredTests -CTestFilterArguments $arguments -TestPreset 'all-tests' }
        $coverage.status='passed'
    } catch { $coverage.status='failed';$coverage.error=$_.Exception.Message; throw }
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
            }
        }
        foreach($artifact in @(Get-Field $coverage 'testExecutablesBeforeRun' @())){
            $path=Join-Path $repositoryRoot $artifact.executable
            if(-not [IO.File]::Exists($path) -or (Get-FileDigest $path) -ne $artifact.sha256){$coverage.status='invalidated';$coverage.error='Test executable changed during run.'}
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
        Write-Host "Manual diagnostic tests are excluded; select them explicitly with name or label."
    }
    Invoke-FilteredTests @options
    if ($Mode -eq "full") {
        Write-Host ""
        Write-Host "=== Verifying isolated test output ==="
        Test-IsolatedOutput
    }
}

Write-Host ""
Write-Host "=== Tests complete ($Mode) ==="
