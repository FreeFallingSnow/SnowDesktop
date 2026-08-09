[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet(
        "menu",
        "status",
        "package",
        "sync-release",
        "prepare",
        "squash",
        "publish",
        "github-release",
        "open")]
    [string]$Command = "menu",

    [string]$Message = "",
    [string]$ConfirmVersion = "",
    [string]$CertificatePath = "",
    [string]$CertificateThumbprint = "",
    [ValidateSet("CurrentUser", "LocalMachine")]
    [string]$CertificateStoreLocation = "CurrentUser",
    [switch]$Development,
    [switch]$Json,
    [switch]$Yes
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $scriptDirectory ".."))
$releaseRepository = Join-Path $repositoryRoot "release"
$artifactsRoot = Join-Path $repositoryRoot "artifacts"
$packageScript = Join-Path $scriptDirectory "package_release.ps1"
$buildScript = Join-Path $scriptDirectory "build.bat"
$squashScript = Join-Path $scriptDirectory "squash_release_to_main.bat"
$sourceRemote = "https://github.com/FreeFallingSnow/SnowDesktop.git"
$binaryRemote =
    "https://github.com/FreeFallingSnow/SnowDesktop_Release.git"
$githubRepository = "FreeFallingSnow/SnowDesktop"
$isMenu = $Command -eq "menu"

function Get-Version {
    $versionPath = Join-Path $repositoryRoot "version.json"
    $value = (Get-Content -LiteralPath $versionPath -Encoding UTF8 -Raw |
        ConvertFrom-Json).version
    $parts = $value -split "\."
    if ($parts.Count -ne 4 -or
        $parts[0] -eq "0" -or
        $parts[3] -ne "0") {
        throw "version.json must use Store-compatible A.B.C.0 format."
    }
    foreach ($part in $parts) {
        if ($part -notmatch "^(0|[1-9][0-9]*)$" -or
            [uint64]$part -gt 65535) {
            throw "Invalid version component '$part' in $value."
        }
    }
    return $value
}

function Invoke-GitCapture {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $output = @(& git -C $WorkingDirectory @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ') failed in $WorkingDirectory.`n$($output -join "`n")"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Text = ($output -join "`n").Trim()
        Lines = $output
    }
}

function Get-GitValue {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    return (Invoke-GitCapture `
        -WorkingDirectory $WorkingDirectory `
        -Arguments $Arguments).Text
}

