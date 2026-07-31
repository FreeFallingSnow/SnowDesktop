@echo off
setlocal
cd /d "%~dp0.."

echo === Configuring tests ===
cmake -B .build -S . -DBUILD_TESTING=ON
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Building test targets ===
cmake --build .build --config Release --target ^
    SnowDesktopApplicationDataLifecycleTests ^
    SnowDesktopHttpSecurityTests ^
    SnowDesktopCalendarServiceTests ^
    SnowDesktopLocalizationContractTests ^
    SnowDesktopSlotContractMatrixTests ^
    SnowDesktopSlotRuntimeContractTests ^
    SnowDesktopWidgetInteractionRulesTests ^
    SnowDesktopQuickNavigationRulesTests ^
    SnowDesktopItemLocationTests ^
    SnowDesktopDockAndWindowRulesTests ^
    SnowDesktopPopupAnimationRulesTests ^
    --parallel
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Running CTest ===
ctest --test-dir .build -C Release --output-on-failure
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Tests complete ===
exit /b 0
