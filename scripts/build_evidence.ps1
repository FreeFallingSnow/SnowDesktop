# Immutable per-test observations are embedded in the owning batch result.
# This is provenance, not permission to skip a test with unclosed dependencies.
function New-BatchTestEvidence([string]$Root,$Result) {
    $coverage=Get-Field $Result 'coverage'
    if(-not $coverage){return [pscustomobject]@{schemaVersion=1;status='not-recorded';tests=@();reason='No configured test coverage was recorded.'}}
    $retry=Get-Field $Result 'testRetry'
    $context=Get-Field $coverage 'executionContext'
    $definitions=@(Get-Field $coverage 'testDefinitionsBeforeRun' @())
    $artifacts=@(Get-Field $coverage 'testExecutablesBeforeRun' @())
    $items=@();$reports=@{}
    foreach($name in @(Get-Field $coverage 'selected' @()|Sort-Object -Unique)) {
        $observed=@(Get-Field $coverage 'completed' @()|Where-Object name -eq $name)
        $binary=@($artifacts|Where-Object test -eq $name)
        $definition=@($definitions|Where-Object name -eq $name)
        $status=if($observed.Count -eq 1){[string]$observed[0].status}else{'not-run'}
        $attempts=@()
        $retryTests=Get-Field $retry 'tests'
        if($retryTests -and $retryTests.PSObject.Properties[$name]){
            $entry=$retryTests.$name
            $attempts=@($entry.attempts|ForEach-Object{[pscustomobject]@{attempt=$_.attempt;status=$_.status;exitCode=(Get-Field $_ 'exitCode');report=(Get-Field $_ 'report')}})
        }
        if(-not $attempts.Count -and $observed.Count -eq 1){$attempts=@([pscustomobject]@{attempt=1;status=$status;exitCode=$null;report=(Get-Field $coverage 'report')})}
        foreach($attempt in $attempts){
            $digest=$null
            if($attempt.report){
                $path=[IO.Path]::GetFullPath([string]$attempt.report)
                if($path.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) -and [IO.File]::Exists($path)){
                    if(-not $reports.ContainsKey($path)){$reports[$path]=Get-FileDigest $path}
                    $digest=$reports[$path]
                }
            }
            Set-Field $attempt 'reportSha256' $digest
        }
        $source=Get-Field $Result 'inputStart'
        $binding=[pscustomobject]@{sourceDigest=(Get-Field $source 'digest');configuration=(Get-Field (Get-Field $Result 'testingPlan') 'configuration');
            assertionBinary=$(if($binary.Count -eq 1){$binary[0].sha256}else{$null});
            invocationDigest=$(if($definition.Count -eq 1){$definition[0].digest}else{$null});environmentDigest=(Get-Field $context 'digest')}
        $complete=$binding.sourceDigest -and $binding.assertionBinary -and $binding.invocationDigest -and $binding.environmentDigest
        $stable=(Get-Field $Result 'inputCheck') -eq 'stable'
        $items+=@([pscustomobject]@{id=[Guid]::NewGuid().ToString('N');test=$name;status=$status;
            seconds=$(if($observed.Count -eq 1){Get-Field $observed[0] 'seconds'}else{$null});
            batchId=$Result.batchId;runId=(Get-Field $Result 'runId');logicalBatchId=(Get-Field $Result 'logicalBatchId' $Result.batchId);attempt=(Get-Field $Result 'attempt' 1);
            requesters=@(Get-Field $coverage 'tasks' @()|Where-Object {$_.requested -contains $name}|ForEach-Object participant);
            binding=$binding;bindingDigest=$(if($complete -and $stable){Get-VerificationTextDigest ($binding|ConvertTo-Json -Compress)}else{$null});
            inputCheck=(Get-Field $Result 'inputCheck');inputEndDigest=(Get-Field (Get-Field $Result 'inputEnd') 'digest');attempts=$attempts;
            identityStatus=$(if(-not $stable){'invalidated'}elseif($complete){'recorded'}else{'incomplete'});
            reuseAllowed=$false;dependencyCoverage='Repository inputs, invocation and binary are bound; external and semantic dependencies are not yet closed.'})
    }
    return [pscustomobject]@{schemaVersion=1;status='recorded';tests=$items;reason='Observations remain attached to their original immutable attempt; no automatic cross-candidate reuse.'}
}
