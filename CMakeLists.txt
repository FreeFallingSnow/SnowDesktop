cmake_minimum_required(VERSION 3.24)

project(SnowDesktop LANGUAGES C CXX)

file(READ "${CMAKE_SOURCE_DIR}/version.json" VERSION_JSON)
string(JSON SNOWDESKTOP_VERSION GET "${VERSION_JSON}" "version")

file(READ
    "${CMAKE_SOURCE_DIR}/packaging/store-identity.json"
    STORE_IDENTITY_JSON)
string(JSON SNOWDESKTOP_STORE_ID
    GET "${STORE_IDENTITY_JSON}" "storeId")
string(JSON SNOWDESKTOP_PACKAGE_FAMILY_NAME
    GET "${STORE_IDENTITY_JSON}" "packageFamilyName")
string(LENGTH "${SNOWDESKTOP_STORE_ID}" SNOWDESKTOP_STORE_ID_LENGTH)
if (NOT SNOWDESKTOP_STORE_ID MATCHES "^[A-Z0-9]+$" OR
    NOT SNOWDESKTOP_STORE_ID_LENGTH EQUAL 12)
    message(FATAL_ERROR
        "packaging/store-identity.json contains an invalid Store ID")
endif()
if (SNOWDESKTOP_PACKAGE_FAMILY_NAME STREQUAL "")
    message(FATAL_ERROR
        "packaging/store-identity.json contains an empty package family name")
endif()

file(READ
    "${CMAKE_SOURCE_DIR}/packaging/steam-identity.json"
    STEAM_IDENTITY_JSON)
string(JSON SNOWDESKTOP_STEAM_APP_ID
    GET "${STEAM_IDENTITY_JSON}" "appId")
string(JSON SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID
    GET "${STEAM_IDENTITY_JSON}" "windowsDepotId")
foreach(STEAM_ID IN ITEMS
        SNOWDESKTOP_STEAM_APP_ID
        SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID)
    if (NOT ${STEAM_ID} MATCHES "^[1-9][0-9]*$" OR
        ${STEAM_ID} GREATER 4294967295)
        message(FATAL_ERROR
            "packaging/steam-identity.json contains an invalid Steam ID")
    endif()
endforeach()

string(REPLACE "." ";" SNOWDESKTOP_VERSION_PARTS "${SNOWDESKTOP_VERSION}")
list(LENGTH SNOWDESKTOP_VERSION_PARTS SNOWDESKTOP_VERSION_PART_COUNT)
if (NOT SNOWDESKTOP_VERSION_PART_COUNT EQUAL 4)
    message(FATAL_ERROR
        "version.json must contain a four-part version: A.B.C.D")
endif()

foreach(VERSION_PART IN LISTS SNOWDESKTOP_VERSION_PARTS)
    if (NOT VERSION_PART MATCHES "^(0|[1-9][0-9]*)$")
        message(FATAL_ERROR
            "Version components must be canonical non-negative integers: ${SNOWDESKTOP_VERSION}")
    endif()
    if (VERSION_PART GREATER 65535)
        message(FATAL_ERROR
            "Each version component must be between 0 and 65535: ${SNOWDESKTOP_VERSION}")
    endif()
endforeach()

list(GET SNOWDESKTOP_VERSION_PARTS 0 SNOWDESKTOP_VERSION_MAJOR)
list(GET SNOWDESKTOP_VERSION_PARTS 1 SNOWDESKTOP_VERSION_MINOR)
list(GET SNOWDESKTOP_VERSION_PARTS 2 SNOWDESKTOP_VERSION_PATCH)
list(GET SNOWDESKTOP_VERSION_PARTS 3 SNOWDESKTOP_VERSION_REVISION)
if (SNOWDESKTOP_VERSION_MAJOR EQUAL 0)
    message(FATAL_ERROR
        "The first version component must be greater than zero for MSIX packages")
endif()
if (NOT SNOWDESKTOP_VERSION_REVISION EQUAL 0)
    message(FATAL_ERROR
        "The fourth version component must be zero for Microsoft Store submissions")
endif()

set(SNOWDESKTOP_VERSION_RESOURCE_DIR
    "${CMAKE_BINARY_DIR}/generated/version")
file(MAKE_DIRECTORY "${SNOWDESKTOP_VERSION_RESOURCE_DIR}")

set(SNOWDESKTOP_FILE_DESCRIPTION "SnowDesktop")
set(SNOWDESKTOP_ORIGINAL_FILENAME "SnowDesktop.exe")
set(SNOWDESKTOP_FILE_TYPE "VFT_APP")
set(SNOWDESKTOP_LEGAL_COPYRIGHT
    "Copyright (c) SnowDesktop contributors; portions copyright (c) TranslucentTB contributors")
set(SNOWDESKTOP_EXE_VERSION_RESOURCE
    "${SNOWDESKTOP_VERSION_RESOURCE_DIR}/SnowDesktop.version.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.rc.in"
    "${SNOWDESKTOP_EXE_VERSION_RESOURCE}"
    @ONLY)

set(SNOWDESKTOP_FILE_DESCRIPTION "SnowDesktop taskbar hook")
set(SNOWDESKTOP_ORIGINAL_FILENAME "SnowDesktopTaskbarHook.dll")
set(SNOWDESKTOP_FILE_TYPE "VFT_DLL")
set(SNOWDESKTOP_LEGAL_COPYRIGHT
    "Copyright (c) SnowDesktop contributors; portions copyright (c) TranslucentTB contributors")
set(SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE
    "${SNOWDESKTOP_VERSION_RESOURCE_DIR}/SnowDesktopTaskbarHook.version.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.rc.in"
    "${SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE}"
    @ONLY)

