cmake_minimum_required(VERSION 3.24)

project(SnowDesktop LANGUAGES C CXX)

file(READ "${CMAKE_SOURCE_DIR}/version.json" VERSION_JSON)
string(JSON SNOWDESKTOP_VERSION GET "${VERSION_JSON}" "version")
message(STATUS "SnowDesktop version: ${SNOWDESKTOP_VERSION}")

set(SNOWDESKTOP_SOURCES
    src/main.cpp
    src/app/app.cpp
    src/app/desktop_backdrop_compositor.cpp
    src/core/item.cpp
    src/core/dock.cpp
    src/core/slot.cpp
    src/core/container.cpp
    src/core/desktop.cpp
    src/data_paths.cpp
    src/utils.cpp
    src/settings_window.cpp
    src/personalization.cpp
    src/dock_settings.cpp
    src/navigation_settings.cpp
    src/general_settings.cpp
    src/category_settings.cpp
    src/search_match.cpp
    src/everything_search.cpp
    src/widget_engine.cpp
    src/system_snapshot.cpp
    src/http_runtime.cpp
    src/widgets/widget_base.cpp
    src/widgets/collection.cpp
    src/widgets/file_categories.cpp
    src/widgets/folder_mapping.cpp
    src/widgets/lua_script.cpp
    src/widgets/guide_widget.cpp
)

set(EVERYTHING_SOURCES
    third_party/everything/src/Everything.c
)

set(PINYIN_DATA_SOURCES
    third_party/pinyin-data/pinyin_full_table.cpp
)

set(IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)

set(LUA_SOURCES
    third_party/lua/lapi.c third_party/lua/lauxlib.c third_party/lua/lbaselib.c
    third_party/lua/lcode.c third_party/lua/lcorolib.c third_party/lua/lctype.c
    third_party/lua/ldblib.c third_party/lua/ldebug.c third_party/lua/ldo.c
    third_party/lua/ldump.c third_party/lua/lfunc.c third_party/lua/lgc.c
    third_party/lua/linit.c third_party/lua/liolib.c third_party/lua/llex.c
    third_party/lua/lmathlib.c third_party/lua/lmem.c third_party/lua/loadlib.c
    third_party/lua/lobject.c third_party/lua/lopcodes.c third_party/lua/loslib.c
    third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c
    third_party/lua/lstrlib.c third_party/lua/ltable.c third_party/lua/ltablib.c
    third_party/lua/ltm.c third_party/lua/lundump.c third_party/lua/lutf8lib.c
    third_party/lua/lvm.c third_party/lua/lzio.c
)

set(THIRD_PARTY_SOURCES
    ${IMGUI_SOURCES}
    ${LUA_SOURCES}
)

