cmake_minimum_required(VERSION 3.24)

project(SnowDesktopWallpaperHook32 LANGUAGES C CXX)

if (NOT MSVC)
    message(FATAL_ERROR "SnowDesktop 32-bit Wallpaper Engine Hook requires MSVC")
endif()
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure this project with -A Win32")
endif()

get_filename_component(SNOWDESKTOP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MINHOOK_ROOT "${SNOWDESKTOP_ROOT}/third_party/minhook")
set(SNOWDESKTOP_OUTPUT_DIR "${SNOWDESKTOP_ROOT}/.build/Release")

set(MINHOOK32_SOURCES
    "${MINHOOK_ROOT}/src/buffer.c"
    "${MINHOOK_ROOT}/src/hook.c"
    "${MINHOOK_ROOT}/src/trampoline.c"
    "${MINHOOK_ROOT}/src/hde/hde32.c"
)

add_library(SnowDesktopWallpaperHook32 SHARED
    wallpaper_hook.cpp
    wallpaper_hook_protocol.h
    ${MINHOOK32_SOURCES}
)
target_compile_features(SnowDesktopWallpaperHook32 PRIVATE cxx_std_20)
target_include_directories(SnowDesktopWallpaperHook32 PRIVATE
    "${MINHOOK_ROOT}/include"
    "${MINHOOK_ROOT}/src"
    "${MINHOOK_ROOT}/src/hde"
)
target_compile_definitions(SnowDesktopWallpaperHook32 PRIVATE
    _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(SnowDesktopWallpaperHook32 PRIVATE d3d9 d3d11 dxgi user32)
set_property(TARGET SnowDesktopWallpaperHook32 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(SnowDesktopWallpaperHook32 PRIVATE
    /W4 /permissive- /EHsc /utf-8)
set_source_files_properties(${MINHOOK32_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/wd4100;/wd4201;/wd4244;/wd4267;/wd4310;/wd4701")

add_executable(SnowDesktopWallpaperInjector32 injector32.cpp)
target_compile_features(SnowDesktopWallpaperInjector32 PRIVATE cxx_std_20)
target_compile_definitions(SnowDesktopWallpaperInjector32 PRIVATE
    _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SnowDesktopWallpaperInjector32 PRIVATE
    /W4 /permissive- /EHsc /utf-8)
set_property(TARGET SnowDesktopWallpaperInjector32 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

foreach(target SnowDesktopWallpaperHook32 SnowDesktopWallpaperInjector32)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SNOWDESKTOP_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SNOWDESKTOP_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SNOWDESKTOP_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SNOWDESKTOP_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SNOWDESKTOP_OUTPUT_DIR}")
endforeach()
