include_guard(GLOBAL)
# Keep configure, generated targets and tool regressions on the entry's engine.
# This is executable selection only; it does not establish execution authority.
if(DEFINED ENV{SNOWDESKTOP_ENTRY_POWERSHELL} AND EXISTS "$ENV{SNOWDESKTOP_ENTRY_POWERSHELL}")
    set(SNOWDESKTOP_POWERSHELL_EXECUTABLE "$ENV{SNOWDESKTOP_ENTRY_POWERSHELL}")
else()
    find_program(SNOWDESKTOP_POWERSHELL_EXECUTABLE NAMES pwsh.exe pwsh
        HINTS "$ENV{ProgramFiles}/PowerShell/7" "$ENV{ProgramW6432}/PowerShell/7" NO_CACHE)
    if(NOT SNOWDESKTOP_POWERSHELL_EXECUTABLE)
        set(SNOWDESKTOP_POWERSHELL_EXECUTABLE "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
    endif()
endif()
if(NOT EXISTS "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}")
    message(FATAL_ERROR "Neither PowerShell 7 nor Windows PowerShell 5.1 is available")
endif()
set(ENV{SNOWDESKTOP_ENTRY_POWERSHELL} "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}")
set(ENV{PSExecutionPolicyPreference} "Bypass")
