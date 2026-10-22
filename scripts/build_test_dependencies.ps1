# Observations only: no configure/build, test selection or result reuse.
$runtimeInputModule=Join-Path $PSScriptRoot 'build_test_runtime_inputs.ps1'
if([IO.File]::Exists($runtimeInputModule)){. $runtimeInputModule}
function Get-TestDependencyField($Object,[string]$Name,$Default=$null) {
    if($null -ne $Object -and $Object.PSObject.Properties[$Name]){return $Object.$Name};return $Default
}
function Get-TestDependencyDigest([string]$Text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
}
function Get-TestDependencyOrdered([string[]]$Values) {
    $unique=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach($value in $Values){[void]$unique.Add($value)}
    [string[]]$ordered=@($unique);[Array]::Sort($ordered,[StringComparer]::Ordinal)
    return $ordered
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
function Get-TestDependencyMetadata($Item,[string]$Name,[string]$Configuration) {
    foreach($node in $Item.SelectNodes('*[local-name()="'+$Name+'"]')){
        $condition=$node.GetAttribute('Condition')
        if(-not $condition -or $condition.Contains("'"+$Configuration+'|')){return $node.InnerText}
    }
    return $null
}
function Resolve-TestDependencyPath([string]$Value,[string]$Directory,[string]$TrackingRoot,[string]$Configuration) {
    $path=$Value.Replace('$(IntDir)',$TrackingRoot+'\').Replace('$(ProjectDir)',$Directory+'\').Replace('$(Configuration)',$Configuration)
    if($path.Contains('$(') -or $path.Contains('%(')){return $null}
    if(-not [IO.Path]::IsPathRooted($path)){$path=Join-Path $Directory $path}
    return [IO.Path]::GetFullPath($path)
}
function Read-TestDependencyProject($Context,[string]$Project) {
    $absolute=[IO.Path]::GetFullPath($Project);$key=$absolute.ToLowerInvariant()
    if($Context.projects.ContainsKey($key)){return $Context.projects[$key]}
    if($Context.projects.Count -ge 4096){throw 'Observed test dependency project budget exceeded.'}
    $node=[pscustomobject]@{project=$absolute;files=@();references=@();issues=@();compilerReadLogs=0;linkerReadLogs=0;sourceFiles=@();reportedSources=@();sourceReports=0;expectedReports=@{}}
    $Context.projects[$key]=$node # Cache before descending through references.
    $node.files+=Add-TestDependencyFile $Context $absolute
    if(-not [IO.File]::Exists($absolute)){$node.issues+='missing-project';return $node}
    $settings=New-Object Xml.XmlReaderSettings;$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
    $reader=$null;$document=New-Object Xml.XmlDocument;$document.XmlResolver=$null
    try{$reader=[Xml.XmlReader]::Create($absolute,$settings);$document.Load($reader)}finally{if($reader){$reader.Dispose()}}
    $directory=[IO.Path]::GetDirectoryName($absolute)
    $trackingRoot=Join-Path $directory ([IO.Path]::GetFileNameWithoutExtension($absolute)+'.dir/'+$Context.configuration)
    foreach($item in $document.SelectNodes('//*[local-name()="ClCompile" or local-name()="ClInclude" or local-name()="ResourceCompile" or local-name()="Midl" or local-name()="CustomBuild" or local-name()="ProjectReference"][@Include]')){
        $path=$item.GetAttribute('Include').Replace('$(Configuration)',$Context.configuration).Replace('$(ProjectDir)',$directory+'\')
        if($path.Contains('$(') -or $path.Contains('*') -or $path.Contains('?')){$node.issues+=('unresolved-project-item:'+ $path);continue}
        if(-not [IO.Path]::IsPathRooted($path)){$path=Join-Path $directory $path};$path=[IO.Path]::GetFullPath($path)
        if($item.LocalName -eq 'ClCompile'){
            $sourceKey=$path.ToLowerInvariant();$node.sourceFiles+=$sourceKey
            $spec=Get-TestDependencyMetadata $item 'SourceDependenciesFile' $Context.configuration
            if($spec){
                if($spec.StartsWith('$([System.IO.Path]::GetDirectoryName(')){
                    $objectSpec=Get-TestDependencyMetadata $item 'ObjectFileName' $Context.configuration
                    if(-not $objectSpec){$objectSpec='$(IntDir)'}
                    $objectPath=Resolve-TestDependencyPath $objectSpec $directory $trackingRoot $Context.configuration
                    $expected=if($objectPath){Join-Path ([IO.Path]::GetDirectoryName($objectPath)) ([IO.Path]::GetFileName($path)+'.json')}else{$null}
                }else{$expected=Resolve-TestDependencyPath $spec $directory $trackingRoot $Context.configuration}
                if($expected){$node.expectedReports[$sourceKey]=$expected.ToLowerInvariant()}else{$node.issues+=('unresolved-source-report-path:'+ $path)}
            }
        }
        if($item.LocalName -eq 'ProjectReference'){$node.references+=$path}else{$node.files+=Add-TestDependencyFile $Context $path}
    }
    if($Context.reportingDisabled -and $node.sourceFiles.Count){$node.issues+='compiler-source-reporting-disabled'}
    if([IO.Directory]::Exists($trackingRoot)){
        # Unlike FileTracker, /sourceDependencies reports includes under Temp
        # and AppData too. A report is associated with its declared source.
        # https://learn.microsoft.com/en-us/cpp/build/reference/sourcedependencies
        foreach($report in @(Get-ChildItem -LiteralPath $trackingRoot -Filter '*.json' -File -Recurse|Where-Object Name -match '(^sd-[a-f0-9]{16}\.json$)|\.(c|cc|cpp|cxx|ixx)\.json$')){
            if($Context.reportingDisabled){continue}
            try {
                if($report.Length -gt 64MB){throw 'source-report-too-large'}
                $json=[IO.File]::ReadAllText($report.FullName)|ConvertFrom-Json
                if($json.Version -notin '1.0','1.1','1.2'){throw 'unsupported-source-report-version'}
                $data=$json.Data;$source=[IO.Path]::GetFullPath([string]$data.Source).ToLowerInvariant()
                if($node.sourceFiles -notcontains $source){throw 'source-report-not-in-generated-project'}
                if($node.expectedReports.ContainsKey($source) -and $node.expectedReports[$source] -ne $report.FullName.ToLowerInvariant()){continue} # Retained output from an older report layout.
                $inputs=@($data.Source)+@(Get-TestDependencyField $data 'Includes' @())+@(Get-TestDependencyField $data 'PCH' @())
                foreach($module in @(Get-TestDependencyField $data 'ImportedModules' @())){$inputs+=Get-TestDependencyField $module 'BMI'}
                foreach($unit in @(Get-TestDependencyField $data 'ImportedHeaderUnits' @())){$inputs+=@(Get-TestDependencyField $unit 'Header')+@(Get-TestDependencyField $unit 'BMI')}
                foreach($reportInput in @($inputs|Where-Object {$_})){
                    if(-not [IO.Path]::IsPathRooted([string]$reportInput)){throw 'relative-source-report-input'}
                    $node.files+=Add-TestDependencyFile $Context ([string]$reportInput)
                }
                $node.files+=Add-TestDependencyFile $Context $report.FullName
                $node.reportedSources+=$source;$node.sourceReports++
            }catch{$node.issues+=('invalid-source-report:'+ $report.FullName+':'+$_.Exception.Message)}
        }
        foreach($log in @(Get-ChildItem -LiteralPath $trackingRoot -Filter '*.read.*.tlog' -File -Recurse)){
            if($log.Length -gt 64MB){$node.issues+='tracking-log-too-large';continue}
            if($log.Name -match '(?i)(^|\.)CL\.read\.'){$node.compilerReadLogs++};if($log.Name -match '(?i)^(Lib-)?link\.read\.'){$node.linkerReadLogs++}
            foreach($line in [IO.File]::ReadLines($log.FullName)){foreach($inputPath in $line.TrimStart('^').Split('|')){
                if(-not $inputPath){continue};if(-not [IO.Path]::IsPathRooted($inputPath)){$node.issues+=('unresolved-tracking-input:'+ $inputPath);continue}
                $node.files+=Add-TestDependencyFile $Context $inputPath
            }}
        }
    }
    $node.files=@($node.files|Sort-Object -Unique);$node.references=@($node.references|Sort-Object -Unique)
    if(@($document.SelectNodes('//*[local-name()="ClCompile"][@Include]')).Count -and $node.compilerReadLogs -eq 0){$node.issues+='missing-compiler-read-log'}
    if(@($node.sourceFiles|Where-Object {$node.reportedSources -notcontains $_}).Count){$node.issues+='missing-compiler-source-report'}
    foreach($reference in $node.references){[void](Read-TestDependencyProject $Context $reference)};return $node
}
function Get-TestDependencyManifest {
    param([string]$Root,$Tests,[string]$Configuration='Release')
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/');if($Configuration -notin 'Release','Debug'){throw 'Explicit Release/Debug configuration required.'}
    $build=Join-Path $Root '.build';$cache=Join-Path $build 'CMakeCache.txt'
    $disabled=[IO.File]::Exists($cache) -and [IO.File]::ReadAllText($cache) -match '(?m)^SNOWDESKTOP_SOURCE_DEPENDENCIES:BOOL=OFF\r?$'
    $context=[pscustomobject]@{root=$Root;configuration=$Configuration;files=@{};projects=@{};reportingDisabled=$disabled};$rows=@();$projectIndex=$null
    foreach($test in @($Tests|Sort-Object name)){
        $issues=@();$project=$null;$keys=@();$visited=@{};$projectNames=@();$compilerLogs=0;$linkerLogs=0;$sourceReports=0
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
                $node=$context.projects[$id];$keys+=@($node.files);$issues+=@($node.issues);$projectNames+=$node.project;$compilerLogs+=$node.compilerReadLogs;$linkerLogs+=$node.linkerReadLogs;$sourceReports+=$node.sourceReports
                foreach($reference in $node.references){$pending.Enqueue($reference)}
            }
        }
        foreach($relative in @('CMakeLists.txt','CMakePresets.json','CMakeUserPresets.json','.build/CMakeCache.txt')){$path=Join-Path $Root $relative;if([IO.File]::Exists($path)){$keys+=Add-TestDependencyFile $context $path}}
        foreach($path in @($command)+$required){if([IO.Path]::IsPathRooted([string]$path)){
            if([IO.File]::Exists([string]$path)){$keys+=Add-TestDependencyFile $context ([string]$path)}elseif($required -contains $path){$issues+=($(if([IO.Directory]::Exists([string]$path)){'unclosed-runtime-directory:'}else{'missing-explicit-runtime:'})+$path)}
        }}
        $runtimeInputs=if([IO.File]::Exists($runtimeInputModule) -and (Get-Command Get-TestRuntimeInputs -ErrorAction SilentlyContinue)){Get-TestRuntimeInputs $context $test.name}else{[pscustomobject]@{status='unavailable';fileIds=@();bindings=@();digest=$null;issues=@('runtime-input-observer-unavailable');runtimeClosure='unclosed';reuseAllowed=$false}}
        $keys=@(Get-TestDependencyOrdered $keys);foreach($key in $keys){if($context.files[$key].status -ne 'observed'){$issues+=('missing-observed-input:'+ $context.files[$key].path)}}
        $keys=@(Get-TestDependencyOrdered (@($keys)+@($runtimeInputs.fileIds)))
        $canonical=($keys|ForEach-Object {$key=$_;$file=$context.files[$key];$key+[char]0+$file.status+[char]0+$file.sha256}) -join "`n"
        $canonical+="`nRuntime file inputs:"+$runtimeInputs.status+':'+$runtimeInputs.digest
        $rows+=[pscustomobject]@{test=$test.name;target=$(if($project){[IO.Path]::GetFileNameWithoutExtension($project)}else{$null});status=$(if($issues.Count){'incomplete'}else{'observed'});
            fileIds=$keys;projects=@($projectNames|Sort-Object -Unique);compilerReadLogs=$compilerLogs;linkerReadLogs=$linkerLogs;sourceReports=$sourceReports;sourceClosure='unproven';digest=(Get-TestDependencyDigest $canonical);issues=@($issues|Sort-Object -Unique);
            runtimeInputs=$runtimeInputs;runtimeClosure='unclosed';reuseAllowed=$false;reason='Generated projects, compiler/linker reads and reviewed runtime file inputs are observations; DLL loads and external state remain unclosed.'}
    }
    $files=@((Get-TestDependencyOrdered @($context.files.Keys))|ForEach-Object {$key=$_;[pscustomobject]@{id=$key;path=$context.files[$key].path;status=$context.files[$key].status;sha256=$context.files[$key].sha256;bytes=$context.files[$key].bytes}})
    $excluded=@([IO.Path]::GetTempPath(),[Environment]::GetFolderPath('ApplicationData'),[Environment]::GetFolderPath('LocalApplicationData'),[Environment]::GetFolderPath('CommonApplicationData'),(Join-Path ([Environment]::GetFolderPath('UserProfile')) 'AppData/LocalLow'))
    $excluded=@($excluded|Where-Object {$_}|ForEach-Object {[IO.Path]::GetFullPath($_).TrimEnd('\','/').ToLowerInvariant()+'\'}|Sort-Object -Unique)
    return [pscustomobject]@{schemaVersion=1;root=$Root;configuration=$Configuration;files=$files;tests=$rows;reuseAllowed=$false;trackingExcludedRoots=$excluded;
        provenance='Generated MSBuild references, compiler source reports and actual read tracking plus explicit CTest runtime files; tracking filters and unclosed runtime semantics prohibit reuse.'}
}
function Get-TestDependencyAdvice($Manifest,[string[]]$ChangedPaths) {
    $changes=@($ChangedPaths|ForEach-Object {if([IO.Path]::IsPathRooted($_)){[IO.Path]::GetFullPath($_).ToLowerInvariant()}else{[IO.Path]::GetFullPath((Join-Path $Manifest.root $_)).ToLowerInvariant()}}|Sort-Object -Unique)
    $directories=@($Manifest.tests|ForEach-Object {Get-TestDependencyField (Get-TestDependencyField $_ 'runtimeInputs') 'bindings' @()}|Where-Object kind -ne 'file'|ForEach-Object {$_.path.ToLowerInvariant().TrimEnd('\','/')})
    $known=@{};foreach($file in $Manifest.files){$known[$file.id]=$true};$unmapped=@($changes|Where-Object {$change=$_;-not $known.ContainsKey($change) -and -not @($directories|Where-Object {$change -eq $_ -or $change.StartsWith($_+'\',[StringComparison]::OrdinalIgnoreCase)}).Count})
    $rows=@($Manifest.tests|ForEach-Object {$test=$_;$runtime=Get-TestDependencyField $test 'runtimeInputs';$bindings=@(Get-TestDependencyField $runtime 'bindings' @());$matched=@($changes|Where-Object {$change=$_;$test.fileIds -contains $change -or @($bindings|Where-Object {$change -eq $_.path.ToLowerInvariant() -or ($_.kind -ne 'file' -and $change.StartsWith($_.path.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase))}).Count});[pscustomobject]@{
        test=$test.test;target=$test.target;recommendRun=($matched.Count -gt 0 -or $unmapped.Count -gt 0 -or $test.status -ne 'observed');matchingPaths=$matched;
        reason=$(if($unmapped.Count){'unmapped-change'}elseif($test.status -ne 'observed'){'incomplete-build-dependencies'}elseif($matched.Count){'observed-dependency-changed'}else{'no-observed-source-impact'});reuseAllowed=$false}})
    return [pscustomobject]@{schemaVersion=1;tests=$rows;unmappedPaths=$unmapped;advisoryOnly=$true;selectionChanged=$false;reason='Advice never removes explicitly requested tests; unclosed runtime dependencies cannot authorize reuse.'}
}
function Save-TestDependencyManifest([string]$Root,$Tests) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    try{$manifest=Get-TestDependencyManifest -Root $Root -Tests $Tests}catch{return [pscustomobject]@{status='unavailable';reason=$_.Exception.Message;elapsedSeconds=$timer.Elapsed.TotalSeconds;reuseAllowed=$false;tests=@()}}
    $directory=Join-Path $Root '.build/Testing';[void][IO.Directory]::CreateDirectory($directory)
    $path=Join-Path $directory ('dependencies-'+[Guid]::NewGuid().ToString('N')+'.json');$text=$manifest|ConvertTo-Json -Depth 12 -Compress
    [IO.File]::WriteAllText($path,$text,(New-Object Text.UTF8Encoding($false)));$sha=[Security.Cryptography.SHA256]::Create()
    try{$digest=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($path))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
    return [pscustomobject]@{schemaVersion=1;path=$path;sha256=$digest;elapsedSeconds=$timer.Elapsed.TotalSeconds;uniqueFiles=$manifest.files.Count;
        tests=@($manifest.tests|Select-Object test,target,status,digest,compilerReadLogs,linkerReadLogs,sourceReports,sourceClosure,issues,runtimeClosure,reuseAllowed,
            @{n='runtimeInputStatus';e={$_.runtimeInputs.status}},@{n='runtimeInputDigest';e={$_.runtimeInputs.digest}},@{n='runtimeInputIssues';e={$_.runtimeInputs.issues}})}
}
