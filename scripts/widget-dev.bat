@echo off
setlocal
cd /d "%~dp0.."

powershell -NoLogo -NoProfile -ExecutionPolicy Bypass ^
    -File "%~dp0widget_dev.ps1" %*
exit /b %ERRORLEVEL%
