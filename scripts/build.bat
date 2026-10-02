@echo off
setlocal
set "SNOWDESKTOP_ENTRY_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
rem SHIFT also changes %%0; preserve the entry point before consuming arguments.
set "SNOWDESKTOP_BUILD_SCRIPT_DIR=%~dp0"
cd /d "%SNOWDESKTOP_BUILD_SCRIPT_DIR%.."

rem Shared-directory collaboration commands reuse the standard build below.
if /i "%~1"=="watch" goto localwait
if /i "%~1"=="resource" goto resource
if /i "%~1"=="ready-and-wait" goto collaboration
if /i "%~1"=="repair" goto collaboration
if /i "%~1"=="repair-abandon" goto collaboration
if /i "%~1"=="begin" goto collaboration
if /i "%~1"=="finish" goto collaboration
if /i "%~1"=="status" goto collaboration
if /i "%~1"=="recover" goto collaboration
if /i "%~1"=="ready" goto collaboration
if /i "%~1"=="wait" goto collaboration
if /i "%~1"=="check" goto collaboration
if /i "%~1"=="plan" goto collaboration

if /i "%~1"=="claim" goto collaboration

if /i "%~1"=="commit" goto collaboration

if /i "%~1"=="issue" goto collaboration

set "RELOAD_SHELL="
if /i "%~1"=="--reload-shell" (
    set "RELOAD_SHELL=1"
    shift
)
if not "%~1"=="" (
    echo Usage: scripts\build.bat [--reload-shell]
    echo Collaboration: scripts\build.bat begin ID ^| finish ID -Batch BATCH ^| status ^| recover -Batch BATCH
    exit /b 2
)

set "RELOAD_SHELL_ARG="
if defined RELOAD_SHELL set "RELOAD_SHELL_ARG=-ReloadShell"
rem Acquire execution authority before any preflight/process action or output write.
if defined SNOWDESKTOP_EXECUTION_TOKEN goto leased
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\build_entry.ps1 -Action release %RELOAD_SHELL_ARG%
exit /b %ERRORLEVEL%
:leased
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\build_entry.ps1 -Action verify
if %ERRORLEVEL% NEQ 0 exit /b 2

if defined RELOAD_SHELL (
    "%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%SNOWDESKTOP_BUILD_SCRIPT_DIR%build_preflight.ps1" -ReloadShell
) else (
    "%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%SNOWDESKTOP_BUILD_SCRIPT_DIR%build_preflight.ps1"
)
rem PowerShell startup failures can return a negative exit code.
if %ERRORLEVEL% NEQ 0 exit /b 3

:configure
echo === Configuring CMake (Release preset) ===
cmake --preset release
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    exit /b 1
)

echo.
echo === Building SnowDesktop.exe, Steam launcher, Workshop Manager, Steam Bridge and snowwidget.exe ===
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
echo === Arranging private runtime directory ===
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\arrange_build_output.ps1 -BuildOutput "%CD%\.build\Release"
if %ERRORLEVEL% NEQ 0 (
    echo Build output arrangement FAILED
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo Steam launcher: .build\Release\SnowDesktopLauncher.exe
echo Steam bridge: .build\Release\SnowDesktopSteamBridge.exe
echo Workshop manager: .build\Release\SnowDesktopWorkshopManager.exe
echo Widget package tool: .build\Release\snowwidget.exe
echo Runtime directory: .build\Release\SnowDesktop.Runtime
echo Taskbar appearance Hook: .build\Release\SnowDesktop.Runtime\SnowDesktopTaskbarHook.dll
echo Wallpaper Engine 64-bit Hook: .build\Release\SnowDesktop.Runtime\SnowDesktopWallpaperHook.dll
echo Wallpaper Engine 32-bit Hook: .build\Release\SnowDesktop.Runtime\SnowDesktopWallpaperHook32.dll
echo Wallpaper Engine 32-bit injector: .build\Release\SnowDesktop.Runtime\SnowDesktopWallpaperInjector32.exe
echo.
echo For a version release, run scripts\release.bat to open the unified release center.
echo Agent and automation usage is available through scripts\release.bat COMMAND.
exit /b 0

:localwait
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%SNOWDESKTOP_BUILD_SCRIPT_DIR%build_runtime.ps1" dashboard >nul 2>&1
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%SNOWDESKTOP_BUILD_SCRIPT_DIR%build_runtime.ps1" watch %*
exit /b %ERRORLEVEL%

:collaboration
if /i "%~1"=="status" goto collaboration_run
rem Automatically ensure the read-only monitor; unavailable Python/port must not block builds.
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%SNOWDESKTOP_BUILD_SCRIPT_DIR%build_runtime.ps1" dashboard >nul 2>&1
:collaboration_run
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File scripts\build_manager.ps1 %*
exit /b %ERRORLEVEL%

:resource
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%SNOWDESKTOP_BUILD_SCRIPT_DIR%build_runtime.ps1" resource %*
exit /b %ERRORLEVEL%
