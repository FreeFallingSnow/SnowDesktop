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
echo === Verifying isolated test output ===
powershell -NoProfile -Command "$testRoot=[IO.Path]::GetFullPath('.build\Release\tests'); $tests=@(Get-ChildItem -LiteralPath $testRoot -File -Filter 'SnowDesktop*Tests.exe' -ErrorAction Stop); $rootTests=@(Get-ChildItem -LiteralPath '.build\Release' -File -Filter 'SnowDesktop*Tests.exe' -ErrorAction Stop); $rootDlls=@(Get-ChildItem -LiteralPath '.build\Release' -File -Filter '*.dll' -ErrorAction Stop); if ($tests.Count -eq 0 -or $rootTests.Count -ne 0 -or $rootDlls.Count -ne 0) { Write-Error 'Build or CTest output escaped its dedicated runtime/test directory.'; exit 1 }"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Tests complete ===
echo Test binaries: .build\Release\tests
exit /b 0
