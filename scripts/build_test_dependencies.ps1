# Observations only: no configure/build, test selection or result reuse.
function Get-TestDependencyField($Object,[string]$Name,$Default=$null) {
    if($null -ne $Object -and $Object.PSObject.Properties[$Name]){return $Object.$Name};return $Default
}
function Get-TestDependencyDigest([string]$Text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
}
function Add-TestDependencyFile($Context,[string]$Path) {
    $absolute=[IO.Path]::GetFullPath($Path);$key=$absolute.ToLowerInvariant()
    if($Context.files.ContainsKey($key)){return $key}
    if($Context.files.Count -ge 32768){throw 'Observed test dependency file budget exceeded.'}
    $status='missing';$digest=$null;$bytes=$null
    if([IO.File]::Exists($absolute)){
        $stream=[IO.File]::Open($absolute,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$sha=[Security.Cryptography.SHA256]::Create()
        try{$bytes=$stream.Length;$digest=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant();$status='observed'}finally{$sha.Dispose();$stream.Dispose()}
    }
    $Context.files[$key]=[pscustomobject]@{path=$absolute;status=$status;sha256=$digest;bytes=$bytes};return $key
}
function Read-TestDependencyProject($Context,[string]$Project) {
    $absolute=[IO.Path]::GetFullPath($Project);$key=$absolute.ToLowerInvariant()
    if($Context.projects.ContainsKey($key)){return $Context.projects[$key]}
    if($Context.projects.Count -ge 4096){throw 'Observed test dependency project budget exceeded.'}
    $node=[pscustomobject]@{project=$absolute;files=@();references=@();issues=@();compilerReadLogs=0;linkerReadLogs=0}
    $Context.projects[$key]=$node # Cache before descending through references.
    $node.files+=Add-TestDependencyFile $Context $absolute
    if(-not [IO.File]::Exists($absolute)){$node.issues+='missing-project';return $node}
    $settings=New-Object Xml.XmlReaderSettings;$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
    $reader=$null;$document=New-Object Xml.XmlDocument;$document.XmlResolver=$null
    try{$reader=[Xml.XmlReader]::Create($absolute,$settings);$document.Load($reader)}finally{if($reader){$reader.Dispose()}}
    $directory=[IO.Path]::GetDirectoryName($absolute)
    foreach($item in $document.SelectNodes('//*[local-name()="ClCompile" or local-name()="ClInclude" or local-name()="ResourceCompile" or local-name()="Midl" or local-name()="CustomBuild" or local-name()="ProjectReference"][@Include]')){
        $path=$item.GetAttribute('Include').Replace('$(Configuration)',$Context.configuration).Replace('$(ProjectDir)',$directory+'\')
        if($path.Contains('$(') -or $path.Contains('*') -or $path.Contains('?')){$node.issues+=('unresolved-project-item:'+ $path);continue}
        if(-not [IO.Path]::IsPathRooted($path)){$path=Join-Path $directory $path};$path=[IO.Path]::GetFullPath($path)
        if($item.LocalName -eq 'ProjectReference'){$node.references+=$path}else{$node.files+=Add-TestDependencyFile $Context $path}
    }
    $trackingRoot=Join-Path $directory ([IO.Path]::GetFileNameWithoutExtension($absolute)+'.dir/'+$Context.configuration)
    if([IO.Directory]::Exists($trackingRoot)){
        foreach($log in @(Get-ChildItem -LiteralPath $trackingRoot -Filter '*.read.*.tlog' -File -Recurse)){
            if($log.Length -gt 64MB){$node.issues+='tracking-log-too-large';continue}
            if($log.Name -match '(?i)(^|\.)CL\.read\.'){$node.compilerReadLogs++};if($log.Name -match '(?i)^link\.read\.'){$node.linkerReadLogs++}
            foreach($line in [IO.File]::ReadLines($log.FullName)){foreach($inputPath in $line.TrimStart('^').Split('|')){
                if(-not $inputPath){continue};if(-not [IO.Path]::IsPathRooted($inputPath)){$node.issues+=('unresolved-tracking-input:'+ $inputPath);continue}
                $node.files+=Add-TestDependencyFile $Context $inputPath
            }}
        }
    }
    $node.files=@($node.files|Sort-Object -Unique);$node.references=@($node.references|Sort-Object -Unique)
    if(@($document.SelectNodes('//*[local-name()="ClCompile"][@Include]')).Count -and $node.compilerReadLogs -eq 0){$node.issues+='missing-compiler-read-log'}
    foreach($reference in $node.references){[void](Read-TestDependencyProject $Context $reference)};return $node
}
function Get-TestDependencyManifest {
    param([string]$Root,$Tests,[string]$Configuration='Release')
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/');if($Configuration -notin 'Release','Debug'){throw 'Explicit Release/Debug configuration required.'}
    $context=[pscustomobject]@{root=$Root;configuration=$Configuration;files=@{};projects=@{}};$build=Join-Path $Root '.build';$rows=@();$projectIndex=$null
    foreach($test in @($Tests|Sort-Object name)){
        $issues=@();$project=$null;$keys=@();$visited=@{};$projectNames=@();$compilerLogs=0;$linkerLogs=0
        $command=@(Get-TestDependencyField $test 'command' @());$required=@($test.properties|Where-Object name -eq 'REQUIRED_FILES'|ForEach-Object value)
        $executables=@(@($command|Select-Object -First 1)+$required|Where-Object {[IO.Path]::IsPathRooted([string]$_) -and [IO.Path]::GetExtension([string]$_) -eq '.exe'}|Sort-Object -Unique)
        if($executables.Count -eq 1){
            $target=[IO.Path]::GetFileNameWithoutExtension([string]$executables[0]);$direct=Join-Path $build ($target+'.vcxproj')
            if([IO.File]::Exists($direct)){$project=$direct}else{
                if($null -eq $projectIndex){$projectIndex=@(Get-ChildItem -LiteralPath $build -Filter '*.vcxproj' -File -Recurse -ErrorAction SilentlyContinue)}
                $matches=@($projectIndex|Where-Object BaseName -eq $target);if($matches.Count -eq 1){$project=$matches[0].FullName}else{$issues+='missing-or-ambiguous-generated-project'}
            }
        }else{$issues+='missing-or-ambiguous-native-executable'}
        if($project){
            [void](Read-TestDependencyProject $context $project);$pending=New-Object 'System.Collections.Generic.Queue[string]';$pending.Enqueue($project)
            while($pending.Count){
                $next=$pending.Dequeue();$id=$next.ToLowerInvariant();if($visited.ContainsKey($id)){continue};$visited[$id]=$true
                $node=$context.projects[$id];$keys+=@($node.files);$issues+=@($node.issues);$projectNames+=$node.project;$compilerLogs+=$node.compilerReadLogs;$linkerLogs+=$node.linkerReadLogs
                foreach($reference in $node.references){$pending.Enqueue($reference)}
            }
        }
        foreach($relative in @('CMakeLists.txt','CMakePresets.json','CMakeUserPresets.json','.build/CMakeCache.txt')){$path=Join-Path $Root $relative;if([IO.File]::Exists($path)){$keys+=Add-TestDependencyFile $context $path}}
        foreach($path in @($command)+$required){if([IO.Path]::IsPathRooted([string]$path)){
            if([IO.File]::Exists([string]$path)){$keys+=Add-TestDependencyFile $context ([string]$path)}elseif($required -contains $path){$issues+=('missing-explicit-runtime:'+ $path)}
        }}
        $keys=@($keys|Sort-Object -Unique);foreach($key in $keys){if($context.files[$key].status -ne 'observed'){$issues+=('missing-observed-input:'+ $context.files[$key].path)}}
        $canonical=($keys|ForEach-Object {$key=$_;$file=$context.files[$key];$key+[char]0+$file.status+[char]0+$file.sha256}) -join "`n"
        $rows+=[pscustomobject]@{test=$test.name;target=$(if($project){[IO.Path]::GetFileNameWithoutExtension($project)}else{$null});status=$(if($issues.Count){'incomplete'}else{'observed'});
            fileIds=$keys;projects=@($projectNames|Sort-Object -Unique);compilerReadLogs=$compilerLogs;linkerReadLogs=$linkerLogs;digest=(Get-TestDependencyDigest $canonical);issues=@($issues|Sort-Object -Unique);
            runtimeClosure='unclosed';reuseAllowed=$false;reason='Generated projects and compiler/linker reads are observed; runtime loads, external state and semantic dependencies require a reviewed closure contract.'}
    }
    $files=@($context.files.Keys|Sort-Object|ForEach-Object {$key=$_;[pscustomobject]@{id=$key;path=$context.files[$key].path;status=$context.files[$key].status;sha256=$context.files[$key].sha256;bytes=$context.files[$key].bytes}})
    return [pscustomobject]@{schemaVersion=1;root=$Root;configuration=$Configuration;files=$files;tests=$rows;reuseAllowed=$false;
        provenance='Generated MSBuild references and actual read tracking plus explicit CTest runtime files; all project configurations conservatively included.'}
}
function Get-TestDependencyAdvice($Manifest,[string[]]$ChangedPaths) {
    $changes=@($ChangedPaths|ForEach-Object {if([IO.Path]::IsPathRooted($_)){[IO.Path]::GetFullPath($_).ToLowerInvariant()}else{[IO.Path]::GetFullPath((Join-Path $Manifest.root $_)).ToLowerInvariant()}}|Sort-Object -Unique)
    $known=@{};foreach($file in $Manifest.files){$known[$file.id]=$true};$unmapped=@($changes|Where-Object {-not $known.ContainsKey($_)})
    $rows=@($Manifest.tests|ForEach-Object {$test=$_;$matched=@($changes|Where-Object {$test.fileIds -contains $_});[pscustomobject]@{
        test=$test.test;target=$test.target;recommendRun=($matched.Count -gt 0 -or $unmapped.Count -gt 0 -or $test.status -ne 'observed');matchingPaths=$matched;
        reason=$(if($unmapped.Count){'unmapped-change'}elseif($test.status -ne 'observed'){'incomplete-build-dependencies'}elseif($matched.Count){'observed-dependency-changed'}else{'no-observed-source-impact'});reuseAllowed=$false}})
    return [pscustomobject]@{schemaVersion=1;tests=$rows;unmappedPaths=$unmapped;advisoryOnly=$true;selectionChanged=$false;reason='Advice never removes explicitly requested tests; unclosed runtime dependencies cannot authorize reuse.'}
}
function Save-TestDependencyManifest([string]$Root,$Tests) {
    $timer=[Diagnostics.Stopwatch]::StartNew();$manifest=Get-TestDependencyManifest -Root $Root -Tests $Tests;$directory=Join-Path $Root '.build/Testing';[void][IO.Directory]::CreateDirectory($directory)
    $path=Join-Path $directory ('dependencies-'+[Guid]::NewGuid().ToString('N')+'.json');$text=$manifest|ConvertTo-Json -Depth 12 -Compress
    [IO.File]::WriteAllText($path,$text,(New-Object Text.UTF8Encoding($false)));$sha=[Security.Cryptography.SHA256]::Create()
    try{$digest=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($path))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
    return [pscustomobject]@{schemaVersion=1;path=$path;sha256=$digest;elapsedSeconds=$timer.Elapsed.TotalSeconds;uniqueFiles=$manifest.files.Count;
        tests=@($manifest.tests|Select-Object test,target,status,digest,compilerReadLogs,linkerReadLogs,issues,runtimeClosure,reuseAllowed)}
}