function Get-ReleaseContext {
    $version = Get-Version
    $tag = "v$version"
    $versionDirectory = Join-Path $artifactsRoot $tag
    $sourceBranch = Get-GitValue `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("branch", "--show-current")
    $sourceStatus = Get-GitValue `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("status", "--short")
    $sourceOrigin = Get-GitValue `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("remote", "get-url", "origin")
    $sourceCommit = Get-GitValue `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("rev-parse", "--short=12", "HEAD")

    $releaseExists = Test-Path `
        -LiteralPath (Join-Path $releaseRepository ".git") `
        -PathType Container
    $releaseBranch = "(not found)"
    $releaseStatus = ""
    $releaseOrigin = ""
    if ($releaseExists) {
        $releaseBranch = Get-GitValue `
            -WorkingDirectory $releaseRepository `
            -Arguments @("branch", "--show-current")
        $releaseStatus = Get-GitValue `
            -WorkingDirectory $releaseRepository `
            -Arguments @("status", "--short")
        $releaseOrigin = Get-GitValue `
            -WorkingDirectory $releaseRepository `
            -Arguments @("remote", "get-url", "origin")
    }

    $requiredNames = @(
        "SnowDesktop-portable-x64-$version.zip",
        "SnowDesktop-Store-x64-$version.msix",
        "SnowDesktop-Store-x64-$version.appxsym",
        "SnowDesktop-Store-x64-$version.msixupload",
        "package-info.json",
        "SHA256SUMS.txt",
        "release-summary.md",
        "release-notes.md"
    )
    $missing = @($requiredNames | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $versionDirectory $_) `
            -PathType Leaf)
    })

    $tagResult = Invoke-GitCapture `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("rev-parse", "-q", "--verify", "refs/tags/$tag") `
        -AllowFailure

    return [pscustomobject]@{
        Version = $version
        Tag = $tag
        ExpectedBranch = "release/v$version"
        VersionDirectory = $versionDirectory
        SourceBranch = $sourceBranch
        SourceStatus = $sourceStatus
        SourceDirty = -not [string]::IsNullOrWhiteSpace($sourceStatus)
        SourceOrigin = $sourceOrigin
        SourceCommit = $sourceCommit
        SourceTagExists = $tagResult.ExitCode -eq 0
        ReleaseExists = $releaseExists
        ReleaseBranch = $releaseBranch
        ReleaseStatus = $releaseStatus
        ReleaseDirty = -not [string]::IsNullOrWhiteSpace($releaseStatus)
        ReleaseOrigin = $releaseOrigin
        PackagesReady = $missing.Count -eq 0
        MissingPackages = $missing
    }
}

function Write-StatusFlag {
    param(
        [Parameter(Mandatory = $true)][bool]$Value,
        [Parameter(Mandatory = $true)][string]$TrueText,
        [Parameter(Mandatory = $true)][string]$FalseText
    )

    if ($Value) {
        Write-Host $TrueText -ForegroundColor Green
    }
    else {
        Write-Host $FalseText -ForegroundColor Yellow
    }
}

function Show-Dashboard {
    param([switch]$Clear)

    if ($Clear) {
        Clear-Host
    }
    $context = Get-ReleaseContext
    Write-Host "SnowDesktop 发布中心" -ForegroundColor Cyan
    Write-Host ("=" * 66) -ForegroundColor DarkGray
    Write-Host "版本        : $($context.Tag)"
    Write-Host "源码分支    : $($context.SourceBranch)"
    Write-Host "预期分支    : $($context.ExpectedBranch)"
    Write-Host "源码提交    : $($context.SourceCommit)"
    Write-Host "版本目录    : $($context.VersionDirectory)"
    Write-Host -NoNewline "发行包      : "
    Write-StatusFlag `
        -Value $context.PackagesReady `
        -TrueText "完整" `
        -FalseText "未生成或不完整"
    Write-Host -NoNewline "源码工作区  : "
    Write-StatusFlag `
        -Value (-not $context.SourceDirty) `
        -TrueText "干净" `
        -FalseText "有未提交修改"
    Write-Host -NoNewline "Release 仓库: "
    Write-StatusFlag `
        -Value $context.ReleaseExists `
        -TrueText "$($context.ReleaseBranch)，已连接" `
        -FalseText "未找到"
    if ($context.ReleaseExists) {
        Write-Host -NoNewline "Release 状态 : "
        Write-StatusFlag `
            -Value (-not $context.ReleaseDirty) `
            -TrueText "干净" `
            -FalseText "有待发布修改"
    }
    Write-Host ("=" * 66) -ForegroundColor DarkGray
    return $context
}

function Wait-ForMenu {
    if ($isMenu) {
        Write-Host ""
        [void](Read-Host "按 Enter 返回菜单")
    }
}

function Confirm-Interactive {
    param([Parameter(Mandatory = $true)][string]$Prompt)

    $answer = Read-Host "$Prompt [y/N]"
    return $answer -match "^(?i:y|yes)$"
}

function Assert-ExplicitVersionConfirmation {
    param(
        [Parameter(Mandatory = $true)][string]$ActionName,
        [Parameter(Mandatory = $true)]$Context
    )

    if ($isMenu) {
        $answer = Read-Host `
            "$ActionName 会改变 Git 状态。请输入 $($Context.Tag) 确认"
        if ($answer -ne $Context.Tag) {
            throw "$ActionName 已取消：确认版本不匹配。"
        }
        return
    }

    if (-not $Yes -or
        ($ConfirmVersion -ne $Context.Version -and
        $ConfirmVersion -ne $Context.Tag)) {
        throw "$ActionName requires -Yes -ConfirmVersion $($Context.Version) in CLI mode."
    }
}

