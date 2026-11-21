<#
.SYNOPSIS
Runs the isolated SnowDesktop animation fixture and reports host frame pacing.
.EXAMPLE
./scripts/test_animations.ps1 -PlanOnly -Scenarios dock-hover,popup-resize,window-genie
.EXAMPLE
./scripts/test_animations.ps1 -Scenarios popup-fan,navigation-dock -Repeats 4 -Offline
#>
[CmdletBinding(DefaultParameterSetName = 'Select')]
param(
    [string]$ExecutablePath,
    [string]$OutputDirectory,
    [switch]$PlanOnly,
    [Parameter(ParameterSetName = 'Select')][string[]]$Scenarios = @(),
    [Parameter(ParameterSetName = 'Select')][ValidateRange(1, 8)][int]$Repeats = 3,
    [Parameter(Mandatory, ParameterSetName = 'Replay')][string]$PlanPath,
    [switch]$Offline
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
if (!$ExecutablePath) { $ExecutablePath = Join-Path $repository '.build/Release/SnowDesktop.exe' }
if (!$OutputDirectory) {
    $directory = 'animation-test-' + [Guid]::NewGuid().ToString('N')
    $OutputDirectory = Join-Path $repository ".codex-probes/$directory"
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Choose a new output directory. $output already exists." }
$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
if ($PlanPath) {
    $plan = Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
    if ($plan.schemaVersion -ne 1 -or $plan.fixtureVersion -ne 1) {
        throw 'This script cannot replay that fixture plan version.'
    }
    $Repeats = $plan.repeats
    $Scenarios = @($plan.scenarios.id)
    if ($Repeats -lt 1 -or $Repeats -gt 8 -or !$Scenarios.Count) { throw 'Invalid test plan.' }
}

$sha256 = (Get-FileHash -LiteralPath $ExecutablePath).Hash
$start = [Diagnostics.ProcessStartInfo]::new($ExecutablePath)
$start.UseShellExecute = $false
$mode = if ($PlanOnly) { 'plan' } else { 'run' }
foreach ($argument in @('--animation-test', $mode, $output, [string]$Repeats) + $Scenarios) {
    $start.ArgumentList.Add($argument)
}
$process = [Diagnostics.Process]::Start($start)
try {
    if (!$process.WaitForExit(630000)) {
        $process.Kill()
        throw 'The animation fixture did not finish within the run time limit.'
    }
    $status = $process.ExitCode
    $fixturePid = $process.Id
} finally {
    $process.Dispose()
}

$planFile = Join-Path $output 'plan.json'
if (!(Test-Path -LiteralPath $planFile)) { throw "The fixture could not generate a plan. Exit code $status." }
if ($PlanOnly) {
    if ($status) { throw "Plan generation failed with exit code $status." }
    Get-Content -LiteralPath $planFile -Raw
    return
}

$actions = Get-Content -LiteralPath (Join-Path $output 'actions.json') -Raw | ConvertFrom-Json
if ($actions.processId -ne $fixturePid) { throw 'The recorded fixture process does not match the launched process.' }
if ($actions.trace -ne 'completed' -or !$actions.actions.Count) {
    throw "Fixture did not complete the trace or execute cases. $($actions.error) Evidence: $output"
}
$manifest = Join-Path $output 'session.json'
$analysis = & (Join-Path $PSScriptRoot 'profile_analyze.ps1') -ManifestPath $manifest -Offline:$Offline | ConvertFrom-Json
$summary = Get-Content -LiteralPath $analysis.summary -Raw | ConvertFrom-Json
if (!$summary.quality.valid -or $summary.processId -ne $fixturePid -or $summary.executableSha256 -ne $sha256) {
    throw 'The analyzed process, image hash, or trace quality did not match the fixture.'
}

$windows = foreach ($action in $actions.actions) {
    $scenario = $action.scenario.Replace("'", "''")
    $edge = $action.edge.Replace("'", "''")
    $surface = $action.surface.Replace("'", "''")
    "('$scenario',$([int]$action.iteration),'$edge','$surface',$([long]$action.startNs),$([long]$action.ackNs),$([long]$action.endNs),$([int][bool]$action.confirmed))"
}
$template = Get-Content -LiteralPath (Join-Path $repository 'tools/performance/animation_test_pacing.sql') -Raw
$sql = Join-Path $output 'animation-pacing.sql'
$template.Replace('{{WINDOWS}}', ($windows -join (',' + [Environment]::NewLine))) |
    Set-Content -LiteralPath $sql -Encoding utf8
. (Join-Path $repository 'tools/performance/ensure_trace_processor.ps1')
$processor = Get-SnowDesktopTraceProcessor -Offline:$Offline
$csv = Join-Path $output 'animation-pacing.csv'
& $processor query -f $sql (Join-Path $output 'trace.pftrace') 2> (Join-Path $output 'animation-pacing.log') |
    Set-Content -LiteralPath $csv -Encoding utf8
if ($LASTEXITCODE) { throw "Perfetto pacing query failed with exit code $LASTEXITCODE. Evidence: $output" }
$rows = @(Import-Csv -LiteralPath $csv)
if ($rows.Count -ne $actions.actions.Count -or @($rows | Where-Object windowCovered -ne '1').Count) {
    throw "The trace does not fully cover the action timeline. Evidence: $output"
}
$rows | Format-Table scenario,repetition,edge,confirmed,uiUpdates,p95GapMs,maxGapMs,longGaps,sharedCommits -AutoSize
Write-Host "Plan: $planFile"
Write-Host "Metrics: $csv"
Write-Host "Perfetto report: $($analysis.report)"
if ($status -or !$actions.ok -or @($rows | Where-Object confirmed -ne '1').Count) {
    throw "One or more targets did not reach the expected state. $($actions.error) Evidence: $output"
}
