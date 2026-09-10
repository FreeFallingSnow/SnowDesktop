[CmdletBinding()]
param(
    [ValidateSet("full", "fast", "core", "label", "name", "list")]
    [string]$Mode = "full",
    [string]$Filter = ""
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
        $reportPath = Join-Path $reportRoot ("test-run-" + [Guid]::NewGuid().ToString("N") + ".xml")
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
    param([string[]]$CTestFilterArguments = @())

    $arguments = @(
        "--test-dir", ".build",
        "-C", "Release",
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
        [string]$BuildPreset = ""
    )

    $selection = Get-TestSelection -CTestFilterArguments $CTestFilterArguments
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
    Write-Host "=== Running $($selection.Tests.Count) selected test(s) ==="
    Invoke-Checked -FilePath "ctest" -Arguments (
        @("--preset", "tests") + $CTestFilterArguments) -ExpectedTests @($selection.Tests.name)
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

Write-Host "=== Configuring tests ==="
Invoke-Checked -FilePath "cmake" -Arguments @("--preset", "tests")

switch ($Mode) {
    "full" {
        Invoke-FilteredTests -CTestFilterArguments @() -BuildPreset "tests"
        Write-Host ""
        Write-Host "=== Verifying isolated test output ==="
        Test-IsolatedOutput
    }
    "core" {
        Invoke-FilteredTests -CTestFilterArguments @("-L", "^core$") -BuildPreset "core-tests"
    }
    "fast" {
        Invoke-FilteredTests -CTestFilterArguments @(
            "-LE", "^integration$") -BuildPreset "fast-tests"
    }
    "label" {
        if ([string]::IsNullOrWhiteSpace($Filter)) {
            throw "label mode requires a non-empty label regular expression."
        }
        Invoke-FilteredTests -CTestFilterArguments @("-L", $Filter)
    }
    "name" {
        if ([string]::IsNullOrWhiteSpace($Filter)) {
            throw "name mode requires a non-empty test-name regular expression."
        }
        Invoke-FilteredTests -CTestFilterArguments @("-R", $Filter)
    }
    "list" {
        $selection = Get-TestSelection
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
}

Write-Host ""
Write-Host "=== Tests complete ($Mode) ==="
