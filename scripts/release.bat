@echo off
setlocal
call "%~dp0powershell_runtime.bat"
if errorlevel 1 exit /b 2
cd /d "%~dp0.."

if "%~1"=="" (
    "%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile ^
        -File "%~dp0release_manager.ps1" menu
) else (
    "%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile ^
        -File "%~dp0release_manager.ps1" %*
)
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
    echo.
    echo Release center exited with code %RESULT%.
)
exit /b %RESULT%
