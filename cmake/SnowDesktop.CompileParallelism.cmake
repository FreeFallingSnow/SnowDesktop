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

unset(_snowdesktop_host_processors)
unset(_snowdesktop_default_compile_jobs)
