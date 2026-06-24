@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo Reading version from version.json...
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content version.json | ConvertFrom-Json).version"`) do set TAG=v%%v

if "%TAG%"=="v" (
    echo Failed to read version from version.json
    pause
    exit /b 1
)

echo Version: %TAG%
echo.

echo [1/4] Building Release...
echo.
cmake -B .build -S .
if errorlevel 1 ( echo CMake configure FAILED & pause & exit /b 1 )

cmake --build .build --config Release --target SnowDesktop
if errorlevel 1 ( echo Build FAILED & pause & exit /b 1 )

echo.
echo [2/4] Staging artifacts to release\ ...
echo.

if not exist release mkdir release

echo Copying SnowDesktop.exe (.build\Release\SnowDesktop.exe -^> release\)
copy /Y ".build\Release\SnowDesktop.exe" "release\" >nul
if errorlevel 1 ( echo Failed to copy SnowDesktop.exe & pause & exit /b 1 )
echo   OK

echo Copying widgets\ (including documentation and skills)...
if exist "release\widgets" rd /s /q "release\widgets"
xcopy /E /I /Y "widgets" "release\widgets" >nul
if errorlevel 1 ( echo Failed to copy widgets & pause & exit /b 1 )
echo   OK

echo.
echo [3/4] Changes staged in the release repository for tag %TAG%
echo.
cd /d release
echo Files to be released:
git status --short
echo.
echo Publish the binary release repository with:
echo   cd release
echo   git add -A
echo   git commit -m "%TAG%"
echo   git tag -a %TAG% -m "%TAG%"
echo   git push origin main
echo   git push origin %TAG%
echo.
cd /d "%~dp0"

echo [4/4] Main source repository squash and local tag
echo.
echo Create the single local version commit and local annotated tag with:
echo   squash_release_to_main.bat "%TAG% - update summary"
echo.
echo That script performs local operations only and never pushes.
echo After reviewing and testing local main, push main and the tag manually.
echo.
choice /C YN /N /M "Run the guarded local squash and tag step now? [Y/N] "
if errorlevel 2 (
    echo Skipped. When the release branch is ready, run:
    echo   squash_release_to_main.bat "%TAG% - update summary"
    echo.
    pause
    exit /b 0
)

call "%~dp0squash_release_to_main.bat" "%TAG% - version update"
if errorlevel 1 (
    echo.
    echo Local squash commit and tag were not completed.
    echo Resolve the message above, then run squash_release_to_main.bat again.
    pause
    exit /b 1
)

echo.
echo Release preparation and local source release commit completed.
echo Review and test before manually pushing main and %TAG%.
echo.
pause
