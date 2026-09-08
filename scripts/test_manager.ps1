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
        [string[]]$Arguments = @()
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE."
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
        if (-not $test.command -or $test.command.Count -eq 0) {
            continue
        }
        $executable = [IO.Path]::GetFileName([string]$test.command[0])
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

    if ($selection.Targets -contains "SnowDesktopWidgetAuthorPreviewCliTests") {
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
        @("--preset", "tests") + $CTestFilterArguments)
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
        Write-Host ""
        Write-Host "=== Building all test targets ==="
        Invoke-Checked -FilePath "cmake" -Arguments @(
            "--build", "--preset", "tests")
        Write-Host ""
        Write-Host "=== Running full CTest suite ==="
        Invoke-Checked -FilePath "ctest" -Arguments @("--preset", "tests")
        Write-Host ""
        Write-Host "=== Verifying isolated test output ==="
        Test-IsolatedOutput
    }
    "core" {
        Write-Host ""
        Write-Host "=== Building core test targets ==="
        Invoke-Checked -FilePath "cmake" -Arguments @(
            "--build", "--preset", "core-tests")
        Write-Host ""
        Write-Host "=== Running core CTest suite ==="
        Invoke-Checked -FilePath "ctest" -Arguments @(
            "--preset", "core-tests")
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
