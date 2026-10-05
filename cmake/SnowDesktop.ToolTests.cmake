# One unconditional declaration inventory, including explicit blocked commands
# when Python is unavailable. The test runner can then account for every item.
include("${CMAKE_CURRENT_LIST_DIR}/SnowDesktop.PowerShell.cmake")
function(snowdesktop_add_tool_test name script labels timeout)
    if(SNOWDESKTOP_PYTHON_USABLE)
        add_test(NAME "${name}" COMMAND "${SNOWDESKTOP_PYTHON_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}" ${ARGN})
    else()
        add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}" -E env "PSExecutionPolicyPreference=Bypass" "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_missing_dependency.ps1"
            -TestName "${name}" -Reason "${SNOWDESKTOP_PYTHON_REASON}")
        set_tests_properties("${name}" PROPERTIES LABELS "${labels};environment-blocked")
    endif()
    if(SNOWDESKTOP_PYTHON_USABLE)
        set_tests_properties("${name}" PROPERTIES LABELS "${labels}")
    endif()
    set_tests_properties("${name}" PROPERTIES TIMEOUT "${timeout}"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass")
endfunction()
if(BUILD_TESTING AND WIN32)
    find_program(SNOWDESKTOP_PYTHON_EXECUTABLE NAMES python.exe python)
    set(SNOWDESKTOP_PYTHON_USABLE FALSE)
    set(SNOWDESKTOP_PYTHON_REASON "Python 3.8+ unavailable; required regression not executed")
    if(SNOWDESKTOP_PYTHON_EXECUTABLE)
        execute_process(COMMAND "${SNOWDESKTOP_PYTHON_EXECUTABLE}" -c
            "import sys;sys.exit(0 if sys.version_info >= (3,8) else 78)"
            TIMEOUT 5 RESULT_VARIABLE python_code OUTPUT_QUIET ERROR_QUIET)
        if(python_code STREQUAL "0")
            set(SNOWDESKTOP_PYTHON_USABLE TRUE)
        else()
            set(SNOWDESKTOP_PYTHON_REASON "Python probe failed or version below 3.8; required regression not executed")
        endif()
    endif()
    snowdesktop_add_tool_test(build_workflow build_workflow_tests.py "integration;tools" 300 --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    snowdesktop_add_tool_test(build_plan_execution build_plan_execution_tests.py "tools;core" 120)
    snowdesktop_add_tool_test(build_dashboard build_dashboard_tests.py "tools;core;retry-isolated-resource" 60 --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    snowdesktop_add_tool_test(build_dashboard_browser build_dashboard_tests.py "manual;integration;tools;retry-isolated-resource" 180 --repo "${CMAKE_CURRENT_SOURCE_DIR}" --browser)
    snowdesktop_add_tool_test(build_wait_retry build_wait_retry_tests.py "integration;tools" 240 --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    snowdesktop_add_tool_test(build_shared_resources build_shared_resources_tests.py "tools;core" 90 --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    snowdesktop_add_tool_test(build_foreground_wait build_foreground_wait_tests.py "tools;integration" 90 --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    snowdesktop_add_tool_test(powershell_runtime powershell_runtime_tests.py "tools;core" 90 --repo "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
