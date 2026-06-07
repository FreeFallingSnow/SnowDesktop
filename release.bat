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

echo [1/3] Building Release...
echo.
cmake -B .build -S .
if errorlevel 1 ( echo CMake configure FAILED & pause & exit /b 1 )

cmake --build .build --config Release --target SnowDesktop
if errorlevel 1 ( echo Build FAILED & pause & exit /b 1 )

echo.
echo [2/3] Staging artifacts to release\ ...
echo.

if not exist release mkdir release

echo Copying SnowDesktop.exe (.build\Release\SnowDesktop.exe -^> release\)
copy /Y ".build\Release\SnowDesktop.exe" "release\" >nul
if errorlevel 1 ( echo Failed to copy SnowDesktop.exe & pause & exit /b 1 )
echo   OK

if exist ".build\Release\widgets" (
    echo Copying widgets\ ...
    if exist "release\widgets" rd /s /q "release\widgets"
    xcopy /E /I /Y ".build\Release\widgets" "release\widgets" >nul
    echo   OK
)

echo.
echo [3/3] Changes staged in release\ for tag %TAG%
echo.
cd /d release
echo Files to be released:
git status --short
echo.
echo Run the following to publish:
echo   cd release
echo   git add -A
echo   git commit -m "%TAG%"
echo   git tag %TAG%
echo   git push origin main --tags
echo.
cd /d "%~dp0"
pause
