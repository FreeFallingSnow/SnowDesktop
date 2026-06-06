@echo off
cd /d "%~dp0"

echo === Configuring CMake (Debug) ===
cmake -B .build_debug -S .
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    pause
    exit /b 1
)

echo.
echo === Building SnowDesktop.exe (Debug) ===
cmake --build .build_debug --config Debug --target SnowDesktop
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build_debug\Debug\SnowDesktop.exe
echo.
pause
