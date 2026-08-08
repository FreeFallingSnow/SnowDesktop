@echo off
setlocal
cd /d "%~dp0.."

echo === Configuring tests ===
cmake --preset tests
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Building test targets ===
cmake --build --preset tests
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Running CTest ===
ctest --preset tests
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Tests complete ===
exit /b 0
