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
echo === Building SnowDesktop.exe (Release) ===
cmake --build .build --config Release --target SnowDesktop
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    pause
    exit /b 1
)

echo.
echo === Building SnowDesktopOO.exe (Release) ===
cmake --build .build --config Release --target SnowDesktopOO
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktopOO build FAILED
    pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe:   .build\Release\SnowDesktop.exe
echo SnowDesktopOO.exe: .build\Release\SnowDesktopOO.exe
echo.
pause
