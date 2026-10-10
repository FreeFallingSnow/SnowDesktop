cmake_minimum_required(VERSION 3.24)
project(SnowDesktopDockMinimize32 LANGUAGES CXX)
if (NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Dock minimize helper requires MSVC and -A Win32")
endif()
if (NOT DEFINED SNOWDESKTOP_OUTPUT_DIR)
    message(FATAL_ERROR "SNOWDESKTOP_OUTPUT_DIR must be provided by the host build")
endif()
add_library(SnowDesktopDockMinimizeHook32 SHARED dock_minimize_hook.cpp)
add_executable(SnowDesktopDockMinimizeHost32 WIN32 dock_minimize_hook_host.cpp)
target_link_libraries(SnowDesktopDockMinimizeHost32 PRIVATE shell32 user32)
target_link_libraries(SnowDesktopDockMinimizeHook32 PRIVATE user32)
foreach(target SnowDesktopDockMinimizeHook32 SnowDesktopDockMinimizeHost32)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SNOWDESKTOP_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SNOWDESKTOP_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SNOWDESKTOP_OUTPUT_DIR}")
endforeach()
if (SNOWDESKTOP_BUILD_HOOK_FIXTURE)
    add_executable(SnowDesktopDockMinimizeFixture32 "../../tests/dock/dock_minimize_fixture.cpp")
    target_compile_features(SnowDesktopDockMinimizeFixture32 PRIVATE cxx_std_20)
    target_compile_definitions(SnowDesktopDockMinimizeFixture32 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(SnowDesktopDockMinimizeFixture32 PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(SnowDesktopDockMinimizeFixture32 PRIVATE user32)
    set_property(TARGET SnowDesktopDockMinimizeFixture32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(SnowDesktopDockMinimizeFixture32 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SNOWDESKTOP_OUTPUT_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SNOWDESKTOP_OUTPUT_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SNOWDESKTOP_OUTPUT_DIR}/tests")
endif()
