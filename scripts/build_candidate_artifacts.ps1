# Published bytes are separate from a reusable compiler slot. Publication is
# create-only and its manifest is last; partial exports remain for inspection.
function Get-CandidateArtifactDirectory([string]$Root,[string]$Id) {
    if($Id -notmatch '^[a-f0-9]{32}$'){throw 'Artifact requires its producing Run ID.'}
    $store=Get-CandidateStore $Root
    return Resolve-CandidatePath ([IO.Path]::GetDirectoryName($store.slots)) ('candidate-artifacts/'+$Id)
}
function Open-CandidateRuntimeLease([string]$Root,[switch]$Exclusive) {
    $directory=Resolve-CandidatePath $Root '.build/collaboration'
    [void][IO.Directory]::CreateDirectory($directory)
    $path=Resolve-CandidatePath $directory 'runtime-retention.lock'
    $watch=[Diagnostics.Stopwatch]::StartNew()
    do {
        try {
            $share=if($Exclusive){[IO.FileShare]::None}else{[IO.FileShare]::ReadWrite}
            return [IO.File]::Open($path,'OpenOrCreate','ReadWrite',$share)
        } catch {
            if(-not (Test-EntrySharingViolation $_.Exception)){throw}
            if($Exclusive){return $null}
            if($watch.Elapsed.TotalSeconds -ge 30){throw 'Saved runtime cleanup is busy; retry the operation.'}
            Start-Sleep -Milliseconds 100
        }
    } while($true)
}
function Remove-CandidateRetiredDirectory([string]$Base,[string]$Relative) {
    # Resolve every directory before recursion. Never traverse a junction or
    # let a generated manifest select a path outside the exact cleanup root.
    $path=Resolve-CandidatePath $Base $Relative
    if(-not [IO.Directory]::Exists($path)){return}
    $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($path)
    while($pending.Count){
        foreach($item in [IO.Directory]::EnumerateFileSystemEntries($pending.Pop())){
            if([IO.File]::GetAttributes($item) -band [IO.FileAttributes]::ReparsePoint){throw 'Retired runtime contains a linked entry; retained for inspection.'}
            if([IO.Directory]::Exists($item)){$pending.Push($item)}
        }
    }
    Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
}
function Invoke-CandidateRuntimeRetention([string]$Root,[switch]$Preview) {
    $lease=Open-CandidateRuntimeLease $Root -Exclusive
    if(-not $lease){return [pscustomobject]@{status='busy';removed=@();deferred=@()}}
    $outputLease=$null
    $removed=@();$deferred=@();$planned=@()
    try {
        $stateRoot=Resolve-CandidatePath $Root '.build/collaboration'
        try {$outputLease=[IO.File]::Open((Resolve-CandidatePath $stateRoot 'build.lock'),'OpenOrCreate','ReadWrite','None')}
        catch {if(Test-EntrySharingViolation $_.Exception){return [pscustomobject]@{status='busy';removed=@();deferred=@()}};throw}
        $current=Read-EntryJson (Join-Path $stateRoot 'activation-current.json')
        $protected=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        if($current -and $current.runId -match '^[a-f0-9]{32}$'){[void]$protected.Add($current.runId)}
        $artifacts=Resolve-CandidatePath $Root '.build/candidate-artifacts'
        $candidates=@()
        if([IO.Directory]::Exists($artifacts)){
            foreach($directory in [IO.Directory]::EnumerateDirectories($artifacts)){
                $id=[IO.Path]::GetFileName($directory)
                if($id -notmatch '^[a-f0-9]{32}$'){continue}
                try {
                    $manifestPath=Resolve-CandidatePath $artifacts ($id+'/artifact.json')
                    if(-not [IO.File]::Exists($manifestPath)){continue}
                    $manifest=Read-CandidateJson $manifestPath
                    $payload=Resolve-CandidatePath $artifacts ($id+'/runtime')
                    if(-not [IO.Directory]::Exists($payload)){continue}
                    if($manifest.id -ne $id -or $manifest.runId -ne $id -or -not $manifest.publicationComplete -or $manifest.runtime -ne $payload){throw 'Unrecognized artifact identity.'}
                    # Persisted running states can outlive their process. Use
                    # the actual executor lease without rewriting its record.
                    $ownerPath=Resolve-CandidatePath $stateRoot ('runs/'+$id+'.owner.lock')
                    if([IO.File]::Exists($ownerPath)){
                        $owner=$null
                        try {$owner=[IO.File]::Open($ownerPath,'Open','ReadWrite','None')}
                        catch {if(Test-EntrySharingViolation $_.Exception){[void]$protected.Add($id)}else{throw}}
                        finally {if($owner){$owner.Dispose()}}
                    }
                    $candidates+=@([pscustomobject]@{id=$id;manifest=$manifest;created=[DateTimeOffset]::Parse($manifest.createdUtc)})
                } catch {$deferred+=@([pscustomobject]@{path=$directory;error=$_.Exception.Message})}
            }
        }
        # A standalone packaging build can finish before it is activated.
        # Keep its latest product as well until the next successful activation.
        $latest=@($candidates|Where-Object {$_.manifest.productRuntime}|Sort-Object created -Descending|Select-Object -First 1)
        if(-not $latest.Count){$latest=@($candidates|Sort-Object created -Descending|Select-Object -First 1)}
        foreach($item in $latest){[void]$protected.Add($item.id)}
        foreach($item in $candidates){
            if($protected.Contains($item.id)){continue}
            $relative=$item.id+'/runtime';$path=Resolve-CandidatePath $artifacts $relative;$planned+=@($path)
            if($Preview){continue}
            try {
                Remove-CandidateRetiredDirectory $artifacts $relative
                # Retain the immutable manifest, hashes and all Run evidence.
                Write-CandidateJson ([pscustomobject]@{schemaVersion=1;runId=$item.id;runtimeRemoved=$true;removedUtc=[DateTime]::UtcNow.ToString('o');reason='superseded-runtime'}) (Resolve-CandidatePath $artifacts ($item.id+'/retention.json'))
                $removed+=@($path)
            } catch {$deferred+=@([pscustomobject]@{path=$path;error=$_.Exception.Message})}
        }
        $backups=Resolve-CandidatePath $Root '.build/activation-backups'
        if([IO.Directory]::Exists($backups)){
            $directories=@([IO.Directory]::EnumerateDirectories($backups)|Where-Object {[IO.Path]::GetFileName($_) -match '^[a-f0-9]{32}-[a-f0-9]{32}$'}|Sort-Object {[IO.Directory]::GetLastWriteTimeUtc($_)} -Descending)
            $keep=if($directories.Count){$directories[0]}else{''}
            if($current -and $current.backup -and $current.backup -in $directories){$keep=$current.backup}
            foreach($directory in $directories){
                if($directory -eq $keep){continue}
                $planned+=@($directory)
                if($Preview){continue}
                try {
                    $name=[IO.Path]::GetFileName($directory)
                    $activation=Read-EntryJson (Join-Path $stateRoot ($name.Substring(0,32)+'.activation.json'))
                    if($activation -and $activation.status -notin 'activated','installed','already-current'){
                        $failedAt=Get-Field $activation 'completedUtc'
                        $currentAt=Get-Field $current 'completedUtc'
                        if(-not $failedAt -or -not $currentAt -or [DateTimeOffset]::Parse($failedAt) -ge [DateTimeOffset]::Parse($currentAt)){
                            throw 'Activation recovery may still need this backup; retain it until a later successful activation.'
                        }
                    }
                    Remove-CandidateRetiredDirectory $backups $name;$removed+=@($directory)
                }
                catch {$deferred+=@([pscustomobject]@{path=$directory;error=$_.Exception.Message})}
            }
        }
        $receipt=[pscustomobject]@{schemaVersion=1;status=$(if($Preview){'preview'}elseif($deferred.Count){'deferred'}else{'completed'});
            completedUtc=[DateTime]::UtcNow.ToString('o');retainedArtifacts=@($protected);planned=$planned;removed=$removed;deferred=$deferred}
        if(-not $Preview){Write-CandidateJson $receipt (Join-Path $stateRoot 'runtime-retention.json')}
        return $receipt
    } finally {if($outputLease){$outputLease.Dispose()};$lease.Dispose()}
}
function Get-CandidateRuntimeFiles($Context,[string]$Action) {
    $output=Resolve-CandidatePath $Context.output $Context.configuration
    $files=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $profile='test-files'
    if($Action -in 'release','debug','batch-product'){
        Import-Module (Join-Path $PSScriptRoot 'deployment_payload.psm1') -Force -Global
        $deployment=Read-SnowDesktopDeploymentManifest -BuildOutput $output
        # The deployment manifest is the inventory, including build-layout
        # aliases, XBF/WinMD, assets, fragments and notices. A second whitelist
        # must never silently drop a dependency of the saved manifest.
        foreach($file in @($deployment.files)+@($deployment.appxFragments)+@($deployment.notices)){
            $relative=if($file.PSObject.Properties['buildPath'] -and $file.buildPath){$file.buildPath}else{$file.path}
            [void]$files.Add((Get-CandidateRelativePath $relative))
        }
        foreach($name in @('SnowDesktop.exe','snowwidget.exe','SnowDesktop.deployment.json')){
            if(-not [IO.File]::Exists((Resolve-CandidatePath $output $name))){throw 'Completed product build is missing its required runtime.'}
            [void]$files.Add($name)
        }
        foreach($file in [IO.Directory]::EnumerateFiles($output)){
            $name=[IO.Path]::GetFileName($file)
            if($name -in 'SnowDesktopLauncher.exe','SnowDesktopSteamBridge.exe','SnowDesktopWorkshopManager.exe' -or
                [IO.Path]::GetExtension($name) -in '.manifest','.pri' -or $name -eq 'SnowDesktop.pdb'){[void]$files.Add($name)}
        }
        # Steam's bridge has build-generated sidecars outside the WinAppSDK
        # deployment inventory. Keep them with that executable, not with a
        # mutable source checkout or a later packaging repair directory.
        if($files.Contains('SnowDesktopSteamBridge.exe')){
            foreach($name in @('SnowDesktopSteamBridge-LICENSE.txt','SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md')){
                if(-not [IO.File]::Exists((Resolve-CandidatePath $output $name))){throw "Bridge build sidecar is missing: $name"}
                [void]$files.Add($name)
            }
        }
        foreach($name in @('SnowDesktop.Runtime','widgets','lang','licenses')){
            $directory=Resolve-CandidatePath $output $name
            if(-not [IO.Directory]::Exists($directory)){continue}
            $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($directory)
            while($pending.Count){
                foreach($item in [IO.Directory]::EnumerateFileSystemEntries($pending.Pop())){
                    if([IO.File]::GetAttributes($item) -band [IO.FileAttributes]::ReparsePoint){throw 'Linked runtime files cannot be published.'}
                    if([IO.Directory]::Exists($item)){$pending.Push($item)}else{[void]$files.Add($item.Substring($output.Length+1).Replace('\','/'))}
                    if($files.Count -gt 32768){throw 'Candidate runtime file budget exceeded.'}
                }
            }
        }
        $profile='product-runtime'
    }elseif($Action -eq 'tests'){
        $coveragePath=Join-Path $Context.runRoot ('.build/collaboration/runs/'+$Context.runId+'.candidate-tests.json')
        if(-not [IO.File]::Exists($coveragePath)){return $null}
        $coverage=Read-CandidateJson $coveragePath
        if($coverage.runId -ne $Context.runId -or $coverage.candidate -ne $Context.snapshot -or $coverage.exitCode -ne 0){throw 'Test runtime export differs from its passed candidate coverage.'}
        foreach($file in @($coverage.binaries|ForEach-Object path)+@($coverage.runtimeFiles|ForEach-Object file)){
            $absolute=if([IO.Path]::IsPathRooted($file)){[IO.Path]::GetFullPath($file)}else{[IO.Path]::GetFullPath((Join-Path $Context.source $file))}
            foreach($prefix in @($output,(Join-Path $Context.source ('.build/'+$Context.configuration)),(Join-Path $Context.source ('.build_debug/'+$Context.configuration)))){
                $prefix=[IO.Path]::GetFullPath($prefix).TrimEnd('\','/')
                if($absolute.StartsWith($prefix+'\',[StringComparison]::OrdinalIgnoreCase)){
                    $relative=$absolute.Substring($prefix.Length+1).Replace('\','/')
                    # Never copy a portable profile or a user's layout into an
                    # artifact merely because a test mentioned it.
                    if($relative.Split('/')[0] -ne 'data'){[void]$files.Add($relative)}
                }
            }
        }
        if(-not $files.Count){return $null}
    }else{return $null}
    return [pscustomobject]@{root=$output;profile=$profile;files=@($files|Sort-Object)}
}
function Publish-CandidateRuntime($Context,[string]$Action,[scriptblock]$CheckCancellation={}) {
    $runtimeLease=Open-CandidateRuntimeLease $Context.runRoot
    try {
    $live=Get-CandidateEntryContext $Context.source
    if(-not $live -or $live.runId -ne $Context.runId -or $live.nonce -ne $Context.nonce){throw 'Artifact publication requires its live candidate Run.'}
    Assert-EntryExecutionOwner $Context.source
    $selection=Get-CandidateRuntimeFiles $Context $Action
    if(-not $selection){return $null}
    $snapshot=Read-CandidateManifest $Context.runRoot $Context.snapshot
    if($snapshot.digest -ne $Context.manifestDigest){throw 'Artifact source identity changed.'}
    $directory=Get-CandidateArtifactDirectory $Context.runRoot $Context.runId
    if([IO.Directory]::Exists($directory)){throw 'Artifact path already exists; preserve partial or completed publication.'}
    [void][IO.Directory]::CreateDirectory($directory)
    $payload=Resolve-CandidatePath $directory 'runtime';[void][IO.Directory]::CreateDirectory($payload)
    $opened=[Collections.Generic.List[object]]::new();$records=@();$bytes=0L
    try{
        # Deny writes/replacement to every selected runtime file at once.
        foreach($relative in $selection.files){
            & $CheckCancellation
            if($relative.Replace('\','/').Split('/')[0] -eq 'data'){throw 'User data is not a candidate runtime.'}
            $path=Resolve-CandidatePath $selection.root $relative
            $stream=[IO.File]::Open($path,'Open','Read',[IO.FileShare]::Read)
            $opened.Add([pscustomobject]@{relative=$relative;stream=$stream;path=$path})
            $bytes+=$stream.Length;if($bytes -gt 8GB){throw 'Candidate runtime byte budget exceeded.'}
        }
        $buffer=New-Object byte[] 1048576
        foreach($item in $opened){
            & $CheckCancellation
            $destination=Resolve-CandidatePath $payload $item.relative
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
            $target=[IO.File]::Open($destination,'CreateNew','Write','None')
            try{
                while(($count=$item.stream.Read($buffer,0,$buffer.Length)) -gt 0){& $CheckCancellation;$target.Write($buffer,0,$count)}
                $target.Flush($true)
            }finally{$target.Dispose()}
            $digest=Get-FileDigest $destination
            if($digest -ne (Get-FileDigest $item.path)){throw 'Candidate runtime copy failed integrity verification.'}
            $records+=@([pscustomobject]@{path=$item.relative;sha256=$digest;bytes=$item.stream.Length})
        }
        if($selection.profile -eq 'product-runtime'){
            Import-Module (Join-Path $PSScriptRoot 'deployment_payload.psm1') -Force -Global
            $null=Read-SnowDesktopDeploymentManifest -BuildOutput $payload
        }
        $manifest=[pscustomobject]@{schemaVersion=1;id=$Context.runId;runId=$Context.runId;candidate=$Context.snapshot;
            sourceDigest=$snapshot.manifest.sourceDigest;sourceManifestDigest=$snapshot.digest;sourceHead=$snapshot.manifest.head;
            contextDigest=$snapshot.manifest.context.digest;configuration=$Context.configuration;profile=$selection.profile;
            runtime=$payload;files=$records;bytes=$bytes;createdUtc=[DateTime]::UtcNow.ToString('o');publicationComplete=$true;
            productRuntime=($selection.profile -eq 'product-runtime');externalDependenciesClosed=$false;reuseAllowed=$false;
            scope=$(if($selection.profile -eq 'product-runtime'){'Arranged product runtime; user data excluded.'}else{'Explicit test output files only; source arguments and external runtime dependencies remain in original evidence.'})}
        $jsonPath=Resolve-CandidatePath $directory 'artifact.json'
        # The expected digest is persisted in the producing Run as well.
        $temporary=Resolve-CandidatePath $directory 'artifact.pending.json'
        Write-CandidateJson $manifest $temporary -Immutable
        $digest=Get-FileDigest $temporary
        & $CheckCancellation
        [IO.File]::WriteAllText($jsonPath+'.sha256',$digest,[Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary,$jsonPath)
        return [pscustomobject]@{schemaVersion=1;id=$Context.runId;candidate=$Context.snapshot;manifest=$jsonPath;manifestDigest=$digest;
            runtime=$payload;profile=$selection.profile;files=$records.Count;bytes=$bytes;productRuntime=$manifest.productRuntime;publicationComplete=$true;reuseAllowed=$false}
    }finally{foreach($item in $opened){$item.stream.Dispose()}}
    } finally {$runtimeLease.Dispose()}
}
function Read-CandidateArtifact([string]$Root,[string]$Id,[string]$ExpectedDigest='',[switch]$VerifyFiles) {
    $directory=Get-CandidateArtifactDirectory $Root $Id
    $path=Resolve-CandidatePath $directory 'artifact.json'
    $digest=[IO.File]::ReadAllText($path+'.sha256').Trim()
    if($digest -notmatch '^[a-f0-9]{64}$' -or ($ExpectedDigest -and $digest -ne $ExpectedDigest) -or (Get-FileDigest $path) -ne $digest){throw 'Candidate artifact manifest integrity check failed.'}
    $manifest=Read-CandidateJson $path
    $runtime=Resolve-CandidatePath $directory 'runtime'
    if(-not [IO.Directory]::Exists($runtime)){throw 'Saved runtime was retired by the retention policy; its manifest and validation evidence are preserved. Select a retained build or build again.'}
    if($manifest.schemaVersion -ne 1 -or $manifest.id -ne $Id -or $manifest.runId -ne $Id -or -not $manifest.publicationComplete -or
        $manifest.profile -notin 'product-runtime','test-files' -or $manifest.runtime -ne $runtime -or @($manifest.files).Count -lt 1 -or @($manifest.files).Count -gt 32768){throw 'Invalid candidate artifact identity.'}
    $snapshot=Read-CandidateManifest $Root $manifest.candidate
    if($snapshot.digest -ne $manifest.sourceManifestDigest -or $snapshot.manifest.sourceDigest -ne $manifest.sourceDigest -or
        $snapshot.manifest.context.digest -ne $manifest.contextDigest -or $snapshot.manifest.head -ne $manifest.sourceHead -or
        $manifest.configuration -notin 'Release','Debug' -or $snapshot.manifest.context.binding.configuration -ne $manifest.configuration -or
        $manifest.productRuntime -ne ($manifest.profile -eq 'product-runtime') -or $manifest.reuseAllowed -ne $false){throw 'Candidate artifact source binding changed.'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $total=0L
    foreach($file in $manifest.files){
        $absolute=Resolve-CandidatePath $runtime $file.path
        if(-not $seen.Add($file.path.Replace('\','/')) -or $file.path.Replace('\','/').Split('/')[0] -eq 'data' -or $file.sha256 -notmatch '^[a-f0-9]{64}$' -or $file.bytes -lt 0){throw 'Invalid candidate artifact file identity.'}
        if($VerifyFiles -and (-not [IO.File]::Exists($absolute) -or (Get-Item -LiteralPath $absolute).Length -ne $file.bytes -or (Get-FileDigest $absolute) -ne $file.sha256)){throw 'Candidate artifact runtime file changed.'}
        $total+=$file.bytes;if($total -gt 8GB){throw 'Candidate artifact byte budget exceeded.'}
    }
    if($total -ne $manifest.bytes){throw 'Candidate artifact size identity changed.'}
    if($manifest.productRuntime){
        foreach($name in @('SnowDesktop.exe','snowwidget.exe','SnowDesktop.deployment.json')){
            if(-not $seen.Contains($name)){throw 'Candidate product runtime is incomplete.'}
        }
        if($VerifyFiles){
            Import-Module (Join-Path $PSScriptRoot 'deployment_payload.psm1') -Force -Global
            $deployment=Read-SnowDesktopDeploymentManifest -BuildOutput $runtime
            foreach($file in @($deployment.files)+@($deployment.appxFragments)+@($deployment.notices)){
                $relative=if($file.PSObject.Properties['buildPath'] -and $file.buildPath){$file.buildPath}else{$file.path}
                if(-not $seen.Contains($relative.Replace('\','/'))){throw 'Candidate product runtime omits a deployment dependency.'}
            }
        }
    }
    return [pscustomobject]@{manifest=$manifest;path=$path;digest=$digest;runtime=$runtime}
}
