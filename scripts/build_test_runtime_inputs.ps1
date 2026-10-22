# Reviewed file reads are one part of runtime closure. DLL loading and external
# state remain unclosed; these observations never authorize result reuse.
function Get-TestRuntimeTokenDigest([string[]]$Tokens) {
    $text=New-Object Text.StringBuilder
    foreach($token in $Tokens){[void]$text.Append(([Text.Encoding]::UTF8.GetByteCount($token)).ToString([Globalization.CultureInfo]::InvariantCulture));[void]$text.Append(':');[void]$text.Append($token)}
    return Get-TestDependencyDigest $text.ToString()
}
function Resolve-TestRuntimePath($Context,[string]$Relative) {
    $relative=$Relative.Replace('\','/')
    if(-not $relative -or [IO.Path]::IsPathRooted($relative)){throw 'Runtime input requires a relative repository path.'}
    $path=$Context.root
    foreach($part in $relative.Split('/')){
        if(-not $part -or $part -in '.','..' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
           $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)'){throw 'Invalid runtime input path component.'}
        $path=Join-Path $path $part
        if([IO.File]::Exists($path) -or [IO.Directory]::Exists($path)){
            if([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint){
                # The standard private candidate deliberately maps .build to
                # its output. Accept only its authenticated live context.
                $allowed=$false
                if($path -eq (Join-Path $Context.root '.build') -and (Get-Command Get-CandidateEntryContext -ErrorAction SilentlyContinue)){
                    $candidate=Get-CandidateEntryContext $Context.root;$item=Get-Item -LiteralPath $path -Force
                    $allowed=$candidate -and @($item.Target).Count -eq 1 -and [IO.Path]::GetFullPath([string]@($item.Target)[0]) -eq $candidate.output
                }
                if(-not $allowed){throw 'Runtime input traverses an unreviewed reparse point.'}
            }
        }
    }
    return [IO.Path]::GetFullPath($path)
}
function Initialize-TestRuntimeContracts($Context) {
    if($Context.PSObject.Properties['runtimeContracts']){return}
    $Context|Add-Member NoteProperty runtimeContracts @{}
    $Context|Add-Member NoteProperty runtimeRegistryKey $null
    $Context|Add-Member NoteProperty runtimeRegistryIssue ''
    try{
        $path=Resolve-TestRuntimePath $Context 'scripts/test_runtime_contracts.json'
        if(-not [IO.File]::Exists($path)){return}
        $Context.runtimeRegistryKey=Add-TestDependencyFile $Context $path
        if((Get-Item -LiteralPath $path).Length -gt 4MB){throw 'Runtime contract registry exceeds its byte budget.'}
        $registry=[IO.File]::ReadAllText($path)|ConvertFrom-Json
        if((Get-TestDependencyField $registry 'schemaVersion') -ne 1 -or -not $registry.PSObject.Properties['tests'] -or @($registry.tests).Count -gt 4096){throw 'Unsupported runtime contract registry.'}
        foreach($contract in @($registry.tests)){
            $name=[string](Get-TestDependencyField $contract 'test')
            if($name -notmatch '^[A-Za-z0-9_-]{1,160}$' -or $Context.runtimeContracts.ContainsKey($name)){throw 'Runtime contract name is invalid or repeated.'}
            $Context.runtimeContracts[$name]=$contract
        }
    }catch{$Context.runtimeRegistryIssue=$_.Exception.Message;$Context.runtimeContracts=@{}}
}
function Get-TestRuntimeDirectory($Context,[string]$Path,[string]$Kind) {
    $entries=New-Object 'System.Collections.Generic.List[string]';$keys=New-Object 'System.Collections.Generic.List[string]'
    if($Kind -eq 'directory-presence'){return [pscustomobject]@{entries=@();fileIds=@()}}
    $pending=New-Object 'System.Collections.Generic.Queue[string]';$pending.Enqueue($Path)
    while($pending.Count){
        $directory=$pending.Dequeue()
        foreach($child in [IO.Directory]::EnumerateFileSystemEntries($directory)){
            if($entries.Count -ge 32768){throw 'Runtime directory entry budget exceeded.'}
            $attributes=[IO.File]::GetAttributes($child)
            if($attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Runtime directory contains an unreviewed reparse point.'}
            $relative=$child.Substring($Path.Length+1).Replace('\','/')
            if($attributes -band [IO.FileAttributes]::Directory){
                $entries.Add('directory:'+ $relative)
                if($Kind -eq 'directory-tree'){
                    if($relative.Split('/').Count -ge 64){throw 'Runtime directory depth budget exceeded.'};$pending.Enqueue($child)
                }
            }else{
                $value='file:'+ $relative
                if($Kind -eq 'directory-tree'){$key=Add-TestDependencyFile $Context $child;$keys.Add($key);$value+=':'+ $Context.files[$key].status+':'+$Context.files[$key].sha256}
                $entries.Add($value)
            }
        }
    }
    return [pscustomobject]@{entries=@(Get-TestDependencyOrdered @($entries));fileIds=@($keys)}
}
function Get-TestRuntimeInputs($Context,[string]$Name) {
    Initialize-TestRuntimeContracts $Context
    $result=[pscustomobject]@{status='unreviewed';sourceReview='unreviewed';digest=$null;fileIds=@();bindings=@();issues=@();runtimeClosure='unclosed';reuseAllowed=$false}
    if($Context.runtimeRegistryKey){$result.fileIds+=@($Context.runtimeRegistryKey)}
    if($Context.runtimeRegistryIssue){$result.status='incomplete';$result.issues+=('invalid-runtime-registry:'+ $Context.runtimeRegistryIssue);return $result}
    if(-not $Context.runtimeContracts.ContainsKey($Name)){$result.issues+='runtime-contract-not-declared';return $result}
    $contract=$Context.runtimeContracts[$Name];$tokens=@('reviewed-file-inputs-v1');$review=@();$seen=@{}
    try{
        if((Get-TestDependencyField $contract 'kind') -ne 'reviewed-file-inputs-v1' -or
           -not $contract.PSObject.Properties['reviewedSources'] -or @($contract.reviewedSources).Count -lt 1 -or @($contract.reviewedSources).Count -gt 128 -or
           -not $contract.PSObject.Properties['inputs'] -or @($contract.inputs).Count -gt 1024){throw 'Unsupported runtime file-input contract.'}
        $result.sourceReview='valid'
        foreach($source in @($contract.reviewedSources)){
            $relative=[string](Get-TestDependencyField $source 'path');$expected=[string](Get-TestDependencyField $source 'sha256')
            $path=Resolve-TestRuntimePath $Context $relative;$key=Add-TestDependencyFile $Context $path
            if($expected -notmatch '^[a-fA-F0-9]{64}$' -or $seen.ContainsKey($key)){throw 'Source review identity is invalid or repeated.'};$seen[$key]=$true
            $result.fileIds+=@($key);$actual=$Context.files[$key]
            if($actual.status -ne 'observed' -or $actual.sha256 -ne $expected){$result.sourceReview='stale';$result.issues+=('runtime-source-review-stale:'+ $relative)}
            $review+=($relative.Replace('\','/')+':'+$expected.ToLowerInvariant()+':'+$actual.status+':'+$actual.sha256)
        }
        $tokens+=@(Get-TestDependencyOrdered $review);$seen=@{}
        foreach($input in @($contract.inputs)){
            $kind=[string](Get-TestDependencyField $input 'kind');$relative=[string](Get-TestDependencyField $input 'path');$required=Get-TestDependencyField $input 'required' $true
            if($kind -notin 'file','directory-presence','directory-entries','directory-tree' -or $required -isnot [bool]){throw 'Invalid runtime input kind or required flag.'}
            $path=Resolve-TestRuntimePath $Context $relative;$identity=$kind+':'+$path.ToLowerInvariant()
            if($seen.ContainsKey($identity)){throw 'Repeated runtime input binding.'};$seen[$identity]=$true
            $present=if($kind -eq 'file'){[IO.File]::Exists($path)}else{[IO.Directory]::Exists($path)}
            $row=[pscustomobject]@{kind=$kind;path=$path;required=$required;present=$present;entries=@();fileIds=@();digest=$null}
            if($required -and -not $present){$result.issues+=('missing-runtime-input:'+ $relative)}
            if($kind -eq 'file'){$key=Add-TestDependencyFile $Context $path;$row.fileIds=@($key);$value=$Context.files[$key];$row.entries=@($value.status+':'+$value.sha256)}
            elseif($present){$directory=Get-TestRuntimeDirectory $Context $path $kind;$row.entries=@($directory.entries);$row.fileIds=@($directory.fileIds)}
            $row.digest=Get-TestRuntimeTokenDigest (@($kind,$relative.Replace('\','/'),$present.ToString(),$required.ToString())+@($row.entries))
            $result.bindings+=@($row);$result.fileIds+=@($row.fileIds)
        }
        $tokens+=@(Get-TestDependencyOrdered @($result.bindings|ForEach-Object {$_.kind+':'+$_.path.ToLowerInvariant()+':'+$_.digest}))
        $result.digest=Get-TestRuntimeTokenDigest $tokens
        $result.status=if($result.issues.Count){'incomplete'}else{'observed'}
    }catch{$result.status='incomplete';$result.issues+=('invalid-runtime-contract:'+ $_.Exception.Message)}
    $result.fileIds=@(Get-TestDependencyOrdered $result.fileIds)
    return $result
}
function Compare-TestRuntimeInputs([string]$Root,$Tests,$Before) {
    $rows=@();$context=[pscustomobject]@{root=$Root;files=@{}}
    try{
        if(-not $Before -or -not [IO.File]::Exists($Before.path) -or (Get-Item -LiteralPath $Before.path).Length -gt 64MB){throw 'Runtime input baseline is unavailable.'}
        $bytes=[IO.File]::ReadAllBytes($Before.path);$sha=[Security.Cryptography.SHA256]::Create()
        try{$hash=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
        if($hash -ne $Before.sha256){throw 'Runtime input baseline hash changed.'}
        $baseline=[Text.Encoding]::UTF8.GetString($bytes)|ConvertFrom-Json
        foreach($test in $Tests){
            $prior=@($baseline.tests|Where-Object test -eq $test.name);$old=if($prior.Count -eq 1){Get-TestDependencyField $prior[0] 'runtimeInputs'}else{$null}
            $current=Get-TestRuntimeInputs $context $test.name
            # A missing required file has an incomplete current contract, but
            # its observed absence still proves the prior endpoint changed.
            $status=if($old -and $old.status -eq 'observed' -and $current.digest -and $old.digest -ne $current.digest){'invalidated'}elseif(-not $old -or $old.status -ne 'observed' -or $current.status -ne 'observed'){'unclosed'}else{'stable'}
            $rows+=@([pscustomobject]@{test=$test.name;status=$status;beforeDigest=(Get-TestDependencyField $old 'digest');afterDigest=$current.digest;issues=$current.issues;runtimeClosure='unclosed';reuseAllowed=$false})
        }
        return [pscustomobject]@{status='recorded';tests=$rows;reason='File-input endpoints only; DLL loading and external runtime state remain unclosed.'}
    }catch{return [pscustomobject]@{status='unavailable';tests=@();reason=$_.Exception.Message;reuseAllowed=$false}}
}