message(STATUS "SnowDesktop version: ${SNOWDESKTOP_VERSION}")
message(STATUS
    "SnowDesktop Steam App/Windows Depot: ${SNOWDESKTOP_STEAM_APP_ID}/${SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID}")

set(SNOWDESKTOP_SOURCES
    src/main.cpp
    src/application_crash_watchdog.cpp
    src/icon_beautify.cpp
    src/app/app_bitmap_cache.cpp
    src/app/app_background_menu.cpp
    src/app/app_animation_scheduler.cpp
    src/app/app_clipboard_interaction.cpp
    src/app/app_composition_animation_overlay.cpp
    src/app/app_container_navigation.cpp
    src/app/app_data_object.cpp
    src/app/app_desktop_enumeration.cpp
    src/app/app_desktop_layout.cpp
    src/app/app_desktop_reload.cpp
    src/app/app_dock_drop.cpp
    src/app/app_dock_launch.cpp
    src/app/app_dock_layout.cpp
    src/app/app_dock_menu.cpp
    src/app/app_dock_model.cpp
    src/app/app_dock_render.cpp
    src/app/app_dock_window_control.cpp
    src/app/app_dock_window_tracking.cpp
    src/app/app_drag_hint_window.cpp
    src/app/app_drag_lifecycle.cpp
    src/app/app_drag_target_update.cpp
    src/app/app_drop_data_extraction.cpp
    src/app/app_drop_execution.cpp
    src/app/app_drop_preview.cpp
    src/app/app_drop_render.cpp
    src/app/app_floating_dock_interaction.cpp
    src/app/app_floating_dock_lifecycle.cpp
    src/app/app_floating_dock_render.cpp
    src/app/app_floating_dock_window.cpp
    src/app/app_graphics_device.cpp
    src/app/app_glass_render.cpp
    src/app/app_grid_drag.cpp
    src/app/app_grid_placement.cpp
    src/app/app_hit_testing.cpp
    src/app/app_icon_loader.cpp
    src/app/app_icon_render.cpp
    src/app/app_item_sorting.cpp
    src/app/app_item_menu.cpp
    src/app/app_keyboard_input.cpp
    src/app/app_keyboard_navigation.cpp
    src/app/app_layout.cpp
    src/app/app_lifecycle.cpp
    src/app/app_lua_panel.cpp
    src/app/app_lua_panel_render.cpp
    src/app/app_lua_bridge.cpp
    src/app/app_menu_icons.cpp
    src/app/app_message_dispatch.cpp
    src/shell_context_menu_site.cpp
    src/shell_launch_worker.cpp
    src/shell_file_operation_worker.cpp
    src/app/app_navigation_render.cpp
    src/app/app_ole_drop_routing.cpp
    src/app/app_ole_drop_session.cpp
    src/app/app_overlay_render.cpp
    src/app/app_page_grid.cpp
    src/app/app_paint.cpp
    src/app/app_pointer_context.cpp
    src/app/app_pointer_down.cpp
    src/app/app_pointer_move.cpp
    src/app/app_pointer_release.cpp
    src/app/app_popup_geometry.cpp
    src/app/app_popup_dwell_interaction.cpp
    src/app/app_popup_lifecycle.cpp
    src/app/app_popup_render.cpp
    src/app/app_popup_transition.cpp
    src/app/app_quick_navigation_interaction.cpp
    src/app/app_quick_navigation_layout.cpp
    src/app/app_quick_navigation_messages.cpp
    src/app/app_quick_navigation_model.cpp
    src/app/app_quick_navigation_render.cpp
    src/app/app_quick_navigation_window.cpp
    src/app/app_rename_begin.cpp
    src/app/app_rename_commit.cpp
    src/app/app_rename_editor.cpp
    src/app/app_rename_target.cpp
    src/app/app_render_geometry.cpp
    src/app/app_render_primitives.cpp
    src/app/app_run.cpp
    src/app/app_shell_context_menu.cpp
    src/app/app_shell_file_operation.cpp
    src/app/app_shell_menu.cpp
    src/app/app_scene_render.cpp
    src/app/app_scroll_interaction.cpp
    src/app/app_selection_interaction.cpp
    src/app/app_settings_apply.cpp
    src/app/app_text_render.cpp
    src/app/app_taskbar_integration.cpp
    src/app/app_steam_workshop.cpp
    src/app/app_timer_dispatch.cpp
    src/app/app_tray.cpp
    src/app/app_window_dispatch.cpp
    src/app/app_widget_context_menu.cpp
    src/app/app_widget_grouping.cpp
    src/app/app_widget_marquee_composition.cpp
    src/app/app_widget_placement.cpp
    src/app/app_widget_refresh.cpp
    src/app/drag_drop_controller.cpp
    src/app/drag_render_cache.cpp
    src/app/ole_drag_drop_adapter.cpp
    src/app/tray_icon_controller.cpp
    src/app/widget_notification_presenter.cpp
    src/app/desktop_backdrop_compositor.cpp
    src/atomic_file.cpp
    src/dock_window_preview.cpp
    src/dock_window_transition.cpp
    src/taskbar_dynamic/search_visibility_detector.cpp
    src/core/item.cpp
    src/core/drag_target_resolver.cpp
    src/core/dock.cpp
    src/core/slot.cpp
    src/core/container.cpp
    src/core/desktop.cpp
    src/data_paths.cpp
    src/calendar_service.cpp
    src/full_data_backup.cpp
    src/portable_data_migration.cpp
    src/single_instance.cpp
    src/deployment_context.cpp
    src/utils.cpp
    src/settings_window.cpp
    src/ui_animation_scheduler.cpp
    src/personalization.cpp
    src/dock_settings.cpp
    src/navigation_settings.cpp
    src/general_settings.cpp
    src/category_settings.cpp
    src/component_preview.cpp
    src/widget_author_preview.cpp
    src/widget_preview_scene.cpp
    src/widget_preview_scene_model.cpp
    src/l10n.cpp
    src/item_location.cpp
    src/layout_storage.cpp
    src/menu_icon_render.cpp
    src/modern_menu.cpp
    src/search_match.cpp
    src/everything_search.cpp
    src/lua_runtime.cpp
    src/widget_api_registry.cpp
    src/widget_package_image_cache.cpp
    src/widget_l10n_format.cpp
    src/widget_time.cpp
    src/widget_preview_context.cpp
    src/widget_data_broker.cpp
    src/widget_task_broker.cpp
    src/widget_notification_runtime.cpp
    src/widget_notification_schedule_store.cpp
    src/widget_media_task_executor.cpp
    src/widget_audio_output_task_executor.cpp
    src/widget_clipboard_task_executor.cpp
    src/widget_filesystem_handle_store.cpp
    src/widget_filesystem_task_executor.cpp
    src/widget_filesystem_watch_service.cpp
    src/widget_app_task_executor.cpp
    src/widget_system_settings.cpp
    src/widget_system_data_provider.cpp
    src/widget_audio_analysis_provider.cpp
    src/widget_draw_geometry.cpp
    src/widget_interaction_region.cpp
    src/widget_accessibility_events.cpp
    src/widget_accessibility_provider.cpp
    src/widget_view_accessibility.cpp
    src/widget_view_contract.cpp
    src/widget_view_tree.cpp
    src/widget_view_lua.cpp
    src/widget_text_input_rules.cpp
    src/widget_storage_transaction.cpp
    src/widget_storage_value.cpp
    src/widget_storage_write_budget.cpp
    src/widget_secret_store.cpp
    src/widget_setting_rules.cpp
    src/widget_logical_slot.cpp
    src/widget_lua_lifecycle.cpp
    src/widget_runtime_diagnostics.cpp
    src/widget_runtime_scheduler.cpp
    src/widget_engine.cpp
    src/widget_permission_broker.cpp
    src/widget_permission_state.cpp
    src/widget_package.cpp
    src/widget_package_paths.cpp
    src/steam_workshop_cache.cpp
    src/steam_workshop_source.cpp
    src/steam_workshop_sync.cpp
    src/system_snapshot.cpp
    src/http_runtime.cpp
    src/widgets/widget_base.cpp
    src/widgets/collection.cpp
    src/widgets/collection_group.cpp
    src/widgets/file_group.cpp
    src/widgets/file_categories.cpp
    src/widgets/folder_mapping.cpp
    src/widgets/lua_script.cpp
    src/widgets/lua_logical_slot.cpp
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

add_library(SnowDesktopImgui STATIC ${IMGUI_SOURCES})
target_compile_features(SnowDesktopImgui PRIVATE cxx_std_20)
target_include_directories(SnowDesktopImgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
if(MSVC)
    set_property(TARGET SnowDesktopImgui PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopImgui PRIVATE /MP4 /W4 /permissive- /EHsc /utf-8 /Z7)
    target_compile_definitions(SnowDesktopImgui PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(SnowDesktopLua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(SnowDesktopLua PUBLIC third_party/lua)
if(MSVC)
    set_property(TARGET SnowDesktopLua PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopLua PRIVATE /MP4
        /wd4244 /wd4310 /wd4324 /wd4701 /wd4702)
endif()

add_library(SnowDesktopEverything STATIC ${EVERYTHING_SOURCES})
set_source_files_properties(${EVERYTHING_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(SnowDesktopEverything PUBLIC
    third_party/everything/include)
if(MSVC)
    set_property(TARGET SnowDesktopEverything PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopEverything PRIVATE /MP4 /wd4100 /wd4133 /wd4996)
endif()

add_library(SnowDesktopPinyinData STATIC ${PINYIN_DATA_SOURCES})
target_compile_features(SnowDesktopPinyinData PRIVATE cxx_std_20)
target_include_directories(SnowDesktopPinyinData PUBLIC
    third_party/pinyin-data)
if(MSVC)
    set_property(TARGET SnowDesktopPinyinData PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopPinyinData PRIVATE /MP4)
endif()

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
    set(MIDLRT_EXECUTABLE "${CPPWINRT_ARCH_DIR}/midlrt.exe")
    if (NOT EXISTS "${MIDLRT_EXECUTABLE}")
        message(FATAL_ERROR "midlrt.exe was not found next to cppwinrt.exe")
    endif()
    get_filename_component(CPPWINRT_VERSION_DIR "${CPPWINRT_ARCH_DIR}" DIRECTORY)
    get_filename_component(CPPWINRT_SDK_VERSION "${CPPWINRT_VERSION_DIR}" NAME)
    set(WINDOWS_WINMD
        "${WINDOWS_KITS_ROOT}/UnionMetadata/${CPPWINRT_SDK_VERSION}/Windows.winmd")
    if (NOT EXISTS "${WINDOWS_WINMD}")
        message(FATAL_ERROR "Windows.winmd was not found for SDK ${CPPWINRT_SDK_VERSION}")
    endif()

    set(CPPWINRT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/cppwinrt")
    set(SHELLVIEW_WINMD_DIR "${CMAKE_BINARY_DIR}/generated/winmd")
    set(SHELLVIEW_WINMD "${SHELLVIEW_WINMD_DIR}/WindowsUdk.winmd")
    file(TO_NATIVE_PATH
        "${WINDOWS_KITS_ROOT}/Include/${CPPWINRT_SDK_VERSION}/winrt"
        MIDLRT_INCLUDE_DIR)
    file(TO_NATIVE_PATH
        "${WINDOWS_KITS_ROOT}/UnionMetadata/${CPPWINRT_SDK_VERSION}"
        MIDLRT_METADATA_DIR)
    file(TO_NATIVE_PATH "${WINDOWS_WINMD}" MIDLRT_WINDOWS_WINMD)
    file(TO_NATIVE_PATH "${SHELLVIEW_WINMD_DIR}" MIDLRT_OUTPUT_DIR)
    add_custom_command(
        OUTPUT "${SHELLVIEW_WINMD}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHELLVIEW_WINMD_DIR}"
        COMMAND "${MIDLRT_EXECUTABLE}"
            /winrt /nomidl /no_cpp
            /I "${MIDLRT_INCLUDE_DIR}"
            /metadata_dir "${MIDLRT_METADATA_DIR}"
            /reference "${MIDLRT_WINDOWS_WINMD}"
            /out "${MIDLRT_OUTPUT_DIR}"
            /winmd WindowsUdk.winmd
            "ShellViewCoordinator.idl"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/src/taskbar_dynamic/ShellViewCoordinator.idl"
            "${WINDOWS_WINMD}"
        COMMENT "Compiling the ShellViewCoordinator WinMD..."
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/taskbar_dynamic"
        VERBATIM)
    add_custom_command(
        OUTPUT
            "${CPPWINRT_OUTPUT_DIR}/winrt/base.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.ApplicationModel.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.Foundation.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.Shapes.h"
            "${CPPWINRT_OUTPUT_DIR}/winrt/WindowsUdk.UI.Shell.h"
        COMMAND "${CPPWINRT_EXECUTABLE}"
            -in "${WINDOWS_WINMD}"
            -in "${SHELLVIEW_WINMD}"
            -out "${CPPWINRT_OUTPUT_DIR}"
        DEPENDS "${WINDOWS_WINMD}" "${SHELLVIEW_WINMD}"
        COMMENT "Generating C++/WinRT projections for the taskbar XAML hook..."
        VERBATIM)
    add_custom_target(SnowDesktopCppWinRT DEPENDS
        "${CPPWINRT_OUTPUT_DIR}/winrt/base.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.ApplicationModel.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.Foundation.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/Windows.UI.Xaml.Shapes.h"
        "${CPPWINRT_OUTPUT_DIR}/winrt/WindowsUdk.UI.Shell.h")

    add_library(SnowDesktopTaskbarHook SHARED
        src/taskbar_hook/taskbar_hook.cpp
        src/taskbar_hook/taskbar_hook_protocol.h
        src/taskbar_hook/taskview_visibility.h
        src/taskbar_hook/taskbar_hook.def
        "${SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE}"
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
    src/resource.rc
    "${SNOWDESKTOP_EXE_VERSION_RESOURCE}"
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
)

if (MSVC)
    set_property(TARGET SnowDesktop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktop PRIVATE /MP4 /bigobj /W4 /permissive- /EHsc /utf-8 /Z7)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
        SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
        SNOWDESKTOP_STORE_ID="${SNOWDESKTOP_STORE_ID}"
        SNOWDESKTOP_PACKAGE_FAMILY_NAME="${SNOWDESKTOP_PACKAGE_FAMILY_NAME}")
    target_precompile_headers(SnowDesktop PRIVATE
        <windows.h>
        <shlwapi.h>
        <shlobj.h>
        <shellapi.h>
        <d2d1_1.h>
        <wrl/client.h>
        <algorithm>
        <atomic>
        <cmath>
        <cstdint>
        <fstream>
        <functional>
        <memory>
        <mutex>
        <sstream>
        <string>
        <thread>
        <unordered_map>
        <unordered_set>
        <vector>
    )
    target_link_options(SnowDesktop PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
        /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF)
else()
    target_compile_options(SnowDesktop PRIVATE -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
        SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
        SNOWDESKTOP_STORE_ID="${SNOWDESKTOP_STORE_ID}"
        SNOWDESKTOP_PACKAGE_FAMILY_NAME="${SNOWDESKTOP_PACKAGE_FAMILY_NAME}")
endif()

target_link_libraries(SnowDesktop PRIVATE
    SnowDesktopImgui SnowDesktopLua SnowDesktopEverything SnowDesktopPinyinData
    comctl32 d2d1 d3d11 dbghelp dcomp dwmapi dwrite dxgi dxguid gdi32 msimg32
    advapi32 bcrypt crypt32 imm32 iphlpapi ole32 oleaut32 pdh propsys runtimeobject shcore shell32 shlwapi uiautomationcore urlmon user32 windowsapp windowscodecs uuid winhttp ws2_32
    version wbemuuid
)

add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/widgets"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets"
    COMMENT "Copying widget scripts to output (SnowDesktop)..."
)
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/lang"
        "$<TARGET_FILE_DIR:SnowDesktop>/lang"
    COMMENT "Copying language files to output (SnowDesktop)..."
)

add_executable(snowwidget
    tools/snowwidget/main.cpp
    src/widget_author_lint.cpp
    src/widget_author_permissions.cpp
    src/widget_author_test.cpp
    src/widget_api_contract_json.cpp
    src/widget_api_registry.cpp
    src/widget_logical_slot.cpp
    src/lua_runtime.cpp
    src/widget_package.cpp
    src/widget_permission_broker.cpp
    src/widget_permission_state.cpp
    src/widget_system_contract_json.cpp
    src/widget_view_contract.cpp
    src/widget_view_contract_json.cpp)
target_compile_features(snowwidget PRIVATE cxx_std_20)
target_include_directories(snowwidget PRIVATE src)
target_link_libraries(snowwidget PRIVATE SnowDesktopLua bcrypt ole32)
if (MSVC)
    set_property(TARGET snowwidget PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(snowwidget PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(snowwidget PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
else()
    target_compile_options(snowwidget PRIVATE -Wall -Wextra -pedantic)
    target_compile_definitions(snowwidget PRIVATE
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
endif()

# The Steam bridge is a separate MIT-licensed executable. It deliberately does
# not link SnowDesktop or any other GPL target from this build. Configure it
# after snowwidget so its Agent Skill bundle can embed the CLI.
add_subdirectory(steam_bridge)
target_link_libraries(SnowDesktop PRIVATE
    SnowDesktopWorkshopManagerSupport)

add_dependencies(SnowDesktop snowwidget)
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:snowwidget>"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/snowwidget.exe"
    COMMENT "Embedding snowwidget CLI in the Agent Skill for SnowDesktop...")

include(CTest)
if (BUILD_TESTING)
    set(SNOWDESKTOP_TEST_TARGETS)

    function(snowdesktop_add_test TEST_TARGET TEST_NAME)
        set(multiValueArgs
            SOURCES
            INCLUDE_DIRECTORIES
            LINK_LIBRARIES
            COMMAND_ARGUMENTS
            LABELS)
        cmake_parse_arguments(
            SNOW_TEST "" "" "${multiValueArgs}" ${ARGN})

        if (NOT SNOW_TEST_SOURCES)
            message(FATAL_ERROR
                "${TEST_TARGET} must declare at least one source file")
        endif()

        add_executable(${TEST_TARGET} ${SNOW_TEST_SOURCES})
        target_compile_features(${TEST_TARGET} PRIVATE cxx_std_20)
        if (SNOW_TEST_INCLUDE_DIRECTORIES)
            target_include_directories(
                ${TEST_TARGET} PRIVATE
                ${SNOW_TEST_INCLUDE_DIRECTORIES})
        endif()
        if (SNOW_TEST_LINK_LIBRARIES)
            target_link_libraries(
                ${TEST_TARGET} PRIVATE
                ${SNOW_TEST_LINK_LIBRARIES})
        endif()

        if (MSVC)
            set_property(
                TARGET ${TEST_TARGET}
                PROPERTY MSVC_RUNTIME_LIBRARY
                    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            target_compile_options(
                ${TEST_TARGET} PRIVATE
                /W4 /permissive- /EHsc /utf-8)
            target_compile_definitions(
                ${TEST_TARGET} PRIVATE
                _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        else()
            target_compile_options(
                ${TEST_TARGET} PRIVATE
                -Wall -Wextra -pedantic)
        endif()

        add_test(
            NAME ${TEST_NAME}
            COMMAND ${TEST_TARGET}
                ${SNOW_TEST_COMMAND_ARGUMENTS})
        set_tests_properties(
            ${TEST_NAME} PROPERTIES TIMEOUT 60)
        if (SNOW_TEST_LABELS)
            set_tests_properties(
                ${TEST_NAME} PROPERTIES
                LABELS "${SNOW_TEST_LABELS}")
        endif()

        list(APPEND SNOWDESKTOP_TEST_TARGETS ${TEST_TARGET})
        set(SNOWDESKTOP_TEST_TARGETS
            "${SNOWDESKTOP_TEST_TARGETS}" PARENT_SCOPE)
    endfunction()

    snowdesktop_add_test(
        SnowDesktopApplicationDataLifecycleTests
        application_data_lifecycle
        SOURCES
            tests/application_data_lifecycle_tests.cpp
            src/atomic_file.cpp
            src/full_data_backup.cpp
            src/layout_storage.cpp
            src/widget_logical_slot.cpp
            src/widget_package.cpp
            src/widget_permission_broker.cpp
            src/widget_permission_state.cpp
            src/portable_data_migration.cpp
            src/single_instance.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt ole32 shell32 user32 version
        LABELS integration data)

    snowdesktop_add_test(
        SnowDesktopHttpSecurityTests
        http_security
        SOURCES
            tests/http_security_tests.cpp
            src/http_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES shlwapi winhttp ws2_32
        LABELS contract security)

    snowdesktop_add_test(
        SnowDesktopLuaRuntimeTests
        lua_runtime
        SOURCES
            tests/lua_runtime_tests.cpp
            src/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetApiRegistryTests
        widget_api_registry
        SOURCES
            tests/widget_api_registry_tests.cpp
            src/widget_api_contract_json.cpp
            src/widget_api_registry.cpp
            src/widget_permission_state.cpp
            src/widget_system_contract_json.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetL10nFormatTests
        widget_l10n_format
        SOURCES
            tests/widget_l10n_format_tests.cpp
            src/widget_l10n_format.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract l10n lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetTimeTests
        widget_time
        SOURCES
            tests/widget_time_tests.cpp
            src/widget_time.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract l10n lua time widget)

    snowdesktop_add_test(
        SnowDesktopWidgetRuntimeDiagnosticsTests
        widget_runtime_diagnostics
        SOURCES
            tests/widget_runtime_diagnostics_tests.cpp
            src/widget_runtime_diagnostics.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract widget)

    snowdesktop_add_test(
        SnowDesktopWidgetPreviewContextTests
        widget_preview_context
        SOURCES
            tests/widget_preview_context_tests.cpp
            src/widget_preview_context.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua preview widget)

    snowdesktop_add_test(
        SnowDesktopWidgetRuntimeHealthTests
        widget_runtime_health
        SOURCES tests/widget_runtime_health_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetHostStateTests
        widget_host_state
        SOURCES tests/widget_host_state_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetDataBrokerTests
        widget_data_broker
        SOURCES
            tests/widget_data_broker_tests.cpp
            src/widget_data_broker.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract data lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetTaskBrokerTests
        widget_task_broker
        SOURCES
            tests/widget_task_broker_tests.cpp
            src/widget_task_broker.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lifecycle task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetNotificationRuntimeTests
        widget_notification_runtime
        SOURCES
            tests/widget_notification_runtime_tests.cpp
            src/widget_notification_runtime.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lifecycle task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetNotificationScheduleStoreTests
        widget_notification_schedule_store
        SOURCES
            tests/widget_notification_schedule_store_tests.cpp
            src/widget_notification_schedule_store.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lifecycle storage task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetMediaTaskExecutorTests
        widget_media_task_executor
        SOURCES
            tests/widget_media_task_executor_tests.cpp
            src/widget_media_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES windowsapp
        LABELS integration media task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAudioOutputTaskExecutorTests
        widget_audio_output_task_executor
        SOURCES
            tests/widget_audio_output_task_executor_tests.cpp
            src/widget_audio_output_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32
        LABELS contract audio task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetClipboardTaskExecutorTests
        widget_clipboard_task_executor
        SOURCES
            tests/widget_clipboard_task_executor_tests.cpp
            src/widget_clipboard_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES gdi32 ole32 shell32 windowscodecs
        LABELS contract clipboard task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemHandleStoreTests
        widget_filesystem_handle_store
        SOURCES
            tests/widget_filesystem_handle_store_tests.cpp
            src/widget_filesystem_handle_store.cpp
            src/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt
        LABELS contract filesystem security task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemTaskExecutorTests
        widget_filesystem_task_executor
        SOURCES
            tests/widget_filesystem_task_executor_tests.cpp
            src/widget_filesystem_task_executor.cpp
            src/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract filesystem security task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemWatchServiceTests
        widget_filesystem_watch_service
        SOURCES
            tests/widget_filesystem_watch_service_tests.cpp
            src/widget_filesystem_watch_service.cpp
        INCLUDE_DIRECTORIES src
        LABELS integration filesystem security data lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAppTaskExecutorTests
        widget_app_task_executor
        SOURCES
            tests/widget_app_task_executor_tests.cpp
            src/widget_app_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract app task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSystemSettingsTests
        widget_system_settings
        SOURCES
            tests/widget_system_settings_tests.cpp
            src/widget_system_settings.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract system task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSystemDataProviderTests
        widget_system_data_provider
        SOURCES
            tests/widget_system_data_provider_tests.cpp
            src/widget_system_data_provider.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES advapi32 dxgi dxguid iphlpapi ole32 pdh propsys runtimeobject shcore user32 windowsapp windowscodecs
        LABELS contract data system widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAudioAnalysisProviderTests
        widget_audio_analysis_provider
        SOURCES
            tests/widget_audio_analysis_provider_tests.cpp
            src/widget_audio_analysis_provider.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32
        LABELS contract data audio widget)

    snowdesktop_add_test(
        SnowDesktopWidgetPackageImageCacheTests
        widget_package_image_cache
        SOURCES
            tests/widget_package_image_cache_tests.cpp
            src/widget_package_image_cache.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 windowscodecs
        LABELS contract resource render widget)

    snowdesktop_add_test(
        SnowDesktopWidgetLuaLifecycleTests
        widget_lua_lifecycle
        SOURCES
            tests/widget_lua_lifecycle_tests.cpp
            src/widget_lua_lifecycle.cpp
            src/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS contract lifecycle lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetRuntimeSchedulerTests
        widget_runtime_scheduler
        SOURCES
            tests/widget_runtime_scheduler_tests.cpp
            src/widget_runtime_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetPermissionStateTests
        widget_permission_state
        SOURCES
            tests/widget_permission_state_tests.cpp
            src/widget_permission_broker.cpp
            src/widget_permission_state.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt
        LABELS contract security widget)

    snowdesktop_add_test(
        SnowDesktopApplicationRestartPolicyTests
        application_restart_policy
        SOURCES
            tests/application_restart_policy_tests.cpp
            src/application_crash_watchdog.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract recovery)

    snowdesktop_add_test(
        SnowDesktopWidgetLayoutContextTests
        widget_layout_context
        SOURCES tests/widget_layout_context_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopCalendarServiceTests
        calendar_service
        SOURCES
            tests/calendar_service_tests.cpp
            src/calendar_service.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32
        LABELS service)

    snowdesktop_add_test(
        SnowDesktopLocalizationContractTests
        localization_contract
        SOURCES
            tests/localization_contract_tests.cpp
            src/l10n.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract l10n)

    snowdesktop_add_test(
        SnowDesktopGeneralSettingsTests
        general_settings
        SOURCES
            tests/general_settings_tests.cpp
            src/general_settings.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract settings)

    snowdesktop_add_test(
        SnowDesktopDemoModeRulesTests
        demo_mode_rules
        SOURCES tests/demo_mode_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract settings render)

    snowdesktop_add_test(
        SnowDesktopSettingsWindowOpenRulesTests
        settings_window_open_rules
        SOURCES tests/settings_window_open_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract settings)

    snowdesktop_add_test(
        SnowDesktopSlotContractMatrixTests
        slot_contract_matrix
        SOURCES tests/slot_contract_matrix_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract slot matrix)

    snowdesktop_add_test(
        SnowDesktopSlotRuntimeContractTests
        slot_runtime_contract
        SOURCES
            tests/slot_runtime_contract_tests.cpp
            src/app/drag_drop_controller.cpp
            src/app/ole_drag_drop_adapter.cpp
            src/app/tray_icon_controller.cpp
            src/core/drag_target_resolver.cpp
            src/core/slot.cpp
            src/core/container.cpp
            src/l10n.cpp
        INCLUDE_DIRECTORIES src src/core
        LINK_LIBRARIES d2d1 ole32 shell32 user32 uuid
        LABELS contract interaction slot integration)

    snowdesktop_add_test(
        SnowDesktopWidgetInteractionRulesTests
        widget_interaction_rules
        SOURCES tests/widget_interaction_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules widget)

    snowdesktop_add_test(
        SnowDesktopWidgetInteractionRegionTests
        widget_interaction_region
        SOURCES
            tests/widget_interaction_region_tests.cpp
            src/widget_interaction_region.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetDrawGeometryTests
        widget_draw_geometry
        SOURCES
            tests/widget_draw_geometry_tests.cpp
            src/widget_draw_geometry.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract render lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetViewAccessibilityTests
        widget_view_accessibility
        SOURCES
            tests/widget_view_accessibility_tests.cpp
            src/widget_view_accessibility.cpp
            src/widget_view_contract.cpp
            src/widget_view_tree.cpp
            src/widget_view_lua.cpp
            src/widget_interaction_region.cpp
            src/widget_logical_slot.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS accessibility contract layout lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAccessibilityProviderTests
        widget_accessibility_provider
        SOURCES
            tests/widget_accessibility_provider_tests.cpp
            src/widget_accessibility_events.cpp
            src/widget_accessibility_provider.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 oleaut32 uiautomationcore user32
        LABELS accessibility contract integration widget)

    snowdesktop_add_test(
        SnowDesktopWidgetViewContractTests
        widget_view_contract
        SOURCES
            tests/widget_view_contract_tests.cpp
            src/widget_view_contract.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract layout lua widget)
    target_sources(SnowDesktopWidgetViewContractTests PRIVATE
        src/widget_view_contract_json.cpp)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorLintTests
        widget_author_lint
        SOURCES
            tests/widget_author_lint_tests.cpp
            src/widget_author_lint.cpp
            src/widget_api_registry.cpp
            src/widget_view_contract.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS authoring contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorTestRunnerTests
        widget_author_test_runner
        SOURCES
            tests/widget_author_test_tests.cpp
            src/widget_author_test.cpp
            src/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS authoring contract lua security widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorToolsTests
        widget_author_tools
        SOURCES
            tests/widget_author_tools_tests.cpp
            src/widget_author_permissions.cpp
            src/widget_api_registry.cpp
            src/widget_logical_slot.cpp
            src/widget_package.cpp
            src/widget_permission_broker.cpp
            src/widget_permission_state.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua bcrypt ole32
        LABELS authoring contract permission security widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorPreviewCliTests
        widget_author_preview_cli
        SOURCES tests/widget_author_preview_cli_tests.cpp
        LINK_LIBRARIES ole32 windowscodecs
        COMMAND_ARGUMENTS
            "$<TARGET_FILE:snowwidget>"
            "$<TARGET_FILE:SnowDesktop>"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS authoring integration lua preview render widget)
    add_dependencies(SnowDesktopWidgetAuthorPreviewCliTests
        snowwidget SnowDesktop)

    snowdesktop_add_test(
        SnowDesktopWidgetViewTreeTests
        widget_view_tree
        SOURCES
            tests/widget_view_tree_tests.cpp
            src/widget_view_contract.cpp
            src/widget_view_tree.cpp
            src/widget_view_lua.cpp
            src/widget_interaction_region.cpp
            src/widget_logical_slot.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS contract interaction layout lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetTextInputRulesTests
        widget_text_input_rules
        SOURCES
            tests/widget_text_input_rules_tests.cpp
            src/widget_text_input_rules.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetStorageTransactionTests
        widget_storage_transaction
        SOURCES
            tests/widget_storage_transaction_tests.cpp
            src/widget_storage_transaction.cpp
            src/widget_storage_value.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetStorageWriteBudgetTests
        widget_storage_write_budget
        SOURCES
            tests/widget_storage_write_budget_tests.cpp
            src/widget_storage_write_budget.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSettingRulesTests
        widget_setting_rules
        SOURCES
            tests/widget_setting_rules_tests.cpp
            src/widget_setting_rules.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua settings widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSecretStoreTests
        widget_secret_store
        SOURCES
            tests/widget_secret_store_tests.cpp
            src/widget_secret_store.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt crypt32
        LABELS contract lua security storage widget)

    snowdesktop_add_test(
        SnowDesktopBuiltinWidgetSourceContractTests
        builtin_widget_source_contract
        SOURCES tests/builtin_widget_source_contract_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract lua render storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetLogicalSlotTests
        widget_logical_slot
        SOURCES
            tests/widget_logical_slot_tests.cpp
            src/widget_logical_slot.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopLuaLogicalSlotContainerTests
        lua_logical_slot_container
        SOURCES
            tests/lua_logical_slot_container_tests.cpp
            src/widgets/lua_logical_slot.cpp
            src/core/slot.cpp
            src/core/container.cpp
            src/l10n.cpp
        INCLUDE_DIRECTORIES src src/core third_party/lua
        LINK_LIBRARIES d2d1 user32
        LABELS contract interaction lua slot widget)

    snowdesktop_add_test(
        SnowDesktopFolderSelfDropRulesTests
        folder_self_drop_rules
        SOURCES tests/folder_self_drop_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules folder)

    snowdesktop_add_test(
        SnowDesktopFolderMappingRulesTests
        folder_mapping_rules
        SOURCES tests/folder_mapping_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules folder widget)

    snowdesktop_add_test(
        SnowDesktopRightClickContractTests
        right_click_contract
        SOURCES tests/right_click_contract_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules interaction)

    snowdesktop_add_test(
        SnowDesktopQuickNavigationRulesTests
        quick_navigation_rules
        SOURCES
            tests/quick_navigation_rules_tests.cpp
            src/navigation_settings.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopPinyinData
        LABELS rules navigation)

    snowdesktop_add_test(
        SnowDesktopShortcutApplicationRulesTests
        shortcut_application_rules
        SOURCES tests/shortcut_application_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules shell)

    snowdesktop_add_test(
        SnowDesktopDesktopKeyboardRulesTests
        desktop_keyboard_rules
        SOURCES tests/desktop_keyboard_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction shell)

    snowdesktop_add_test(
        SnowDesktopIconRenderRulesTests
        icon_render_rules
        SOURCES tests/icon_render_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules shell ui)

    snowdesktop_add_test(
        SnowDesktopIconBeautifyTests
        icon_beautify
        SOURCES
            tests/icon_beautify_tests.cpp
            src/icon_beautify.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules icon ui)

    snowdesktop_add_test(
        SnowDesktopItemLocationTests
        item_location
        SOURCES
            tests/item_location_tests.cpp
            src/item_location.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 shlwapi uuid
        LABELS integration shell)

    snowdesktop_add_test(
        SnowDesktopShellContextMenuInvokeTests
        shell_context_menu_invoke
        SOURCES tests/shell_context_menu_invoke_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32
        LABELS contract shell)

    snowdesktop_add_test(
        SnowDesktopShellIntegrationContractTests
        shell_integration_contract
        SOURCES tests/shell_integration_contract_tests.cpp
        COMMAND_ARGUMENTS ${CMAKE_SOURCE_DIR}
        LABELS contract shell settings)

    snowdesktop_add_test(
        SnowDesktopShellLaunchWorkerTests
        shell_launch_worker
        SOURCES
            tests/shell_launch_worker_tests.cpp
            src/shell_launch_worker.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 user32
        LABELS integration shell interaction)

    snowdesktop_add_test(
        SnowDesktopShellFileOperationWorkerTests
        shell_file_operation_worker
        SOURCES
            tests/shell_file_operation_worker_tests.cpp
            src/item_location.cpp
            src/shell_file_operation_worker.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 shlwapi uuid
        LABELS integration shell interaction)

    snowdesktop_add_test(
        SnowDesktopDockAndWindowRulesTests
        dock_and_window_rules
        SOURCES
            tests/dock_and_window_rules_tests.cpp
            src/dock_window_preview.cpp
            src/dock_window_transition.cpp
            src/ui_animation_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES d2d1 dwrite dwmapi gdi32 shcore user32
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS rules window)

    snowdesktop_add_test(
        SnowDesktopUiAnimationSchedulerTests
        ui_animation_scheduler
        SOURCES
            tests/ui_animation_scheduler_tests.cpp
            src/ui_animation_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32
        LABELS rules animation)

    snowdesktop_add_test(
        SnowDesktopMenuIconRenderTests
        menu_icon_render
        SOURCES
            tests/menu_icon_render_tests.cpp
            src/menu_icon_render.cpp
            src/resource.rc
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES gdi32 user32
        LABELS contract ui)

    snowdesktop_add_test(
        SnowDesktopModernMenuInteractionTests
        modern_menu_interaction
        SOURCES
            tests/modern_menu_interaction_tests.cpp
            src/menu_icon_render.cpp
            src/modern_menu.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES gdi32 imm32 user32
        LABELS integration ui)

    snowdesktop_add_test(
        SnowDesktopComponentPreviewTests
        component_preview
        SOURCES
            tests/component_preview_tests.cpp
            src/component_preview.cpp
            src/menu_icon_render.cpp
            src/widget_preview_scene.cpp
            src/widget_preview_scene_model.cpp
        INCLUDE_DIRECTORIES src src/core
        LINK_LIBRARIES gdi32 user32
        LABELS integration ui preview)

    snowdesktop_add_test(
        SnowDesktopSteamWorkshopManagerTests
        steam_workshop_manager
        SOURCES
            tests/steam_workshop_manager_tests.cpp
            src/steam_workshop_cache.cpp
            src/steam_workshop_sync.cpp
        INCLUDE_DIRECTORIES steam_bridge/src src
        LINK_LIBRARIES
            SnowDesktopWorkshopManagerSupport
            SnowDesktopSteamAppIdentity
            advapi32
        COMMAND_ARGUMENTS
            "$<TARGET_FILE:snowwidget>"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract integration security steam sdk-free)
    add_dependencies(SnowDesktopSteamWorkshopManagerTests snowwidget)

    # Interactive visual smoke harness. It is excluded from normal builds and
    # CTest; run the target explicitly when reviewing the modern popup menu.
    add_executable(SnowDesktopModernMenuPreview WIN32 EXCLUDE_FROM_ALL
        tests/modern_menu_preview.cpp
        src/menu_icon_render.cpp
        src/modern_menu.cpp
        src/resource.rc)
    target_compile_features(SnowDesktopModernMenuPreview PRIVATE cxx_std_20)
    target_include_directories(SnowDesktopModernMenuPreview PRIVATE src)
    target_link_libraries(SnowDesktopModernMenuPreview PRIVATE
        gdi32 imm32 user32)
    if (MSVC)
        set_property(TARGET SnowDesktopModernMenuPreview PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(SnowDesktopModernMenuPreview PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(SnowDesktopModernMenuPreview PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    snowdesktop_add_test(
        SnowDesktopPopupAnimationRulesTests
        popup_animation_rules
        SOURCES tests/popup_animation_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules animation)

    add_custom_target(
        SnowDesktopTests
        DEPENDS ${SNOWDESKTOP_TEST_TARGETS})
endif()


