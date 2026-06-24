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
echo === Building SnowDesktop.exe ===
cmake --build .build --config Release --target SnowDesktop
if %ERRORLEVEL% NEQ 0 (
    echo SnowDesktop build FAILED
    pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo.
echo For a version release, run release.bat after the release branch has been
echo completed. It includes the guarded local squash and tag workflow.
echo.
pause
