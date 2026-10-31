add_library(SnowDesktopHookEntry SHARED src/taskbar_hook/hook_entry.cpp)
target_compile_features(SnowDesktopHookEntry PRIVATE cxx_std_20)
target_compile_definitions(SnowDesktopHookEntry PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SnowDesktopHookEntry PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(SnowDesktopHookEntry PRIVATE user32)
set_property(TARGET SnowDesktopHookEntry PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(SnowDesktop SnowDesktopHookEntry)

add_library(SnowDesktopDockMinimizeHook SHARED src/dock/dock_minimize_hook.cpp)
target_compile_features(SnowDesktopDockMinimizeHook PRIVATE cxx_std_20)
target_compile_definitions(SnowDesktopDockMinimizeHook PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SnowDesktopDockMinimizeHook PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(SnowDesktopDockMinimizeHook PRIVATE user32 dwmapi)
set_property(TARGET SnowDesktopDockMinimizeHook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(ExternalProject)
snowdesktop_observed_build_command(SNOWDESKTOP_DOCK32_BUILD_COMMAND
    "${CMAKE_BINARY_DIR}/dock_minimize32" "$<CONFIG>")
ExternalProject_Add(SnowDesktopDockMinimize32
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/dock"
    BINARY_DIR "${CMAKE_BINARY_DIR}/dock_minimize32"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM Win32
    CMAKE_ARGS "-DSNOWDESKTOP_OUTPUT_DIR=${CMAKE_BINARY_DIR}/$<CONFIG>"
        "-DSNOWDESKTOP_BUILD_HOOK_FIXTURE=${BUILD_TESTING}"
    BUILD_COMMAND ${SNOWDESKTOP_DOCK32_BUILD_COMMAND}
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND "")
add_dependencies(SnowDesktop SnowDesktopDockMinimizeHook SnowDesktopDockMinimize32)
include(cmake/SnowDesktop.WindowSupport.cmake)
snowdesktop_window_support_build_identity()
foreach(target SnowDesktopTaskbarHook SnowDesktopWallpaperHook SnowDesktopHookEntry SnowDesktopDockMinimizeHook)
    if(TARGET ${target})
        snowdesktop_private_runtime_target(${target} "${CMAKE_BINARY_DIR}/$<CONFIG>")
    endif()
endforeach()
