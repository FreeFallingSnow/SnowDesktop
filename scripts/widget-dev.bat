@echo off
setlocal
call "%~dp0powershell_runtime.bat"
if errorlevel 1 exit /b 2
cd /d "%~dp0.."

"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoLogo -NoProfile ^
    -File "%~dp0widget_dev.ps1" %*
exit /b %ERRORLEVEL%
