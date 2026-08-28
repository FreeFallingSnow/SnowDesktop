@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0steam_dev.ps1" %*
exit /b %ERRORLEVEL%
