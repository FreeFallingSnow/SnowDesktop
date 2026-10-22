# Completed references have their own receipts. They never manufacture a new
# execution, alter the original evidence, or grant candidate editing authority.
function Get-ReviewedVerificationContext([string]$Root,$Tests) {
    try {
        . (Join-Path $PSScriptRoot 'build_test_dependencies.ps1')
        $context=[pscustomobject]@{root=$Root;files=@{}};$reviews=@();$names=@()
        Initialize-TestRuntimeContracts $context
        foreach($test in $Tests|Sort-Object name){
            $contract=$context.runtimeContracts[$test.name];$policy=Get-Field $contract 'resultReuse'
            if(-not $policy -or $policy.kind -ne 'reviewed-native-files-v1' -or $policy.binarySha256 -notmatch '^[a-f0-9]{64}$'){return $null}
            $command=@($test.command)
            if($command.Count -lt 5 -or [IO.Path]::GetFileName($command[0]) -ne 'SnowDesktopTestRuntimeObserver.exe' -or $command[1] -ne '--test' -or $command[2] -cne $test.name -or $command[3] -ne '--'){return $null}
            $binary=Resolve-TestRuntimePath $context $policy.binary
            if([IO.Path]::GetFullPath($command[4]) -ne $binary -or (Get-FileDigest $binary) -ne $policy.binarySha256){return $null}
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
        $token=& (Join-Path $system 'whoami.exe') /all /fo csv /nh
        if($LASTEXITCODE -ne 0){return $null}
        $boot=(Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime().Ticks.ToString()
        $platform=[pscustomobject]@{os=[Environment]::OSVersion.VersionString;boot=$boot;system=$system;
            kernel=(Get-FileDigest (Join-Path $system 'ntoskrnl.exe'));apiSet=(Get-FileDigest (Join-Path $system 'ApiSetSchema.dll'));
            tokenDigest=(Get-VerificationTextDigest ($token -join "`n"));knownDlls=(Get-VerificationTextDigest ($knownBindings -join "`n"))}
        return [pscustomobject]@{policy='reviewed-native-files-v1';platformDigest=(Get-VerificationTextDigest ($platform|ConvertTo-Json -Compress));reviews=$reviews}
    }catch{return $null} # Unknown review/host conditions execute a fresh test.
}
function Get-CompletedVerificationProposal([string]$Root,$Verification) {
    $binding=Get-Field $Verification.binding 'candidate';$lease=$null
    if(-not $binding -or -not (Get-Field $Verification.binding 'reviewedReuse')){return $null}
    try {
        . (Join-Path $PSScriptRoot 'build_candidate.ps1')
        . (Join-Path $PSScriptRoot 'build_candidate_execution.ps1')
        $snapshot=Read-CandidateManifest $Root $binding.snapshot
        $prefix=$snapshot.manifest.context.digest.Substring(0,16)+'-'
        if(-not $binding.slot.StartsWith($prefix,[StringComparison]::Ordinal)){return $null}
        $lease=Enter-CandidateSlot $snapshot $binding.slot.Substring($prefix.Length)
        # The materializer and executors use this same exclusive slot handle.
        # Keep it until the coordinator commits the reference, never the Run.
        $current=Get-VerificationSnapshot $Root $Verification.baseline ($Verification.binding.names -join ',') (Get-Field $Verification.binding 'resourceBudget')
        if($current.key -ne $Verification.key){return $null}
        $state=Read-EntryJson (Join-Path $Root '.build/collaboration/state.json')
        $ledger=Get-Field $state 'verificationLedger'
        foreach($prior in @(Get-Field $ledger 'entries' @()|Where-Object key -eq $current.key)){
            $priorView=Get-RunView $prior.runId
            if(-not $priorView.terminal -or $priorView.status -in 'failed','invalidated','interrupted'){return $null}
        }
        foreach($entry in @(Get-Field $ledger 'entries' @()|Where-Object key -eq $current.key|Sort-Object createdUtc -Descending)){
            try {
                $view=Get-RunView $entry.runId
                if(-not $view.terminal -or $view.status -ne 'passed' -or $view.record.exitCode -ne 0 -or $view.request.mode -ne 'verify' -or $view.request.verification.key -ne $current.key){continue}
                if($view.record.inputStart.digest -ne $current.source.digest -or $view.record.inputEnd.digest -ne $current.source.digest){continue}
                $coveragePath=Join-Path $Root ('.build/collaboration/runs/'+$entry.runId+'.verification.json')
                $coverage=Read-EntryJson $coveragePath
                if($coverage.runId -ne $entry.runId -or $coverage.status -ne 'passed' -or @($coverage.selected).Count -ne @($current.binding.names).Count -or @(Compare-Object @($coverage.selected|Sort-Object) @($current.binding.names)).Count){continue}
                $files=Compare-TestRuntimeInputs $binding.source $current.tests $coverage.dependencyManifest
                if(@($files.tests).Count -ne @($current.tests).Count -or @($files.tests|Where-Object status -ne 'stable').Count){continue}
                $proofs=@();$valid=$true
                foreach($test in $current.tests){
                    $evidence=@($view.record.evidence.tests|Where-Object test -eq $test.name)
                    $modules=@($coverage.runtimeModules.tests|Where-Object test -eq $test.name)
                    if($evidence.Count -ne 1 -or $evidence[0].status -ne 'passed' -or $evidence[0].identityStatus -ne 'recorded' -or $modules.Count -ne 1 -or $modules[0].status -ne 'observed'){ $valid=$false;break }
                    if($evidence[0].runId -ne $entry.runId -or $evidence[0].binding.sourceDigest -ne $current.source.digest -or
                       $evidence[0].binding.environmentDigest -ne $current.executionContext.digest -or
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
                $proposal=[pscustomobject]@{runId=$entry.runId;key=$current.key;runSha256=(Get-FileDigest (Join-Path $Root ('.build/collaboration/runs/'+$entry.runId+'.json')));coverageSha256=(Get-FileDigest $coveragePath);review=$current.binding.reviewedReuse;tests=$proofs;checkedUtc=[DateTime]::UtcNow.ToString('o');lease=$lease.lease}
                $lease=$null;return $proposal
            }catch{continue} # Corrupt/incomplete history never suppresses execution.
        }
        return $null
    }catch{return $null}finally{if($lease){$lease.lease.Dispose()}}
}
function Add-CompletedVerificationReference([string]$Root,$Entry,$TaskRecord,[string]$Baseline,$Proposal) {
    $references=@(Get-Field $Entry 'references' @())
    $old=@($references|Where-Object {$_.task -eq $TaskRecord.id -and $_.requirementsRevision -eq $TaskRecord.requirementsRevision -and $_.baseline -eq $Baseline -and $_.sourceRunSha256 -eq $Proposal.runSha256})
    foreach($existing in $old){
        $existingPath=Join-Path $Root ('.build/verification/references/'+$existing.id+'.json')
        if($existing.id -match '^[a-f0-9]{32}$' -and [IO.File]::Exists($existingPath)){
            $saved=Read-EntryJson $existingPath
            if(($saved|ConvertTo-Json -Depth 15 -Compress) -ceq ($existing|ConvertTo-Json -Depth 15 -Compress)){return $existing}
        }
    }
    if($references.Count -ge 128){throw 'Completed verification reference budget requires explicit maintenance.'}
    $receipt=[pscustomobject]@{schemaVersion=1;kind='completed-verification-reference';id=[Guid]::NewGuid().ToString('N');task=$TaskRecord.id;taskRevision=$TaskRecord.revision;requirementsRevision=$TaskRecord.requirementsRevision;
        baseline=$Baseline;sourceRunId=$Proposal.runId;sourceRunSha256=$Proposal.runSha256;coverageSha256=$Proposal.coverageSha256;key=$Proposal.key;checkedUtc=$Proposal.checkedUtc;review=$Proposal.review;tests=$Proposal.tests;execution='referenced';newExecution=$false;reuseAllowed=$true;scope='exact-candidate-reviewed-binary'}
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
