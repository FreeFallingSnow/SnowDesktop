cmake_minimum_required(VERSION 3.24)

project(SnowDesktop LANGUAGES CXX)

# ── Shared sources (used by both targets) ────────────────────
set(SHARED_SOURCES
    src/utils.cpp
    src/settings_window.cpp
    src/personalization.cpp
    src/widget_engine.cpp
    # OO class system
    src/item.cpp
    src/slot.cpp
    src/container.cpp
    src/desktop.cpp
    # Widgets
    src/widgets/widget_base.cpp
    src/widgets/collection.cpp
    src/widgets/file_categories.cpp
    src/widgets/folder_mapping.cpp
    src/widgets/lua_script.cpp
)

set(THIRD_PARTY_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    # Lua 5.4
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

# ── Common compile settings (function) ───────────────────────
function(configure_snowdesktop_target TARGET)
    target_compile_features(${TARGET} PRIVATE cxx_std_20)
    target_include_directories(${TARGET} PRIVATE
        third_party/spdlog/include
        third_party/imgui
        third_party/imgui/backends
        third_party/lua
    )
    if (MSVC)
        target_compile_options(${TARGET} PRIVATE /W4 /permissive- /EHsc /utf-8 /Zi)
        target_compile_definitions(${TARGET} PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        target_link_options(${TARGET} PRIVATE
            "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
            /DEBUG:FULL /OPT:REF /OPT:ICF)
    else()
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra -pedantic)
        target_compile_definitions(${TARGET} PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(${TARGET} PRIVATE
        comctl32 d2d1 d3d11 dcomp dwrite dxgi gdi32 msimg32
        ole32 shell32 shlwapi user32 windowscodecs uuid
    )
endfunction()

# ── New sources (src_oo/) ─────────────────────────────────────
set(SOURCES_OO
    src_oo/main.cpp
    src_oo/app/app.cpp
    src_oo/core/item.cpp
    src_oo/core/slot.cpp
    src_oo/core/container.cpp
    src_oo/core/desktop.cpp
    src_oo/utils.cpp
    src_oo/settings_window.cpp
    src_oo/personalization.cpp
    src_oo/widget_engine.cpp
    src_oo/widgets/widget_base.cpp
    src_oo/widgets/collection.cpp
    src_oo/widgets/file_categories.cpp
    src_oo/widgets/folder_mapping.cpp
    src_oo/widgets/lua_script.cpp
)

# ── Target 1: SnowDesktop.exe (legacy, src/) ───────────────────
add_executable(SnowDesktopLegacy WIN32
    src/main.cpp
    src/app.cpp
    ${SHARED_SOURCES}
    ${THIRD_PARTY_SOURCES}
    src/resource.rc
)
configure_snowdesktop_target(SnowDesktopLegacy)

add_custom_command(TARGET SnowDesktopLegacy PRE_BUILD
    COMMAND cmd /c "taskkill /f /im SnowDesktopLegacy.exe 2>nul & exit 0"
    COMMENT "Stopping running SnowDesktopLegacy.exe before build..."
)
add_custom_command(TARGET SnowDesktopLegacy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/widgets"
        "$<TARGET_FILE_DIR:SnowDesktopLegacy>/widgets"
    COMMENT "Copying widget scripts to output (SnowDesktopLegacy)..."
)

# ── Target 2: SnowDesktop (new OO implementation) ────────────────
add_executable(SnowDesktop WIN32
    ${SOURCES_OO}
    ${THIRD_PARTY_SOURCES}
    src_oo/resource.rc
)
configure_snowdesktop_target(SnowDesktop)
target_include_directories(SnowDesktop PRIVATE src_oo src_oo/core src_oo/app)

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

# ── Lua: compile as C ───────────────────────────────────────
set_source_files_properties(
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
    PROPERTIES LANGUAGE C
)
