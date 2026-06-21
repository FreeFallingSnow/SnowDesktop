cmake_minimum_required(VERSION 3.24)

project(SnowDesktop LANGUAGES CXX)

file(READ "${CMAKE_SOURCE_DIR}/version.json" VERSION_JSON)
string(JSON SNOWDESKTOP_VERSION GET "${VERSION_JSON}" "version")
message(STATUS "SnowDesktop version: ${SNOWDESKTOP_VERSION}")

set(SNOWDESKTOP_SOURCES
    src/main.cpp
    src/app/app.cpp
    src/core/item.cpp
    src/core/slot.cpp
    src/core/container.cpp
    src/core/desktop.cpp
    src/utils.cpp
    src/settings_window.cpp
    src/personalization.cpp
    src/navigation_settings.cpp
    src/widget_engine.cpp
    src/system_snapshot.cpp
    src/http_runtime.cpp
    src/widgets/widget_base.cpp
    src/widgets/collection.cpp
    src/widgets/file_categories.cpp
    src/widgets/folder_mapping.cpp
    src/widgets/lua_script.cpp
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

add_executable(SnowDesktop WIN32
    ${SNOWDESKTOP_SOURCES}
    ${THIRD_PARTY_SOURCES}
    src/resource.rc
)

target_compile_features(SnowDesktop PRIVATE cxx_std_20)
target_include_directories(SnowDesktop PRIVATE
    src
    src/core
    src/app
    third_party/spdlog/include
    third_party/imgui
    third_party/imgui/backends
    third_party/lua
)

if (MSVC)
    target_compile_options(SnowDesktop PRIVATE /W4 /permissive- /EHsc /utf-8 /Zi /FS)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
    target_link_options(SnowDesktop PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
        /DEBUG:FULL /OPT:REF /OPT:ICF)
else()
    target_compile_options(SnowDesktop PRIVATE -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
endif()

target_link_libraries(SnowDesktop PRIVATE
    comctl32 d2d1 d3d11 dbghelp dcomp dwmapi dwrite dxgi gdi32 msimg32
    bcrypt iphlpapi ole32 pdh runtimeobject shcore shell32 shlwapi urlmon user32 windowscodecs uuid winhttp
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

if (MSVC)
    set_source_files_properties(${LUA_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4310;/wd4324;/wd4701;/wd4702")
endif()
