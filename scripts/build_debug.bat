@echo off
setlocal
cd /d "%~dp0.."

set "RELOAD_SHELL="
if /i "%~1"=="--reload-shell" (
    set "RELOAD_SHELL=1"
    shift
)
if not "%~1"=="" (
    echo Usage: scripts\build_debug.bat [--reload-shell]
    exit /b 2
)

if defined RELOAD_SHELL (
    echo WARNING: --reload-shell stops SnowDesktop and restarts Explorer.
    taskkill /f /im SnowDesktop.exe >nul 2>&1
    tasklist /fi "IMAGENAME eq explorer.exe" /nh 2>nul | find /i "explorer.exe" >nul
    if not errorlevel 1 (
        taskkill /f /im explorer.exe >nul 2>&1
        timeout /t 2 /nobreak >nul
        start "" explorer.exe >nul 2>&1
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
    echo Run scripts\build_debug.bat --reload-shell only when an Explorer restart is acceptable.
    exit /b 3
)

:configure
echo === Configuring CMake (Debug preset) ===
cmake --preset debug
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    exit /b 1
)

echo.
echo === Building SnowDesktop.exe and SnowDesktopSteamBridge.exe (Debug) ===
cmake --build --preset debug
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    exit /b 1
)

echo.
echo === Configuring 32-bit Wallpaper Engine one-shot capture helper (Debug) ===
cmake -B .build_debug\wallpaper_hook32 -S src\wallpaper_hook -A Win32 "-DSNOWDESKTOP_OUTPUT_DIR=%CD%/.build_debug/Debug"
if %ERRORLEVEL% NEQ 0 (
    echo 32-bit Wallpaper Engine helper configure FAILED
    exit /b 1
)

echo.
echo === Building 32-bit Wallpaper Engine Hook and injector (Debug) ===
cmake --build .build_debug\wallpaper_hook32 --config Debug --target SnowDesktopWallpaperHook32 SnowDesktopWallpaperInjector32
if %ERRORLEVEL% NEQ 0 (
    echo 32-bit Wallpaper Engine helper build FAILED
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build_debug\Debug\SnowDesktop.exe
echo Steam bridge: .build_debug\Debug\SnowDesktopSteamBridge.exe
echo Wallpaper Engine 64-bit Hook: .build_debug\Debug\SnowDesktopWallpaperHook.dll
echo Wallpaper Engine 32-bit Hook: .build_debug\Debug\SnowDesktopWallpaperHook32.dll
echo Wallpaper Engine 32-bit injector: .build_debug\Debug\SnowDesktopWallpaperInjector32.exe
exit /b 0
