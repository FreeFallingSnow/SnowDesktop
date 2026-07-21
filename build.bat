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
echo === Configuring 32-bit Wallpaper Engine Hook ===
cmake -B .build\hook32 -S src\wallpaper_hook -A Win32
if %ERRORLEVEL% NEQ 0 (
    echo 32-bit Hook CMake configure FAILED
    pause
    exit /b 1
)

echo.
echo === Building 32-bit Wallpaper Engine Hook and injector ===
cmake --build .build\hook32 --config Release --target SnowDesktopWallpaperHook32 SnowDesktopWallpaperInjector32
if %ERRORLEVEL% NEQ 0 (
    echo 32-bit Hook build FAILED
    pause
    exit /b 1
)

echo.
echo === Build complete ===
echo SnowDesktop.exe: .build\Release\SnowDesktop.exe
echo Taskbar appearance Hook: .build\Release\SnowDesktopTaskbarHook.dll
echo 64-bit Hook: .build\Release\SnowDesktopWallpaperHook.dll
echo 32-bit Hook: .build\Release\SnowDesktopWallpaperHook32.dll
echo 32-bit injector: .build\Release\SnowDesktopWallpaperInjector32.exe
echo.
echo For a version release, run release.bat after the release branch has been
echo completed. It includes the guarded local squash and tag workflow.
echo.
pause
