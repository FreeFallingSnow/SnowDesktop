# Native shared-output configure and IDE builds must use the same live lease as
# the supported batch/PowerShell entries. Independent diagnostic trees remain
# available; a preset or environment variable alone is not execution authority.
function(snowdesktop_check_shared_output)
    cmake_path(NORMAL_PATH CMAKE_BINARY_DIR OUTPUT_VARIABLE output)
    cmake_path(NORMAL_PATH CMAKE_SOURCE_DIR OUTPUT_VARIABLE source)
    if(output STREQUAL "${source}/.build" OR output STREQUAL "${source}/.build_debug")
        execute_process(COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/build_entry.ps1" -Action verify
            RESULT_VARIABLE code ERROR_VARIABLE error)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "${error}")
        endif()
        set(SNOWDESKTOP_SHARED_OUTPUT TRUE PARENT_SCOPE)
    endif()
endfunction()
function(snowdesktop_guard_directory directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        if(NOT target STREQUAL "SnowDesktopSharedBuildGuard")
            add_dependencies("${target}" SnowDesktopSharedBuildGuard)
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        snowdesktop_guard_directory("${child}")
    endforeach()
endfunction()
function(snowdesktop_install_shared_guard)
    if(SNOWDESKTOP_SHARED_OUTPUT)
        add_custom_target(SnowDesktopSharedBuildGuard
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/build_entry.ps1" -Action verify
            COMMENT "Verify live shared-output execution lease" VERBATIM)
        snowdesktop_guard_directory("${CMAKE_SOURCE_DIR}")
    endif()
endfunction()
