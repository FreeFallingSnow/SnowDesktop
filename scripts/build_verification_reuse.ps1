# Completed references have their own receipts. They never manufacture a new
# execution, alter the original evidence, or grant candidate editing authority.
function Get-ReviewedVerificationContext([string]$Root,$Tests,$Candidate) {
    try {
        if(-not $Candidate -or $Candidate.source -ne $Root){return $null}
        . (Join-Path $PSScriptRoot 'build_test_dependencies.ps1')
        $context=[pscustomobject]@{root=$Root;files=@{}};$reviews=@();$names=@();$compatible=$true
        Initialize-TestRuntimeContracts $context
        foreach($test in $Tests|Sort-Object name){
            $contract=$context.runtimeContracts[$test.name];$policy=Get-Field $contract 'resultReuse'
            if(-not $policy -or $policy.kind -notin 'reviewed-native-files-v1','reviewed-native-files-v2' -or $policy.binarySha256 -notmatch '^[a-f0-9]{64}$'){return $null}
            if($policy.kind -eq 'reviewed-native-files-v2'){
                $permission=Get-Field $policy 'crossCandidate'
                if($permission -isnot [bool] -or -not $permission){return $null}
            }else{$compatible=$false}
            $command=@($test.command)
            if($command.Count -lt 5 -or [IO.Path]::GetFileName($command[0]) -ne 'SnowDesktopTestRuntimeObserver.exe' -or $command[1] -ne '--test' -or $command[2] -cne $test.name -or $command[3] -ne '--'){return $null}
            $relative=([string]$policy.binary).Replace('\','/')
            if(-not $relative.StartsWith('.build/',[StringComparison]::Ordinal)){return $null}
            # This is the already authenticated immutable candidate binding,
            # not an execution credential. Resolve the physical output without
            # traversing the .build junction through a live executor API.
            $binary=Resolve-CandidatePath $Candidate.output $relative.Substring(7)
            $alias=[IO.Path]::GetFullPath((Join-Path $Root $relative))
            $invoked=[IO.Path]::GetFullPath($command[4])
            if(($invoked -ne $alias -and $invoked -ne $binary) -or (Get-FileDigest $binary) -ne $policy.binarySha256){return $null}
            # Build-output data inputs need a separate reviewed mapping. Do not
            # let availability of a live credential change cache eligibility.
            if(@($contract.inputs|Where-Object {([string]$_.path).Replace('\','/').StartsWith('.build/',[StringComparison]::Ordinal)}).Count){return $null}
            if(@(@($contract.reviewedSources)+@($policy.reviewedSources)|Where-Object {([string]$_.path).Replace('\','/').StartsWith('.build/',[StringComparison]::Ordinal)}).Count){return $null}
            foreach($image in @($binary,[string]$command[0])){if([IO.File]::Exists($image+'.local') -or [IO.Directory]::Exists($image+'.local') -or [IO.File]::Exists($image+'.manifest')){return $null}}
            $inputs=Get-TestRuntimeInputs $context $test.name
            if($inputs.status -ne 'observed' -or $inputs.sourceReview -ne 'valid'){return $null}
            if(@($policy.reviewedSources).Count -lt 1 -or @($policy.reviewedSources).Count -gt 128){return $null}
            foreach($source in $policy.reviewedSources){
                if($source.sha256 -notmatch '^[a-f0-9]{64}$' -or (Get-FileDigest (Resolve-TestRuntimePath $context $source.path)) -ne $source.sha256){return $null}
            }
            $reviews+=@([pscustomobject]@{test=$test.name;policyDigest=(Get-VerificationTextDigest ($policy|ConvertTo-Json -Depth 8 -Compress));filesDigest=$inputs.digest})
            $names+=@([IO.Path]::GetFileName($binary))
        }
        if(-not $reviews.Count -or -not [Environment]::Is64BitProcess -or [Environment]::OSVersion.Platform -ne 'Win32NT'){return $null}
        # The reviewed code uses constants and declared file bytes, with the
        # native Windows/CRT runtime as its platform trust boundary. AppInit,
        # per-image debuggers/verifiers and compatibility layers are outside it.
        $view=[Microsoft.Win32.RegistryView]::Registry64
        $machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine',$view)
        $user=[Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser',$view)
        $knownBindings=@()
        try {
            $windows=$machine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows')
            try{if($windows -and ($windows.GetValue('LoadAppInit_DLLs',0) -ne 0 -or [string]$windows.GetValue('AppInit_DLLs',''))){return $null}}finally{if($windows){$windows.Dispose()}}
            $cert=$machine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\AppCertDlls')
            try{if($cert -and ($cert.ValueCount -or $cert.SubKeyCount)){return $null}}finally{if($cert){$cert.Dispose()}}
            foreach($name in @($names)+@('SnowDesktopTestRuntimeObserver.exe','conhost.exe')){
                $ifeo=$machine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\'+$name)
                try{if($ifeo -and ($ifeo.ValueCount -or $ifeo.SubKeyCount)){return $null}}finally{if($ifeo){$ifeo.Dispose()}}
            }
            foreach($hive in @($machine,$user)){
                $layers=$hive.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers')
                try{if($layers){foreach($test in $Tests){if($layers.GetValue([string]$test.command[4],$null)) {return $null}}}}finally{if($layers){$layers.Dispose()}}
            }
            $known=$machine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs')
            try{if(-not $known){return $null};$knownBindings=@($known.GetValueNames()|Sort-Object|ForEach-Object {$_+':'+[string]$known.GetValue($_)})}finally{if($known){$known.Dispose()}}
        }finally{$machine.Dispose();$user.Dispose()}
        $system=[Environment]::SystemDirectory
        if(-not ('SnowDesktop.Verification.NativeToken' -as [type])){
            Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_verification_token.cs')))
        }
        $token=[SnowDesktop.Verification.NativeToken]::Capture()
        $boot=(Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime().Ticks.ToString()
        $platform=[pscustomobject]@{os=[Environment]::OSVersion.VersionString;boot=$boot;system=$system;
            kernel=(Get-FileDigest (Join-Path $system 'ntoskrnl.exe'));apiSet=(Get-FileDigest (Join-Path $system 'ApiSetSchema.dll'));
            tokenKind='windows-token-v1';tokenDigest=(Get-VerificationTextDigest $token);knownDlls=(Get-VerificationTextDigest ($knownBindings -join "`n"))}
        $result=[pscustomobject]@{policy='reviewed-native-files-v1';platformDigest=(Get-VerificationTextDigest ($platform|ConvertTo-Json -Compress));platformBinding=$platform;reviews=$reviews}
        if($compatible){Set-Field $result 'candidateCompatibility' 'same-output-reviewed-inputs-v1'}
        return $result
    }catch{return $null} # Unknown review/host conditions execute a fresh test.
}
function Get-VerificationCompatibility($Verification) {
    $review=Get-Field $Verification.binding 'reviewedReuse';$candidate=Get-Field $Verification.binding 'candidate'
    if(-not $candidate -or (Get-Field $review 'candidateCompatibility') -ne 'same-output-reviewed-inputs-v1'){return $null}
    # This is a second identity for completed results, never the identity of an
    # active execution. Keep absolute invocation/output paths: moving a test is
    # not covered by this source-input review. Cache bytes may change between
    # configurations; actual invocation, executable bytes, presets and tools
    # are bound separately, along with every effective environment value.
    $implementation=@()
    foreach($file in @('build_verification.ps1','build_verification_reuse.ps1','build_test_runtime_inputs.ps1','build_test_runtime_modules.ps1','build_verification_token.cs','test_manager.ps1')){
        $digest=Get-FileDigest (Join-Path $candidate.source ('scripts/'+$file))
        if($digest -notmatch '^[a-f0-9]{64}$'){return $null}
        $implementation+=@([pscustomobject]@{file=$file;sha256=$digest})
    }
    $context=$Verification.executionContext;$rows=@()
    foreach($name in $Verification.binding.names){
        $definitions=@($Verification.binding.definitions|Where-Object name -eq $name)
        $artifacts=@($Verification.binding.artifacts|Where-Object test -eq $name)
        $runtime=@($Verification.binding.runtimeFiles|Where-Object test -eq $name)
        $reviews=@($review.reviews|Where-Object test -eq $name)
        if($definitions.Count -ne 1 -or $artifacts.Count -ne 1 -or -not $runtime.Count -or $reviews.Count -ne 1){return $null}
        $binding=[pscustomobject]@{schemaVersion=1;scope='same-output-reviewed-inputs-v1';test=$name;configuration=$Verification.binding.configuration;
            paths=[pscustomobject]@{source=$candidate.source;output=$candidate.output;slot=$candidate.slot;runRoot=$candidate.runRoot};
            definitions=$definitions;artifacts=$artifacts;runtimeFiles=$runtime;review=$reviews[0];platform=$review.platformDigest;
            environment=$context.environmentDigest;tools=$context.tools;presets=[pscustomobject]@{shared=$context.files.'CMakePresets.json';user=$context.files.'CMakeUserPresets.json'};
            implementation=$implementation;resourceBudget=(Get-Field $Verification.binding 'resourceBudget')}
        $rows+=@([pscustomobject]@{test=$name;key=(Get-VerificationTextDigest ($binding|ConvertTo-Json -Depth 20 -Compress));binding=$binding})
    }
    return [pscustomobject]@{schemaVersion=1;scope='same-output-reviewed-inputs-v1';tests=$rows}
}
function Get-VerificationCompatibleMatch($Entry,$Verification,[switch]$Any) {
    if($Entry.key -eq $Verification.key){return $true}
    $compatibility=Get-Field $Verification 'compatibility';$index=@(Get-Field $Entry 'compatibleTests' @())
    if(-not $compatibility -or $compatibility.schemaVersion -ne 1 -or -not $index.Count -or $index.Count -gt 128){return $false}
    $matches=0
    foreach($test in $compatibility.tests){
        $found=@($index|Where-Object {$_.test -ceq $test.test -and $_.key -ceq $test.key})
        if($found.Count -eq 1){$matches++}
    }
    return $(if($Any){$matches -gt 0}else{$matches -eq @($compatibility.tests).Count})
}
function Test-VerificationCompatibilityProof($Current,$Origin,$Entry) {
    $old=Get-Field $Origin 'compatibility';$now=Get-Field $Current 'compatibility'
    if(-not $old -or -not $now -or $old.schemaVersion -ne 1 -or @($old.tests).Count -gt 128){return $false}
    if($Origin.key -ne $Entry.key -or $Origin.key -ne (Get-VerificationTextDigest ($Origin.binding|ConvertTo-Json -Depth 20 -Compress))){return $false}
    foreach($test in $now.tests){
        $prior=@($old.tests|Where-Object test -CEQ $test.test)
        if($prior.Count -ne 1 -or $prior[0].key -cne $test.key -or $prior[0].key -cne (Get-VerificationTextDigest ($prior[0].binding|ConvertTo-Json -Depth 20 -Compress))){return $false}
        # Tie the stored compatibility identity back to the original exact
        # request and its original environment, rather than trusting an index.
        $binding=$prior[0].binding;$originalReview=Get-Field $Origin.binding 'reviewedReuse'
        if(($binding.definitions|ConvertTo-Json -Depth 8 -Compress) -cne (@($Origin.binding.definitions|Where-Object name -eq $test.test)|ConvertTo-Json -Depth 8 -Compress) -or
           ($binding.artifacts|ConvertTo-Json -Depth 8 -Compress) -cne (@($Origin.binding.artifacts|Where-Object test -eq $test.test)|ConvertTo-Json -Depth 8 -Compress) -or
           ($binding.runtimeFiles|ConvertTo-Json -Depth 8 -Compress) -cne (@($Origin.binding.runtimeFiles|Where-Object test -eq $test.test)|ConvertTo-Json -Depth 8 -Compress) -or
           $binding.environment -cne $Origin.executionContext.environmentDigest -or $binding.platform -cne $originalReview.platformDigest -or
           ($binding.tools|ConvertTo-Json -Depth 8 -Compress) -cne ($Origin.executionContext.tools|ConvertTo-Json -Depth 8 -Compress) -or
           ($binding.review|ConvertTo-Json -Depth 8 -Compress) -cne (@($originalReview.reviews|Where-Object test -eq $test.test)|ConvertTo-Json -Depth 8 -Compress) -or
           $binding.paths.source -cne $Origin.binding.candidate.source -or $binding.paths.output -cne $Origin.binding.candidate.output -or
           $binding.paths.slot -cne $Origin.binding.candidate.slot -or $binding.paths.runRoot -cne $Origin.binding.candidate.runRoot -or
           $binding.presets.shared -cne $Origin.executionContext.files.'CMakePresets.json' -or $binding.presets.user -cne $Origin.executionContext.files.'CMakeUserPresets.json' -or
           ($binding.resourceBudget|ConvertTo-Json -Depth 8 -Compress) -cne ((Get-Field $Origin.binding 'resourceBudget')|ConvertTo-Json -Depth 8 -Compress)){return $false}
    }
    return $true
}
function Get-CompletedVerificationProposal([string]$Root,$Verification) {
    $binding=Get-Field $Verification.binding 'candidate';$lease=$null
    Set-Field $Verification 'referenceCheck' 'unreviewed-native-context'
    if(-not $binding -or -not (Get-Field $Verification.binding 'reviewedReuse')){return $null}
    try {
        . (Join-Path $PSScriptRoot 'build_test_dependencies.ps1')
        . (Join-Path $PSScriptRoot 'build_test_runtime_modules.ps1')
        . (Join-Path $PSScriptRoot 'build_candidate.ps1')
        . (Join-Path $PSScriptRoot 'build_candidate_execution.ps1')
        Set-Field $Verification 'referenceCheck' 'checking-candidate-slot'
        $snapshot=Read-CandidateManifest $Root $binding.snapshot
        $prefix=$snapshot.manifest.context.digest.Substring(0,16)+'-'
        if(-not $binding.slot.StartsWith($prefix,[StringComparison]::Ordinal)){return $null}
        $slotWait=[Diagnostics.Stopwatch]::StartNew()
        while(-not $lease){
            try{$lease=Enter-CandidateSlot $snapshot $binding.slot.Substring($prefix.Length)}
            catch{
                if($_.Exception.Message -notlike 'Candidate slot is busy;*' -or $slotWait.Elapsed.TotalSeconds -ge 30){throw}
                # Another read check can own this handle without any active
                # Run to join. Wait for that check instead of launching work.
                Start-Sleep -Milliseconds 75
            }
        }
        # The materializer and executors use this same exclusive slot handle.
        # Keep it until the coordinator commits the reference, never the Run.
        $current=Get-VerificationSnapshot $Root $Verification.baseline ($Verification.binding.names -join ',') (Get-Field $Verification.binding 'resourceBudget')
        if($current.key -ne $Verification.key){return $null}
        Set-Field $Verification 'referenceCheck' 'checking-completed-history'
        $state=Read-EntryJson (Join-Path $Root '.build/collaboration/state.json')
        $ledger=Get-Field $state 'verificationLedger'
        # Candidate-compatible history uses the same ledger, never an execution join.
        foreach($prior in @(Get-Field $ledger 'entries' @()|Where-Object {Get-VerificationCompatibleMatch $_ $current -Any})){
            $priorView=Get-RunView $prior.runId
            if(-not $priorView.terminal -or $priorView.status -ne 'passed'){return $null}
        }
        foreach($entry in @(Get-Field $ledger 'entries' @()|Where-Object {Get-VerificationCompatibleMatch $_ $current}|Sort-Object createdUtc -Descending)){
            try {
                $view=Get-RunView $entry.runId
                if(-not $view.terminal -or $view.status -ne 'passed' -or $view.record.exitCode -ne 0 -or $view.request.mode -ne 'verify' -or $view.request.verification.key -ne $entry.key){continue}
                $origin=$view.request.verification;$crossCandidate=$entry.key -ne $current.key
                if($crossCandidate -and -not (Test-VerificationCompatibilityProof $current $origin $entry)){continue}
                if($view.record.inputStart.digest -ne $origin.source.digest -or $view.record.inputEnd.digest -ne $origin.source.digest){continue}
                $coveragePath=Join-Path $Root ('.build/collaboration/runs/'+$entry.runId+'.verification.json')
                $coverage=Read-EntryJson $coveragePath
                if($coverage.runId -ne $entry.runId -or $coverage.status -ne 'passed' -or @($current.binding.names|Where-Object {$_ -notin @($coverage.selected)}).Count){continue}
                $files=Compare-TestRuntimeInputs $binding.source $current.tests $coverage.dependencyManifest
                Set-Field $Verification 'referenceCheck' 'checking-file-and-report-evidence'
                if(@($files.tests).Count -ne @($current.tests).Count -or @($files.tests|Where-Object status -ne 'stable').Count){continue}
                $proofs=@();$valid=$true
                foreach($test in $current.tests){
                    $evidence=@($view.record.evidence.tests|Where-Object test -eq $test.name)
                    $modules=@($coverage.runtimeModules.tests|Where-Object test -eq $test.name)
                    if($evidence.Count -ne 1 -or $evidence[0].status -ne 'passed' -or $evidence[0].identityStatus -ne 'recorded' -or $modules.Count -ne 1 -or $modules[0].status -ne 'observed'){ $valid=$false;break }
                    if($evidence[0].runId -ne $entry.runId -or $evidence[0].binding.sourceDigest -ne $origin.source.digest -or
                       $evidence[0].binding.environmentDigest -ne $origin.executionContext.digest -or
                       $evidence[0].bindingDigest -ne (Get-VerificationTextDigest ($evidence[0].binding|ConvertTo-Json -Compress))){$valid=$false;break}
                    $attempts=@($evidence[0].attempts)
                    $reportRoot=Join-Path $binding.source '.build/Testing'
                    if($attempts.Count -ne 1 -or -not [IO.Path]::GetFullPath($attempts[0].report).StartsWith($reportRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or
                       -not [IO.Path]::GetFullPath($modules[0].report).StartsWith($reportRoot+'\',[StringComparison]::OrdinalIgnoreCase)){ $valid=$false;break }
                    if($attempts.Count -ne 1 -or $attempts[0].status -ne 'passed' -or $attempts[0].reportSha256 -notmatch '^[a-f0-9]{64}$' -or (Get-FileDigest $attempts[0].report) -ne $attempts[0].reportSha256 -or (Get-FileDigest $modules[0].report) -ne $modules[0].reportSha256){$valid=$false;break}
                    if((Get-Item -LiteralPath $attempts[0].report).Length -gt 64MB){$valid=$false;break}
                    $junit=New-Object Xml.XmlDocument;$junit.XmlResolver=$null
                    $settings=New-Object Xml.XmlReaderSettings;$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
                    $reader=[Xml.XmlReader]::Create($attempts[0].report,$settings)
                    try{$junit.Load($reader)}finally{$reader.Dispose()}
                    $cases=@($junit.SelectNodes("//testcase[@name='"+$test.name+"']"))
                    if($cases.Count -ne 1 -or $cases[0].SelectSingleNode('failure|error|skipped') -or $cases[0].GetAttribute('status') -in 'notrun','disabled'){$valid=$false;break}
                    $observed=Get-TestRuntimeModuleObservations $binding.source @($test) ([IO.Path]::GetDirectoryName($modules[0].report))
                    if(@($observed.tests).Count -ne 1 -or $observed.tests[0].status -ne 'observed' -or $observed.tests[0].digest -ne $modules[0].digest -or $observed.tests[0].rootExitCode -ne 0){$valid=$false;break}
                    $raw=Read-EntryJson $modules[0].report
                    $system=([Environment]::SystemDirectory).TrimEnd('\')+'\'
                    foreach($image in $raw.images){
                        if($image.kind -eq 'executable' -and $image.pid -eq $raw.rootPid){continue}
                        if(-not $image.path.StartsWith($system,[StringComparison]::OrdinalIgnoreCase) -or
                           ($image.kind -eq 'executable' -and [IO.Path]::GetFileName($image.path) -ine 'conhost.exe') -or
                           ($image.pid -eq $raw.rootPid -and [IO.Path]::GetFileName($image.path) -notin 'ntdll.dll','kernel32.dll','KernelBase.dll','kernel.appcore.dll','msvcrt.dll')){$valid=$false;break}
                        if($image.pid -eq $raw.rootPid){
                            $directories=@([IO.Path]::GetDirectoryName([string]$test.command[4]))+@($test.properties|Where-Object name -eq 'WORKING_DIRECTORY'|ForEach-Object value)
                            foreach($directory in $directories){
                                if(-not [IO.Path]::IsPathRooted([string]$directory)){$valid=$false;break}
                                $override=Join-Path $directory ([IO.Path]::GetFileName($image.path))
                                if([IO.File]::Exists($override) -and -not $override.StartsWith($system,[StringComparison]::OrdinalIgnoreCase)){$valid=$false;break}
                            }
                        }
                    }
                    if(-not $valid){break}
                    $proofs+=@([pscustomobject]@{test=$test.name;evidenceId=$evidence[0].id;reportSha256=$attempts[0].reportSha256;moduleReportSha256=$modules[0].reportSha256;moduleDigest=$modules[0].digest})
                }
                if(-not $valid -or $proofs.Count -ne @($current.tests).Count){continue}
                $proposal=[pscustomobject]@{runId=$entry.runId;key=$current.key;originKey=$entry.key;scope=$(if($crossCandidate){'candidate-compatible-reviewed-binary'}else{'exact-candidate-reviewed-binary'});
                    currentCandidate=$current.binding.candidate;originCandidate=$origin.binding.candidate;compatibleTests=@(Get-Field (Get-Field $current 'compatibility') 'tests' @()|Select-Object test,key);
                    runSha256=(Get-FileDigest (Join-Path $Root ('.build/collaboration/runs/'+$entry.runId+'.json')));coverageSha256=(Get-FileDigest $coveragePath);review=$current.binding.reviewedReuse;tests=$proofs;checkedUtc=[DateTime]::UtcNow.ToString('o');lease=$lease.lease}
                Set-Field $Verification 'referenceCheck' 'completed-origin-validated'
                $lease=$null;return $proposal
            }catch{Set-Field $Verification 'referenceCheck' ('origin-unavailable: '+$_.Exception.Message.Substring(0,[Math]::Min(400,$_.Exception.Message.Length)));continue} # Corrupt/incomplete history never suppresses execution.
        }
        return $null
    }catch{Set-Field $Verification 'referenceCheck' ('reference-unavailable: '+$_.Exception.Message.Substring(0,[Math]::Min(400,$_.Exception.Message.Length)));return $null}finally{if($lease){$lease.lease.Dispose()}}
}
function Add-CompletedVerificationReference([string]$Root,$Entry,$TaskRecord,[string]$Baseline,$Proposal) {
    $references=@(Get-Field $Entry 'references' @())
    $old=@($references|Where-Object {$_.task -eq $TaskRecord.id -and $_.requirementsRevision -eq $TaskRecord.requirementsRevision -and $_.baseline -eq $Baseline -and $_.key -eq $Proposal.key -and $_.sourceRunSha256 -eq $Proposal.runSha256})
    foreach($existing in $old){
        $existingPath=Join-Path $Root ('.build/verification/references/'+$existing.id+'.json')
        if($existing.id -match '^[a-f0-9]{32}$' -and [IO.File]::Exists($existingPath)){
            $saved=Read-EntryJson $existingPath
            if(($saved|ConvertTo-Json -Depth 15 -Compress) -ceq ($existing|ConvertTo-Json -Depth 15 -Compress)){return $existing}
        }
    }
    if($references.Count -ge 128){throw 'Completed verification reference budget requires explicit maintenance.'}
    $receipt=[pscustomobject]@{schemaVersion=1;kind='completed-verification-reference';id=[Guid]::NewGuid().ToString('N');task=$TaskRecord.id;taskRevision=$TaskRecord.revision;requirementsRevision=$TaskRecord.requirementsRevision;
        baseline=$Baseline;sourceRunId=$Proposal.runId;sourceRunSha256=$Proposal.runSha256;coverageSha256=$Proposal.coverageSha256;key=$Proposal.key;originKey=$Proposal.originKey;
        currentCandidate=$Proposal.currentCandidate;originCandidate=$Proposal.originCandidate;compatibleTests=$Proposal.compatibleTests;
        checkedUtc=$Proposal.checkedUtc;review=$Proposal.review;tests=$Proposal.tests;execution='referenced';newExecution=$false;reuseAllowed=$true;scope=$Proposal.scope}
    $directory=Join-Path $Root '.build/verification/references';[void][IO.Directory]::CreateDirectory($directory)
    $path=Join-Path $directory ($receipt.id+'.json');$temp=$path+'.tmp'
    try{
        $bytes=[Text.Encoding]::UTF8.GetBytes(($receipt|ConvertTo-Json -Depth 15))
        $stream=[IO.File]::Open($temp,'CreateNew','Write','None')
        try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        [IO.File]::Move($temp,$path)
    }finally{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
    Set-Field $Entry 'references' ($references+@($receipt));return $receipt
}
