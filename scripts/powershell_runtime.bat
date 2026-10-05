@echo off
rem Same process-only policy as -ExecutionPolicy Bypass, without its slow
rem command-line policy initialization. Never changes a registry policy.
set "PSExecutionPolicyPreference=Bypass"
rem Reuse the selected engine throughout a build; selection needs no child shell.
if defined SNOWDESKTOP_ENTRY_POWERSHELL if exist "%SNOWDESKTOP_ENTRY_POWERSHELL%" exit /b 0
set "SNOWDESKTOP_ENTRY_POWERSHELL="
if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "SNOWDESKTOP_ENTRY_POWERSHELL=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not defined SNOWDESKTOP_ENTRY_POWERSHELL if exist "%ProgramW6432%\PowerShell\7\pwsh.exe" set "SNOWDESKTOP_ENTRY_POWERSHELL=%ProgramW6432%\PowerShell\7\pwsh.exe"
if not defined SNOWDESKTOP_ENTRY_POWERSHELL for /f "delims=" %%p in ('where pwsh.exe 2^>nul') do if not defined SNOWDESKTOP_ENTRY_POWERSHELL set "SNOWDESKTOP_ENTRY_POWERSHELL=%%p"
if not defined SNOWDESKTOP_ENTRY_POWERSHELL set "SNOWDESKTOP_ENTRY_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%SNOWDESKTOP_ENTRY_POWERSHELL%" exit /b 2
exit /b 0
