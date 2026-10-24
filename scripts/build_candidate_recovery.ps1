# Explicit repair of the one frozen source tree. Compiler output is never
# cleared, copied or traversed; old source bytes remain in a non-executable
# recovery archive for inspection, including files outside the managed manifest.
function Assert-CandidateRecoveryLease($Snapshot,$Lease) {
    $store=Get-CandidateStore $Snapshot.manifest.sourceRoot
    if($store.root -ne $Snapshot.store.root -or $store.commonGitDirectory -ne $Snapshot.store.commonGitDirectory){throw 'Recovery snapshot store changed.'}
    $directory=Get-CandidateWorkspaceDirectory $store
    if($Lease.key -ne 'workspace' -or $Lease.directory -ne $directory -or $Lease.source -ne (Resolve-CandidatePath $directory 'source') -or
        $Lease.output -ne (Resolve-CandidatePath $directory 'output') -or $Lease.state -ne (Resolve-CandidatePath $directory 'materialization.json')){
        throw 'Recovery workspace path changed.'
    }
    $lock=Resolve-CandidatePath $directory 'slot.lock'
    if($Lease.lease -isnot [IO.FileStream] -or -not $Lease.lease.CanWrite -or
        [IO.Path]::GetFullPath($Lease.lease.Name) -ne $lock){throw 'An exclusive compiler workspace lease is required for recovery.'}
    $probe=$null
    try {
        # A FileStream's Name/CanWrite alone cannot prove exclusive ownership.
        # A permissively shared fake handle would let this second writer open.
        $probe=[IO.File]::Open($lock,'Open','ReadWrite',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    }catch [IO.IOException]{
        if(($_.Exception.HResult -band 0xffff) -notin 32,33){throw}
        return $store
    }finally{if($probe){$probe.Dispose()}}
    throw 'An exclusive compiler workspace lease is required for recovery.'
}

function Get-CandidateRecoveryInventory([string]$Source,[string]$Output,$Opened) {
    $records=[Collections.Generic.List[object]]::new()
    if(-not [IO.Directory]::Exists($Source)){return}
    $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($Source)
    $bytes=0L
    while($pending.Count){
        $directory=$pending.Pop()
        foreach($path in [IO.Directory]::EnumerateFileSystemEntries($directory)){
            $relative=$path.Substring($Source.Length+1).Replace('\','/')
            $attributes=[IO.File]::GetAttributes($path)
            if($attributes -band [IO.FileAttributes]::ReparsePoint){
                $item=Get-Item -LiteralPath $path -Force
                if($directory -ne $Source -or $relative -notin '.build','.build_debug' -or
                    $item.LinkType -ne 'Junction' -or @($item.Target).Count -ne 1 -or
                    [IO.Path]::GetFullPath([string]@($item.Target)[0]) -ne $Output){throw 'Recovery source contains an unsafe reparse point.'}
                $records.Add([pscustomobject]@{path=$relative;kind='output-alias';target=$Output})
                continue
            }
            $null=Resolve-CandidatePath $Source $relative
            if($attributes -band [IO.FileAttributes]::Directory){
                $pending.Push($path);$records.Add([pscustomobject]@{path=$relative;kind='directory'})
                if($records.Count -gt 32768){throw 'Recovery source entry budget exceeded.'};continue
            }
            # Deny writes during inspection. These streams are closed before
            # renaming the source directory; the workspace lease stays held.
            $stream=[IO.File]::Open($path,'Open','Read',([IO.FileShare]::Read -bor [IO.FileShare]::Delete))
            $Opened.Add($stream)
            $bytes+=$stream.Length;if($bytes -gt 4GB){throw 'Recovery source byte budget exceeded.'}
            $records.Add([pscustomobject]@{path=$relative;kind='file';bytes=$stream.Length;sha256=(Get-FileDigest $path);
                lastWriteTicks=[IO.File]::GetLastWriteTimeUtc($path).Ticks.ToString()})
            if($records.Count -gt 32768){throw 'Recovery source entry budget exceeded.'}
        }
    }
    return $records.ToArray()
}

function Repair-CandidateWorkspace($Snapshot,$Lease,[scriptblock]$CheckCancellation={param($Phase)}) {
    $store=Assert-CandidateRecoveryLease $Snapshot $Lease
    $snapshot=Read-CandidateManifest $Snapshot.manifest.sourceRoot $Snapshot.manifest.id -VerifyObjects -StoreContext $store
    if($snapshot.path -ne $Snapshot.path -or $snapshot.digest -ne $Snapshot.digest){throw 'Recovery snapshot identity changed.'}
    $manifest=$snapshot.manifest
    $actual=Get-CandidateContext $manifest.files $manifest.context.binding.configuration $(if(Get-Field $manifest.context.binding 'checkoutRoot'){$manifest.sourceRoot}else{''})
    if($actual.digest -ne $manifest.context.digest){throw 'Recovery tools/environment changed; capture a new snapshot.'}
    if(-not [IO.Directory]::Exists($Lease.output)){throw 'Recovery requires the existing compiler output directory.'}
    $opened=[Collections.Generic.List[object]]::new()
    $archive=$null;$journal=$null;$receipt=$null
    try {
        $inventory=@(Get-CandidateRecoveryInventory $Lease.source $Lease.output $opened)
        $stateStream=$null;$previousCompiler=$null
        if([IO.File]::Exists($Lease.state)){
            $stateStream=[IO.File]::Open($Lease.state,'Open','Read',([IO.FileShare]::Read -bor [IO.FileShare]::Delete))
            $opened.Add($stateStream)
            try{
                $previous=Read-CandidateJson $Lease.state
                $oldSnapshot=Read-CandidateManifest $manifest.sourceRoot $previous.snapshot -StoreContext $store
                if($previous.manifestDigest -eq $oldSnapshot.digest){$previousCompiler=Get-CandidateCompilerContextDigest $oldSnapshot.manifest.context}
            }catch{}
        }
        & $CheckCancellation 'before-archive'
        # Windows can deny the parent directory rename while child files are
        # open, even with FILE_SHARE_DELETE. Inspection is complete and the
        # actual workspace lease still excludes other materializers/executors.
        foreach($stream in $opened){$stream.Dispose()}
        $opened.Clear()
        $archive=Resolve-CandidatePath $Lease.directory ('recovery/'+[Guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($archive)
        $journal=Resolve-CandidatePath $archive 'recovery.json'
        $receipt=[pscustomobject]@{schemaVersion=1;status='prepared';snapshot=$manifest.id;manifestDigest=$snapshot.digest;
            workspace=$Lease.directory;archive=$archive;source=$Lease.source;output=$Lease.output;files=$inventory;
            archivedSource=$false;archivedState=$false;startedUtc=[DateTime]::UtcNow.ToString('o');completedUtc=$null;error=''}
        Write-CandidateJson $receipt $journal -Immutable
        if([IO.Directory]::Exists($Lease.source)){
            [IO.Directory]::Move($Lease.source,(Resolve-CandidatePath $archive 'source'))
            $receipt.archivedSource=$true
        }
        if($stateStream){
            [IO.File]::Move($Lease.state,(Resolve-CandidatePath $archive 'materialization.json'))
            $receipt.archivedState=$true
        }
        $receipt.status='archived';Write-CandidateJson $receipt $journal
        & $CheckCancellation 'after-archive'
        # Set-CandidateSlotSource sees a clean source and no previous state.
        # A failed restoration can itself be archived on the next explicit try.
        $prepared=Set-CandidateSlotSource $snapshot $Lease
        $compilerContext=Get-CandidateCompilerContextDigest $manifest.context
        $resetPath=Resolve-CandidatePath $Lease.output 'collaboration/configuration-reset.json'
        $resetRequired=$previousCompiler -ne $compilerContext
        if([IO.File]::Exists($resetPath)){
            $existingReset=$null
            try{$existingReset=Read-CandidateJson $resetPath}catch{}
            if(-not $existingReset -or (Get-Field $existingReset 'schemaVersion' 0) -ne 1 -or (Get-Field $existingReset 'compilerContextDigest' '') -ne $compilerContext){
                [IO.File]::Copy($resetPath,(Resolve-CandidatePath $archive 'configuration-reset.json'),$false)
                $resetRequired=$true
            }
        }
        if($resetRequired){
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($resetPath))
            Write-CandidateJson ([pscustomobject]@{schemaVersion=1;compilerContextDigest=$compilerContext;configured=@()}) $resetPath
        }
        $oldFiles=@{}
        foreach($file in $inventory){if($file.kind -eq 'file'){$oldFiles[$file.path]=$file}}
        $preserved=0
        foreach($file in $manifest.files){
            & $CheckCancellation 'restore-input'
            if($file.state -ne 'present' -or -not $oldFiles.ContainsKey($file.path) -or $oldFiles[$file.path].sha256 -ne $file.sha256){continue}
            $path=Resolve-CandidatePath $Lease.source $file.path
            [IO.File]::SetLastWriteTimeUtc($path,[DateTime]::new([long]$oldFiles[$file.path].lastWriteTicks,[DateTimeKind]::Utc));$preserved++
        }
        foreach($name in @('.build','.build_debug')){
            $alias=[IO.Path]::GetFullPath((Join-Path $Lease.source $name))
            New-Item -ItemType Junction -Path $alias -Target $Lease.output -ErrorAction Stop|Out-Null
        }
        $receipt.status='complete';$receipt.completedUtc=[DateTime]::UtcNow.ToString('o');Write-CandidateJson $receipt $journal
        return [pscustomobject]@{snapshot=$manifest.id;source=$Lease.source;output=$Lease.output;archive=$archive;
            restoredFiles=$prepared.copied;unchangedTimesPreserved=$preserved;outputPreserved=$true;recovered=$true;receipt=$journal}
    }catch{
        if($receipt){
            $receipt.status='failed';$receipt.error=$_.Exception.Message;$receipt.completedUtc=[DateTime]::UtcNow.ToString('o')
            try{Write-CandidateJson $receipt $journal}catch{}
        }
        throw
    }finally{foreach($stream in $opened){$stream.Dispose()}}
}