function Assert-Remote {
    param(
        [Parameter(Mandatory = $true)][string]$Actual,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$RepositoryName
    )

    if ($Actual -ne $Expected) {
        throw "$RepositoryName origin is '$Actual'; expected '$Expected'."
    }
}

function Get-LogsDirectory {
    param([Parameter(Mandatory = $true)]$Context)

    $path = Join-Path $Context.VersionDirectory "logs"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

function Write-NewLogContent {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ref]$DisplayedLength
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return
    }
    try {
        $content = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    }
    catch {
        return
    }
    if ($null -eq $content) {
        return
    }
    if ($content.Length -lt $DisplayedLength.Value) {
        $DisplayedLength.Value = 0
    }
    if ($content.Length -gt $DisplayedLength.Value) {
        $newContent = $content.Substring($DisplayedLength.Value)
        Write-Host -NoNewline $newContent
        $DisplayedLength.Value = $content.Length
    }
}

function Invoke-BatchWithLiveLog {
    param(
        [Parameter(Mandatory = $true)][string]$BatchPath,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    $commandLine = "call `"$BatchPath`" > `"$LogPath`" 2>&1"
    $process = Start-Process `
        -FilePath "cmd.exe" `
        -ArgumentList @("/d", "/c", $commandLine) `
        -WindowStyle Hidden `
        -PassThru
    $displayedLength = 0
    try {
        while (-not $process.HasExited) {
            Write-NewLogContent `
                -Path $LogPath `
                -DisplayedLength ([ref]$displayedLength)
            Start-Sleep -Milliseconds 100
            $process.Refresh()
        }
        $process.WaitForExit()
        Write-NewLogContent `
            -Path $LogPath `
            -DisplayedLength ([ref]$displayedLength)
        return $process.ExitCode
    }
    finally {
        $process.Dispose()
    }
}

function Set-ReleaseState {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][object]$Value
    )

    New-Item -ItemType Directory `
        -Path $Context.VersionDirectory -Force | Out-Null
    $statePath = Join-Path $Context.VersionDirectory "release-state.json"
    $state = [ordered]@{
        version = $Context.Version
    }
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        $existing = Get-Content -LiteralPath $statePath -Encoding UTF8 -Raw |
            ConvertFrom-Json
        foreach ($property in $existing.PSObject.Properties) {
            $state[$property.Name] = $property.Value
        }
    }
    $state[$Name] = $Value
    $state["updatedAt"] = (Get-Date).ToUniversalTime().ToString("o")
    $state | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $statePath -Encoding utf8
}

function Invoke-Package {
    param([switch]$AskBeforeBuild)

    $context = Get-ReleaseContext
    New-Item -ItemType Directory `
        -Path $context.VersionDirectory -Force | Out-Null
    $logsDirectory = Get-LogsDirectory -Context $context
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $buildLog = Join-Path $logsDirectory "build-$timestamp.log"
    $packageLog = Join-Path $logsDirectory "package-$timestamp.log"

    Write-Host ""
    Write-Host "[1/2] 使用 scripts/build.bat 构建 Release" -ForegroundColor Cyan
    $buildExitCode = Invoke-BatchWithLiveLog `
        -BatchPath $buildScript `
        -LogPath $buildLog
    if ($buildExitCode -ne 0) {
        throw "scripts/build.bat failed with exit code $buildExitCode. See $buildLog"
    }
    $releaseExecutable = Join-Path `
        $repositoryRoot ".build\Release\SnowDesktop.exe"
    if (-not (Test-Path -LiteralPath $releaseExecutable -PathType Leaf)) {
        throw "Release executable was not generated: $releaseExecutable"
    }

    Write-Host ""
    Write-Host "[2/2] 生成携带版、MSIX 和商店上传包" `
        -ForegroundColor Cyan
    $powershell = Join-Path $PSHOME "powershell.exe"
    $packageArguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $packageScript,
        "-SkipBuild",
        "-OutputDirectory", $context.VersionDirectory
    )
    if ($Development) {
        $packageArguments += "-Development"
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificatePath)) {
        $packageArguments += @("-CertificatePath", $CertificatePath)
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        $packageArguments += @(
            "-CertificateThumbprint", $CertificateThumbprint,
            "-CertificateStoreLocation", $CertificateStoreLocation)
    }
    & $powershell @packageArguments 2>&1 |
        Tee-Object -FilePath $packageLog
    $packageExitCode = $LASTEXITCODE
    if ($packageExitCode -ne 0) {
        throw "Package generation failed with exit code $packageExitCode. See $packageLog"
    }

    $context = Get-ReleaseContext
    if (-not $context.PackagesReady) {
        throw "Packaging completed but required files are missing: $($context.MissingPackages -join ', ')"
    }
    Set-ReleaseState `
        -Context $context `
        -Name "packagedAt" `
        -Value (Get-Date).ToUniversalTime().ToString("o")
    Write-Host ""
    Write-Host "全部发行包已生成：" -ForegroundColor Green
    Write-Host "  $($context.VersionDirectory)"
    return $true
}

function Reset-TemporaryDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd("\") + "\"
    if (-not $fullPath.StartsWith(
        $fullParent, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset path outside $Parent`: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $fullPath -Force | Out-Null
}

