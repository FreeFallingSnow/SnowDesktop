@echo off
cd /d "%~dp0"

echo === Configuring CMake (Release) ===
cmake -B .build -S .
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    if not defined SNOWDESKTOP_NONINTERACTIVE pause
    exit /b 1
)

echo.
echo === Ensuring hook DLL is not locked by explorer ===
tasklist /fi "IMAGENAME eq SnowDesktop.exe" 2>nul | find /i "SnowDesktop.exe" >nul
if %ERRORLEVEL% EQU 0 (
    echo Stopping running SnowDesktop.exe...
    taskkill /f /im SnowDesktop.exe >nul 2>&1
    ping 127.0.0.1 -n 2 >nul
)
rem Explorer keeps the hook DLL loaded; terminate it briefly to release the lock.
taskkill /f /im explorer.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Explorer terminated to release DLL lock.
    ping 127.0.0.1 -n 3 >nul
    echo Starting explorer...
    start "" explorer.exe >nul 2>&1
)

echo.
echo === Building SnowDesktop.exe and snowwidget.exe ===
cmake --build .build --config Release --target SnowDesktop snowwidget --parallel
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    if not defined SNOWDESKTOP_NONINTERACTIVE pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo Widget package tool: .build\Release\snowwidget.exe
echo Taskbar appearance Hook: .build\Release\SnowDesktopTaskbarHook.dll
echo.
echo For a version release, run release.bat to open the unified release center.
echo Agent and automation usage is available through release.bat COMMAND.
echo.
if not defined SNOWDESKTOP_NONINTERACTIVE pause
