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
powershell -NoProfile -Command "$releaseRoot=[IO.Path]::GetFullPath('.build\Release'); $testRoot=Join-Path $releaseRoot 'tests'; $runtimeRoot=Join-Path $releaseRoot 'SnowDesktop.Runtime'; $tests=@(Get-ChildItem -LiteralPath $testRoot -File -Filter 'SnowDesktop*Tests.exe' -ErrorAction Stop); $rootTests=@(Get-ChildItem -LiteralPath $releaseRoot -File -Filter 'SnowDesktop*Tests.exe' -ErrorAction Stop); $rootDlls=@(Get-ChildItem -LiteralPath $releaseRoot -File -Filter '*.dll' -ErrorAction Stop); $runtimeDirectoryNames=@(Get-ChildItem -LiteralPath $runtimeRoot -Directory -ErrorAction Stop | ForEach-Object Name); $emptyRuntimeDirs=@(Get-ChildItem -LiteralPath $releaseRoot -Directory -ErrorAction Stop | Where-Object { $runtimeDirectoryNames -contains $_.Name -and [IO.Directory]::GetFileSystemEntries($_.FullName).Count -eq 0 }); if ($tests.Count -eq 0 -or $rootTests.Count -ne 0 -or $rootDlls.Count -ne 0 -or $emptyRuntimeDirs.Count -ne 0) { Write-Error 'Build or CTest output escaped its dedicated runtime/test directory.'; exit 1 }"
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo === Tests complete ===
echo Test binaries: .build\Release\tests
exit /b 0
