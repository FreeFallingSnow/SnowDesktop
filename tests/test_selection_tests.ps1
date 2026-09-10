Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Execute the actual selection functions without running test_manager's entry
# point. Only the CTest query and build/test subprocess boundary are replaced.
$managerPath = Join-Path $PSScriptRoot "../scripts/test_manager.ps1"
$parseTokens = $null
$parseErrors = $null
$manager = [System.Management.Automation.Language.Parser]::ParseFile(
    $managerPath, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "test manager must parse" }
foreach ($functionName in @("Get-TestSelection", "Invoke-FilteredTests")) {
    $definition = $manager.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $functionName
    }, $false)
    if ($null -eq $definition) { throw "missing selection function: $functionName" }
    . ([scriptblock]::Create($definition.Extent.Text))
}

$script:fixture = ""
$script:queryArguments = @()
$script:invocations = @()
function ctest {
    $script:queryArguments = @($args)
    $global:LASTEXITCODE = 0
    $script:fixture
}
function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments = @())
    $script:invocations += [pscustomobject]@{ FilePath = $FilePath; Arguments = $Arguments }
}
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Set-Inventory([object[]]$Tests) {
    $script:fixture = @{ tests = @($Tests) } | ConvertTo-Json -Depth 8 -Compress
    $script:invocations = @()
}
function Expect-Failure([scriptblock]$Action, [string]$MessagePart) {
    $caught = $null
    try { & $Action } catch { $caught = $_.Exception.Message }
    Check ($null -ne $caught -and $caught.Contains($MessagePart)) "expected failure: $MessagePart"
}

$testBinary = "C:/isolated-tests/SnowDesktopSelectionFixtureTests.exe"
# A real CTest cold-build manifest has no command field at all.
$unbuilt = @{ name = "unbuilt"; properties = @(
    @{ name = "REQUIRED_FILES"; value = @($testBinary) }) }
Set-Inventory @($unbuilt)
Invoke-FilteredTests -CTestFilterArguments @("-R", "^unbuilt$")
Check ($script:invocations.Count -eq 2) "cold selection must build then run once"
Check ($script:invocations[0].FilePath -eq "cmake" -and
    ($script:invocations[0].Arguments -join " ") -eq
        "--build --preset tests --target SnowDesktopSelectionFixtureTests") "cold selection must build only the declared target"
Check ($script:invocations[1].FilePath -eq "ctest" -and
    ($script:invocations[1].Arguments -join " ") -eq
        "--preset tests -R ^unbuilt$") "cold selection must retain the requested test filter"

$built = @{ name = "built"; command = @($testBinary, "case-a"); properties = @() }
$alias = @{ name = "alias"; command = @($testBinary, "case-b"); properties = @() }
Set-Inventory @($built, $alias)
$selection = Get-TestSelection -CTestFilterArguments @("-L", "widget")
Check ($selection.Tests.Count -eq 2 -and $selection.Targets.Count -eq 1 -and
    $selection.Targets[0] -eq "SnowDesktopSelectionFixtureTests") "aliases must share one build target without losing test entries"
Check (($script:queryArguments -join " ").EndsWith("-L widget")) "labels must reach CTest selection"

Set-Inventory @(@{ name = "script"; command = @("powershell.exe", "test.ps1"); properties = @() })
Invoke-FilteredTests -CTestFilterArguments @("-R", "script")
Check ($script:invocations.Count -eq 1 -and $script:invocations[0].FilePath -eq "ctest") "script tests must run without inventing a native build target"

Set-Inventory @()
Expect-Failure { Invoke-FilteredTests -CTestFilterArguments @("-R", "missing") } "did not match"
Check ($script:invocations.Count -eq 0) "zero matches must neither build nor pass a test run"

Set-Inventory @(@{ name = "missing-metadata"; properties = @() })
Expect-Failure { Get-TestSelection } "Cannot resolve the build target"
Set-Inventory @(@{ name = "ambiguous-metadata"; properties = @(
    @{ name = "REQUIRED_FILES"; value = @($testBinary, "C:/isolated-tests/SnowDesktopOtherTests.exe") }) })
Expect-Failure { Get-TestSelection } "Cannot resolve the build target"

Write-Output "Test selection regressions passed (cold build, aliases, scripts, filters and invalid metadata)."
