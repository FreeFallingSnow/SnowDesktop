# Command-boundary adaptation. Frozen requests and CPU reservations stay fixed;
# supplemental memory uses the original broker transaction and kernel handles.
function Get-CompileWorkload($Context,[string[]]$Arguments) {
    if($Context.generator -notlike 'Visual Studio *'){return 'heavy'}
    $targets=@();$index=[Array]::IndexOf($Arguments,'--target')
    if($index -ge 0){
        for($at=$index+1;$at -lt $Arguments.Count -and -not $Arguments[$at].StartsWith('-');$at++){$targets+=@($Arguments[$at])}
    }
    elseif($Context.PSObject.Properties['targets']){$targets=@($Context.targets)}
    if(-not $targets.Count){return 'heavy'} # Aggregate/preset and unknown graphs.
    $pending=New-Object 'System.Collections.Generic.Queue[string]'
    foreach($target in $targets){$pending.Enqueue((Join-Path $Context.directory ($target+'.vcxproj')))}
    $seen=@{}
    while($pending.Count){
        $path=[IO.Path]::GetFullPath($pending.Dequeue())
        if($seen.ContainsKey($path)){continue};$seen[$path]=$true
        if($seen.Count -gt 128 -or -not [IO.File]::Exists($path)){return 'heavy'}
        if((Get-Item -LiteralPath $path).Length -gt 4MB){return 'heavy'}
        $project=New-Object Xml.XmlDocument;$project.XmlResolver=$null
        $project.LoadXml([IO.File]::ReadAllText($path))
        # Treat PCH, XAML/WinRT and unresolved expressions conservatively.
        if($project.SelectNodes("//*[local-name()='PrecompiledHeader' and (text()='Use' or text()='Create')]").Count -or
            $project.OuterXml -match '(?i)CppWinRT|WindowsAppSDK|<Page\b|<ApplicationDefinition\b'){return 'heavy'}
        foreach($reference in @($project.SelectNodes("//*[local-name()='ProjectReference' and @Include]"))){
            $relative=[string]$reference.Include
            if($relative -match '\$\('){return 'heavy'}
            $pending.Enqueue([IO.Path]::Combine([IO.Path]::GetDirectoryName($path),$relative))
        }
    }
    return 'ordinary'
}
function Get-AdaptiveCompileDecision($Budget,[string]$Workload,[long]$AvailableMiB,[int]$OtherReservedMiB=0) {
    $perCompiler=if($Workload -eq 'ordinary'){768}else{2048}
    # System headroom plus a small transition reserve remain unassigned.
    # Available physical memory already excludes this Run's resident pages.
    # Never add private commit back as if it were free physical memory.
    $live=[long][Math]::Max(0,$AvailableMiB-$Budget.headroomMiB-$OtherReservedMiB-256)
    $desired=[long][Math]::Min($Budget.memoryCapacityMiB,1024+[long]$Budget.cpuSlots*$perCompiler)
    $memory=[int]([Math]::Floor([Math]::Min($desired,$live)/256)*256)
    $jobs=[int][Math]::Min($Budget.cpuSlots,[Math]::Max(1,[Math]::Floor(($memory-1024)/[double]$perCompiler)))
    return [pscustomobject]@{workload=$Workload;perCompilerMiB=$perCompiler;overheadMiB=1024;transitionMiB=256;
        availableMemoryMiB=$AvailableMiB;otherReservedMiB=$OtherReservedMiB;
        desiredMemoryMiB=$memory;compileJobs=$jobs;cpuSlots=$Budget.cpuSlots;headroomMiB=$Budget.headroomMiB}
}
function Enter-AdaptiveCompileBudget([string]$Root,$Budget,$Context,[string[]]$Arguments) {
    Initialize-ResourceRuntime
    $workload=Get-CompileWorkload $Context $Arguments
    $directory=[IO.Path]::GetDirectoryName($Budget.lockPath)
    $transaction=$null;$extra=$null;$reason='within-base';$decision=$null
    try {
        $transaction=Open-ResourceTransaction $directory 0
        $policy=Read-ResourcePolicySnapshot $Budget.policyPath
        if(-not $policy -or $policy.hash -ne $Budget.policyHash){throw 'Resource policy changed before compile scheduling.'}
        $occupied=[SnowDesktop.Build.ResourceBroker]::MemoryOccupancy($directory,[int]($Budget.memoryCapacityMiB/256))
        $baseSlots=[int]($Budget.reservedMemoryMiB/256)
        if($occupied -lt $baseSlots){throw 'Admitted base memory reservation disappeared.'}
        $available=[SnowDesktop.Build.Job]::AvailableMemoryBytes()
        $decision=Get-AdaptiveCompileDecision $Budget $workload ([long][Math]::Floor($available/1MB)) (($occupied-$baseSlots)*256)
        $extraSlots=[int][Math]::Max(0,($decision.desiredMemoryMiB-$Budget.reservedMemoryMiB)/256)
        if($extraSlots -gt 0){
            if($Budget.compatibilityMode -eq 'shared' -and [SnowDesktop.Build.ResourceWaiter]::HasPending($directory)){$reason='earlier-admission-request'}
            else {
                $attempt=[SnowDesktop.Build.ResourceBroker]::TrySupplementMemory($directory,[int]($Budget.memoryCapacityMiB/256),$baseSlots,$extraSlots,$available,([ulong]$Budget.headroomMiB*1MB))
                $extra=$attempt.Lease;$reason=if($extra){'supplemented'}else{$attempt.BusyKind}
            }
        }
        $assigned=[int]$Budget.reservedMemoryMiB+$(if($extra){$extraSlots*256}else{0})
        $usable=[int][Math]::Min($assigned,$decision.desiredMemoryMiB)
        $decision.compileJobs=[int][Math]::Min($Budget.cpuSlots,[Math]::Max(1,[Math]::Floor(($usable-1024)/[double]$decision.perCompilerMiB)))
        $decision|Add-Member NoteProperty assignedMemoryMiB $assigned
        $decision|Add-Member NoteProperty supplementalMemoryMiB $(if($extra){$extraSlots*256}else{0})
        $decision|Add-Member NoteProperty reason $reason
        return [pscustomobject]@{decision=$decision;lease=$extra}
    } catch {
        if($extra){$extra.Dispose()}
        if(-not (Test-EntrySharingViolation $_.Exception)){throw}
        # Contention never blocks behind a second reservation while holding the
        # first one. Lower dispatch within the existing allocation instead.
        $decision=Get-AdaptiveCompileDecision $Budget $workload ([long][Math]::Floor([SnowDesktop.Build.Job]::AvailableMemoryBytes()/1MB))
        $decision.compileJobs=[int][Math]::Min($decision.compileJobs,(Get-CompileResourceJobs $Budget))
        $decision|Add-Member NoteProperty assignedMemoryMiB $Budget.reservedMemoryMiB
        $decision|Add-Member NoteProperty supplementalMemoryMiB 0
        $decision|Add-Member NoteProperty reason 'resource-transaction-busy'
        return [pscustomobject]@{decision=$decision;lease=$null}
    } finally {if($transaction){$transaction.Dispose()}}
}
