@echo off
setlocal
cd /d "%~dp0.."

set "MODE=full"
set "FILTER="

if "%~1"=="" goto run
if /i "%~1"=="full" set "MODE=full"& goto validate_tail
if /i "%~1"=="fast" set "MODE=fast"& goto validate_tail
if /i "%~1"=="core" set "MODE=core"& goto validate_tail
if /i "%~1"=="list" set "MODE=list"& goto validate_tail
if /i "%~1"=="label" (
    if "%~2"=="" goto usage
    set "MODE=label"
    set "FILTER=%~2"
    goto validate_filter_tail
)
if /i "%~1"=="name" (
    if "%~2"=="" goto usage
    set "MODE=name"
    set "FILTER=%~2"
    goto validate_filter_tail
)
goto usage

:validate_tail
if not "%~2"=="" goto usage
goto run

:validate_filter_tail
if not "%~3"=="" goto usage

:run
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test_manager.ps1 -Mode "%MODE%" -Filter "%FILTER%"
exit /b %ERRORLEVEL%

:usage
echo Usage:
echo   scripts\test.bat                         Full suite
echo   scripts\test.bat full                    Full suite
echo   scripts\test.bat fast                    Exclude integration tests
echo   scripts\test.bat core                    Core tests only
echo   scripts\test.bat label ^<regex^>           Tests matching a CTest label
echo   scripts\test.bat name ^<regex^>            Tests matching a CTest name
echo   scripts\test.bat list                    List tests and labels
echo.
echo Examples:
echo   scripts\test.bat label rules
echo   scripts\test.bat label "^(ui^|winui^)$"
echo   scripts\test.bat name quick_navigation
exit /b 2
