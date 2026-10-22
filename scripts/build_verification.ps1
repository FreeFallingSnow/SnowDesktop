# Existing-artifact verification uses the ordinary durable Run. This module
# binds an intent; the coordinator state serializes subscriptions, not execution.
function Get-VerificationEnvironment([string]$Root) {
    $files=[ordered]@{}
    foreach($relative in @('CMakePresets.json','CMakeUserPresets.json','.build/CMakeCache.txt')){
        $path=Join-Path $Root $relative
        $files[$relative]=if([IO.File]::Exists($path)){Get-FileDigest $path}else{$null}
    }
    $tools=[ordered]@{}
    foreach($name in @('cmake.exe','ctest.exe','node.exe','python.exe')){
        $tool=Get-Command $name -CommandType Application -ErrorAction SilentlyContinue|Select-Object -First 1
        $tools[$name]=if($tool -and [IO.File]::Exists($tool.Source)){[pscustomobject]@{path=$tool.Source;sha256=(Get-FileDigest $tool.Source)}}else{$null}
    }
    $runtime=Get-BuildPowerShell
    $tools['powershell']=[pscustomobject]@{path=$runtime;sha256=(Get-FileDigest $runtime)}
    $values=[Environment]::GetEnvironmentVariables('Process')
    $environment=[ordered]@{}
    foreach($name in @($values.Keys|Sort-Object)){
        # These exact values identify execution/telemetry, not test behavior.
        if($name -in 'SNOWDESKTOP_EXECUTION_TOKEN','SNOWDESKTOP_RUN_ID','SNOWDESKTOP_STAGE_PARENT','SNOWDESKTOP_RETRY_SIGNAL_DIR','SNOWDESKTOP_RETRY_RUN_TOKEN'){continue}
        $environment[([string]$name).ToUpperInvariant()]=[string]$values[$name]
    }
    # The standard entry always sets these before executing a test process.
    $environment['SNOWDESKTOP_ENTRY_POWERSHELL']=$runtime
    $environment['MSBUILDDISABLENODEREUSE']='1'
    $environment['PSEXECUTIONPOLICYPREFERENCE']='Bypass'
    # cmd.exe, used by the leased pipeline, supplies this default only when the
    # caller has no PROMPT. Bind its effective value; custom prompts still differ.
    if(-not $environment.Contains('PROMPT')){$environment['PROMPT']='$P$G'}
    $canonical=[ordered]@{}
    foreach($name in @($environment.Keys|Sort-Object)){$canonical[$name]=$environment[$name]}
    $fieldDigests=[ordered]@{}
    foreach($name in $canonical.Keys){$fieldDigests[$name]=Get-VerificationTextDigest ([string]$canonical[$name])}
    $binding=[pscustomobject]@{files=$files;tools=$tools;environmentDigest=(Get-VerificationTextDigest ($canonical|ConvertTo-Json -Compress))}
    return [pscustomobject]@{schemaVersion=2;digest=(Get-VerificationTextDigest ($binding|ConvertTo-Json -Depth 8 -Compress));files=$files;tools=$tools;
        environmentDigest=$binding.environmentDigest;environmentFields=$fieldDigests;externalDependenciesClosed=$false}
}
function Get-VerificationDefinitions($Tests) {
    return @($Tests|Sort-Object name|ForEach-Object {
        # COST is CTest's learned scheduling history, not a test assertion.
        # Keep all invocation, environment, resource and fixture properties.
        $definition=[pscustomobject]@{name=$_.name;command=@(Get-Field $_ 'command' @());properties=@($_.properties|Where-Object name -ne 'COST'|Sort-Object name)}
        [pscustomobject]@{name=$_.name;policyVersion=2;digest=(Get-VerificationTextDigest ($definition|ConvertTo-Json -Depth 20 -Compress))}
    })
}
function Get-VerificationRuntimeFiles([string]$Root,$Tests) {
    $files=@();$cache=@{}
    foreach($test in $Tests|Sort-Object name){
        $paths=@(Get-Field $test 'command' @())+@($test.properties|Where-Object name -eq 'REQUIRED_FILES'|ForEach-Object value)
        foreach($path in @($paths|Where-Object {[IO.Path]::IsPathRooted([string]$_) -and [IO.File]::Exists([string]$_)}|Sort-Object -Unique)){
            $absolute=[IO.Path]::GetFullPath([string]$path)
            if(-not $cache.ContainsKey($absolute)){$cache[$absolute]=[pscustomobject]@{sha256=(Get-FileDigest $absolute);bytes=(Get-Item -LiteralPath $absolute).Length}}
            $files+=@([pscustomobject]@{test=$test.name;file=$(if($absolute.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){$absolute.Substring($Root.Length+1)}else{$absolute});sha256=$cache[$absolute].sha256;bytes=$cache[$absolute].bytes})
            if($files.Count -gt 1024){throw 'Explicit verification runtime file budget exceeded.'}
        }
    }
    return $files
}
function Get-VerificationNames([string]$Filter) {
    $names=@($Filter.Split(',')|ForEach-Object {$_.Trim()})
    if($names.Count -lt 1 -or $names.Count -gt 128 -or @($names|Where-Object {$_ -notmatch '^[A-Za-z0-9_-]{1,160}$'}).Count -or @($names|Sort-Object -Unique).Count -ne $names.Count){throw 'Verification requires 1..128 unique, literal comma-separated CTest names.'}
    return @($names|Sort-Object)
}
function Get-VerificationSnapshot([string]$Root,[string]$Baseline,[string]$Filter) {
    if($Baseline -notmatch '^[a-f0-9]{32}$'){throw 'Verification requires -PlanBatch with an immutable baseline batch ID.'}
    $result=Read-EntryJson (Join-Path $Root ('.build/collaboration/'+$Baseline+'.json'))
    if(-not $result -or $result.repositoryRoot -ne $Root -or $result.batchId -ne $Baseline -or $result.inputCheck -ne 'stable' -or $result.outcome -notin 'passed','failed'){throw 'Verification baseline is not a stable, completed candidate.'}
    $source=Get-BuildInputIdentity $Root
    if($source.digest -ne $result.inputEnd.digest){throw 'Verification source no longer matches the baseline candidate; build the changed inputs first.'}
    $names=@(Get-VerificationNames $Filter)
    $pattern='^('+ (($names|ForEach-Object {[regex]::Escape($_)}) -join '|') +')$'
    $output=& ctest --preset all-tests --test-dir (Join-Path $Root '.build') -C Release --show-only=json-v1 -R $pattern
    if($LASTEXITCODE -ne 0){throw 'Unable to query existing configured tests; verification never configures or builds.'}
    $tests=@((($output -join [Environment]::NewLine)|ConvertFrom-Json).tests)
    if($tests.Count -ne $names.Count -or @(Compare-Object $names @($tests.name|Sort-Object)).Count){throw 'Verification selection differs from the requested existing tests.'}
    $definitions=@(Get-VerificationDefinitions $tests);$artifacts=@()
    foreach($test in $tests|Sort-Object name){
        $name=$test.name
        $oldDefinition=@(Get-Field $result.coverage 'testDefinitionsBeforeRun' @()|Where-Object name -eq $name)
        $definition=@($definitions|Where-Object name -eq $name)[0]
        if($oldDefinition.Count -ne 1 -or (Get-Field $oldDefinition[0] 'policyVersion') -ne 2 -or $oldDefinition[0].digest -ne $definition.digest){throw "Verification definition changed or baseline identity is unavailable: $name"}
        $cmd=@(Get-Field $test 'command' @())
        if(-not $cmd.Count -or -not [IO.Path]::IsPathRooted([string]$cmd[0])){throw "Verification executable is unavailable: $name"}
        $path=[IO.Path]::GetFullPath([string]$cmd[0])
        $old=@(Get-Field $result.coverage 'testExecutablesBeforeRun' @()|Where-Object test -eq $name)
        if(-not $path.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) -or -not [IO.File]::Exists($path) -or $old.Count -ne 1 -or $old[0].executable -ne $path.Substring($Root.Length+1) -or $old[0].sha256 -ne (Get-FileDigest $path)){throw "Verification binary no longer matches the baseline: $name"}
        $artifacts+=@([pscustomobject]@{test=$name;executable=$path.Substring($Root.Length+1);sha256=$old[0].sha256;bytes=(Get-Item -LiteralPath $path).Length})
    }
    $context=Get-VerificationEnvironment $Root
    $runtimeFiles=@(Get-VerificationRuntimeFiles $Root $tests)
    $oldRuntime=@(Get-Field $result.coverage 'testRuntimeFilesBeforeRun' @()|Where-Object {$names -contains $_.test})
    if(($runtimeFiles|ConvertTo-Json -Depth 8 -Compress) -ne ($oldRuntime|ConvertTo-Json -Depth 8 -Compress)){throw 'Explicit verification runtime files changed or their baseline identity is unavailable.'}
    $binding=[pscustomobject]@{sourceDigest=$source.digest;configuration='Release';names=$names;definitions=$definitions;artifacts=$artifacts;runtimeFiles=$runtimeFiles;environmentDigest=$context.digest}
    return [pscustomobject]@{schemaVersion=1;key=(Get-VerificationTextDigest ($binding|ConvertTo-Json -Depth 20 -Compress));baseline=$Baseline;binding=$binding;source=$source;executionContext=$context;tests=$tests;pattern=$pattern;noBuild=$true;reuseAllowed=$false}
}
function Get-VerificationLedger($State) {
    $ledger=Get-Field $State 'verificationLedger'
    if(-not $ledger){$ledger=[pscustomobject]@{schemaVersion=1;entries=@()};Set-Field $State 'verificationLedger' $ledger}
    if($ledger.schemaVersion -ne 1 -or @($ledger.entries).Count -ge 4096){throw 'Verification ledger version/count budget requires explicit maintenance.'}
    return $ledger
}
function Add-VerificationSubscription($Entry,$TaskRecord,[string]$Baseline,[string]$Reason) {
    $existing=@($Entry.subscriptions|Where-Object {$_.task -eq $TaskRecord.id -and $_.requirementsRevision -eq $TaskRecord.requirementsRevision -and $_.baseline -eq $Baseline -and -not $_.withdrawnUtc})
    if($existing.Count){return}
    if(@($Entry.subscriptions).Count -ge 128){throw 'Verification subscriber budget exceeded.'}
    $Entry.subscriptions=@($Entry.subscriptions)+@([pscustomobject]@{id=[Guid]::NewGuid().ToString('N');task=$TaskRecord.id;taskRevision=$TaskRecord.revision;requirementsRevision=$TaskRecord.requirementsRevision;baseline=$Baseline;reason=$Reason;subscribedUtc=[DateTime]::UtcNow.ToString('o');withdrawnUtc=$null})
}
