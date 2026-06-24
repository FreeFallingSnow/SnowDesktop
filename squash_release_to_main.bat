@echo off
setlocal
cd /d "%~dp0"

for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content version.json | ConvertFrom-Json).version"`) do set VERSION=%%v
if "%VERSION%"=="" (
    echo Failed to read version from version.json.
    exit /b 1
)

for /f "delims=" %%b in ('git branch --show-current 2^>nul') do set CURRENT_BRANCH=%%b
if /i not "%CURRENT_BRANCH:~0,9%"=="release/v" (
    echo Refusing to squash: current branch is "%CURRENT_BRANCH%".
    echo Expected a version branch named release/vX.Y.Z.
    exit /b 1
)

set "TAG=%CURRENT_BRANCH:~8%"
set "BRANCH_VERSION=%CURRENT_BRANCH:~9%"
if /i not "%VERSION%"=="%BRANCH_VERSION%" (
    echo Refusing to squash: version.json does not match the release branch.
    echo Branch version : %BRANCH_VERSION%
    echo version.json   : %VERSION%
    echo Update version.json and commit it on %CURRENT_BRANCH% before retrying.
    exit /b 1
)

set DIRTY=
for /f "delims=" %%s in ('git status --porcelain --untracked-files^=normal') do set DIRTY=1
if defined DIRTY (
    echo Refusing to squash: the working tree is not clean.
    git status --short
    exit /b 1
)

git show-ref --verify --quiet refs/heads/main
if errorlevel 1 (
    echo Refusing to squash: local branch "main" does not exist.
    exit /b 1
)

git merge-base --is-ancestor main "%CURRENT_BRANCH%"
if errorlevel 1 (
    echo Refusing to squash: local main is not an ancestor of %CURRENT_BRANCH%.
    echo Update or reconcile the local branches before retrying.
    exit /b 1
)

set "COMMIT_MESSAGE=%~1"
if not defined COMMIT_MESSAGE (
    set /p "COMMIT_MESSAGE=Version commit message [%TAG% - version update]: "
)
if not defined COMMIT_MESSAGE set "COMMIT_MESSAGE=%TAG% - version update"

powershell -NoProfile -Command "if (-not $env:COMMIT_MESSAGE.StartsWith($env:TAG)) { exit 1 }"
if errorlevel 1 (
    echo Refusing to squash: the version commit message must start with %TAG%.
    echo Message: %COMMIT_MESSAGE%
    exit /b 1
)

git rev-parse -q --verify "refs/tags/%TAG%" >nul 2>&1
if not errorlevel 1 (
    echo Refusing to squash: local tag %TAG% already exists.
    exit /b 1
)

echo.
echo Local squash release
echo   Source : %CURRENT_BRANCH%
echo   Target : main
echo   Commit : "%COMMIT_MESSAGE%"
echo.
echo This script performs local Git operations only.
echo It will not fetch, pull, push, call remote APIs, or delete any branch.
echo After the squash commit, it will create the local annotated tag %TAG%.
echo.
choice /C YN /N /M "Continue with the local squash merge? [Y/N] "
if errorlevel 2 (
    echo Squash merge cancelled.
    exit /b 0
)

git switch main
if errorlevel 1 (
    echo Failed to switch to local main.
    exit /b 1
)

git merge --squash "%CURRENT_BRANCH%"
if errorlevel 1 (
    echo.
    echo Squash merge stopped before commit, possibly because of conflicts.
    echo Resolve the local working tree manually. Nothing was pushed.
    exit /b 1
)

git diff --cached --quiet
if not errorlevel 1 (
    echo No changes were produced by the squash merge.
    echo Nothing was committed or pushed.
    exit /b 1
)

git commit -m "%COMMIT_MESSAGE%"
if errorlevel 1 (
    echo.
    echo Failed to create the local version commit.
    echo Squashed changes remain staged. Nothing was pushed.
    exit /b 1
)

git tag -a "%TAG%" -m "%COMMIT_MESSAGE%"
if errorlevel 1 (
    echo.
    echo The local squash commit was created, but local tag %TAG% failed.
    echo Nothing was pushed.
    exit /b 1
)

echo.
echo Local squash merge completed successfully.
git log -1 --oneline --decorate
echo.
echo Review and test local main before publishing.
echo No remote operation was performed.
exit /b 0