function Copy-MirroredDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Package directory is missing: $Source"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & robocopy.exe $Source $Destination `
        /MIR /NFL /NDL /NJH /NJS /NP 2>&1 |
        Tee-Object -FilePath $LogPath -Append
    $robocopyExit = $LASTEXITCODE
    if ($robocopyExit -ge 8) {
        throw "robocopy failed with exit code $robocopyExit."
    }
}

function Sync-ReleaseRepository {
    $context = Get-ReleaseContext
    if (-not $context.ReleaseExists) {
        throw "Binary release repository was not found at $releaseRepository"
    }
    Assert-Remote `
        -Actual $context.ReleaseOrigin `
        -Expected $binaryRemote `
        -RepositoryName "Binary release repository"
    if ($context.ReleaseBranch -ne "main") {
        throw "Binary release repository must be on main; current branch is $($context.ReleaseBranch)."
    }
    if ($context.ReleaseDirty) {
        if ($isMenu) {
            Write-Host "二进制 Release 仓库已有未提交修改：" `
                -ForegroundColor Yellow
            Write-Host $context.ReleaseStatus
            if (-not (Confirm-Interactive `
                "同步可能覆盖上述发行文件，确认继续吗？")) {
                Write-Host "已取消同步。"
                return $false
            }
        }
        elseif (-not $Yes -or
            ($ConfirmVersion -ne $context.Version -and
            $ConfirmVersion -ne $context.Tag)) {
            throw "Binary release repository is dirty. Retry with -Yes -ConfirmVersion $($context.Version) after reviewing its changes."
        }
    }
    if (-not $context.PackagesReady) {
        throw "Package files are incomplete. Run 'package' first."
    }

    $portablePath = Join-Path $context.VersionDirectory `
        "SnowDesktop-portable-x64-$($context.Version).zip"
    $temporary = Join-Path $context.VersionDirectory "_release-payload"
    Reset-TemporaryDirectory `
        -Path $temporary `
        -Parent $context.VersionDirectory
    try {
        Expand-Archive `
            -LiteralPath $portablePath `
            -DestinationPath $temporary `
            -Force
        foreach ($name in @(
                "SnowDesktop.exe",
                "SnowDesktopTaskbarHook.dll",
                "README.md",
                "README.en.md")) {
            $source = Join-Path $temporary $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "Portable package is missing $name."
            }
            Copy-Item `
                -LiteralPath $source `
                -Destination (Join-Path $releaseRepository $name) `
                -Force
        }
        $license = Join-Path $temporary "LICENSE"
        if (Test-Path -LiteralPath $license -PathType Leaf) {
            Copy-Item `
                -LiteralPath $license `
                -Destination (Join-Path $releaseRepository "LICENSE") `
                -Force
        }

        $logsDirectory = Get-LogsDirectory -Context $context
        $syncLog = Join-Path $logsDirectory `
            "release-sync-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
        Copy-MirroredDirectory `
            -Source (Join-Path $temporary "widgets") `
            -Destination (Join-Path $releaseRepository "widgets") `
            -LogPath $syncLog
        Copy-MirroredDirectory `
            -Source (Join-Path $temporary "lang") `
            -Destination (Join-Path $releaseRepository "lang") `
            -LogPath $syncLog

        $status = Get-GitValue `
            -WorkingDirectory $releaseRepository `
            -Arguments @("status", "--short")
        $statusPath = Join-Path `
            $context.VersionDirectory "release-repository-status.txt"
        $status | Set-Content -LiteralPath $statusPath -Encoding utf8
        Set-ReleaseState `
            -Context $context `
            -Name "releaseRepositorySyncedAt" `
            -Value (Get-Date).ToUniversalTime().ToString("o")
        Write-Host ""
        Write-Host "二进制 Release 仓库已同步（尚未提交或推送）：" `
            -ForegroundColor Green
        if ([string]::IsNullOrWhiteSpace($status)) {
            Write-Host "  内容与当前仓库相同。"
        }
        else {
            Write-Host $status
        }
        return $true
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Recurse -Force
        }
    }
}

function Invoke-LocalSquash {
    $context = Get-ReleaseContext
    Assert-Remote `
        -Actual $context.SourceOrigin `
        -Expected $sourceRemote `
        -RepositoryName "Source repository"
    if ($context.SourceBranch -ne $context.ExpectedBranch) {
        throw "Current branch is $($context.SourceBranch); expected $($context.ExpectedBranch)."
    }
    if ($context.SourceDirty) {
        throw "Source working tree must be clean before squash merge.`n$($context.SourceStatus)"
    }
    Assert-ExplicitVersionConfirmation `
        -ActionName "本地压缩合并与创建标签" `
        -Context $context

    $commitMessage = $Message
    if ([string]::IsNullOrWhiteSpace($commitMessage) -and $isMenu) {
        $commitMessage = Read-Host `
            "版本提交说明 [$($context.Tag) - version update]"
    }
    if ([string]::IsNullOrWhiteSpace($commitMessage)) {
        $commitMessage = "$($context.Tag) - version update"
    }
    if (-not $commitMessage.StartsWith($context.Tag)) {
        throw "Commit message must start with $($context.Tag)."
    }

    $logsDirectory = Get-LogsDirectory -Context $context
    $logPath = Join-Path $logsDirectory `
        "squash-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
    $temporarySquashScript = Join-Path `
        ([System.IO.Path]::GetTempPath()) `
        ("SnowDesktop-squash-{0}-{1}.bat" -f `
            $PID, [guid]::NewGuid().ToString("N"))
    Copy-Item `
        -LiteralPath $squashScript `
        -Destination $temporarySquashScript `
        -Force
    $previousApproval = $env:SNOWDESKTOP_RELEASE_APPROVED
    $previousMessage = $env:SNOWDESKTOP_RELEASE_MESSAGE
    $previousRepositoryRoot = $env:SNOWDESKTOP_REPOSITORY_ROOT
    try {
        $env:SNOWDESKTOP_RELEASE_APPROVED = "1"
        $env:SNOWDESKTOP_RELEASE_MESSAGE = $commitMessage
        $env:SNOWDESKTOP_REPOSITORY_ROOT = $repositoryRoot
        $exitCode = Invoke-BatchWithLiveLog `
            -BatchPath $temporarySquashScript `
            -LogPath $logPath
        if ($exitCode -ne 0) {
            throw "Local squash failed with exit code $exitCode. See $logPath"
        }
    }
    finally {
        $env:SNOWDESKTOP_RELEASE_APPROVED = $previousApproval
        $env:SNOWDESKTOP_RELEASE_MESSAGE = $previousMessage
        $env:SNOWDESKTOP_REPOSITORY_ROOT = $previousRepositoryRoot
        if (Test-Path -LiteralPath $temporarySquashScript) {
            Remove-Item -LiteralPath $temporarySquashScript -Force
        }
    }

    Set-ReleaseState `
        -Context $context `
        -Name "sourceSquashedAt" `
        -Value (Get-Date).ToUniversalTime().ToString("o")
    Write-Host ""
    Write-Host "本地 main 和标签已生成。请先完整测试本地 main，" `
        -ForegroundColor Green
    Write-Host "确认后再从菜单执行远程发布。"
}

function Invoke-CheckedGit {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    & git -C $WorkingDirectory @Arguments 2>&1 |
        Tee-Object -FilePath $LogPath -Append
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed. See $LogPath"
    }
}

function Publish-SourceAndReleaseRepositories {
    $context = Get-ReleaseContext
    Assert-ExplicitVersionConfirmation `
        -ActionName "远程发布源码与二进制仓库" `
        -Context $context
    Assert-Remote `
        -Actual $context.SourceOrigin `
        -Expected $sourceRemote `
        -RepositoryName "Source repository"
    Assert-Remote `
        -Actual $context.ReleaseOrigin `
        -Expected $binaryRemote `
        -RepositoryName "Binary release repository"
    if ($context.SourceBranch -ne "main") {
        throw "Source repository must be on main; current branch is $($context.SourceBranch)."
    }
    if ($context.SourceDirty) {
        throw "Source working tree must be clean before publishing."
    }
    if (-not $context.SourceTagExists) {
        throw "Local source tag $($context.Tag) does not exist."
    }
    $head = Get-GitValue `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("rev-parse", "HEAD")
    $tagCommit = Get-GitValue `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("rev-list", "-n", "1", $context.Tag)
    if ($head -ne $tagCommit) {
        throw "Tag $($context.Tag) does not point to local main HEAD."
    }
    if ($context.ReleaseBranch -ne "main") {
        throw "Binary release repository must be on main."
    }

    $logsDirectory = Get-LogsDirectory -Context $context
    $logPath = Join-Path $logsDirectory `
        "publish-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"

    $releaseTagResult = Invoke-GitCapture `
        -WorkingDirectory $releaseRepository `
        -Arguments @("rev-parse", "-q", "--verify",
            "refs/tags/$($context.Tag)") `
        -AllowFailure
    if ($releaseTagResult.ExitCode -ne 0) {
        if (-not $context.ReleaseDirty) {
            throw "Binary release repository has no changes to commit for $($context.Tag)."
        }
        Invoke-CheckedGit `
            -WorkingDirectory $releaseRepository `
            -Arguments @("add", "-A") `
            -LogPath $logPath
        Invoke-CheckedGit `
            -WorkingDirectory $releaseRepository `
            -Arguments @("commit", "-m", $context.Tag) `
            -LogPath $logPath
        Invoke-CheckedGit `
            -WorkingDirectory $releaseRepository `
            -Arguments @("tag", "-a", $context.Tag, "-m", $context.Tag) `
            -LogPath $logPath
    }
    elseif ($context.ReleaseDirty) {
        throw "Binary tag $($context.Tag) already exists but its repository is dirty."
    }

    Write-Host "推送源码 main 与 $($context.Tag)..." -ForegroundColor Cyan
    Invoke-CheckedGit `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("push", "origin", "main") `
        -LogPath $logPath
    Invoke-CheckedGit `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("push", "origin", $context.Tag) `
        -LogPath $logPath

    Write-Host "推送二进制 Release 仓库..." -ForegroundColor Cyan
    Invoke-CheckedGit `
        -WorkingDirectory $releaseRepository `
        -Arguments @("push", "origin", "main") `
        -LogPath $logPath
    Invoke-CheckedGit `
        -WorkingDirectory $releaseRepository `
        -Arguments @("push", "origin", $context.Tag) `
        -LogPath $logPath

    Set-ReleaseState `
        -Context $context `
        -Name "repositoriesPublishedAt" `
        -Value (Get-Date).ToUniversalTime().ToString("o")
    Write-Host "源码与二进制仓库发布完成。" -ForegroundColor Green
}

function Publish-GitHubRelease {
    $context = Get-ReleaseContext
    Assert-ExplicitVersionConfirmation `
        -ActionName "创建 GitHub Release" `
        -Context $context
    if (-not $context.PackagesReady) {
        throw "Package files are incomplete."
    }

    $remoteTag = Invoke-GitCapture `
        -WorkingDirectory $repositoryRoot `
        -Arguments @("ls-remote", "--exit-code", "--tags", "origin",
            "refs/tags/$($context.Tag)") `
        -AllowFailure
    if ($remoteTag.ExitCode -ne 0) {
        throw "Tag $($context.Tag) is not published on source origin."
    }

    $notes = Join-Path $context.VersionDirectory "release-notes.md"
    $portable = Join-Path $context.VersionDirectory `
        "SnowDesktop-portable-x64-$($context.Version).zip"
    $checksums = Join-Path $context.VersionDirectory "SHA256SUMS.txt"
    $assets = @($portable, $checksums)
    $packageInfo = Get-Content `
        -LiteralPath (Join-Path $context.VersionDirectory "package-info.json") `
        -Encoding UTF8 -Raw | ConvertFrom-Json
    if ($packageInfo.msix.signed) {
        $assets += Join-Path $context.VersionDirectory $packageInfo.msix.path
    }

    $logsDirectory = Get-LogsDirectory -Context $context
    $logPath = Join-Path $logsDirectory `
        "github-release-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"

    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if ($gh) {
        $view = @(& $gh.Source release view $context.Tag `
            --repo $githubRepository 2>&1)
        if ($LASTEXITCODE -eq 0) {
            throw "GitHub Release $($context.Tag) already exists; refusing to overwrite it."
        }

        & $gh.Source release create $context.Tag @assets `
            --repo $githubRepository `
            --title "SnowDesktop $($context.Tag)" `
            --notes-file $notes 2>&1 |
            Tee-Object -FilePath $logPath
        if ($LASTEXITCODE -ne 0) {
            throw "GitHub Release creation failed. See $logPath"
        }
    }
    elseif (-not [string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
        $headers = @{
            Authorization = "Bearer $($env:GITHUB_TOKEN)"
            Accept = "application/vnd.github+json"
            "X-GitHub-Api-Version" = "2022-11-28"
            "User-Agent" = "SnowDesktop-release-manager"
        }
        $apiRoot = "https://api.github.com/repos/$githubRepository"
        $encodedTag = [System.Uri]::EscapeDataString($context.Tag)
        try {
            [void](Invoke-RestMethod `
                -Method Get `
                -Uri "$apiRoot/releases/tags/$encodedTag" `
                -Headers $headers)
            throw "GitHub Release $($context.Tag) already exists; refusing to overwrite it."
        }
        catch {
            $statusCode = $null
            if ($_.Exception.Response) {
                $statusCode = [int]$_.Exception.Response.StatusCode
            }
            if ($statusCode -ne 404) {
                throw
            }
        }

        $releaseBody = @{
            tag_name = $context.Tag
            target_commitish = "main"
            name = "SnowDesktop $($context.Tag)"
            body = Get-Content -LiteralPath $notes -Encoding UTF8 -Raw
            draft = $false
            prerelease = $false
        } | ConvertTo-Json
        $release = Invoke-RestMethod `
            -Method Post `
            -Uri "$apiRoot/releases" `
            -Headers $headers `
            -ContentType "application/json; charset=utf-8" `
            -Body ([System.Text.Encoding]::UTF8.GetBytes($releaseBody))
        "Created release id $($release.id)." |
            Tee-Object -FilePath $logPath
        $uploadRoot = $release.upload_url -replace "\{\?name,label\}$", ""
        foreach ($asset in $assets) {
            $assetName = [System.IO.Path]::GetFileName($asset)
            $encodedName = [System.Uri]::EscapeDataString($assetName)
            [void](Invoke-RestMethod `
                -Method Post `
                -Uri "${uploadRoot}?name=$encodedName" `
                -Headers $headers `
                -ContentType "application/octet-stream" `
                -InFile $asset)
            "Uploaded $assetName." |
                Tee-Object -FilePath $logPath -Append
        }
    }
    else {
        throw "GitHub publishing needs an authenticated gh CLI or a GITHUB_TOKEN environment variable. No credential was found."
    }
    Set-ReleaseState `
        -Context $context `
        -Name "githubReleaseCreatedAt" `
        -Value (Get-Date).ToUniversalTime().ToString("o")
    Write-Host "GitHub Release $($context.Tag) 创建完成。" `
        -ForegroundColor Green
}

