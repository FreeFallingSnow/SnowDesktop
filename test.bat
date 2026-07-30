@echo off
setlocal
cd /d "%~dp0"

echo === Configuring tests ===
cmake -B .build -S . -DBUILD_TESTING=ON
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Building behavior tests ===
cmake --build .build --config Release --target SnowDesktopWidgetPackageTests SnowDesktopSlotContractTests SnowDesktopSlotRuntimeTests SnowDesktopCollectionGroupRulesTests SnowDesktopQuickNavigationRulesTests SnowDesktopItemLocationTests SnowDesktopDockWindowRulesTests SnowDesktopPopupAnimationRulesTests --parallel
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Running CTest ===
ctest --test-dir .build -C Release --output-on-failure
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Checking localization references ===
python scripts\check_l10n.py
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Tests complete ===
exit /b 0
