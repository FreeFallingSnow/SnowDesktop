@echo off
setlocal

set "ROOT=%~dp0"
set "SOURCE=%ROOT%widgets"
set "TARGET=%~1"

if "%TARGET%"=="" set "TARGET=%ROOT%.build\Release"

if not exist "%SOURCE%" (
    echo ERROR: widgets folder not found: %SOURCE%
    pause
    exit /b 1
)

if not exist "%TARGET%" (
    mkdir "%TARGET%"
    if errorlevel 1 (
        echo ERROR: failed to create target folder: %TARGET%
        pause
        exit /b 1
    )
)

echo Syncing widgets to %TARGET%\widgets ...
robocopy "%SOURCE%" "%TARGET%\widgets" /MIR /NFL /NDL /NJH /NJS /NP >nul
set "RC=%ERRORLEVEL%"

if %RC% GEQ 8 (
    echo ERROR: widget sync failed with robocopy code %RC%.
    pause
    exit /b %RC%
)

echo Done.
echo Target: %TARGET%\widgets
pause