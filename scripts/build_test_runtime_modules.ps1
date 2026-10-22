# Native debugger observations bind actual loads, including unloaded DLLs.
# Debugging changes execution timing; external state is still unclosed.
function Get-TestRuntimeModuleObservations([string]$Root,$Tests,[string]$Directory) {
    $context=[pscustomobject]@{root=$Root;files=@{}};$rows=@()
    foreach($test in $Tests){
        $command=@(Get-TestDependencyField $test 'command' @())
        $row=[pscustomobject]@{test=$test.name;status='not-instrumented';report=$null;reportSha256=$null;digest=$null;imageCount=0;processCount=0;issues=@();runtimeClosure='unclosed';reuseAllowed=$false}
        if(-not $command.Count -or [IO.Path]::GetFileName([string]$command[0]) -ne 'SnowDesktopTestRuntimeObserver.exe'){$rows+=@($row);continue}
        try{
            if($command.Count -lt 5 -or $command[1] -ne '--test' -or $command[2] -cne $test.name -or $command[3] -ne '--'){throw 'Unrecognized native runtime observer invocation.'}
            $reports=@(Get-ChildItem -LiteralPath $Directory -Filter ($test.name+'.*.json') -File -ErrorAction SilentlyContinue)
            if($reports.Count -ne 1){throw 'Runtime module report is missing or ambiguous.'}
            $file=$reports[0];if($file.Length -gt 8MB){throw 'Runtime module report exceeds its byte budget.'}
            $key=Add-TestDependencyFile $context $file.FullName;$row.report=$file.FullName;$row.reportSha256=$context.files[$key].sha256
            $observed=[IO.File]::ReadAllText($file.FullName)|ConvertFrom-Json
            if((Get-TestDependencyField $observed 'schemaVersion') -ne 1 -or $observed.test -cne $test.name -or $observed.mode -ne 'run' -or @($observed.images).Count -gt 4096){throw 'Runtime module report identity/schema changed.'}
            $expected=@($command|Select-Object -Skip 4)
            if(@($observed.argv).Count -ne $expected.Count){throw 'Runtime module argv count differs.'}
            for($i=0;$i -lt $expected.Count;$i++){if(-not [string]::Equals([string]$expected[$i],[string]$observed.argv[$i],[StringComparison]::Ordinal)){throw 'Runtime module argv changed.'}}
            $row.processCount=$observed.processCount;$row.imageCount=@($observed.images).Count;$tokens=@('runtime-modules-v1');$rootImage=@()
            foreach($image in @($observed.images)){
                if($image.kind -notin 'dll','executable' -or -not [IO.Path]::IsPathRooted([string]$image.path) -or $image.sha256 -notmatch '^[a-f0-9]{64}$'){throw 'Invalid module file identity.'}
                $imageKey=Add-TestDependencyFile $context ([string]$image.path);$current=$context.files[$imageKey]
                if($current.status -ne 'observed' -or $current.sha256 -ne $image.sha256){$row.issues+=('loaded-module-file-changed:'+ $image.path)}
                $tokens+=($image.kind+':'+$imageKey+':'+$image.sha256+':'+$image.startup+':'+$image.unloaded+':'+$image.identitySource)
                if($image.kind -eq 'executable' -and $image.pid -eq $observed.rootPid){$rootImage+=@($image)}
            }
            $executableKey=Add-TestDependencyFile $context ([string]$command[4])
            if($rootImage.Count -ne 1 -or $rootImage[0].sha256 -ne $context.files[$executableKey].sha256){throw 'Observed root image differs from the selected test binary.'}
            $working=@($test.properties|Where-Object name -eq 'WORKING_DIRECTORY')
            if($working.Count -eq 1 -and -not [string]::Equals([IO.Path]::GetFullPath([string]$working[0].value),[IO.Path]::GetFullPath([string]$observed.workingDirectory),[StringComparison]::OrdinalIgnoreCase)){throw 'Observed test working directory changed.'}
            $row.issues+=@($observed.issues);$row.digest=Get-TestRuntimeTokenDigest @(Get-TestDependencyOrdered $tokens)
            $row.status=if($row.issues.Count -or $observed.status -ne 'observed'){'incomplete'}else{'observed'}
            $row|Add-Member NoteProperty rootExitCode $observed.rootExitCode
            $row|Add-Member NoteProperty timingComparableToUnobservedRun $false
        }catch{$row.status='unavailable';$row.issues+=@($_.Exception.Message)}
        $rows+=@($row)
    }
    return [pscustomobject]@{status='recorded';tests=$rows;reason='Actual image-load events and current file identities; external state and semantic runtime closure remain unclosed.'}
}
