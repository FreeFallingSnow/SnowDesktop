include_guard(GLOBAL)

# Build each product graph once. Passing several VS targets to cmake --build
# starts separate MSBuild traversals, including repeated shared-output guards.
function(snowdesktop_add_product_build target)
    foreach(dependency IN LISTS ARGN)
        if(NOT TARGET "${dependency}")
            message(FATAL_ERROR "Missing product dependency: ${dependency}")
        endif()
    endforeach()
    add_custom_target("${target}")
    add_dependencies("${target}" ${ARGN})
endfunction()

# The arranger replaces SnowDesktop.Runtime. Keep linker state and import
# libraries outside it, while producing runtime files at their final paths.
function(snowdesktop_private_runtime_target target output_root)
    set_target_properties("${target}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${output_root}/SnowDesktop.Runtime>"
        PDB_OUTPUT_DIRECTORY "$<1:${output_root}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${output_root}>")
    foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties("${target}" PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${configuration} "$<1:${output_root}/SnowDesktop.Runtime>"
            PDB_OUTPUT_DIRECTORY_${configuration} "$<1:${output_root}>"
            ARCHIVE_OUTPUT_DIRECTORY_${configuration} "$<1:${output_root}>")
    endforeach()
endfunction()

# Nested standard builds inherit the main Run's live authority and telemetry.
# Independent diagnostic output keeps the ordinary CMake build command.
function(snowdesktop_observed_build_command result binary_dir configuration)
    if(SNOWDESKTOP_SHARED_OUTPUT)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SnowDesktop.PowerShell.cmake")
        get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
        set(command "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile -File "${root}/scripts/build_command.ps1"
            -Operation build -Directory "${binary_dir}" -Configuration "${configuration}")
    else()
        set(command "${CMAKE_COMMAND}" --build "${binary_dir}" --config "${configuration}")
    endif()
    set("${result}" ${command} PARENT_SCOPE)
endfunction()
