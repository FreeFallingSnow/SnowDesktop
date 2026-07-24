@echo off
cd /d "%~dp0"

echo === Configuring CMake (Release) ===
cmake -B .build -S .
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    pause
    exit /b 1
)

echo.
echo === Ensuring hook DLL is not locked by explorer ===
tasklist /fi "IMAGENAME eq SnowDesktop.exe" 2>nul | find /i "SnowDesktop.exe" >nul
if %ERRORLEVEL% EQU 0 (
    echo Stopping running SnowDesktop.exe...
    taskkill /f /im SnowDesktop.exe >nul 2>&1
    timeout /t 1 /nobreak >nul
)
rem Explorer keeps the hook DLL loaded; terminate it briefly to release the lock.
taskkill /f /im explorer.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Explorer terminated to release DLL lock.
    timeout /t 2 /nobreak >nul
    echo Starting explorer...
    start explorer.exe
)

echo.
echo === Building SnowDesktop.exe ===
cmake --build .build --config Release --target SnowDesktop --parallel
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo Taskbar appearance Hook: .build\Release\SnowDesktopTaskbarHook.dll
echo.
echo For a version release, run release.bat after the release branch has been
echo completed. It includes the guarded local squash and tag workflow.
echo.
pause
