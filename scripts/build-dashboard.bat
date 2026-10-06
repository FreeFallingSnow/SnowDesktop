@echo off
setlocal
python.exe "%~dp0..\tools\build-dashboard\manage.py" %*
exit /b %ERRORLEVEL%
