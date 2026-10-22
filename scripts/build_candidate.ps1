[CmdletBinding()]
param(
    [Alias('Verb')][ValidateSet('candidate')][string]$CandidateVerb='candidate',
    [Alias('Command')][ValidateSet('inspect','materialize')][string]$CandidateCommand='inspect',
    [Alias('Snapshot')][ValidatePattern('^[a-f0-9]{32}$')][string]$CandidateSnapshot,
    [Alias('Task')][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$CandidateTask,
    [Alias('Slot')][ValidatePattern('^[a-z0-9][a-z0-9-]{0,31}$')][string]$CandidateSlotName='default'
)

# Snapshot content is separate from Git's index and from mutable build output.
# This foundation deliberately does not grant an executor or release a batch.
function Resolve-CandidatePath([string]$Base,[string]$Relative) {
    $relative=$Relative.Replace('\','/')
    if(-not $relative -or [IO.Path]::IsPathRooted($relative) -or $relative -match '[:*?"<>|\x00-\x1f]' -or
        @($relative.Split('/')|Where-Object {$_ -in '', '.', '..' -or $_.EndsWith('.') -or $_.EndsWith(' ') -or $_ -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)'}).Count){throw 'Unsafe candidate relative path.'}
    $basePath=[IO.Path]::GetFullPath($Base).TrimEnd('\','/')
    $path=[IO.Path]::GetFullPath((Join-Path $basePath $relative))
    if(-not $path.StartsWith($basePath+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Candidate path escaped its root.'}
    $cursor=$path
    while($cursor){
        try{
            $attributes=[IO.File]::GetAttributes($cursor)
            if($attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Candidate paths cannot traverse a reparse point.'}
        }catch{
            if($_.Exception -isnot [IO.FileNotFoundException] -and $_.Exception -isnot [IO.DirectoryNotFoundException] -and
                $_.Exception.InnerException -isnot [IO.FileNotFoundException] -and $_.Exception.InnerException -isnot [IO.DirectoryNotFoundException]){throw}
        }
        $parent=[IO.Path]::GetDirectoryName($cursor)
        if($parent -eq $cursor){break};$cursor=$parent
    }
    return $path
}
function Test-CandidateSourcePath([string]$Relative) {
    return $Relative.Replace('\','/').Split('/')[0] -notin '.git','.build','.build_debug','.codex-probes','.codex-remote-attachments',
        '.tmp','.vs','.vscode','.remotion','artifacts','out','build','bin','obj','release','TestResults','website','SnowDesktop_SteamAssets'
}
function Get-CandidateStore([string]$Root,[switch]$Create) {
    $query=Invoke-InputGit $Root 'rev-parse --path-format=absolute --git-common-dir'
    if($query.code -ne 0 -or -not [IO.Path]::IsPathRooted($query.output.Trim())){throw 'Candidate Git resource scope is unavailable.'}
    $common=[IO.Path]::GetFullPath($query.output.Trim())
    $store=Resolve-CandidatePath $common 'snowdesktop-candidates'
    # Stable paths keep CMake/FileTracker caches warm across snapshots. Do not
    # create one checkout per digest, or use AppData/TEMP compiler exclusions.
    $parent=if([IO.Path]::GetFileName($common) -eq '.git'){[IO.Path]::GetDirectoryName($common)}else{$Root}
    $slots=Resolve-CandidatePath $parent '.build/candidate-slots'
    if($Create){
        foreach($path in @($store,(Join-Path $store 'objects'),(Join-Path $store 'manifests'))){[void][IO.Directory]::CreateDirectory($path)}
    }
    return [pscustomobject]@{root=$store;commonGitDirectory=$common;slots=$slots}
}
function Write-CandidateJson($Value,[string]$Path,[switch]$Immutable) {
    $temp=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp';$backup=$temp+'.bak'
    try {
        $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 16 -Compress))
        $stream=[IO.File]::Open($temp,'CreateNew','Write','None')
        try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        if(-not $Immutable -and [IO.File]::Exists($Path)){[IO.File]::Replace($temp,$Path,$backup)}else{[IO.File]::Move($temp,$Path)}
    } finally {
        foreach($file in @($temp,$backup)){if([IO.File]::Exists($file)){[IO.File]::Delete($file)}}
    }
}
function Read-CandidateJson([string]$Path) {
    $stream=[IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try{
        if($stream.Length -gt 16MB){throw 'Candidate metadata exceeds its byte budget.'}
        $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
        try{return $reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
    }finally{$stream.Dispose()}
}
function Get-CandidatePaths([string]$Root,[string]$ExtraInputs='') {
    $listing=Invoke-InputGit $Root 'ls-files -z --cached --others --exclude-standard'
    if($listing.code -ne 0){throw 'Cannot enumerate candidate files.'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $windowsPaths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($extra in @($ExtraInputs.Split(',')|Where-Object {$_})){
        $absolute=Resolve-CandidatePath $Root $extra
        if(-not (Test-CandidateSourcePath $extra)){throw 'Generated candidate inputs must be outside build/output/state directories.'}
        if(-not [IO.File]::Exists($absolute)){throw 'An explicitly declared candidate input must be an existing file.'}
    }
    $localPreset=if([IO.File]::Exists((Join-Path $Root 'CMakeUserPresets.json'))){@('CMakeUserPresets.json')}else{@()}
    foreach($item in @($listing.output.Split([char]0))+@($ExtraInputs.Split(','))+@($localPreset)){
        if(-not $item){continue};$item=$item.Replace('\','/')
        if(-not (Test-CandidateSourcePath $item)){
            if($ExtraInputs.Split(',') -contains $item){throw 'Generated candidate inputs must be outside build/output/state directories.'}
            continue
        }
        $null=Resolve-CandidatePath $Root $item
        if($paths.Contains($item)){continue}
        if(-not $windowsPaths.Add($item)){throw 'Candidate contains case-colliding Windows paths.'}
        [void]$paths.Add($item)
    }
    if($paths.Count -gt 16384){throw 'Candidate file budget exceeded.'}
    [string[]]$ordered=@($paths);[Array]::Sort($ordered,[StringComparer]::Ordinal)
    return $ordered
}
function Get-CandidateContext($Files,[string]$Configuration) {
    $environment=[ordered]@{}
    foreach($name in @('CMAKE_GENERATOR','CMAKE_GENERATOR_PLATFORM','CMAKE_GENERATOR_TOOLSET','PATH','INCLUDE','LIB','LIBPATH',
        'SNOWDESKTOP_STEAMWORKS_SDK_ROOT','SNOWDESKTOP_STEAMWORKS_SDK_PATH','VCToolsInstallDir','WindowsSdkDir','WindowsSDKVersion')){
        $environment[$name]=Get-VerificationTextDigest ([string][Environment]::GetEnvironmentVariable($name,'Process'))
    }
    $tools=[ordered]@{}
    foreach($name in @('cmake.exe','ctest.exe','git.exe')){
        $tool=Get-Command $name -CommandType Application -ErrorAction SilentlyContinue|Select-Object -First 1
        $tools[$name]=if($tool){[pscustomobject]@{path=$tool.Source;sha256=(Get-FileDigest $tool.Source)}}else{$null}
    }
    $runtime=Get-BuildPowerShell
    $tools['powershell']=[pscustomobject]@{path=$runtime;sha256=(Get-FileDigest $runtime)}
    $presets=@($Files|Where-Object {$_.path -in 'CMakePresets.json','CMakeUserPresets.json'}|Select-Object path,state,sha256)
    $binding=[pscustomobject]@{schemaVersion=1;configuration=$Configuration;presets=$presets;environment=$environment;tools=$tools}
    return [pscustomobject]@{digest=(Get-VerificationTextDigest ($binding|ConvertTo-Json -Depth 10 -Compress));binding=$binding;
        externalDependenciesClosed=$false;scope='Observed configuration, tools and build environment; external SDK/package contents are not yet closed.'}
}
function Get-CandidateSourceDigest($Files,[string]$ContextDigest) {
    $text=[Text.StringBuilder]::new();[void]$text.Append("sha256-candidate-source-v1`n").Append($ContextDigest).Append("`n")
    foreach($file in $Files){[void]$text.Append($file.path).Append([char]0).Append($file.state).Append([char]0).Append($file.sha256).Append([char]0).Append($file.bytes).Append("`n")}
    return Get-VerificationTextDigest $text.ToString()
}
function New-CandidateSnapshot([string]$Root,[string]$TaskId,[long]$RequirementsRevision,[string]$Configuration,[string]$ExtraInputs,[string]$ExpectedDigest,[scriptblock]$CheckCancellation) {
    Assert-EntryExecutionOwner $Root
    $clock=[Diagnostics.Stopwatch]::StartNew();$store=Get-CandidateStore $Root -Create
    $lease=[IO.File]::Open((Join-Path $store.root 'store.lock'),'OpenOrCreate','ReadWrite','None')
    $opened=[Collections.Generic.List[object]]::new();$temporary=@()
    try {
        $paths=@(Get-CandidatePaths $Root $ExtraInputs);$files=@();$total=0L;$reused=0
        $buffer=New-Object byte[] 1048576
        foreach($path in $paths){
            & $CheckCancellation
            $absolute=Resolve-CandidatePath $Root $path
            if([IO.Directory]::Exists($absolute)){throw 'Nested repositories/submodules and directory inputs require an explicit snapshot adapter.'}
            if([IO.File]::Exists($absolute)){
                # Keep every present input open at once before reading bytes;
                # deny cooperating/external writes, deletion and replacement.
                $stream=[IO.File]::Open($absolute,'Open','Read',[IO.FileShare]::Read)
                $opened.Add([pscustomobject]@{path=$path;stream=$stream;ticks=[IO.File]::GetLastWriteTimeUtc($absolute).Ticks.ToString()})
                $total+=$stream.Length;if($total -gt 4GB){throw 'Candidate source byte budget exceeded.'}
            }else{$opened.Add([pscustomobject]@{path=$path;stream=$null;ticks=$null})}
        }
        # All present files remain locked through publication. Recheck the
        # name set and endpoint identity; no TTL or heartbeat is a write fence.
        if((@((Get-CandidatePaths $Root $ExtraInputs)) -join "`n") -cne ($paths -join "`n")){throw 'Candidate file set changed during capture.'}
        if((Get-BuildInputIdentity $Root).digest -ne $ExpectedDigest){throw 'Candidate inputs changed before capture acquired its file handles.'}
        foreach($entry in $opened){
            & $CheckCancellation
            if(-not $entry.stream){$files+=@([pscustomobject]@{path=$entry.path;state='deleted';sha256=$null;bytes=0L;lastWriteTicks=$null});continue}
            $sha=[Security.Cryptography.SHA256]::Create()
            try{$digest=[BitConverter]::ToString($sha.ComputeHash($entry.stream)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
            $object=Resolve-CandidatePath $store.root ('objects/'+$digest)
            if([IO.File]::Exists($object)){
                if((Get-FileDigest $object) -ne $digest){throw 'Candidate content object is corrupt; preserve it for investigation.'};$reused++
            }else{
                $temp=$object+'.'+[Guid]::NewGuid().ToString('N')+'.tmp';$temporary+=@($temp)
                $destination=[IO.File]::Open($temp,'CreateNew','Write','None')
                try{
                    $entry.stream.Position=0
                    while(($count=$entry.stream.Read($buffer,0,$buffer.Length)) -gt 0){& $CheckCancellation;$destination.Write($buffer,0,$count)}
                    $destination.Flush($true)
                }finally{$destination.Dispose()}
                [IO.File]::Move($temp,$object)
            }
            $files+=@([pscustomobject]@{path=$entry.path;state='present';sha256=$digest;bytes=$entry.stream.Length;lastWriteTicks=$entry.ticks})
        }
        if((@((Get-CandidatePaths $Root $ExtraInputs)) -join "`n") -cne ($paths -join "`n") -or (Get-BuildInputIdentity $Root).digest -ne $ExpectedDigest){throw 'Candidate inputs changed before publication.'}
        & $CheckCancellation
        $context=Get-CandidateContext $files $Configuration;$id=[Guid]::NewGuid().ToString('N')
        $head=Invoke-InputGit $Root 'rev-parse --verify HEAD'
        $manifest=[pscustomobject]@{schemaVersion=1;id=$id;sourceRoot=$Root;commonGitDirectory=$store.commonGitDirectory;task=$TaskId;requirementsRevision=$RequirementsRevision;
            capturedUtc=[DateTime]::UtcNow.ToString('o');head=$(if($head.code -eq 0){$head.output.Trim()}else{$null});buildInputDigest=$ExpectedDigest;
            sourceDigest=(Get-CandidateSourceDigest $files $context.digest);context=$context;extraInputs=$ExtraInputs;files=$files;captureComplete=$true;
            executionReady=$false;boundary='Registered writers are fenced by the output lease; open file handles deny writes/replacement during capture. No arbitrary-filesystem-editor or external SDK closure claim.'}
        $manifestPath=Resolve-CandidatePath $store.root ('manifests/'+$id+'.json')
        # Publish checksum first, manifest last. Readers enumerate only complete
        # JSON manifests. Unique temporary/orphan checksum files are not results.
        $json=$manifest|ConvertTo-Json -Depth 16 -Compress
        $manifestDigest=Get-VerificationTextDigest $json
        [IO.File]::WriteAllText($manifestPath+'.sha256',$manifestDigest,[Text.UTF8Encoding]::new($false))
        Write-CandidateJson $manifest $manifestPath -Immutable
        return [pscustomobject]@{schemaVersion=1;id=$id;manifest=$manifestPath;manifestDigest=$manifestDigest;sourceDigest=$manifest.sourceDigest;
            sourceRoot=$Root;fileCount=$files.Count;presentFiles=@($files|Where-Object state -eq 'present').Count;deletedFiles=@($files|Where-Object state -eq 'deleted').Count;
            bytes=$total;objectsReused=$reused;captureSeconds=$clock.Elapsed.TotalSeconds;snapshotReady=$true;executionReady=$false;externalDependenciesClosed=$false}
    } finally {
        foreach($entry in $opened){if($entry.stream){$entry.stream.Dispose()}}
        foreach($temp in $temporary){if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
        $lease.Dispose()
    }
}
function Read-CandidateManifest([string]$Root,[string]$Id,[switch]$VerifyObjects,$StoreContext=$null) {
    if($Id -notmatch '^[a-f0-9]{32}$'){throw 'Candidate snapshot ID is required.'}
    $store=if($StoreContext){$StoreContext}else{Get-CandidateStore $Root}
    $path=Resolve-CandidatePath $store.root ('manifests/'+$Id+'.json')
    $expected=[IO.File]::ReadAllText($path+'.sha256').Trim()
    if($expected -notmatch '^[a-f0-9]{64}$' -or (Get-FileDigest $path) -cne $expected){throw 'Candidate manifest integrity check failed.'}
    $manifest=Read-CandidateJson $path
    if($manifest.schemaVersion -ne 1 -or $manifest.id -ne $Id -or -not $manifest.captureComplete -or $manifest.commonGitDirectory -ne $store.commonGitDirectory -or
        @($manifest.files).Count -gt 16384 -or (Get-CandidateSourceDigest $manifest.files $manifest.context.digest) -ne $manifest.sourceDigest){throw 'Invalid candidate manifest identity.'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($file in $manifest.files){
        $null=Resolve-CandidatePath (Join-Path $store.slots 'validation/source') $file.path
        if(-not (Test-CandidateSourcePath $file.path) -or -not $seen.Add($file.path) -or $file.state -notin 'present','deleted'){throw 'Invalid candidate file identity.'}
        if($file.state -eq 'present'){
            if($file.sha256 -notmatch '^[a-f0-9]{64}$' -or $file.bytes -lt 0 -or $file.lastWriteTicks -notmatch '^[0-9]{1,19}$'){throw 'Invalid candidate object identity.'}
            $object=Resolve-CandidatePath $store.root ('objects/'+$file.sha256)
            if($VerifyObjects -and ((Get-Item -LiteralPath $object).Length -ne $file.bytes -or (Get-FileDigest $object) -ne $file.sha256)){throw 'Candidate content object integrity check failed.'}
        }
    }
    return [pscustomobject]@{manifest=$manifest;path=$path;digest=$expected;store=$store}
}
function Enter-CandidateSlot($Snapshot,[string]$Name) {
    if($Name -notmatch '^[a-z0-9][a-z0-9-]{0,31}$'){throw 'Invalid candidate slot name.'}
    $key=$Snapshot.manifest.context.digest.Substring(0,16)+'-'+$Name
    $directory=Resolve-CandidatePath $Snapshot.store.slots $key
    [void][IO.Directory]::CreateDirectory($directory)
    try{$lease=[IO.File]::Open((Join-Path $directory 'slot.lock'),'OpenOrCreate','ReadWrite','None')}
    catch [IO.IOException]{throw 'Candidate slot is busy; existing materialization/executor retains its files.'}
    return [pscustomobject]@{key=$key;directory=$directory;source=(Join-Path $directory 'source');output=(Join-Path $directory 'output');state=(Join-Path $directory 'materialization.json');lease=$lease}
}
function Set-CandidateSlotSource($Snapshot,$Lease) {
    if(-not $Lease.lease -or -not $Lease.lease.CanWrite){throw 'A live candidate slot lease is required.'}
    $manifest=$Snapshot.manifest
    $actual=Get-CandidateContext $manifest.files $manifest.context.binding.configuration
    if($actual.digest -ne $manifest.context.digest){throw 'Candidate observed tools/environment changed; capture a new context.'}
    $old=if([IO.File]::Exists($Lease.state)){Read-CandidateJson $Lease.state}else{$null}
    if($old -and ($old.slot -ne $Lease.key -or $old.contextDigest -ne $manifest.context.digest)){throw 'Candidate slot context changed; preserve the cache and choose another slot.'}
    [void][IO.Directory]::CreateDirectory($Lease.source);[void][IO.Directory]::CreateDirectory($Lease.output)
    $previous=@{}
    if($old){
        $oldSnapshot=Read-CandidateManifest $manifest.sourceRoot $old.snapshot -VerifyObjects -StoreContext $Snapshot.store
        if($old.manifestDigest -ne $oldSnapshot.digest){throw 'Candidate slot provenance changed.'}
        foreach($file in $oldSnapshot.manifest.files){$previous[$file.path]=$file}
    }
    $next=@{};foreach($file in $manifest.files){$next[$file.path]=$file}
    $copied=0;$kept=0;$removed=0
    # Validate every old managed input before any mutation. Never clean the
    # output tree or overwrite an unrecorded/manual change in a stable slot.
    foreach($file in $previous.Values){
        $path=Resolve-CandidatePath $Lease.source $file.path
        if($file.state -eq 'present' -and (-not [IO.File]::Exists($path) -or (Get-FileDigest $path) -ne $file.sha256)){throw 'Candidate slot source was modified outside its lease.'}
        if($file.state -eq 'deleted' -and (Test-Path -LiteralPath $path)){throw 'Candidate slot deleted input was recreated outside its lease.'}
    }
    foreach($file in $manifest.files){
        $path=Resolve-CandidatePath $Lease.source $file.path
        if(-not $previous.ContainsKey($file.path) -and (Test-Path -LiteralPath $path)){throw 'Candidate slot has an unmanaged input collision.'}
    }
    foreach($file in $previous.Values){
        if($file.state -eq 'present' -and (-not $next.ContainsKey($file.path) -or $next[$file.path].state -eq 'deleted')){
            $path=Resolve-CandidatePath $Lease.source $file.path
            [IO.File]::Delete($path);$removed++
        }
    }
    foreach($file in $manifest.files){
        if($file.state -ne 'present'){continue}
        $path=Resolve-CandidatePath $Lease.source $file.path
        if($previous.ContainsKey($file.path) -and $previous[$file.path].state -eq 'present' -and $previous[$file.path].sha256 -eq $file.sha256){$kept++;continue}
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        $object=Resolve-CandidatePath $Snapshot.store.root ('objects/'+$file.sha256)
        [IO.File]::Copy($object,$path,$true)
        if((Get-FileDigest $path) -ne $file.sha256){throw 'Candidate copied content failed integrity verification.'}
        # A newer candidate may have been edited while its predecessor was
        # compiling. Its captured timestamp can predate that predecessor's
        # objects. Mark changed bytes as materialized now so timestamp-based
        # generators rebuild them; unchanged managed inputs keep their times.
        [IO.File]::SetLastWriteTimeUtc($path,[DateTime]::UtcNow);$copied++
    }
    $record=[pscustomobject]@{schemaVersion=1;slot=$Lease.key;snapshot=$manifest.id;manifestDigest=$Snapshot.digest;contextDigest=$manifest.context.digest;
        sourceDigest=$manifest.sourceDigest;source=$Lease.source;output=$Lease.output;files=$manifest.files;materializedUtc=[DateTime]::UtcNow.ToString('o')}
    Write-CandidateJson $record $Lease.state
    return [pscustomobject]@{snapshot=$manifest.id;slot=$Lease.key;source=$Lease.source;output=$Lease.output;sourceDigest=$manifest.sourceDigest;
        copied=$copied;kept=$kept;removed=$removed;executionReady=$false;externalDependenciesClosed=$false;
        observation='Prepared cache only. The CLI releases its slot lease on return; an executor must retain a lease and revalidate the source before use.'}
}

if($MyInvocation.InvocationName -eq '.'){return}
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
try {
    $snapshotRecord=Read-CandidateManifest $root $CandidateSnapshot -VerifyObjects
    if($CandidateCommand -eq 'inspect'){$snapshotRecord.manifest|ConvertTo-Json -Depth 16;exit 0}
    $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
    if(-not $CandidateTask -or -not $state -or @((Get-Field (Get-Field $state 'taskLedger') 'tasks' @())|Where-Object {$_.id -eq $CandidateTask -and -not $_.closedUtc}).Count -ne 1){throw 'Materialization requires an active registered Task.'}
    $slotLease=Enter-CandidateSlot $snapshotRecord $CandidateSlotName
    try{Set-CandidateSlotSource $snapshotRecord $slotLease|ConvertTo-Json -Depth 8}finally{$slotLease.lease.Dispose()}
    exit 0
}catch{[Console]::Error.WriteLine($_.Exception.Message);exit 2}
