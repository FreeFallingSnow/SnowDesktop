@echo off
setlocal
call "%~dp0powershell_runtime.bat"
if errorlevel 1 exit /b 2
"%SNOWDESKTOP_ENTRY_POWERSHELL%" -NoProfile -File "%~dp0profile.ps1" %*
exit /b %ERRORLEVEL%
