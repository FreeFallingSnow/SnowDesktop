# Limit file-level parallelism separately from the preset's MSBuild project jobs.
# Leave CPU capacity for the desktop and concurrent development work.
include(ProcessorCount)
ProcessorCount(_snowdesktop_host_processors)
math(EXPR _snowdesktop_default_compile_jobs "${_snowdesktop_host_processors} / 2")
if (_snowdesktop_default_compile_jobs LESS 1)
    set(_snowdesktop_default_compile_jobs 1)
elseif (_snowdesktop_default_compile_jobs GREATER 8)
    set(_snowdesktop_default_compile_jobs 8)
endif()

set(SNOWDESKTOP_COMPILE_JOBS "${_snowdesktop_default_compile_jobs}" CACHE STRING
    "Maximum concurrent MSVC compilation units per project (1-65536)")
if (NOT SNOWDESKTOP_COMPILE_JOBS MATCHES "^[1-9][0-9]*$" OR
    SNOWDESKTOP_COMPILE_JOBS GREATER 65536)
    message(FATAL_ERROR "SNOWDESKTOP_COMPILE_JOBS must be an integer from 1 to 65536")
endif()
message(STATUS "SnowDesktop compiler processes per project: ${SNOWDESKTOP_COMPILE_JOBS}")

# Candidate scheduling mode. Keep the measured production default until a fixed
# host workload demonstrates an end-to-end benefit, including peak memory.
set(SNOWDESKTOP_BUILD_TOOL_JOBS "0" CACHE STRING
    "Cross-build MSBuild MultiToolTask process budget (0 disables, 1-65536)")
if (NOT SNOWDESKTOP_BUILD_TOOL_JOBS MATCHES "^(0|[1-9][0-9]*)$" OR
    SNOWDESKTOP_BUILD_TOOL_JOBS GREATER 65536)
    message(FATAL_ERROR "SNOWDESKTOP_BUILD_TOOL_JOBS must be an integer from 0 to 65536")
endif()
if (SNOWDESKTOP_BUILD_TOOL_JOBS GREATER 0)
    if (NOT CMAKE_GENERATOR MATCHES "^Visual Studio " OR NOT MSVC)
        message(FATAL_ERROR "SNOWDESKTOP_BUILD_TOOL_JOBS requires the Visual Studio/MSVC toolchain")
    endif()
    # Set before every target, including third-party libraries. A per-project
    # /MP limit alone can multiply by MSBuild's independent project job count.
    foreach(_snowdesktop_budget_property IN ITEMS
        "UseMultiToolTask=true"
        "EnforceProcessCountAcrossBuilds=true"
        "MultiProcMaxCount=${SNOWDESKTOP_BUILD_TOOL_JOBS}")
        string(REGEX REPLACE "=.*$" "" _snowdesktop_budget_key "${_snowdesktop_budget_property}")
        string(TOLOWER "${_snowdesktop_budget_key}" _snowdesktop_budget_key_lower)
        string(TOLOWER "${_snowdesktop_budget_property}" _snowdesktop_budget_expected)
        set(_snowdesktop_budget_present FALSE)
        foreach(_snowdesktop_global IN LISTS CMAKE_VS_GLOBALS)
            string(REGEX REPLACE "=.*$" "" _snowdesktop_global_key "${_snowdesktop_global}")
            string(TOLOWER "${_snowdesktop_global_key}" _snowdesktop_global_key_lower)
            if (_snowdesktop_global_key_lower STREQUAL _snowdesktop_budget_key_lower)
                string(TOLOWER "${_snowdesktop_global}" _snowdesktop_global_lower)
                if (NOT _snowdesktop_global_lower STREQUAL _snowdesktop_budget_expected)
                    message(FATAL_ERROR "Conflicting CMAKE_VS_GLOBALS ${_snowdesktop_budget_key} for SNOWDESKTOP_BUILD_TOOL_JOBS")
                endif()
                set(_snowdesktop_budget_present TRUE)
            endif()
        endforeach()
        if (NOT _snowdesktop_budget_present)
            list(APPEND CMAKE_VS_GLOBALS "${_snowdesktop_budget_property}")
        endif()
    endforeach()
    message(STATUS "SnowDesktop cross-build tool process budget: ${SNOWDESKTOP_BUILD_TOOL_JOBS} (MultiToolTask)")
endif()

unset(_snowdesktop_host_processors)
unset(_snowdesktop_default_compile_jobs)
unset(_snowdesktop_budget_property)
unset(_snowdesktop_budget_key)
unset(_snowdesktop_budget_key_lower)
unset(_snowdesktop_budget_expected)
unset(_snowdesktop_budget_present)
unset(_snowdesktop_global)
unset(_snowdesktop_global_key)
unset(_snowdesktop_global_key_lower)
unset(_snowdesktop_global_lower)
