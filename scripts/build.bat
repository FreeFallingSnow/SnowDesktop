@echo off
setlocal
cd /d "%~dp0.."

set "RELOAD_SHELL="
if /i "%~1"=="--reload-shell" (
    set "RELOAD_SHELL=1"
    shift
)
if not "%~1"=="" (
    echo Usage: scripts\build.bat [--reload-shell]
    exit /b 2
)

if defined RELOAD_SHELL (
    echo WARNING: --reload-shell stops SnowDesktop and restarts Explorer.
    taskkill /f /im SnowDesktop.exe >nul 2>&1
    tasklist /fi "IMAGENAME eq explorer.exe" /nh 2>nul | find /i "explorer.exe" >nul
    if not errorlevel 1 (
        taskkill /f /im explorer.exe >nul 2>&1
        timeout /t 2 /nobreak >nul
        rem Launch Explorer detached: Start-Process creates the process without
        rem inheriting this script's console/pipe handles, so captured build
        rem output pipelines reach EOF instead of hanging forever.
        powershell -NoProfile -Command "Start-Process explorer.exe"
    )
    goto configure
)

tasklist /fi "IMAGENAME eq SnowDesktop.exe" /nh 2>nul | find /i "SnowDesktop.exe" >nul
if not errorlevel 1 (
    echo Build preflight stopped: SnowDesktop.exe is running.
    echo Exit SnowDesktop normally before building.
    exit /b 3
)
tasklist /m SnowDesktopTaskbarHook.dll /fi "IMAGENAME eq explorer.exe" /nh 2>nul | find /i "SnowDesktopTaskbarHook.dll" >nul
if not errorlevel 1 (
    echo Build preflight stopped: Explorer still has SnowDesktopTaskbarHook.dll loaded.
    echo Run scripts\build.bat --reload-shell only when an Explorer restart is acceptable.
    exit /b 3
)

:configure
echo === Configuring CMake (Release preset) ===
cmake --preset release
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    exit /b 1
)

echo.
echo === Building SnowDesktop.exe, Workshop Manager, Steam Bridge and snowwidget.exe ===
cmake --build --preset release
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo Steam bridge: .build\Release\SnowDesktopSteamBridge.exe
echo Workshop manager: .build\Release\SnowDesktopWorkshopManager.exe
echo Widget package tool: .build\Release\snowwidget.exe
echo Taskbar appearance Hook: .build\Release\SnowDesktopTaskbarHook.dll
echo.
echo For a version release, run scripts\release.bat to open the unified release center.
echo Agent and automation usage is available through scripts\release.bat COMMAND.
exit /b 0
