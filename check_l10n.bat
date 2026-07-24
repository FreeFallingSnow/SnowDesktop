@echo off
setlocal

python -c "import sys" >nul 2>nul
if %errorlevel% equ 0 (
    python "%~dp0scripts\check_l10n.py" %*
) else (
    py -3 "%~dp0scripts\check_l10n.py" %*
)

exit /b %errorlevel%