function Open-VersionDirectory {
    $context = Get-ReleaseContext
    New-Item -ItemType Directory `
        -Path $context.VersionDirectory -Force | Out-Null
    Start-Process explorer.exe -ArgumentList $context.VersionDirectory
}

function Invoke-Prepare {
    param([switch]$AskBeforeBuild)

    $packaged = Invoke-Package -AskBeforeBuild:$AskBeforeBuild
    if (-not $packaged) {
        return
    }
    $synced = Sync-ReleaseRepository
    if (-not $synced) {
        return
    }
    Write-Host ""
    Write-Host "发布准备完成：包已生成，Release 仓库已同步但未提交。" `
        -ForegroundColor Green
    Write-Host "提交源码修改后，可执行本地 squash；测试 main 后再远程发布。"
}

function Invoke-CommandAction {
    param([Parameter(Mandatory = $true)][string]$Name)

    switch ($Name) {
        "status" {
            if ($Json) {
                Get-ReleaseContext | ConvertTo-Json -Depth 4
            }
            else {
                [void](Show-Dashboard)
            }
        }
        "package" {
            [void](Invoke-Package -AskBeforeBuild:$isMenu)
        }
        "sync-release" {
            [void](Sync-ReleaseRepository)
        }
        "prepare" {
            Invoke-Prepare -AskBeforeBuild:$isMenu
        }
        "squash" {
            Invoke-LocalSquash
        }
        "publish" {
            Publish-SourceAndReleaseRepositories
        }
        "github-release" {
            Publish-GitHubRelease
        }
        "open" {
            Open-VersionDirectory
        }
        default {
            throw "Unknown release action: $Name"
        }
    }
}

