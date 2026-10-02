[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidatePattern('^[a-f0-9]{32}$')][string]$Batch)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$planRoot=Join-Path $root '.build\collaboration'
$plan=[IO.File]::ReadAllText((Join-Path $planRoot ($Batch+'.plan.json'))) | ConvertFrom-Json
if ($plan.schemaVersion -ne 1 -or $plan.batchId -ne $Batch -or $plan.configuration -ne 'Release') { throw 'Invalid frozen testing plan.' }
[IO.File]::WriteAllText((Join-Path $planRoot ($Batch+'.stage.json')),([pscustomobject]@{batchId=$Batch;hostBuildPassed=[bool]$plan.buildRequired;testsStartedUtc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json))
if ($plan.mode -eq 'skipped') {
    [pscustomobject]@{schemaVersion=1;batchId=$Batch;mode='skipped';status='not-required';selected=@();completed=@();tasks=@($plan.tasks | ForEach-Object {[pscustomobject]@{participant=$_.participant;status='not-required';requested=@();reason=$_.requirement.reason}})} |
        ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $planRoot ($Batch+'.coverage.json')) -Encoding UTF8
    Write-Output 'Host build/tests explicitly not required by this frozen task plan; no passing host test result is claimed.'
    exit 0
}
& (Join-Path $PSScriptRoot 'test_manager.ps1') -Mode plan -PlanBatch $Batch
exit 0
