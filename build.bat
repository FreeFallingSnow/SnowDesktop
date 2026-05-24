@echo off
setlocal

echo === SnowDesktop Build Script ===

if not exist ".build" (
    mkdir .build
    cd .build
    echo.
    echo [1/2] Configuring CMake...
    cmake -G "Visual Studio 18 2026" -A x64 .. || (
        echo ERROR: CMake configure failed.
        pause
        exit /b 1
    )
) else (
    cd .build
)

echo.
echo [2/2] Building Release...
cmake --build . --config Release || (
    echo ERROR: Build failed.
    pause
    exit /b 1
)

echo.
echo === Build succeeded ===
echo Output: .build\Release\SnowDesktop.exe
echo.
pause