function Start-ReleaseMenu {
    while ($true) {
        [void](Show-Dashboard -Clear)
        Write-Host ""
        Write-Host "[1] 构建并生成全部发行包"
        Write-Host "[2] 同步二进制 Release 仓库（不提交）"
        Write-Host "[3] 发布准备（构建打包 + 同步 Release 仓库）"
        Write-Host "[4] 压缩合并版本分支到本地 main，并创建标签"
        Write-Host "[5] 发布源码与二进制仓库（需先测试本地 main）"
        Write-Host "[6] 创建 GitHub Release 并上传公开附件"
        Write-Host "[7] 刷新状态"
        Write-Host "[O] 打开当前版本目录"
        Write-Host "[Q] 退出"
        Write-Host ""
        $selection = (Read-Host "请选择").Trim().ToUpperInvariant()
        if ($selection -eq "Q") {
            return
        }
        try {
            switch ($selection) {
                "1" { Invoke-CommandAction -Name "package" }
                "2" { Invoke-CommandAction -Name "sync-release" }
                "3" { Invoke-CommandAction -Name "prepare" }
                "4" { Invoke-CommandAction -Name "squash" }
                "5" { Invoke-CommandAction -Name "publish" }
                "6" { Invoke-CommandAction -Name "github-release" }
                "7" { continue }
                "O" { Invoke-CommandAction -Name "open" }
                default {
                    Write-Host "无效选项。" -ForegroundColor Yellow
                }
            }
        }
        catch {
            Write-Host ""
            Write-Host "操作失败：$($_.Exception.Message)" `
                -ForegroundColor Red
        }
        Wait-ForMenu
    }
}

Set-Location $repositoryRoot
if ($isMenu) {
    Start-ReleaseMenu
}
else {
    Invoke-CommandAction -Name $Command
}