if (MSVC)
    cmake_host_system_information(RESULT WINDOWS_KITS_ROOT QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10" VIEW 32_64)
    file(GLOB CPPWINRT_CANDIDATES
        "${WINDOWS_KITS_ROOT}/bin/*/x64/cppwinrt.exe")
    list(SORT CPPWINRT_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    if (NOT CPPWINRT_CANDIDATES)
        message(FATAL_ERROR "cppwinrt.exe was not found in the Windows 10/11 SDK")
    endif()
    list(GET CPPWINRT_CANDIDATES 0 CPPWINRT_EXECUTABLE)
    get_filename_component(CPPWINRT_ARCH_DIR "${CPPWINRT_EXECUTABLE}" DIRECTORY)
    get_filename_component(CPPWINRT_VERSION_DIR "${CPPWINRT_ARCH_DIR}" DIRECTORY)
    get_filename_component(CPPWINRT_SDK_VERSION "${CPPWINRT_VERSION_DIR}" NAME)
    set(WINDOWS_WINMD
        "${WINDOWS_KITS_ROOT}/UnionMetadata/${CPPWINRT_SDK_VERSION}/Windows.winmd")
    if (NOT EXISTS "${WINDOWS_WINMD}")
        message(FATAL_ERROR "Windows.winmd was not found for SDK ${CPPWINRT_SDK_VERSION}")
    endif()

    set(CPPWINRT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/cppwinrt")
    add_custom_command(
        OUTPUT
            "${CPPWINRT_OUTPUT_DIR}/winrt/base.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.Shapes.h"
        COMMAND "${CPPWINRT_EXECUTABLE}"
            -in "${WINDOWS_WINMD}" -out "${CPPWINRT_OUTPUT_DIR}"
        DEPENDS "${WINDOWS_WINMD}"
        COMMENT "Generating C++/WinRT projections for the taskbar XAML hook..."
        VERBATIM)
    add_custom_target(SnowDesktopCppWinRT DEPENDS
        "${CPPWINRT_OUTPUT_DIR}/winrt/base.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.Shapes.h")

    add_library(SnowDesktopTaskbarHook SHARED
        src/taskbar_hook/taskbar_hook.cpp
        src/taskbar_hook/taskbar_hook_protocol.h
        src/taskbar_hook/taskbar_hook.def
    )
    add_dependencies(SnowDesktopTaskbarHook SnowDesktopCppWinRT)
    set_target_properties(SnowDesktopTaskbarHook PROPERTIES
        OUTPUT_NAME "SnowDesktopTaskbarHook")
    target_compile_features(SnowDesktopTaskbarHook PRIVATE cxx_std_20)
    target_include_directories(SnowDesktopTaskbarHook PRIVATE
        src/taskbar_hook
        "${CPPWINRT_OUTPUT_DIR}"
    )
    target_compile_definitions(SnowDesktopTaskbarHook PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00 WINRT_NO_MODULE_LOCK)
    target_link_libraries(SnowDesktopTaskbarHook PRIVATE
        comctl32 d2d1 dxguid ole32 runtimeobject user32 windowsapp)
    set_property(TARGET SnowDesktopTaskbarHook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopTaskbarHook PRIVATE
        /W4 /permissive- /EHsc /utf-8)
endif()

add_executable(SnowDesktop WIN32
    ${SNOWDESKTOP_SOURCES}
    ${EVERYTHING_SOURCES}
    ${PINYIN_DATA_SOURCES}
    ${THIRD_PARTY_SOURCES}
    src/resource.rc
)
if (TARGET SnowDesktopTaskbarHook)
    add_dependencies(SnowDesktop SnowDesktopTaskbarHook SnowDesktopCppWinRT)
    target_include_directories(SnowDesktop PRIVATE "${CPPWINRT_OUTPUT_DIR}")
endif()

target_compile_features(SnowDesktop PRIVATE cxx_std_20)
target_include_directories(SnowDesktop PRIVATE
    src
    src/core
    src/app
    third_party/spdlog/include
    third_party/imgui
    third_party/imgui/backends
    third_party/lua
    third_party/everything/include
    third_party/pinyin-data
)

if (MSVC)
    set_property(TARGET SnowDesktop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktop PRIVATE /W4 /permissive- /EHsc /utf-8 /Zi /FS)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
    target_link_options(SnowDesktop PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
        /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF)
else()
    target_compile_options(SnowDesktop PRIVATE -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
endif()

target_link_libraries(SnowDesktop PRIVATE
    comctl32 d2d1 d3d11 dbghelp dcomp dwmapi dwrite dxgi dxguid gdi32 msimg32
    advapi32 bcrypt imm32 iphlpapi ole32 pdh propsys runtimeobject shcore shell32 shlwapi urlmon user32 windowsapp windowscodecs uuid winhttp
)

add_custom_command(TARGET SnowDesktop PRE_BUILD
    COMMAND cmd /c "taskkill /f /im SnowDesktop.exe 2>nul & exit 0"
    COMMENT "Stopping running SnowDesktop.exe before build..."
)
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/widgets"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets"
    COMMENT "Copying widget scripts to output (SnowDesktop)..."
)

set_source_files_properties(
    ${LUA_SOURCES}
    PROPERTIES LANGUAGE C
)
set_source_files_properties(
    ${EVERYTHING_SOURCES}
    PROPERTIES LANGUAGE C
)

if (MSVC)
    set_source_files_properties(${LUA_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4310;/wd4324;/wd4701;/wd4702")
    set_source_files_properties(${EVERYTHING_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4100;/wd4996")
endif()
