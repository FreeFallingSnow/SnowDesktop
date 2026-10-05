@echo off
setlocal
set "SNOWDESKTOP_ENTRY_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
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

set "RELOAD_SHELL_ARG="
if defined RELOAD_SHELL set "RELOAD_SHELL_ARG=-ReloadShell"
rem Acquire execution authority before any preflight/process action or output write.
if defined SNOWDESKTOP_EXECUTION_TOKEN goto leased
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\build_entry.ps1 -Action debug %RELOAD_SHELL_ARG%
exit /b %ERRORLEVEL%
:leased
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\build_entry.ps1 -Action verify
if %ERRORLEVEL% NEQ 0 exit /b 2

rem The outer lease owner performs requested process actions before creating
rem the private build Job; a delegated child performs read-only preflight.
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\build_preflight.ps1 -Configuration Debug %RELOAD_SHELL_ARG%
if %ERRORLEVEL% NEQ 0 (
    echo Build preflight stopped: Debug output ownership is blocked or unknown.
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
echo === Arranging private runtime directory (Debug) ===
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\arrange_build_output.ps1 -BuildOutput "%CD%\.build_debug\Debug"
if %ERRORLEVEL% NEQ 0 (
    echo Debug build output arrangement FAILED
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build_debug\Debug\SnowDesktop.exe
echo Steam bridge: .build_debug\Debug\SnowDesktopSteamBridge.exe
echo Runtime directory: .build_debug\Debug\SnowDesktop.Runtime
echo Wallpaper Engine 64-bit Hook: .build_debug\Debug\SnowDesktop.Runtime\SnowDesktopWallpaperHook.dll
echo Wallpaper Engine 32-bit Hook: .build_debug\Debug\SnowDesktop.Runtime\SnowDesktopWallpaperHook32.dll
echo Wallpaper Engine 32-bit injector: .build_debug\Debug\SnowDesktop.Runtime\SnowDesktopWallpaperInjector32.exe
exit /b 0
