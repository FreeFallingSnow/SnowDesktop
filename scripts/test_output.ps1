# Private full-test checkpoint shared by the initial run and selective retry
# continuation. It performs no configure, build, CTest or output arrangement.
function Test-IsolatedOutput([string]$Root) {
    $releaseRoot = [IO.Path]::GetFullPath((Join-Path $Root '.build\Release'))
    $testRoot = Join-Path $releaseRoot 'tests'
    $runtimeRoot = Join-Path $releaseRoot 'SnowDesktop.Runtime'
    $tests = @(Get-ChildItem -LiteralPath $testRoot -File -Filter 'SnowDesktop*Tests.exe' -ErrorAction Stop)
    $rootTests = @(Get-ChildItem -LiteralPath $releaseRoot -File -Filter 'SnowDesktop*Tests.exe' -ErrorAction Stop)
    $rootDlls = @(Get-ChildItem -LiteralPath $releaseRoot -File -Filter '*.dll' -ErrorAction Stop)
    $runtimeDirectoryNames = @(Get-ChildItem -LiteralPath $runtimeRoot -Directory -ErrorAction Stop | ForEach-Object Name)
    $emptyRuntimeDirs = @(Get-ChildItem -LiteralPath $releaseRoot -Directory -ErrorAction Stop | Where-Object {
        $runtimeDirectoryNames -contains $_.Name -and [IO.Directory]::GetFileSystemEntries($_.FullName).Count -eq 0
    })
    if ($tests.Count -eq 0 -or $rootTests.Count -ne 0 -or $rootDlls.Count -ne 0 -or $emptyRuntimeDirs.Count -ne 0) {
        throw 'Build or CTest output escaped its dedicated runtime/test directory.'
    }
}
function New-OutputIsolationCheck {
    return [pscustomobject]@{status='not-run';startedUtc=$null;completedUtc=$null;exitCode=$null;error=''}
}
function Invoke-OutputIsolationCheck([string]$Root,$Check) {
    $Check.startedUtc=[DateTime]::UtcNow.ToString('o')
    try {
        Test-IsolatedOutput $Root
        $Check.status='passed';$Check.exitCode=0;$Check.error=''
    } catch {
        $Check.status='failed';$Check.exitCode=1;$Check.error=$_.Exception.Message
        throw
    } finally {$Check.completedUtc=[DateTime]::UtcNow.ToString('o')}
}
