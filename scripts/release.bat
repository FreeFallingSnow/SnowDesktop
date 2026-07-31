@echo off
setlocal
cd /d "%~dp0.."

if "%~1"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass ^
        -File "%~dp0release_manager.ps1" menu
) else (
    powershell -NoProfile -ExecutionPolicy Bypass ^
        -File "%~dp0release_manager.ps1" %*
)
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
    echo.
    echo Release center exited with code %RESULT%.
)
exit /b %RESULT%
