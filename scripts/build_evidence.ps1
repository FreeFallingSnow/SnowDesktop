# Immutable per-test observations are embedded in the owning batch result.
# This is provenance, not permission to skip a test with unclosed dependencies.
function New-BatchTestEvidence([string]$Root,$Result) {
    $coverage=Get-Field $Result 'coverage'
    if(-not $coverage){return [pscustomobject]@{schemaVersion=1;status='not-recorded';tests=@();reason='No configured test coverage was recorded.'}}
    $retry=Get-Field $Result 'testRetry'
    $context=Get-Field $coverage 'executionContext'
    $definitions=@(Get-Field $coverage 'testDefinitionsBeforeRun' @())
    $artifacts=@(Get-Field $coverage 'testExecutablesBeforeRun' @())
    $dependencies=Get-Field $coverage 'dependencyManifest'
    $runtimeInputCheck=Get-Field $coverage 'runtimeInputCheck'
    $items=@();$reports=@{}
    foreach($name in @(Get-Field $coverage 'selected' @()|Sort-Object -Unique)) {
        $observed=@(Get-Field $coverage 'completed' @()|Where-Object name -eq $name)
        $binary=@($artifacts|Where-Object test -eq $name)
        $definition=@($definitions|Where-Object name -eq $name)
        $dependency=@(Get-Field $dependencies 'tests' @()|Where-Object test -eq $name)
        $runtimeInputs=@(Get-Field $runtimeInputCheck 'tests' @()|Where-Object test -eq $name)
        $status=if($observed.Count -eq 1){[string]$observed[0].status}else{'not-run'}
        $attempts=@()
        $retryTests=Get-Field $retry 'tests'
        if($retryTests -and $retryTests.PSObject.Properties[$name]){
            $entry=$retryTests.$name
            $attempts=@($entry.attempts|ForEach-Object{[pscustomobject]@{attempt=$_.attempt;status=$_.status;exitCode=$null;runnerExitCode=(Get-Field $_ 'exitCode');report=(Get-Field $_ 'report')}})
        }
        if(-not $attempts.Count -and $observed.Count -eq 1){$attempts=@([pscustomobject]@{attempt=1;status=$status;exitCode=$null;runnerExitCode=(Get-Field $Result 'pipelineExitCode');report=(Get-Field $coverage 'report')})}
        $seconds=$null
        foreach($attempt in $attempts){
            $seconds=$null
            $digest=$null
            if($attempt.report){
                $path=[IO.Path]::GetFullPath([string]$attempt.report)
                if($path.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) -and [IO.File]::Exists($path)){
                    if(-not $reports.ContainsKey($path)){
                        $times=@{};$reader=$null
                        try{
                            if((Get-Item -LiteralPath $path).Length -le 64MB){
                                $settings=New-Object Xml.XmlReaderSettings;$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
                                $reader=[Xml.XmlReader]::Create($path,$settings)
                                while($reader.Read()){if($reader.NodeType -eq [Xml.XmlNodeType]::Element -and $reader.LocalName -eq 'testcase'){$times[$reader.GetAttribute('name')]=$reader.GetAttribute('time')}}
                            }
                        }catch{$times=@{}}finally{if($reader){$reader.Dispose()}}
                        $reports[$path]=[pscustomobject]@{sha256=(Get-FileDigest $path);times=$times}
                    }
                    $digest=$reports[$path].sha256
                    $value=0.0
                    if($reports[$path].times.ContainsKey($name) -and [double]::TryParse([string]$reports[$path].times[$name],[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value) -and -not [double]::IsNaN($value) -and -not [double]::IsInfinity($value) -and $value -ge 0){$seconds=$value}
                }
            }
            Set-Field $attempt 'reportSha256' $digest
            Set-Field $attempt 'seconds' $seconds
        }
        $source=Get-Field $Result 'inputStart'
        $binding=[pscustomobject]@{sourceDigest=(Get-Field $source 'digest');configuration=(Get-Field (Get-Field $Result 'testingPlan') 'configuration');
            assertionBinary=$(if($binary.Count -eq 1){$binary[0].sha256}else{$null});
            invocationDigest=$(if($definition.Count -eq 1){$definition[0].digest}else{$null});environmentDigest=(Get-Field $context 'digest')}
        $complete=$binding.sourceDigest -and $binding.assertionBinary -and $binding.invocationDigest -and $binding.environmentDigest
        $stable=(Get-Field $Result 'inputCheck') -eq 'stable'
        $items+=@([pscustomobject]@{id=[Guid]::NewGuid().ToString('N');test=$name;status=$status;
            seconds=$seconds;
            batchId=$Result.batchId;runId=(Get-Field $Result 'runId');logicalBatchId=(Get-Field $Result 'logicalBatchId' $Result.batchId);attempt=(Get-Field $Result 'attempt' 1);
            requesters=@(Get-Field $coverage 'tasks' @()|Where-Object {$_.requested -contains $name}|ForEach-Object participant);
            binding=$binding;bindingDigest=$(if($complete -and $stable){Get-VerificationTextDigest ($binding|ConvertTo-Json -Compress)}else{$null});
            inputCheck=(Get-Field $Result 'inputCheck');inputEndDigest=(Get-Field (Get-Field $Result 'inputEnd') 'digest');attempts=$attempts;
            identityStatus=$(if(-not $stable){'invalidated'}elseif($complete){'recorded'}else{'incomplete'});
            dependencies=$(if($dependency.Count -eq 1){[pscustomobject]@{manifest=(Get-Field $dependencies 'path');manifestSha256=(Get-Field $dependencies 'sha256');observation=$dependency[0]}}else{[pscustomobject]@{status='not-recorded';reuseAllowed=$false}});
            runtimeInputs=$(if($runtimeInputs.Count -eq 1){$runtimeInputs[0]}else{[pscustomobject]@{status='not-recorded';runtimeClosure='unclosed';reuseAllowed=$false}});
            reuseAllowed=$false;dependencyCoverage='Repository inputs, invocation and binary are bound; external and semantic dependencies are not yet closed.'})
    }
    return [pscustomobject]@{schemaVersion=1;status='recorded';tests=$items;reason='Observations remain attached to their original immutable attempt; no automatic cross-candidate reuse.'}
}
