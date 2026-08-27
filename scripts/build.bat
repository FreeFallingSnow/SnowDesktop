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
powershell -NoProfile -Command "$expected=[IO.Path]::GetFullPath('.build\Release\SnowDesktopTaskbarHook.dll'); try { $loaded=@(Get-Process -Name explorer -Module -ErrorAction Stop ^| ForEach-Object { $_.Modules } ^| Where-Object { [string]::Equals($_.FileName,$expected,[StringComparison]::OrdinalIgnoreCase) }).Count -ne 0 } catch { $loaded=$true }; if ($loaded) { exit 1 }"
if not errorlevel 1 (
    echo Build preflight stopped: Explorer still has the Release build's SnowDesktopTaskbarHook.dll loaded.
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
echo === Configuring 32-bit Wallpaper Engine one-shot capture helper ===
cmake -B .build\wallpaper_hook32 -S src\wallpaper_hook -A Win32 "-DSNOWDESKTOP_OUTPUT_DIR=%CD%/.build/Release"
if %ERRORLEVEL% NEQ 0 (
    echo 32-bit Wallpaper Engine helper configure FAILED
    exit /b 1
)

echo.
echo === Building 32-bit Wallpaper Engine Hook and injector ===
cmake --build .build\wallpaper_hook32 --config Release --target SnowDesktopWallpaperHook32 SnowDesktopWallpaperInjector32
if %ERRORLEVEL% NEQ 0 (
    echo 32-bit Wallpaper Engine helper build FAILED
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo Steam bridge: .build\Release\SnowDesktopSteamBridge.exe
echo Workshop manager: .build\Release\SnowDesktopWorkshopManager.exe
echo Widget package tool: .build\Release\snowwidget.exe
echo Taskbar appearance Hook: .build\Release\SnowDesktopTaskbarHook.dll
echo Wallpaper Engine 64-bit Hook: .build\Release\SnowDesktopWallpaperHook.dll
echo Wallpaper Engine 32-bit Hook: .build\Release\SnowDesktopWallpaperHook32.dll
echo Wallpaper Engine 32-bit injector: .build\Release\SnowDesktopWallpaperInjector32.exe
echo.
echo For a version release, run scripts\release.bat to open the unified release center.
echo Agent and automation usage is available through scripts\release.bat COMMAND.
exit /b 0
