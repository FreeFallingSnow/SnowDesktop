# Repository inputs, including dirty and untracked files, are identified by
# content rather than Git status/HEAD alone. This is an endpoint check, not a
# filesystem snapshot or a guarantee against change-and-restore during a build.
function Invoke-InputGit([string]$Root, [string]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = (Get-Command git.exe -ErrorAction Stop).Source
    $info.Arguments = $Arguments
    $info.WorkingDirectory = $Root
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = New-Object Text.UTF8Encoding($false)
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $errorRead = $process.StandardError.ReadToEndAsync()
        $output = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ output = $output; error = $errorRead.Result; code = $process.ExitCode }
    }
    finally { $process.Dispose() }
}

function Test-BuildInputPath([string]$Relative) {
    $path = $Relative.Replace('\', '/')
    $top = $path.Split('/')[0]
    if ($top -in '.git', '.build', '.build_debug', '.codex-probes', '.codex-remote-attachments',
        '.tmp', '.vs', '.vscode', '.remotion', 'artifacts', 'out', 'build', 'bin', 'obj',
        'release', 'TestResults', 'website', 'SnowDesktop_SteamAssets') { return $false }
    $leaf = [IO.Path]::GetFileName($path)
    if ($leaf -eq 'AGENTS.md' -or $path -eq 'scripts/README.md') { return $false }
    # Root documentation/reports do not feed the C++ build. Notices are an
    # exception: they are shipped in the runtime and remain an input.
    if (-not $path.Contains('/') -and $leaf -ne 'THIRD_PARTY_NOTICES.md' -and
        $leaf -ne 'CMakeLists.txt' -and [IO.Path]::GetExtension($leaf) -in
        '.md', '.txt', '.log', '.csv', '.pdf', '.docx', '.xlsx', '.pptx', '.out', '.err') { return $false }
    return $true
}

function Get-BuildInputIdentity([string]$Root) {
    $listing = Invoke-InputGit $Root 'ls-files -z --cached --others --exclude-standard'
    if ($listing.code -ne 0) { throw ('Cannot enumerate repository inputs: ' + $listing.error) }
    $headQuery = Invoke-InputGit $Root 'rev-parse --verify HEAD'
    $head = if ($headQuery.code -eq 0) { $headQuery.output.Trim() } else { $null }
    $paths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($path in $listing.output.Split([char]0)) {
        if ($path -and (Test-BuildInputPath $path)) { [void]$paths.Add($path.Replace('\', '/')) }
    }
    # Local presets are intentionally ignored by Git but affect configuration.
    if ([IO.File]::Exists((Join-Path $Root 'CMakeUserPresets.json'))) { [void]$paths.Add('CMakeUserPresets.json') }
    [string[]]$ordered = @($paths)
    [Array]::Sort($ordered, [StringComparer]::Ordinal)
    $content = New-Object Text.StringBuilder
    [void]$content.Append("sha256-repository-inputs-v1`nHEAD`0$head`n")
    $sha = [Security.Cryptography.SHA256]::Create()
    $encoding = New-Object Text.UTF8Encoding($false)
    try {
        foreach ($path in $ordered) {
            $absolute = [IO.Path]::GetFullPath((Join-Path $Root $path))
            if (-not $absolute.StartsWith($Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Input path escaped the repository.'
            }
            $digest = 'missing'
            if ([IO.File]::Exists($absolute)) {
                $stream = [IO.File]::Open($absolute, 'Open', 'Read', ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                try { $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
                finally { $stream.Dispose() }
            }
            [void]$content.Append($path).Append([char]0).Append($digest).Append("`n")
        }
        # Only build-related environment values; never hash arbitrary secrets.
        foreach ($key in @('CMAKE_GENERATOR', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET',
            'SNOWDESKTOP_STEAMWORKS_SDK_PATH', 'VCToolsInstallDir', 'WindowsSdkDir', 'WindowsSDKVersion')) {
            [void]$content.Append('ENV:').Append($key).Append([char]0).Append(
                [string][Environment]::GetEnvironmentVariable($key, 'Process')).Append("`n")
        }
        $digest = [BitConverter]::ToString($sha.ComputeHash($encoding.GetBytes($content.ToString()))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
    return [pscustomobject]@{ algorithm = 'sha256-repository-inputs-v1'; digest = $digest; fileCount = $ordered.Count; head = $head }
}
