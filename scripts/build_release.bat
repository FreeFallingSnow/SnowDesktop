@echo off
setlocal
cd /d "%~dp0.."

REM Build an optimized Release of SnowDesktop and stage a clean, distributable
REM folder under release\ (exe + hook DLL + docs + widgets + lang only; no
REM intermediate build files). The release\ folder is git-ignored.

REM -- Preflight: do not build while SnowDesktop is running --
tasklist /fi "IMAGENAME eq SnowDesktop.exe" /nh 2>nul | find /i "SnowDesktop.exe" >nul
if not errorlevel 1 (
    echo Build preflight stopped: SnowDesktop.exe is running.
    echo Exit SnowDesktop normally before building.
    exit /b 3
)

REM -- Locate the Visual Studio toolchain --
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT (
    echo Visual Studio Build Tools with VC x64 tools was not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if %ERRORLEVEL% NEQ 0 (
    echo Failed to initialize the MSVC x64 environment.
    exit /b 1
)

REM -- Configure an optimized Release build in .build --
cmake -B .build -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    exit /b 1
)

echo.
echo === Building SnowDesktop.exe and SnowDesktopTaskbarHook.dll (Release) ===
cmake --build .build --target SnowDesktop SnowDesktopTaskbarHook --parallel 2
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    exit /b 1
)

REM -- Stage the distributable release folder --
set "STAGE=release"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\licenses" >nul

copy /y ".build\SnowDesktop.exe" "%STAGE%\" >nul
if %ERRORLEVEL% NEQ 0 ( echo Missing SnowDesktop.exe & exit /b 1 )
copy /y ".build\SnowDesktopTaskbarHook.dll" "%STAGE%\" >nul
if %ERRORLEVEL% NEQ 0 ( echo Missing SnowDesktopTaskbarHook.dll & exit /b 1 )

copy /y "LICENSE" "%STAGE%\" >nul
copy /y "THIRD_PARTY_NOTICES.md" "%STAGE%\" >nul
copy /y "README.md" "%STAGE%\" >nul
copy /y "README.en.md" "%STAGE%\" >nul
copy /y "third_party\fluentui-system-icons\LICENSE" "%STAGE%\licenses\FluentSystemIcons-LICENSE.txt" >nul
xcopy /e /i /y "widgets" "%STAGE%\widgets" >nul
xcopy /e /i /y "lang" "%STAGE%\lang" >nul
xcopy /e /i /y "skill" "%STAGE%\skill" >nul

echo.
echo === Release build complete ===
echo SnowDesktop.exe:   %STAGE%\SnowDesktop.exe
echo Taskbar hook:     %STAGE%\SnowDesktopTaskbarHook.dll
echo Widgets:          %STAGE%\widgets
echo Languages:        %STAGE%\lang
echo The %STAGE%\ folder is git-ignored and ready to distribute.
exit /b 0
