@echo off
cd /d "%~dp0"

echo === Configuring CMake (Release) ===
cmake -B .build -S .
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure FAILED
    pause
    exit /b 1
)

echo.
echo === Building SnowDesktop.exe (new OO) ===
cmake --build .build --config Release --target SnowDesktop
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    pause
    exit /b 1
)

echo.
echo === Building SnowDesktopLegacy.exe (old) ===
cmake --build .build --config Release --target SnowDesktopLegacy
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktopLegacy build FAILED
    pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe:       .build\Release\SnowDesktop.exe
echo SnowDesktopLegacy.exe: .build\Release\SnowDesktopLegacy.exe
echo.
pause
