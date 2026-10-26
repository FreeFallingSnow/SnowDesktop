cmake_minimum_required(VERSION 3.24)

include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.SharedBuildGuard.cmake")
snowdesktop_check_shared_output()

project(SnowDesktop LANGUAGES C CXX)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.CompileParallelism.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.DependencyEvidence.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.ProductBuild.cmake")

# Ship a deterministic WebP decoder, independent of installed WIC extensions.
foreach(WEBP_TOOL ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
    set(WEBP_BUILD_${WEBP_TOOL} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
option(SNOWDESKTOP_CACHE_WEBP_PROBES "Reuse unchanged native MSVC WebP SIMD checks" ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowDesktop.WebPProbeCache.cmake")
add_subdirectory(third_party/libwebp EXCLUDE_FROM_ALL)
if(MSVC)
    # The host and native tests use the static CRT. Apply it to the upstream
    # object libraries too; changing only webpdecoder/webp leaves mixed CRTs.
    get_property(SNOWDESKTOP_WEBP_TARGETS DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(SNOWDESKTOP_WEBP_TARGET IN LISTS SNOWDESKTOP_WEBP_TARGETS)
        set_property(TARGET ${SNOWDESKTOP_WEBP_TARGET} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()
add_library(SnowDesktopPuff STATIC third_party/puff/puff.c)
target_include_directories(SnowDesktopPuff PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/puff")
if(MSVC)
    set_property(TARGET SnowDesktopPuff PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowDesktop.Perfetto.cmake")
# Decoder-only upstream target exports its root; expose the public API directory.
target_include_directories(webpdecoder INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp/src>")

# WinUI 3 XAML compilation and NuGet PackageReference integration are MSBuild
# features. Keeping this requirement explicit avoids silently generating a
# project that can compile the native host but cannot generate the XAML/WinRT
# sources required by the settings UI.
if (NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
    message(FATAL_ERROR
        "SnowDesktop WinUI 3 requires a Visual Studio/MSBuild generator")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.VersionResources.cmake")

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
    "${CMAKE_SOURCE_DIR}/src/resources/version.rc.in"
    "${SNOWDESKTOP_EXE_VERSION_RESOURCE}"
    @ONLY)

set(SNOWDESKTOP_FILE_DESCRIPTION "SnowDesktop Steam launcher")
set(SNOWDESKTOP_ORIGINAL_FILENAME "SnowDesktopLauncher.exe")
set(SNOWDESKTOP_FILE_TYPE "VFT_APP")
set(SNOWDESKTOP_LEGAL_COPYRIGHT
    "Copyright (c) SnowDesktop contributors")
set(SNOWDESKTOP_LAUNCHER_VERSION_RESOURCE
    "${SNOWDESKTOP_VERSION_RESOURCE_DIR}/SnowDesktopLauncher.version.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/resources/version.rc.in"
    "${SNOWDESKTOP_LAUNCHER_VERSION_RESOURCE}"
    @ONLY)

set(SNOWDESKTOP_FILE_DESCRIPTION "SnowDesktop taskbar hook")
set(SNOWDESKTOP_ORIGINAL_FILENAME "SnowDesktopTaskbarHook.dll")
set(SNOWDESKTOP_FILE_TYPE "VFT_DLL")
set(SNOWDESKTOP_LEGAL_COPYRIGHT
    "Copyright (c) SnowDesktop contributors; portions copyright (c) TranslucentTB contributors")
set(SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE
    "${SNOWDESKTOP_VERSION_RESOURCE_DIR}/SnowDesktopTaskbarHook.version.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/resources/version.rc.in"
    "${SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE}"
    @ONLY)

message(STATUS "SnowDesktop version: ${SNOWDESKTOP_VERSION}")
message(STATUS
    "SnowDesktop Steam App/Windows Depot: ${SNOWDESKTOP_STEAM_APP_ID}/${SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID}")

# Keep native WinUI and C++/WinRT PackageReferences on one repository-wide
# version so helper targets can use the same restored tools on a clean build.
set(SNOWDESKTOP_WINDOWSAPPSDK_PACKAGE_VERSION "2.4.0")
set(SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION "3.0.260818.1")

set(SNOWDESKTOP_SYSTEM_CONTROL_SOURCES
    src/system/controls/system_controls.cpp
    src/system/controls/system_control_windows.cpp
    src/system/controls/system_control_audio.cpp
    src/system/controls/system_control_power.cpp
    src/system/controls/system_control_brightness.cpp
    src/system/controls/system_control_wifi.cpp
    src/system/controls/system_control_bluetooth.cpp
    src/system/controls/system_control_media.cpp
    src/system/controls/system_control_quick_actions.cpp)
set(SNOWDESKTOP_SYSTEM_CONTROL_LIBRARIES
    dxva2 wlanapi powrprof setupapi oleaut32 wbemuuid)

set(SNOWDESKTOP_SOURCES
    ${SNOWDESKTOP_SYSTEM_CONTROL_SOURCES}
    src/main.cpp
    src/platform/auto_start_manager.cpp
    src/platform/auto_start_manager.h
    src/platform/auto_start_elevation.cpp
    src/platform/auto_start_elevation.h
    src/platform/auto_start_rules.h
    src/platform/startup_privileges_dialog.h
    src/platform/application_crash_watchdog.cpp
    src/desktop/desktop_namespace_registry.cpp
    src/desktop/desktop_namespace_registry.h
    src/desktop/display_color_mode.h
    src/icons/icon_beautify.cpp
    src/icons/icon_backplate_analysis.inc
    src/icons/icon_beautify_preview.h
    src/icons/icon_glass_color.h
    src/icons/icon_appearance.h
    src/icons/icon_appearance_codec.h
    src/icons/icon_appearance_rules.h
    src/icons/icon_customization_lifecycle.h
    src/icons/icon_control_source.h
    src/app/render/app_bitmap_cache.cpp
    src/app/menus/app_background_menu.cpp
    src/app/render/app_animation_scheduler.cpp
    src/app/input/app_clipboard_interaction.cpp
    src/app/render/app_composition_animation_overlay.cpp
    src/app/popup/app_container_navigation.cpp
    src/app/drag_drop/app_data_object.cpp
    src/app/shell/app_desktop_enumeration.cpp
    src/app/render/app_desktop_foreground_composition.cpp
    src/app/layout/app_desktop_layout.cpp
    src/app/shell/app_desktop_reload.cpp
    src/app/dock/app_dock_drop.cpp
    src/app/dock/app_dock_running_drag.cpp
    src/app/dock/dock_running_drag_state.h
    src/app/dock/dock_input_diagnostics.h
    src/dock/dock_running_drag_rules.h
    src/app/dock/app_dock_launch.cpp
    src/app/dock/app_dock_tray.cpp
    src/app/dock/dock_tray_popup_state.h
    src/dock/dock_tray_popup_layout.h
    src/system/tray/tray_native_tooltip.h
    src/dock/dock_installed_application.h
    src/app/dock/app_dock_layout.cpp
    src/dock/dock_appbar.cpp
    src/dock/dock_appbar.h
    src/dock/dock_running_animation.h
    src/dock/dock_entry_removal.h
    src/app/features/app_status_bar.cpp
    src/system/status_bar/status_bar_view.cpp
    src/system/status_bar/status_bar_tooltip.h
    src/system/status_bar/status_bar_view.h
    src/system/status_bar/status_bar_geometry.h
    src/system/status_bar/status_bar_preview.cpp
    src/system/status_bar/status_bar_preview.h
    src/system/status_bar/status_bar.cpp
    src/system/status_bar/status_bar_input_method.cpp
    src/system/status_bar/status_bar_input_method.h
    src/system/status_bar/status_bar_input_method_profiles.h
    src/system/status_bar/status_bar_notification.cpp
    src/system/status_bar/status_bar_notification.h
    src/system/tray/tray_service.cpp
    src/system/tray/tray_menu_placement.cpp
    src/system/tray/tray_menu_placement.h
    src/ui/render/native_tooltip.cpp
    src/ui/render/native_tooltip.h
    src/ui/render/inline_tooltip_backdrop.h
    src/ui/render/native_tooltip_content.h
    src/ui/render/native_tooltip_preferences.h
    src/ui/render/native_tooltip_state.h
    src/ui/render/native_control_geometry.h
    src/ui/render/smooth_corner_geometry.h
    src/system/tray/tray_model.cpp
    src/system/tray/tray_change_notifications.h
    src/system/tray/tray_service.h
    src/system/tray/tray_geometry_table.h
    src/system/tray/tray_focus.h
    src/taskbar_hook/tray_protocol.h
    src/system/status_bar/status_bar.h
    src/system/status_bar/status_bar_shell_shortcut.h
    src/system/status_bar/status_bar_shell_shortcut_relay.h
    src/system/status_bar/status_bar_task_view_input.h
    src/system/status_bar/status_bar_interaction.h
    src/system/status_bar/status_bar_settings.h
    src/system/status_bar/status_bar_reveal.h
    src/system/status_bar/status_bar_appbar.h
    src/app/dock/app_dock_menu.cpp
    src/app/dock/app_dock_model.cpp
    src/app/dock/app_dock_render.cpp
    src/app/dock/app_dock_window_control.cpp
    src/app/dock/app_dock_external_minimize.cpp
    src/dock/dock_external_minimize.cpp
    src/dock/dock_external_minimize.h
    src/dock/dock_minimize_process_cache.h
    src/dock/dock_work_area_reservation.h
    src/dock/dock_minimize_protocol.h
    src/dock/dock_minimize_hook_threads.h
    src/app/dock/app_dock_window_tracking.cpp
    src/app/drag_drop/app_drag_hint_window.cpp
    src/app/drag_drop/app_drag_lifecycle.cpp
    src/app/drag_drop/app_drag_preview_window.cpp
    src/app/drag_drop/app_drag_target_update.cpp
    src/drag_drop/drop_data_url.cpp
    src/drag_drop/drop_data_url.h
    src/drag_drop/drop_image_data.cpp
    src/drag_drop/drop_image_data.h
    src/drag_drop/drop_text_rules.cpp
    src/drag_drop/drop_text_rules.h
    src/app/drag_drop/app_drop_data_extraction.cpp
    src/drag_drop/external_drop_content.cpp
    src/drag_drop/file_source_eligibility.h
    src/app/drag_drop/app_url_drop_download.cpp
    src/app/features/app_website_icon.cpp
    src/app/features/app_data_cleanup.cpp
    src/data/data_cleanup.cpp
    src/icons/website_icon.cpp
    src/app/drag_drop/app_drop_execution.cpp
    src/app/drag_drop/app_external_slot_drop.cpp
    src/app/drag_drop/app_drop_preview.cpp
    src/app/drag_drop/app_drop_render.cpp
    src/app/dock/app_floating_dock_interaction.cpp
    src/app/dock/app_floating_dock_lifecycle.cpp
    src/app/dock/app_floating_dock_render.cpp
    src/app/dock/app_floating_dock_window.cpp
    src/app/popup/app_floating_popup_window.cpp
    src/app/render/app_graphics_device.cpp
    src/app/render/app_glass_render.cpp
    src/app/layout/app_grid_drag.cpp
    src/app/layout/app_grid_placement.cpp
    src/app/input/app_hit_testing.cpp
    src/app/render/app_icon_loader.cpp
    src/app/render/app_icon_render.cpp
    src/app/layout/app_item_sorting.cpp
    src/app/menus/app_item_menu.cpp
    src/app/input/app_keyboard_input.cpp
    src/app/input/app_type_to_select.cpp
    src/app/input/app_keyboard_navigation.cpp
    src/app/layout/app_layout.cpp
    src/app/features/app_large_icons.cpp
    src/app/features/app_large_icon_interaction.cpp
    src/icons/large_icon_assets.cpp
    src/icons/icon_image_codec.cpp
    src/icons/icon_pack_archive.cpp
    src/icons/icon_pack_archive.h
    src/icons/android_icon_pack.cpp
    src/icons/android_icon_pack.h
    src/icons/icon_image_codec.h
    src/icons/icon_override_rules.h
    src/icons/icon_package.cpp
    src/icons/icon_package.h
    src/icons/icon_workshop.cpp
    src/icons/icon_workshop.h
    src/icons/icon_library.cpp
    src/icons/icon_library.h
    src/icons/icon_library_bitmap_cache.h
    src/icons/icon_library_match.h
    src/app/features/app_icon_library.cpp
    src/app/features/app_icon_appearance.cpp
    src/icons/large_icon_renderer.cpp
    src/app/lifecycle/app_lifecycle.cpp
    src/app/widgets/app_lua_panel.cpp
    src/app/widgets/app_lua_panel_render.cpp
    src/app/widgets/app_lua_bridge.cpp
    src/app/widgets/widget_reference_picker.h
    src/app/menus/app_menu_icons.cpp
    src/app/menus/tray_context_menu_placement.h
    src/app/lifecycle/app_message_dispatch.cpp
    src/shell/shell_context_menu_site.cpp
    src/shell/shell_new_menu.h
    src/desktop/windows_desktop_layout.cpp
    src/shell/shell_launch_worker.cpp
    src/shell/shell_launch_process.cpp
    src/shell/shell_extension_menu.cpp
    src/shell/shell_extension_menu_query.h
    src/shell/shell_extension_menu_cache.cpp
    src/shell/shell_extension_catalogue.cpp
    src/shell/shell_menu_registry_watch.h
    src/shell/shell_extension_service.cpp
    src/shell/shell_file_operation_worker.cpp
    src/shell/shell_item_display_name.h
    src/app/render/app_navigation_render.cpp
    src/app/drag_drop/app_ole_drop_routing.cpp
    src/app/drag_drop/app_ole_drop_session.cpp
    src/app/render/app_overlay_render.cpp
    src/app/layout/app_page_grid.cpp
    src/app/render/app_paint.cpp
    src/app/input/app_pointer_context.cpp
    src/app/input/app_pointer_down.cpp
    src/app/input/app_pointer_move.cpp
    src/app/input/native_item_drag_source.h
    src/app/input/app_pointer_release.cpp
    src/app/popup/app_popup_geometry.cpp
    src/app/popup/app_popup_dwell_interaction.cpp
    src/app/popup/app_popup_context_menu.cpp
    src/app/popup/app_popup_lifecycle.cpp
    src/app/popup/app_popup_render.cpp
    src/app/popup/app_popup_transition.cpp
    src/app/navigation/app_quick_navigation_interaction.cpp
    src/app/navigation/app_quick_navigation_features.cpp
    src/app/navigation/app_quick_navigation_preview.cpp
    src/app/features/app_theme_preview.cpp
    src/app/navigation/app_quick_navigation_layout.cpp
    src/app/navigation/app_quick_navigation_messages.cpp
    src/app/navigation/app_quick_navigation_model.cpp
    src/app/navigation/app_quick_navigation_render.cpp
    src/navigation/quick_navigation_attraction_renderer.h
    src/app/navigation/app_quick_navigation_window.cpp
    src/app/input/app_rename_begin.cpp
    src/app/input/software_desktop_hotkey.h
    src/app/input/app_rename_commit.cpp
    src/app/input/app_rename_editor.cpp
    src/app/input/app_rename_target.cpp
    src/app/render/app_render_geometry.cpp
    src/app/render/app_render_primitives.cpp
    src/app/lifecycle/app_run.cpp
    src/app/features/app_usage_guide.cpp
    src/platform/usage_guide.cpp
    src/platform/usage_guide.h
    src/app/shell/app_shell_context_menu.cpp
    src/app/shell/app_shell_file_operation.cpp
    src/app/shell/app_shell_menu.cpp
    src/app/render/app_scene_render.cpp
    src/app/input/app_scroll_interaction.cpp
    src/app/input/app_selection_interaction.cpp
    src/app/lifecycle/app_settings_apply.cpp
    src/app/input/desktop_passthrough_indicator.cpp
    src/app/render/app_text_render.cpp
    src/platform/runtime_module_policy.h
    src/platform/runtime_module_status.h
    src/winui/runtime_modules_page_presenter.h
    src/app/features/app_runtime_modules.cpp
    src/app/features/app_taskbar_integration.cpp
    src/app/dock/app_dock_start_menu.cpp
    src/shell/native_start_menu.cpp
    src/app/features/app_steam_workshop.cpp
    src/app/lifecycle/app_timer_dispatch.cpp
    src/app/tray/app_tray.cpp
    src/app/lifecycle/app_window_dispatch.cpp
    src/app/widgets/app_widget_context_menu.cpp
    src/app/widgets/app_widget_grouping.cpp
    src/app/widgets/app_widget_marquee_composition.cpp
    src/app/widgets/app_widget_composition.cpp
    src/app/widgets/app_widget_placement.cpp
    src/app/widgets/app_widget_refresh.cpp
    src/app/drag_drop/drag_drop_controller.cpp
    src/app/drag_drop/drag_render_cache.cpp
    src/app/drag_drop/ole_drag_drop_adapter.cpp
    src/app/tray/tray_icon_controller.cpp
    src/app/widgets/widget_notification_presenter.cpp
    src/app/render/desktop_backdrop_compositor.cpp
    src/app/lifecycle/startup_animation.cpp
    src/app/features/wallpaper_engine_capture.cpp
    src/data/atomic_file.cpp
    src/dock/dock_window_preview.cpp
    src/dock/dock_window_preview_layout.h
    src/dock/dock_window_pin.cpp
    src/dock/dock_window_transition.cpp
    src/dock/dock_window_source_cloak.h
    src/core/item.cpp
    src/core/drag_target_resolver.cpp
    src/core/dock.cpp
    src/core/dock_entry_removal.cpp
    src/core/slot.cpp
    src/core/container.cpp
    src/core/desktop.cpp
    src/data/data_paths.cpp
    src/drag_drop/virtual_file_drop.cpp
    src/drag_drop/virtual_file_drop.h
    src/drag_drop/url_drop_download_worker.cpp
    src/drag_drop/url_drop_download_worker.h
    src/drag_drop/url_drop_resource.cpp
    src/drag_drop/url_drop_resource.h
    src/steam/steam_runtime_context.cpp
    src/steam/steam_runtime_context.h
    src/steam/steam_entitlement.cpp
    src/steam/steam_entitlement.h
    src/system/calendar/calendar_service.cpp
    src/system/calendar/calendar_display.cpp
    src/data/full_data_backup.cpp
    src/data/portable_data_migration.cpp
    src/platform/single_instance.cpp
    src/platform/force_restart_hotkey.cpp
    src/platform/force_restart_hotkey.h
    src/diagnostics/debug_profile.cpp
    src/platform/deployment_context.cpp
    src/platform/hook_entry_deployment.cpp
    src/platform/hook_entry_deployment.h
    src/platform/hook_entry_protocol.h
    src/platform/hook_registration_retry.h
    src/taskbar_hook/taskbar_failure_feedback.h
    src/dock/dock_cloak_dispatcher.cpp
    src/dock/dock_cloak_dispatcher.h
    src/platform/shell_overlay_window.h
    src/platform/window_peek_policy.h
    src/icons/shortcut_icon_resource.cpp
    src/icons/shortcut_icon_resource.h
    src/icons/icon_load_diagnostics.h
    src/common/utils.cpp
    src/diagnostics/diagnostic_log.cpp
    src/platform/operation_feedback.cpp
    src/desktop/desktop_display_settings.h
    src/settings/settings_controller.cpp
    src/settings/settings_controller.h
    src/settings/settings_route.cpp
    src/settings/settings_route.h
    src/settings/settings_search_index.cpp
    src/settings/settings_search_index.h
    src/settings/personalization_initialization.h
    src/settings/settings_store_native.cpp
    src/settings/settings_window.cpp
    src/settings/settings_ipc_channel.cpp
    src/settings/icon_library_ipc_paging.h
    src/settings/settings_ipc_services.cpp
    src/settings/settings_ipc_widget_service.cpp
    src/settings/settings_process.cpp
    src/winui/settings_ipc_backends.cpp
    src/winui/settings_process_entry.cpp
    src/winui/App.idl
    src/winui/App.xaml
    src/winui/App.xaml.cpp
    src/winui/App.xaml.h
    src/winui/backup_data_page_backend.cpp
    src/winui/backup_data_page_backend.h
    src/winui/backup_data_page_presenter.cpp
    src/winui/backup_data_page_presenter.h
    src/winui/desktop_page_presenter.cpp
    src/winui/desktop_page_presenter.h
    src/winui/dock_page_presenter.cpp
    src/winui/dock_tray_settings_editor.cpp
    src/winui/status_bar_page_presenter.cpp
    src/winui/desktop_style_page_presenter.cpp
    src/winui/desktop_style_page_presenter.h
    src/ui/render/native_ui_scene.cpp
    src/ui/render/native_ui_scene.h
    src/system/panel/system_panel.cpp
    src/system/controls/system_control_prompt.cpp
    src/system/panel/system_panel.h
    src/system/panel/system_panel_backdrop_placement.h
    src/system/panel/system_panel_model.cpp
    src/system/panel/system_panel_model.h
    src/system/calendar/system_calendar_editor.cpp
    src/system/calendar/system_calendar_editor.h
    src/system/calendar/system_calendar_editor_state.h
    src/system/panel/system_panel_preview.cpp
    src/system/panel/system_panel_preview.h
    src/winui/status_bar_page_presenter.h
    src/winui/calendar_page_presenter.cpp
    src/winui/calendar_page_presenter.h
    src/winui/context_menu_page_presenter.cpp
    src/winui/context_menu_page_presenter.h
    src/winui/animation_performance_page_presenter.cpp
    src/winui/animation_performance_page_presenter.h
    src/winui/dock_page_presenter.h
    src/winui/dock_tray_settings_editor.h
    src/winui/SettingsShell.xaml
    src/winui/SettingsShell.xaml.cpp
    src/winui/SettingsShell.xaml.h
    src/winui/general_page_presenter.cpp
    src/winui/general_page_presenter.h
    src/winui/general_advanced_feature_status.h
    src/winui/general_advanced_feature_ipc_values.h
    src/winui/category_creation_acknowledgement.h
    src/settings/category_rule_creation.h
    src/winui/runtime_modules_page_presenter.cpp
    src/winui/home_about_page_presenter.cpp
    src/winui/start_page_presenter.cpp
    src/winui/start_page_presenter.h
    src/winui/home_about_page_presenter.h
    src/winui/home_about_page_model.h
    src/winui/hotkey_recorder.cpp
    src/winui/hotkey_recorder.h
    src/winui/hotkey_recorder_rules.h
    src/winui/page_layout_page_presenter.cpp
    src/winui/large_icon_page_presenter.cpp
    src/winui/icon_appearance_presenter.cpp
    src/winui/icon_appearance_presenter.h
    src/winui/icon_library_page_presenter.cpp
    src/winui/icon_library_page_presenter.h
    src/winui/large_icon_page_presenter.h
    src/winui/page_layout_page_presenter.h
    src/winui/personalization_page_presenter.cpp
    src/winui/personalization_page_presenter.h
    src/winui/pch.h
    src/winui/settings_shell_navigation.cpp
    src/winui/settings_shell_navigation.h
    src/winui/settings_focus_feedback.h
    src/winui/settings_visual_preview.cpp
    src/winui/settings_visual_preview.h
    src/winui/settings_window_host.cpp
    src/winui/settings_window_host.h
    src/winui/widget_settings_presenter.cpp
    src/winui/widget_settings_presenter.h
    src/winui/widgets_page_backend.cpp
    src/winui/widgets_page_backend.h
    src/winui/widgets_page_presenter.cpp
    src/winui/widgets_page_presenter.h
    src/winui/winui_runtime.cpp
    src/winui/winui_runtime.h
    src/winui/xaml_metadata_provider_bridge.cpp
    src/winui/xaml_type_info_bridge.cpp
    src/winui/xaml_type_info_impl_bridge.cpp
    src/ui/render/ui_animation_scheduler.cpp
    src/theme/personalization.cpp
    src/theme/personalization_path.cpp
    src/theme/theme_library.cpp
    src/theme/theme_preview.cpp
    src/theme/theme_workshop.cpp
    src/dock/dock_settings.cpp
    src/dock/dock_attention_dot.h
    src/taskbar_hook/taskbar_symbol_resolver.cpp
    src/navigation/navigation_settings.cpp
    src/settings/general_settings.cpp
    src/ui/render/app_font.cpp
    src/settings/category_settings.cpp
    src/ui/preview/component_preview.cpp
    src/ui/preview/native_component_preview_export.cpp
    src/ui/preview/preview_png_writer.cpp
    src/widget/authoring/widget_author_profile.cpp
    src/widget/authoring/widget_author_profile.h
    src/widget/authoring/widget_author_preview.cpp
    src/widget/preview/widget_preview_stage.cpp
    src/widget/preview/widget_wallpaper_pixels.h
    src/widget/preview/widget_preview_scene.cpp
    src/widget/preview/widget_preview_scene_model.cpp
    src/common/l10n.cpp
    src/layout/item_location.cpp
    src/data/layout_storage.cpp
    src/ui/menu/menu_icon_render.cpp
    src/ui/menu/menu_builtin_icon.cpp
    src/ui/menu/modern_menu.cpp
    src/navigation/search_match.cpp
    src/navigation/everything_search.cpp
    src/navigation/quick_navigation_everything_search.cpp
    src/navigation/quick_navigation_search_async.cpp
    src/widget/runtime/lua_runtime.cpp
    src/widget/api/widget_api_registry.cpp
    src/widget/packages/widget_package_image_cache.cpp
    src/widget/runtime/widget_l10n_format.cpp
    src/widget/runtime/widget_time.cpp
    src/widget/preview/widget_preview_context.cpp
    src/widget/data/widget_data_broker.cpp
    src/widget/data/widget_calendar_event_query.cpp
    src/widget/data/widget_calendar_event_query.h
    src/widget/api/widget_calendar_query_options.cpp
    src/widget/api/widget_calendar_query_options.h
    src/widget/tasks/widget_task_broker.cpp
    src/widget/tasks/widget_native_task_executor.cpp
    src/widget/tasks/widget_location_task_executor.cpp
    src/widget/tasks/widget_notification_runtime.cpp
    src/widget/tasks/widget_notification_schedule_store.cpp
    src/widget/tasks/widget_media_task_executor.cpp
    src/widget/tasks/widget_audio_output_task_executor.cpp
    src/widget/tasks/widget_audio_playback.cpp
    src/widget/tasks/widget_system_control_tasks.cpp
    src/widget/tasks/widget_clipboard_task_executor.cpp
    src/widget/tasks/widget_filesystem_handle_store.cpp
    src/widget/tasks/widget_filesystem_task_executor.cpp
    src/widget/tasks/widget_filesystem_watch_service.cpp
    src/widget/tasks/widget_filesystem_watch_origin.cpp
    src/widget/tasks/widget_filesystem_watch_origin.h
    src/widget/tasks/widget_app_task_executor.cpp
    src/widget/settings/widget_system_settings.cpp
    src/widget/data/widget_system_data_provider.cpp
    src/widget/data/widget_network_status_refresh.h
    src/widget/data/widget_gpu_sampler.cpp
    src/widget/data/widget_audio_analysis_provider.cpp
    src/widget/view/widget_draw_geometry.cpp
    src/widget/view/widget_background_cache.cpp
    src/widget/view/widget_image_sampling.cpp
    src/widget/view/widget_interaction_region.cpp
    src/widget/view/widget_accessibility_events.cpp
    src/widget/view/widget_accessibility_provider.cpp
    src/widget/view/widget_view_accessibility.cpp
    src/widget/api/widget_view_contract.cpp
    src/widget/view/widget_view_tree.cpp
    src/widget/view/widget_view_lua.cpp
    src/widget/view/widget_text_input_rules.cpp
    src/widget/storage/widget_storage_transaction.cpp
    src/widget/storage/widget_storage_value.cpp
    src/widget/storage/widget_storage_write_budget.cpp
    src/widget/storage/widget_secret_store.cpp
    src/widget/settings/widget_settings_model.h
    src/widget/settings/widget_settings_service.cpp
    src/widget/settings/widget_settings_service.h
    src/widget/settings/widget_engine_settings_backend.cpp
    src/widget/settings/widget_engine_settings_backend.h
    src/widget/settings/widget_setting_rules.cpp
    src/widget/runtime/widget_logical_slot.cpp
    src/widget/runtime/widget_lua_lifecycle.cpp
    src/widget/runtime/widget_runtime_diagnostics.cpp
    src/diagnostics/performance_capture.cpp
    src/diagnostics/performance_process_resources.cpp
    src/diagnostics/performance_process_resources.h
    src/app/render/app_resource_statistics.h
    src/diagnostics/performance_analysis.cpp
    src/diagnostics/performance_environment.cpp
    src/diagnostics/performance_control_pipe.cpp
    src/widget/runtime/widget_runtime_scheduler.cpp
    src/widget/runtime/widget_timer_dispatch.h
    src/widget/runtime/widget_calendar_provider.h
    src/widget/runtime/widget_data_subscribe_lua.inc
    src/widget/runtime/widget_engine.cpp
    src/widget/permissions/widget_permission_broker.cpp
    src/widget/permissions/widget_permission_state.cpp
    src/widget/packages/widget_package.cpp
    src/widget/packages/widget_package_paths.cpp
    src/steam/steam_workshop_cache.cpp
    src/steam/steam_workshop_diagnostics.h
    src/steam/steam_workshop_source.cpp
    src/steam/steam_workshop_sync.cpp
    src/system/system_snapshot.cpp
    src/platform/http_runtime.cpp
    src/widgets/widget_base.cpp
    src/widgets/collection.cpp
    src/widgets/collection_group.cpp
    src/widgets/file_group.cpp
    src/widgets/file_categories.cpp
    src/widgets/file_category_auto_collect_rules.h
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

set(MINHOOK64_SOURCES
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)

add_library(SnowDesktopWorkshopImgui STATIC ${IMGUI_SOURCES})
target_compile_features(SnowDesktopWorkshopImgui PRIVATE cxx_std_20)
target_include_directories(SnowDesktopWorkshopImgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
if(MSVC)
    set_property(TARGET SnowDesktopWorkshopImgui PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopWorkshopImgui PRIVATE /MP${SNOWDESKTOP_COMPILE_JOBS} /W4 /permissive- /EHsc /utf-8 /Z7)
    target_compile_definitions(SnowDesktopWorkshopImgui PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(SnowDesktopLua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(SnowDesktopLua PUBLIC third_party/lua)
if(MSVC)
    set_property(TARGET SnowDesktopLua PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopLua PRIVATE /MP${SNOWDESKTOP_COMPILE_JOBS}
        /wd4244 /wd4310 /wd4324 /wd4701 /wd4702)
endif()

add_library(SnowDesktopEverything STATIC ${EVERYTHING_SOURCES})
set_source_files_properties(${EVERYTHING_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(SnowDesktopEverything PUBLIC
    third_party/everything/include)
if(MSVC)
    set_property(TARGET SnowDesktopEverything PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopEverything PRIVATE /MP${SNOWDESKTOP_COMPILE_JOBS} /wd4100 /wd4133 /wd4996)
endif()

add_library(SnowDesktopPinyinData STATIC ${PINYIN_DATA_SOURCES})
target_compile_features(SnowDesktopPinyinData PRIVATE cxx_std_20)
target_include_directories(SnowDesktopPinyinData PUBLIC
    third_party/pinyin-data)
if(MSVC)
    set_property(TARGET SnowDesktopPinyinData PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopPinyinData PRIVATE /MP${SNOWDESKTOP_COMPILE_JOBS})
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
        src/taskbar_hook/tray_collector.cpp
        src/taskbar_hook/tray_protocol.h
        src/taskbar_hook/taskbar_native.cpp
        src/taskbar_hook/taskbar_classic_surface.cpp
        src/taskbar_hook/taskbar_hook_protocol.h
        src/taskbar_hook/taskbar_xaml_readiness.h
        src/taskbar_hook/taskbar_autohide_observer.cpp
        src/taskbar_hook/taskbar_autohide_observer.h
        src/taskbar_hook/taskbar_autohide_trace.h
        src/taskbar_hook/taskbar_autohide_rules.h
        ${MINHOOK64_SOURCES}
        src/taskbar_hook/taskview_visibility.h
        src/taskbar_hook/taskbar_hook.def
        "${SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE}"
    )
    add_dependencies(SnowDesktopTaskbarHook SnowDesktopCppWinRT)
    set_target_properties(SnowDesktopTaskbarHook PROPERTIES
        OUTPUT_NAME "SnowDesktopTaskbarHook")
    target_compile_features(SnowDesktopTaskbarHook PRIVATE cxx_std_20)
    target_include_directories(SnowDesktopTaskbarHook PRIVATE
        src
        src/taskbar_hook
        third_party/minhook/include
        "${CPPWINRT_OUTPUT_DIR}"
    )
    target_compile_definitions(SnowDesktopTaskbarHook PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00 WINRT_NO_MODULE_LOCK)
    target_link_libraries(SnowDesktopTaskbarHook PRIVATE
        advapi32 comctl32 d2d1 d3d11 dcomp dwmapi dxgi dxguid gdi32 ole32 runtimeobject shell32 user32 windowsapp)
    set_property(TARGET SnowDesktopTaskbarHook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopTaskbarHook PRIVATE
        /W4 /permissive- /EHsc /utf-8)

    # Keep the legacy Windows.UI.Xaml/ShellView projection out of the main
    # WinUI 3 executable.  The Windows SDK currently ships a different
    # C++/WinRT generator version than the pinned NuGet package, and mixing
    # their generated headers in one translation unit is rejected by
    # C++/WinRT.  This narrow library has a standard C++ public boundary and
    # may therefore continue using the hook projection independently.
    add_library(SnowDesktopSearchVisibility STATIC
        src/taskbar_dynamic/search_visibility_detector.cpp
        src/taskbar_dynamic/search_visibility_detector.h)
    add_dependencies(SnowDesktopSearchVisibility SnowDesktopCppWinRT)
    set_property(TARGET SnowDesktopSearchVisibility PROPERTY
        VS_PACKAGE_REFERENCES
            "Microsoft.Windows.CppWinRT_${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}")
    set_property(TARGET SnowDesktopSearchVisibility PROPERTY
        VS_GLOBAL_MSBuildProjectExtensionsPath "$(ProjectDir)obj\\$(MSBuildProjectName)\\")
    set_property(TARGET SnowDesktopSearchVisibility PROPERTY VS_USER_PROPS
        "${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.ShellView.props")
    set_property(TARGET SnowDesktopSearchVisibility PROPERTY
        VS_GLOBAL_SnowDesktopCppWinRtPackageVersion
            "${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}")
    target_compile_features(SnowDesktopSearchVisibility PRIVATE cxx_std_20)
    target_include_directories(SnowDesktopSearchVisibility PRIVATE
        src/taskbar_dynamic
        "${CMAKE_BINARY_DIR}/generated/shellview-cppwinrt")
    target_compile_definitions(SnowDesktopSearchVisibility PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(SnowDesktopSearchVisibility PRIVATE
        ole32 runtimeobject windowsapp)
    set_property(TARGET SnowDesktopSearchVisibility PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopSearchVisibility PRIVATE
        /W4 /permissive- /EHsc /utf-8)

    add_library(SnowDesktopWallpaperHook SHARED
        src/wallpaper_hook/wallpaper_hook.cpp
        src/wallpaper_hook/wallpaper_hook_protocol.h
        ${MINHOOK64_SOURCES}
    )
    target_compile_features(SnowDesktopWallpaperHook PRIVATE cxx_std_20)
    target_include_directories(SnowDesktopWallpaperHook PRIVATE
        src/wallpaper_hook
        third_party/minhook/include
        third_party/minhook/src
        third_party/minhook/src/hde
    )
    target_compile_definitions(SnowDesktopWallpaperHook PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(SnowDesktopWallpaperHook PRIVATE
        d3d9 d3d11 dxgi user32)
    set_property(TARGET SnowDesktopWallpaperHook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopWallpaperHook PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    set_source_files_properties(${MINHOOK64_SOURCES} PROPERTIES
        COMPILE_OPTIONS
            "/wd4100;/wd4201;/wd4244;/wd4267;/wd4310;/wd4701")
endif()

set_source_files_properties(src/winui/App.xaml PROPERTIES
    VS_XAML_TYPE "ApplicationDefinition")
set_source_files_properties(
    src/resources/resource.rc
    src/resources/app_icon.rc
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/resources")
set_source_files_properties(src/winui/SettingsShell.xaml PROPERTIES
    VS_XAML_TYPE "Page")
set_source_files_properties(
    src/winui/App.idl
    src/winui/App.xaml.cpp
    src/winui/App.xaml.h
    PROPERTIES
        VS_SETTINGS "DependentUpon=App.xaml;SubType=Code")
set_source_files_properties(
    src/winui/SettingsShell.xaml.cpp
    src/winui/SettingsShell.xaml.h
    PROPERTIES
        VS_SETTINGS "DependentUpon=SettingsShell.xaml;SubType=Code")

# CMake merges .manifest target sources with the linker/Windows App SDK
# manifests. Opt in only native HWND hosts and their matching preview tests;
# preserve generated UAC, WinRT activation and common-controls declarations.
function(snowdesktop_enable_native_window_compatibility TARGET_NAME)
    target_sources(${TARGET_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/cmake/native_windows_compatibility.manifest")
endfunction()

add_executable(SnowDesktop WIN32
    ${SNOWDESKTOP_SOURCES}
    src/resources/resource.rc
    "${SNOWDESKTOP_EXE_VERSION_RESOURCE}"
)
source_group(TREE "${CMAKE_SOURCE_DIR}/src" PREFIX "Source" FILES
    ${SNOWDESKTOP_SOURCES}
    src/resources/resource.rc)
snowdesktop_enable_native_window_compatibility(SnowDesktop)

# Error messages must survive a missing or damaged distribution/lang tree.
# Keep lang/*.json as the only translation source, embedded at configure time.
file(GLOB SNOWDESKTOP_LAUNCHER_CATALOGS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lang/*.json")
set(SNOWDESKTOP_LAUNCHER_MESSAGES "#pragma once\nstruct LauncherMessage { const char* language; const wchar_t* message; const wchar_t* pending; const wchar_t* warning; const wchar_t* startup; const wchar_t* securitySoftwareHint; const wchar_t* startupPrivileges; const wchar_t* startAnyway; const wchar_t* cancel; };\ninline constexpr LauncherMessage kLauncherMessages[] = {\n")
foreach(catalog IN LISTS SNOWDESKTOP_LAUNCHER_CATALOGS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${catalog}")
    get_filename_component(language "${catalog}" NAME_WE)
    file(READ "${catalog}" catalog_json)
    string(JSON message GET "${catalog_json}" "app.launcher.failed")
    string(JSON pending GET "${catalog_json}" "app.launcher.pending")
    string(JSON warning GET "${catalog_json}" "app.launcher.warning")
    string(JSON startup GET "${catalog_json}" "app.operation.startFailed")
    string(JSON security_hint GET "${catalog_json}" "app.launcher.securitySoftwareHint")
    string(JSON startup_privileges GET "${catalog_json}" "app.startup.privilegesFailed")
    string(JSON start_anyway GET "${catalog_json}" "app.startup.startAnyway")
    string(JSON cancel GET "${catalog_json}" "settings.dialog.cancel")
    string(APPEND SNOWDESKTOP_LAUNCHER_MESSAGES "{\"${language}\", LR\"LAUNCHER(${message})LAUNCHER\", LR\"LAUNCHER(${pending})LAUNCHER\", LR\"LAUNCHER(${warning})LAUNCHER\", LR\"LAUNCHER(${startup})LAUNCHER\", LR\"LAUNCHER(${security_hint})LAUNCHER\", LR\"LAUNCHER(${startup_privileges})LAUNCHER\", LR\"LAUNCHER(${start_anyway})LAUNCHER\", LR\"LAUNCHER(${cancel})LAUNCHER\"},\n")
endforeach()
string(APPEND SNOWDESKTOP_LAUNCHER_MESSAGES "};\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_messages.h"
    CONTENT "${SNOWDESKTOP_LAUNCHER_MESSAGES}" @ONLY)

add_executable(SnowDesktopLauncher WIN32
    src/steam/steam_launcher_main.cpp
    src/steam/steam_runtime_context.cpp
    src/steam/steam_runtime_manager.cpp
    src/steam/steam_runtime_manager.h
    src/steam/steam_runtime_context.h
    src/resources/app_icon.rc
    "${SNOWDESKTOP_LAUNCHER_VERSION_RESOURCE}")
target_compile_features(SnowDesktopLauncher PRIVATE cxx_std_20)
target_include_directories(SnowDesktopLauncher PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(SnowDesktopLauncher PRIVATE bcrypt shell32 user32 ole32 oleaut32 advapi32 comctl32)
if (MSVC)
    target_link_options(SnowDesktopLauncher PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    set_property(TARGET SnowDesktopLauncher PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopLauncher PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(SnowDesktopLauncher PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(SnowDesktopLauncher PRIVATE
        -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktopLauncher PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Pin the complete WinUI toolchain on the executable so NuGet restore,
# C++/WinRT projection generation, XAML compilation, and self-contained
# deployment all happen in the CMake-generated MSBuild project.
set_property(TARGET SnowDesktop PROPERTY VS_PACKAGE_REFERENCES
    "Microsoft.WindowsAppSDK_${SNOWDESKTOP_WINDOWSAPPSDK_PACKAGE_VERSION};Microsoft.Windows.CppWinRT_${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}")
set_property(TARGET SnowDesktop PROPERTY VS_USER_PROPS
    "${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.WinUI.props")
set_property(TARGET SnowDesktop PROPERTY
    VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.19041.0")
set_target_properties(SnowDesktop PROPERTIES
    VS_GLOBAL_ROOTNAMESPACE "SnowDesktop"
    VS_GLOBAL_MSBuildProjectExtensionsPath "$(ProjectDir)obj\\$(MSBuildProjectName)\\"
    VS_GLOBAL_SnowDesktopSourceDirectory "${CMAKE_SOURCE_DIR}"
    VS_GLOBAL_SnowDesktopWindowsAppSdkPackageVersion
        "${SNOWDESKTOP_WINDOWSAPPSDK_PACKAGE_VERSION}"
    VS_GLOBAL_SnowDesktopCppWinRtPackageVersion
        "${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}"
    VS_GLOBAL_AppContainerApplication "false"
    VS_GLOBAL_AppxPackage "false"
    VS_GLOBAL_CppWinRTOptimized "true"
    VS_GLOBAL_CppWinRTRootNamespaceAutoMerge "true"
    VS_GLOBAL_EnablePreviewMsixTooling "false"
    # C++/WinRT's MSBuild targets concatenate this value with ProjectDir in
    # several places, so it must remain project-relative.
    VS_GLOBAL_GeneratedFilesDir "generated\\winui\\$(Configuration)\\"
    VS_GLOBAL_TargetPlatformMinVersion "10.0.19041.0"
    VS_GLOBAL_TargetPlatformVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    VS_GLOBAL_UseCrtSDKReferenceStaticWarning "false"
    VS_GLOBAL_UseWinUI "true"
    VS_GLOBAL_WindowsAppSDKSelfContained "true"
    VS_GLOBAL_WindowsAppSdkUndockedRegFreeWinRTInitialize "true"
    VS_GLOBAL_WindowsPackageType "None")
if (TARGET SnowDesktopTaskbarHook)
    add_dependencies(SnowDesktop SnowDesktopTaskbarHook)
endif()
if (TARGET SnowDesktopWallpaperHook)
    add_dependencies(SnowDesktop SnowDesktopWallpaperHook)
    target_compile_definitions(SnowDesktop PRIVATE
        SNOWDESKTOP_ENABLE_WALLPAPER_ENGINE_CAPTURE=1)
endif()

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
foreach(target SnowDesktopTaskbarHook SnowDesktopWallpaperHook SnowDesktopHookEntry SnowDesktopDockMinimizeHook)
    if(TARGET ${target})
        snowdesktop_private_runtime_target(${target} "${CMAKE_BINARY_DIR}/$<CONFIG>")
    endif()
endforeach()

target_compile_features(SnowDesktop PRIVATE cxx_std_20)
target_include_directories(SnowDesktop PRIVATE
    src
    "${CMAKE_BINARY_DIR}/generated"
    src/core
    src/app
    src/winui
    "${CMAKE_BINARY_DIR}/generated/xaml/$<CONFIG>"
)

if (MSVC)
    set_property(TARGET SnowDesktop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktop PRIVATE /MP${SNOWDESKTOP_COMPILE_JOBS} /bigobj /W4 /permissive- /EHsc /utf-8 /Z7)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00
        DISABLE_XAML_GENERATED_MAIN
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
        /DELAYLOAD:Microsoft.UI.Windowing.Core.dll
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
    webpdecoder SnowDesktopPuff
    ${SNOWDESKTOP_SYSTEM_CONTROL_LIBRARIES}
    SnowDesktopLua SnowDesktopEverything SnowDesktopPinyinData
    SnowDesktopSearchVisibility
    comctl32 d2d1 d3d11 dbghelp dcomp dwmapi dwrite dxgi dxguid gdi32 msimg32
    advapi32 bcrypt crypt32 delayimp imm32 iphlpapi ole32 oleacc oleaut32 pdh propsys runtimeobject shcore shell32 shlwapi uiautomationcore urlmon user32 windowsapp windowscodecs uuid winhttp ws2_32 xmllite
    version wbemuuid taskschd
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
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SnowDesktop>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/libwebp/COPYING" "$<TARGET_FILE_DIR:SnowDesktop>/licenses/libwebp-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/libwebp/PATENTS" "$<TARGET_FILE_DIR:SnowDesktop>/licenses/libwebp-PATENTS.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/puff/puff.h" "$<TARGET_FILE_DIR:SnowDesktop>/licenses/puff-LICENSE.txt")

set(SNOWDESKTOP_WIDGET_SKILL_MANIFEST
    "${CMAKE_SOURCE_DIR}/widgets/snowdesktop-lua-widget/skill.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SNOWDESKTOP_WIDGET_SKILL_MANIFEST}")
file(READ "${SNOWDESKTOP_WIDGET_SKILL_MANIFEST}"
    SNOWDESKTOP_WIDGET_SKILL_MANIFEST_JSON)
string(JSON SNOWDESKTOP_WIDGET_SKILL_REVISION GET
    "${SNOWDESKTOP_WIDGET_SKILL_MANIFEST_JSON}" revision)

add_executable(snowwidget
    tools/snowwidget/main.cpp
    tools/snowwidget/gpu_diagnostics.cpp
    src/widget/data/widget_gpu_sampler.cpp
    src/widget/authoring/widget_author_lint.cpp
    src/widget/authoring/widget_author_permissions.cpp
    src/widget/authoring/widget_author_test.cpp
    src/widget/api/widget_api_contract_json.cpp
    src/widget/api/widget_api_registry.cpp
    src/widget/runtime/widget_logical_slot.cpp
    src/widget/runtime/lua_runtime.cpp
    src/widget/packages/widget_package.cpp
    src/data/atomic_file.cpp
    src/widget/permissions/widget_permission_broker.cpp
    src/widget/permissions/widget_permission_state.cpp
    src/widget/api/widget_system_contract_json.cpp
    src/widget/api/widget_view_contract.cpp
    src/widget/api/widget_view_contract_json.cpp)
target_compile_features(snowwidget PRIVATE cxx_std_20)
snowdesktop_add_executable_version_resource(snowwidget "SnowDesktop widget authoring tool")
target_include_directories(snowwidget PRIVATE src)
target_link_libraries(snowwidget PRIVATE SnowDesktopLua SnowDesktopPerfetto bcrypt ole32 pdh dxgi)
target_compile_definitions(snowwidget PRIVATE
    SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
    SNOWDESKTOP_WIDGET_SKILL_REVISION=${SNOWDESKTOP_WIDGET_SKILL_REVISION})
if (MSVC)
    set_property(TARGET snowwidget PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(snowwidget PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(snowwidget PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(snowwidget PRIVATE -Wall -Wextra -pedantic)
endif()

# The Steam bridge is a separate MIT-licensed executable. It deliberately does
# not link SnowDesktop or any other GPL target from this build. Configure it
# after snowwidget so its Agent Skill bundle can embed the CLI.
add_subdirectory(steam_bridge)
target_link_libraries(SnowDesktop PRIVATE
    SnowDesktopWorkshopManagerSupport)

add_dependencies(SnowDesktop snowwidget SnowDesktopSteamBridge)
snowdesktop_add_product_build(SnowDesktopReleaseBuild SnowDesktop SnowDesktopLauncher
    SnowDesktopSteamBridge SnowDesktopWorkshopManager snowwidget)
snowdesktop_add_product_build(SnowDesktopDebugBuild SnowDesktop
    SnowDesktopSteamBridge SnowDesktopWorkshopManager)
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:snowwidget>"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/snowwidget.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SnowDesktopSteamBridge>"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/SnowDesktopSteamBridge.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/steam_bridge/LICENSE"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/SnowDesktopSteamBridge-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/steam_bridge/THIRD_PARTY_NOTICES.md"
        "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md"
    COMMENT "Embedding component authoring and Workshop CLIs in the Agent Skill...")
if (SNOWDESKTOP_STEAMWORKS_SDK_ROOT)
    add_custom_command(TARGET SnowDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:SnowDesktopSteamBridge>/steam_api64.dll"
            "$<TARGET_FILE_DIR:SnowDesktop>/widgets/snowdesktop-lua-widget/bin/steam_api64.dll"
        COMMENT "Embedding the permitted Steamworks runtime for the Agent Skill Workshop CLI...")
endif()

add_library(SnowDesktopTextInput STATIC src/ui/input/text_input_window.cpp src/ui/input/text_input_accessibility.cpp)
target_include_directories(SnowDesktopTextInput PUBLIC src)
target_compile_features(SnowDesktopTextInput PUBLIC cxx_std_20)
target_compile_definitions(SnowDesktopTextInput PRIVATE
    _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SnowDesktopTextInput PUBLIC d2d1 dwrite gdi32 imm32 user32 ole32 oleaut32 uuid uiautomationcore)
if (MSVC)
    set_property(TARGET SnowDesktopTextInput PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopTextInput PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(SnowDesktop PRIVATE SnowDesktopTextInput SnowDesktopPerfetto bcrypt)

include(CTest)
if (BUILD_TESTING)
    set(SNOWDESKTOP_TEST_TARGETS)
    set(SNOWDESKTOP_CORE_TEST_TARGETS)
    set(SNOWDESKTOP_FAST_TEST_TARGETS)
    set(SNOWDESKTOP_APP_TEST_TARGETS)
    set(SNOWDESKTOP_TOOL_TEST_TARGETS)

    function(snowdesktop_add_test TEST_TARGET TEST_NAME)
        set(multiValueArgs
            SOURCES
            INCLUDE_DIRECTORIES
            LINK_LIBRARIES
            COMMAND_ARGUMENTS
            LABELS)
        cmake_parse_arguments(
            SNOW_TEST "NATIVE_WINDOW_COMPATIBILITY;OBSERVE_RUNTIME_MODULES" "" "${multiValueArgs}" ${ARGN})

        if (NOT SNOW_TEST_SOURCES)
            message(FATAL_ERROR
                "${TEST_TARGET} must declare at least one source file")
        endif()

        add_executable(${TEST_TARGET} ${SNOW_TEST_SOURCES})
        target_link_libraries(${TEST_TARGET} PRIVATE SnowDesktopTextInput)
        foreach(probe_source IN LISTS SNOW_TEST_SOURCES)
            if(probe_source IN_LIST SNOWDESKTOP_PERFETTO_PROBE_SOURCES)
                target_link_libraries(${TEST_TARGET} PRIVATE SnowDesktopPerfetto)
                target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}/tests")
                break()
            endif()
        endforeach()
        if (SNOW_TEST_NATIVE_WINDOW_COMPATIBILITY)
            snowdesktop_enable_native_window_compatibility(${TEST_TARGET})
        endif()
        # Keep every CTest binary, symbol file, and import library outside the
        # runnable product root. Generator expressions prevent Visual Studio
        # from appending a second configuration directory.
        set_target_properties(${TEST_TARGET} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
            ARCHIVE_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
            LIBRARY_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
            PDB_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
            COMPILE_PDB_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
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

        if(SNOW_TEST_OBSERVE_RUNTIME_MODULES)
            if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
                get_target_property(existing_launcher ${TEST_TARGET} TEST_LAUNCHER)
                if(existing_launcher)
                    message(STATUS "Runtime module observation unavailable with an existing test launcher; evidence remains unclosed")
                else()
                    set_property(TARGET ${TEST_TARGET} PROPERTY TEST_LAUNCHER
                        "$<TARGET_FILE:SnowDesktopTestRuntimeObserver>;--test;${TEST_NAME};--")
                    add_dependencies(${TEST_TARGET} SnowDesktopTestRuntimeObserver)
                endif()
            else()
                message(STATUS "Runtime module observation unavailable before CMake 3.29; evidence remains unclosed")
            endif()
        endif()
        add_test(
            NAME ${TEST_NAME}
            COMMAND ${TEST_TARGET}
                ${SNOW_TEST_COMMAND_ARGUMENTS})
        set_tests_properties(
            ${TEST_NAME} PROPERTIES
            TIMEOUT 60
            REQUIRED_FILES "$<TARGET_FILE:${TEST_TARGET}>")
        if (SNOW_TEST_LABELS)
            set_tests_properties(
                ${TEST_NAME} PROPERTIES
                LABELS "${SNOW_TEST_LABELS}")
        endif()

        list(APPEND SNOWDESKTOP_TEST_TARGETS ${TEST_TARGET})
        set(SNOWDESKTOP_TEST_TARGETS
            "${SNOWDESKTOP_TEST_TARGETS}" PARENT_SCOPE)
        if ("tools" IN_LIST SNOW_TEST_LABELS)
            list(APPEND SNOWDESKTOP_TOOL_TEST_TARGETS ${TEST_TARGET})
            set(SNOWDESKTOP_TOOL_TEST_TARGETS
                "${SNOWDESKTOP_TOOL_TEST_TARGETS}" PARENT_SCOPE)
        else()
            list(APPEND SNOWDESKTOP_APP_TEST_TARGETS ${TEST_TARGET})
            set(SNOWDESKTOP_APP_TEST_TARGETS
                "${SNOWDESKTOP_APP_TEST_TARGETS}" PARENT_SCOPE)
        endif()
        if ("core" IN_LIST SNOW_TEST_LABELS AND NOT "tools" IN_LIST SNOW_TEST_LABELS)
            list(APPEND SNOWDESKTOP_CORE_TEST_TARGETS ${TEST_TARGET})
            set(SNOWDESKTOP_CORE_TEST_TARGETS
                "${SNOWDESKTOP_CORE_TEST_TARGETS}" PARENT_SCOPE)
        endif()
        if (NOT "integration" IN_LIST SNOW_TEST_LABELS AND NOT "tools" IN_LIST SNOW_TEST_LABELS)
            list(APPEND SNOWDESKTOP_FAST_TEST_TARGETS ${TEST_TARGET})
            set(SNOWDESKTOP_FAST_TEST_TARGETS
                "${SNOWDESKTOP_FAST_TEST_TARGETS}" PARENT_SCOPE)
        endif()
    endfunction()

    snowdesktop_add_test(
        SnowDesktopApplicationDataLifecycleTests
        application_data_lifecycle
        SOURCES
            tests/application_data_lifecycle_tests.cpp
            tests/desktop/desktop_membership_recovery_tests.cpp
            tests/data/data_cleanup_tests.cpp
            src/data/data_cleanup.cpp
            src/data/atomic_file.cpp
            src/data/full_data_backup.cpp
            src/data/layout_storage.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/widget/packages/widget_package.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
            src/data/portable_data_migration.cpp
            src/platform/single_instance.cpp
            src/diagnostics/debug_profile.cpp
            src/steam/steam_runtime_context.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt gdi32 ole32 shell32 user32 version windowscodecs
        LABELS core integration data)

    snowdesktop_add_test(
        SnowDesktopSingleInstanceTests
        single_instance
        SOURCES
            tests/single_instance_tests.cpp
            src/data/atomic_file.cpp
            src/platform/single_instance.cpp
            src/diagnostics/debug_profile.cpp
            src/steam/steam_runtime_context.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 user32 version
        LABELS core contract integration deployment steam)

    snowdesktop_add_test(
        SnowDesktopForceRestartHotkeyTests
        force_restart_hotkey
        SOURCES tests/platform/force_restart_hotkey_tests.cpp
            src/platform/force_restart_hotkey.cpp
            src/platform/single_instance.cpp
            src/data/atomic_file.cpp
            src/diagnostics/debug_profile.cpp
            src/steam/steam_runtime_context.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 user32 version
        LABELS contract integration input settings restart)

    snowdesktop_add_test(
        SnowDesktopHttpSecurityTests
        http_security
        SOURCES
            tests/http_security_tests.cpp
            src/platform/http_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES shlwapi winhttp ws2_32
        LABELS core contract security)

    snowdesktop_add_test(
        SnowDesktopInjectableRuntimeCacheTests
        injectable_runtime_cache
        SOURCES tests/platform/injectable_runtime_cache_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt
        LABELS core contract deployment)

    snowdesktop_add_test(
        SnowDesktopHookRegistrationRetryTests
        hook_registration_retry
        SOURCES tests/platform/hook_registration_retry_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS core rules)

    snowdesktop_add_test(
        SnowDesktopTrayResidentTests
        tray_resident_reconnect
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/system/tray_resident_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktopTaskbarHook>"
        LABELS core integration tray)
    add_dependencies(SnowDesktopTrayResidentTests SnowDesktopTaskbarHook)

    # The default command validates only the observer on its own hidden HWND.
    # Live Shell scenarios require explicit command-line selection; they never
    # launch the desktop host or change saved application/Wallpaper settings.
    snowdesktop_add_test(
        SnowDesktopWallpaperShellProbeTests
        wallpaper_shell_probe
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/shell/wallpaper_shell_probe.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32 dwmapi shell32
        COMMAND_ARGUMENTS --self-test
        LABELS integration shell manual)
    add_dependencies(SnowDesktopWallpaperShellProbeTests SnowDesktopTaskbarHook)

    # Self-contained reproduction tool for sharing with wallpaper developers.
    # The default test only exercises its own hidden observer window.
    snowdesktop_add_test(
        SnowDesktopWallpaperMinimalReproTests
        wallpaper_minimal_repro
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/shell/wallpaper_minimal_repro.cpp
        LINK_LIBRARIES user32
        COMMAND_ARGUMENTS --self-test
        LABELS integration shell manual)

    foreach(marker 1 2)
        add_library(SnowDesktopHookEntryFixture${marker} SHARED tests/platform/hook_entry_fixture.cpp)
        target_compile_features(SnowDesktopHookEntryFixture${marker} PRIVATE cxx_std_20)
        target_compile_definitions(SnowDesktopHookEntryFixture${marker} PRIVATE HOOK_ENTRY_FIXTURE_MARKER=${marker} UNICODE _UNICODE)
        target_compile_options(SnowDesktopHookEntryFixture${marker} PRIVATE /W4 /EHsc /utf-8)
        target_link_libraries(SnowDesktopHookEntryFixture${marker} PRIVATE user32)
        set_property(TARGET SnowDesktopHookEntryFixture${marker} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        set_target_properties(SnowDesktopHookEntryFixture${marker} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    endforeach()
    snowdesktop_add_test(
        SnowDesktopHookEntryTests
        hook_stable_entry
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/platform/hook_entry_tests.cpp src/platform/hook_entry_deployment.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32 bcrypt
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktopHookEntry>"
            "$<TARGET_FILE:SnowDesktopHookEntryFixture1>" "$<TARGET_FILE:SnowDesktopHookEntryFixture2>"
        LABELS core integration deployment)
    add_dependencies(SnowDesktopHookEntryTests SnowDesktopHookEntry SnowDesktopHookEntryFixture1 SnowDesktopHookEntryFixture2)
    add_dependencies(SnowDesktopHookEntryTests SnowDesktopDockMinimize32)
    add_test(NAME hook_stable_entry32 COMMAND SnowDesktopHookEntryTests --run32
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryTests32.exe"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/SnowDesktop.Runtime/SnowDesktopHookEntry32.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_1.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_2.dll")
    set_tests_properties(hook_stable_entry32 PROPERTIES LABELS "core;integration;deployment" TIMEOUT 30
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopHookEntryTests>")
    add_test(NAME hook_stable_entry_cross32 COMMAND SnowDesktopHookEntryTests --cross32
        "${CMAKE_BINARY_DIR}/$<CONFIG>/SnowDesktop.Runtime/SnowDesktopHookEntry32.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_1.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_2.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryTests32.exe")
    set_tests_properties(hook_stable_entry_cross32 PROPERTIES LABELS "core;integration;deployment" TIMEOUT 30
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopHookEntryTests>")

    snowdesktop_add_test(
        SnowDesktopShellOverlayWindowTests
        shell_overlay_window
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/platform/shell_overlay_window_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32 dwmapi
        LABELS core integration window)

    add_test(NAME http_network_integration
        COMMAND $<TARGET_FILE:SnowDesktopHttpSecurityTests> --network)
    set_tests_properties(http_network_integration PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopHttpSecurityTests>"
        LABELS "integration;widget;network;security")

    snowdesktop_add_test(
        SnowDesktopLuaRuntimeTests
        lua_runtime
        SOURCES
            tests/lua_runtime_tests.cpp
            src/widget/runtime/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS core contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetApiRegistryTests
        widget_api_registry
        SOURCES
            tests/widget_api_registry_tests.cpp
            src/widget/api/widget_api_contract_json.cpp
            src/widget/api/widget_api_registry.cpp
            src/widget/permissions/widget_permission_state.cpp
            src/widget/api/widget_system_contract_json.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS core contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetL10nFormatTests
        widget_l10n_format
        SOURCES
            tests/widget_l10n_format_tests.cpp
            src/widget/runtime/widget_l10n_format.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract l10n lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetTimeTests
        widget_time
        SOURCES
            tests/widget_time_tests.cpp
            src/widget/runtime/widget_time.cpp
        INCLUDE_DIRECTORIES src
        LABELS integration contract l10n lua time widget)

    snowdesktop_add_test(
        SnowDesktopWidgetLocationTaskExecutorTests
        widget_location_task_executor
        SOURCES tests/widget_location_task_executor_tests.cpp
            src/widget/tasks/widget_location_task_executor.cpp src/widget/tasks/widget_task_broker.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES windowsapp user32
        LABELS contract permission widget location)

    snowdesktop_add_test(
        SnowDesktopWidgetRuntimeDiagnosticsTests
        widget_runtime_diagnostics
        SOURCES
            tests/widget_runtime_diagnostics_tests.cpp
            tests/performance_capture_tests.cpp
            tests/diagnostics/perfetto_session_tests.cpp
            tests/diagnostics/performance_workload_tests.cpp
            tests/diagnostics/process_resource_statistics_tests.cpp
            src/widget/runtime/widget_runtime_diagnostics.cpp
            src/diagnostics/performance_capture.cpp
            src/diagnostics/performance_process_resources.cpp
            src/diagnostics/performance_analysis.cpp
            src/diagnostics/performance_environment.cpp
            src/diagnostics/performance_control_pipe.cpp
            src/widget/runtime/lua_runtime.cpp
            src/widget/tasks/widget_task_broker.cpp
            src/widget/tasks/widget_filesystem_task_executor.cpp
            src/ui/render/ui_animation_scheduler.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua SnowDesktopPerfetto bcrypt advapi32 pdh psapi user32 shell32 ole32 windowscodecs
        LABELS contract widget)
    target_compile_definitions(SnowDesktopWidgetRuntimeDiagnosticsTests PRIVATE
        SNOWDESKTOP_PERFORMANCE_SOURCE_ROOT=L"${CMAKE_SOURCE_DIR}")

    snowdesktop_add_test(
        SnowDesktopWidgetPreviewContextTests
        widget_preview_context
        SOURCES
            tests/widget_preview_context_tests.cpp
            src/widget/preview/widget_preview_context.cpp
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
            src/widget/data/widget_data_broker.cpp
            src/widget/data/widget_calendar_event_query.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract data lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetDataSubscribeCallbackTests
        widget_data_subscribe_callback
        SOURCES
            tests/widget/widget_data_subscribe_callback_tests.cpp
            src/widget/api/widget_calendar_query_options.cpp
            src/widget/runtime/lua_runtime.cpp
            src/widget/data/widget_data_broker.cpp
            src/widget/data/widget_calendar_event_query.cpp
            src/system/calendar/calendar_service.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua ole32
        LABELS contract data lua widget lifecycle)

    snowdesktop_add_test(
        SnowDesktopWidgetTaskBrokerTests
        widget_task_broker
        SOURCES
            tests/widget_task_broker_tests.cpp
            src/widget/tasks/widget_task_broker.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lifecycle task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetNotificationRuntimeTests
        widget_notification_runtime
        SOURCES
            tests/widget_notification_runtime_tests.cpp
            src/widget/tasks/widget_notification_runtime.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lifecycle task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetNotificationScheduleStoreTests
        widget_notification_schedule_store
        SOURCES
            tests/widget_notification_schedule_store_tests.cpp
            src/widget/tasks/widget_notification_schedule_store.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lifecycle storage task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetMediaTaskExecutorTests
        widget_media_task_executor
        SOURCES
            tests/widget_media_task_executor_tests.cpp
            src/widget/tasks/widget_media_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES windowsapp ole32
        LABELS integration media task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAudioOutputTaskExecutorTests
        widget_audio_output_task_executor
        SOURCES
            tests/widget_audio_output_task_executor_tests.cpp
            src/widget/tasks/widget_audio_output_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32
        LABELS contract audio task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAudioPlaybackTests
        widget_audio_playback
        COMMAND_ARGUMENTS "${CMAKE_SOURCE_DIR}/developer_assets/workshop_widgets/aurora-player/modules/local_queue.lua"
        SOURCES
            tests/widget/widget_audio_playback_tests.cpp
            src/widget/tasks/widget_audio_playback.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua windowsapp shcore ole32
        LABELS contract audio task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetClipboardTaskExecutorTests
        widget_clipboard_task_executor
        SOURCES
            tests/widget_clipboard_task_executor_tests.cpp
            tests/widget/clipboard_platform_contract_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES gdi32 ole32 shell32 windowscodecs
        LABELS contract clipboard task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemHandleStoreTests
        widget_filesystem_handle_store
        SOURCES
            tests/widget_filesystem_handle_store_tests.cpp
            tests/drag_drop/lua_file_delivery_cases.cpp
            src/widget/tasks/widget_filesystem_handle_store.cpp
            src/widget/view/widget_interaction_region.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
            src/widget/runtime/widget_lua_lifecycle.cpp
            src/widget/runtime/lua_runtime.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt SnowDesktopLua
        LABELS contract filesystem security task lua widget)

    add_test(NAME lua_file_drop_delivery
        COMMAND SnowDesktopWidgetFilesystemHandleStoreTests --file-drop-delivery-tests)
    set_tests_properties(lua_file_drop_delivery PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWidgetFilesystemHandleStoreTests>"
        TIMEOUT 90 LABELS "integration;filesystem;security;interaction;lua;widget")

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemTaskExecutorTests
        widget_filesystem_task_executor
        SOURCES
            tests/widget_filesystem_task_executor_tests.cpp
            src/widget/tasks/widget_filesystem_task_executor.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 windowscodecs
        LABELS contract filesystem security task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemWatchServiceTests
        widget_filesystem_watch_service
        SOURCES
            tests/widget_filesystem_watch_service_tests.cpp
            src/widget/tasks/widget_filesystem_watch_service.cpp
            src/widget/tasks/widget_filesystem_watch_origin.cpp
            src/widget/tasks/widget_filesystem_handle_store.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt
        LABELS integration filesystem security data lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAppTaskExecutorTests
        widget_app_task_executor
        SOURCES
            tests/widget_app_task_executor_tests.cpp
            src/widget/tasks/widget_app_task_executor.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract app task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSystemSettingsTests
        widget_system_settings
        SOURCES
            tests/widget_system_settings_tests.cpp
            src/widget/settings/widget_system_settings.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract system task lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSystemDataProviderTests
        widget_system_data_provider
        SOURCES
            tests/widget_system_data_provider_tests.cpp
            tests/system_controls_tests.cpp
            src/widget/tasks/widget_system_control_tasks.cpp
            src/widget/data/widget_system_data_provider.cpp
            src/widget/data/widget_gpu_sampler.cpp
            ${SNOWDESKTOP_SYSTEM_CONTROL_SOURCES}
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ${SNOWDESKTOP_SYSTEM_CONTROL_LIBRARIES} advapi32 dxgi dxguid iphlpapi ole32 pdh propsys runtimeobject shcore user32 windowsapp windowscodecs
        LABELS contract data system widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAudioAnalysisProviderTests
        widget_audio_analysis_provider
        SOURCES
            tests/widget_audio_analysis_provider_tests.cpp
            src/widget/data/widget_audio_analysis_provider.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32
        LABELS contract data audio widget)

    snowdesktop_add_test(
        SnowDesktopWidgetCompositionLayerRulesTests
        widget_composition_layer_rules
        OBSERVE_RUNTIME_MODULES
        SOURCES
            tests/widget_composition_layer_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS ${CMAKE_SOURCE_DIR}
        LABELS contract render widget)

    snowdesktop_add_test(
        SnowDesktopWidgetPackageImageCacheTests
        widget_package_image_cache
        SOURCES
            tests/widget_package_image_cache_tests.cpp
            src/widget/packages/widget_package_image_cache.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 windowscodecs
        LABELS contract resource render widget)

    snowdesktop_add_test(
        SnowDesktopWidgetLuaLifecycleTests
        widget_lua_lifecycle
        SOURCES
            tests/widget_lua_lifecycle_tests.cpp
            src/widget/runtime/widget_lua_lifecycle.cpp
            src/widget/runtime/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS contract lifecycle lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetRuntimeSchedulerTests
        widget_runtime_scheduler
        SOURCES
            tests/widget_runtime_scheduler_tests.cpp
            src/widget/runtime/widget_runtime_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetPermissionStateTests
        widget_permission_state
        SOURCES
            tests/widget_permission_state_tests.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt
        LABELS core contract security widget)

    snowdesktop_add_test(
        SnowDesktopApplicationRestartPolicyTests
        application_restart_policy
        SOURCES
            tests/application_restart_policy_tests.cpp
            src/platform/application_crash_watchdog.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES advapi32 user32
        LABELS contract recovery)

    snowdesktop_add_test(
        SnowDesktopDirectoryChangeWatchTests
        directory_change_watch
        SOURCES tests/shell/directory_change_watch_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract integration shell)

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
            src/system/calendar/calendar_service.cpp
    src/system/calendar/calendar_display.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS service)

    snowdesktop_add_test(
        SnowDesktopLocalizationContractTests
        localization_contract
        SOURCES
            tests/localization_contract_tests.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS core contract l10n)

    snowdesktop_add_test(
        SnowDesktopDiagnosticLogTests
        diagnostic_log
        SOURCES
            tests/diagnostics/diagnostic_log_tests.cpp
            src/diagnostics/diagnostic_log.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract diagnostics integration)

    snowdesktop_add_test(
        SnowDesktopGeneralSettingsTests
        general_settings
        SOURCES
            tests/general_settings_tests.cpp
            tests/theme_library_tests.cpp
            src/theme/theme_library.cpp
            src/settings/general_settings.cpp
            src/data/atomic_file.cpp
            src/ui/render/app_font.cpp
            src/theme/personalization.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES dwrite d2d1 windowscodecs gdi32 ole32
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS core contract settings)

    snowdesktop_add_test(
        SnowDesktopStatusBarVisibilityTests
        status_bar_visibility
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/system/status_bar_visibility_tests.cpp
            src/system/status_bar/status_bar_view.cpp
            src/widget/preview/widget_preview_stage.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES d2d1 d3d11 dcomp dwrite dxgi dxguid gdi32 ole32 user32 windowscodecs
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract render settings status-bar)

    snowdesktop_add_test(
        SnowDesktopThemeWorkflowTests
        theme_workflow
        SOURCES
            tests/theme_preview_tests.cpp
            tests/theme_workshop_tests.cpp
            src/icons/icon_beautify.cpp
            src/theme/theme_preview.cpp
            src/theme/theme_workshop.cpp
            src/ui/preview/preview_png_writer.cpp
            src/widget/preview/widget_preview_stage.cpp
        INCLUDE_DIRECTORIES src steam_bridge/src
        LINK_LIBRARIES SnowDesktopWorkshopManagerSupport d2d1 d3d11 dxguid windowscodecs ole32 shlwapi
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktop>"
        LABELS contract integration settings steam sdk-free host-runtime)
    set_tests_properties(theme_workflow PROPERTIES TIMEOUT 180)
    add_dependencies(SnowDesktopThemeWorkflowTests SnowDesktop)

    snowdesktop_add_test(
        SnowDesktopDemoModeRulesTests
        demo_mode_rules
        OBSERVE_RUNTIME_MODULES
        SOURCES tests/demo_mode_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract settings render)

    snowdesktop_add_test(
        SnowDesktopSettingsWindowOpenRulesTests
        settings_window_open_rules
        SOURCES tests/settings_window_open_rules_tests.cpp
            src/platform/usage_guide.cpp src/data/atomic_file.cpp src/settings/settings_route.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract settings)

    snowdesktop_add_test(
        SnowDesktopDeploymentPackagingContractTests
        deployment_packaging_contract
        SOURCES tests/deployment_packaging_contract_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS build contract packaging winui)

    add_executable(SnowDesktopSteamEntitlementBridgeFixture
        tests/steam_entitlement_bridge_fixture.cpp)
    set_target_properties(SnowDesktopSteamEntitlementBridgeFixture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    target_compile_features(
        SnowDesktopSteamEntitlementBridgeFixture PRIVATE cxx_std_20)
    target_compile_definitions(SnowDesktopSteamEntitlementBridgeFixture PRIVATE
        SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
    if (MSVC)
        set_property(TARGET SnowDesktopSteamEntitlementBridgeFixture PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(SnowDesktopSteamEntitlementBridgeFixture PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopSteamEntitlementBridgeFixture PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    snowdesktop_add_test(
        SnowDesktopSteamEntitlementTests
        steam_entitlement
        SOURCES
            tests/steam_entitlement_tests.cpp
            src/data/atomic_file.cpp
            src/steam/steam_entitlement.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES crypt32
        COMMAND_ARGUMENTS
            "$<TARGET_FILE:SnowDesktopSteamEntitlementBridgeFixture>"
        LABELS core contract security steam)
    target_compile_definitions(SnowDesktopSteamEntitlementTests PRIVATE
        SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")
    add_dependencies(SnowDesktopSteamEntitlementTests
        SnowDesktopSteamEntitlementBridgeFixture)

    snowdesktop_add_test(
        SnowDesktopLauncherIconResourceTests
        launcher_icon_resource
        SOURCES tests/launcher_icon_resource_tests.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktopLauncher>"
        LABELS build contract deployment shell steam)
    add_dependencies(SnowDesktopLauncherIconResourceTests
        SnowDesktopLauncher)

    snowdesktop_add_test(
        SnowDesktopSteamRuntimeUpdateTests
        steam_runtime_update
        SOURCES
            tests/steam_runtime_update_tests.cpp
            src/steam/steam_runtime_context.cpp
            src/steam/steam_runtime_manager.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktopLauncher>"
        LINK_LIBRARIES bcrypt shell32
        LABELS contract deployment integration steam update)
    add_dependencies(SnowDesktopSteamRuntimeUpdateTests SnowDesktopLauncher)

    snowdesktop_add_test(
        SnowDesktopSettingsControllerTests
        settings_controller
        SOURCES
            tests/settings_controller_tests.cpp
            tests/settings_ipc_tests.cpp
            src/settings/settings_ipc_channel.cpp
            src/settings/settings_ipc_services.cpp
            src/settings/settings_ipc_widget_service.cpp
            src/settings/settings_process.cpp
            src/widget/settings/widget_setting_rules.cpp
            src/settings/settings_controller.cpp
            src/settings/settings_route.cpp
        INCLUDE_DIRECTORIES src
        LABELS core contract settings)

    snowdesktop_add_test(
        SnowDesktopSettingsHostActionsSemanticsTests
        settings_host_actions_semantics
        SOURCES tests/settings_host_actions_semantics_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract dock integration settings taskbar)

    snowdesktop_add_test(
        SnowDesktopSettingsSearchIndexTests
        settings_search_index
        SOURCES
            tests/settings_search_index_tests.cpp
            src/settings/settings_search_index.cpp
            src/settings/settings_route.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS core contract l10n navigation settings)

    snowdesktop_add_test(
        SnowDesktopWinUiSettingsNavigationTests
        winui_settings_navigation
        SOURCES
            tests/winui_settings_navigation_tests.cpp
            src/winui/settings_shell_navigation.cpp
            src/settings/settings_route.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract navigation settings ui winui)

    snowdesktop_add_test(
        SnowDesktopHotkeyRecorderRulesTests
        hotkey_recorder_rules
        SOURCES tests/hotkey_recorder_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract hotkey input settings ui winui)

    snowdesktop_add_test(
        SnowDesktopWinUiPresenterBoundaryTests
        winui_personalization_page_presenter
        SOURCES tests/winui_presenter_boundary_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}" personalization
        LABELS contract personalization settings ui winui)

    add_test(NAME winui_desktop_page_presenter
        COMMAND $<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>
            "${CMAKE_CURRENT_SOURCE_DIR}" desktop)
    set_tests_properties(winui_desktop_page_presenter PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>"
        LABELS "contract;desktop;personalization;settings;ui;winui")

    add_test(NAME winui_dock_page_presenter
        COMMAND $<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>
            "${CMAKE_CURRENT_SOURCE_DIR}" dock)
    set_tests_properties(winui_dock_page_presenter PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>"
        LABELS "contract;dock;settings;taskbar;ui;winui")

    add_test(NAME winui_home_about_page_presenter
        COMMAND $<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>
            "${CMAKE_CURRENT_SOURCE_DIR}" home_about)
    set_tests_properties(winui_home_about_page_presenter PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>"
        LABELS "about;backup;contract;settings;ui;update;winui")

    snowdesktop_add_test(
        SnowDesktopSettingsUpdateRulesTests
        settings_update_rules
        SOURCES tests/settings_update_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract settings update)

    snowdesktop_add_test(
        SnowDesktopAutoStartRulesTests
        auto_start_rules
        SOURCES tests/auto_start_rules_tests.cpp
            tests/auto_start_manager_tests.cpp
            src/platform/auto_start_manager.cpp
            src/platform/auto_start_elevation.cpp
        INCLUDE_DIRECTORIES src "${CMAKE_CURRENT_BINARY_DIR}/generated"
        LINK_LIBRARIES taskschd ole32 oleaut32 advapi32 shell32 comctl32
        LABELS contract rules settings startup)
    if (MSVC)
        target_link_options(SnowDesktopAutoStartRulesTests PRIVATE
            "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    endif()

    add_test(NAME auto_start_scheduler
        COMMAND $<TARGET_FILE:SnowDesktopAutoStartRulesTests> --scheduler)
    set_tests_properties(auto_start_scheduler PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopAutoStartRulesTests>"
        LABELS "integration;settings;startup")

    add_test(NAME winui_widget_settings_presenter
        COMMAND $<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>
            "${CMAKE_CURRENT_SOURCE_DIR}" widget_settings)
    set_tests_properties(winui_widget_settings_presenter PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>"
        LABELS "contract;lua;security;settings;storage;ui;widget;winui")

    add_test(NAME winui_widgets_page_presenter
        COMMAND $<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>
            "${CMAKE_CURRENT_SOURCE_DIR}" widgets)
    set_tests_properties(winui_widgets_page_presenter PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>"
        LABELS "contract;package;permission;settings;steam;ui;widget;winui")

    add_test(NAME winui_backup_data_page_presenter
        COMMAND $<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>
            "${CMAKE_CURRENT_SOURCE_DIR}" backup_data)
    set_tests_properties(winui_backup_data_page_presenter PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWinUiPresenterBoundaryTests>"
        LABELS "backup;contract;data;migration;recovery;settings;ui;winui")

    snowdesktop_add_test(
        SnowDesktopWinUiBackupDataPageBackendTests
        winui_backup_data_page_backend
        SOURCES tests/backup_data_page_backend_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS backup contract data migration recovery settings ui winui)

    snowdesktop_add_test(
        SnowDesktopWinUiWidgetsPageBackendTests
        winui_widgets_page_backend
        SOURCES tests/widgets_page_backend_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract package permission settings steam ui widget winui)

    snowdesktop_add_test(
        SnowDesktopWinUiSettingsWindowHostTests
        winui_settings_window_host
        SOURCES tests/winui_settings_window_host_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract lifecycle navigation settings ui winui)

    snowdesktop_add_test(
        SnowDesktopSettingsNoImguiContractTests
        settings_no_imgui_contract
        SOURCES tests/settings_no_imgui_contract_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS build contract lua settings steam ui widget winui)

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
            tests/drag_drop/drop_staging_tests.cpp
            src/drag_drop/external_drop_content.cpp
            src/resources/app_icon.rc
            src/app/drag_drop/drag_drop_controller.cpp
            src/app/drag_drop/ole_drag_drop_adapter.cpp
            src/app/tray/tray_icon_controller.cpp
            src/core/drag_target_resolver.cpp
            src/core/slot.cpp
            src/core/container.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src src/core
        LINK_LIBRARIES advapi32 d2d1 ole32 shell32 user32 uuid
        LABELS core contract interaction slot integration shell)

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
            src/widget/view/widget_interaction_region.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetDrawGeometryTests
        widget_draw_geometry
        SOURCES
            tests/widget_draw_geometry_tests.cpp
            src/widget/view/widget_draw_geometry.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract render lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetViewAccessibilityTests
        widget_view_accessibility
        SOURCES
            tests/widget_view_accessibility_tests.cpp
            src/widget/view/widget_view_accessibility.cpp
            src/widget/api/widget_view_contract.cpp
            src/widget/view/widget_view_tree.cpp
            src/widget/view/widget_view_lua.cpp
            src/widget/view/widget_interaction_region.cpp
            src/widget/runtime/widget_logical_slot.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS accessibility contract layout lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAccessibilityProviderTests
        widget_accessibility_provider
        SOURCES
            tests/widget_accessibility_provider_tests.cpp
            src/widget/view/widget_accessibility_events.cpp
            src/widget/view/widget_accessibility_provider.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 oleaut32 uiautomationcore user32
        LABELS accessibility contract integration widget)

    snowdesktop_add_test(
        SnowDesktopWidgetViewContractTests
        widget_view_contract
        SOURCES
            tests/widget_view_contract_tests.cpp
            src/widget/api/widget_view_contract.cpp
        INCLUDE_DIRECTORIES src
        LABELS core contract layout lua widget)
    target_sources(SnowDesktopWidgetViewContractTests PRIVATE
        src/widget/api/widget_view_contract_json.cpp)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorLintTests
        widget_author_lint
        SOURCES
            tests/widget_author_lint_tests.cpp
            src/widget/authoring/widget_author_lint.cpp
            src/widget/api/widget_api_registry.cpp
            src/widget/api/widget_view_contract.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS authoring contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorTestRunnerTests
        widget_author_test_runner
        SOURCES
            tests/widget_author_test_tests.cpp
            src/widget/authoring/widget_author_test.cpp
            src/widget/runtime/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS authoring contract lua security widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorToolsTests
        widget_author_tools
        SOURCES
            tests/widget_author_tools_tests.cpp
            tests/widget_package_review_tests.cpp
            tools/snowwidget/gpu_diagnostics.cpp
            src/widget/data/widget_gpu_sampler.cpp
            src/widget/authoring/widget_author_permissions.cpp
            src/widget/api/widget_api_registry.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/widget/packages/widget_package.cpp
            src/data/atomic_file.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
        INCLUDE_DIRECTORIES src tools/snowwidget
        LINK_LIBRARIES SnowDesktopLua bcrypt ole32 pdh dxgi
        LABELS authoring contract data permission security widget)

    snowdesktop_add_test(
        SnowDesktopWidgetNativeHelperTests
        widget_native_helper
        SOURCES
            tests/widget/native_helper_tests.cpp
            src/widget/tasks/widget_native_task_executor.cpp
            src/widget/packages/widget_package.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src tests
        LINK_LIBRARIES bcrypt ole32
        LABELS integration security permission widget)

    # Keep file-export validation independent from installation-review fixtures.
    add_test(NAME widget_package_file_export
        COMMAND SnowDesktopWidgetAuthorToolsTests --package-export)
    set_tests_properties(widget_package_file_export PROPERTIES
        TIMEOUT 30 LABELS "authoring;contract;data;security;widget"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWidgetAuthorToolsTests>")

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorProfileCliTests
        widget_author_profile_cli
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/widget/authoring/widget_author_profile_cli_tests.cpp
        INCLUDE_DIRECTORIES src tests
        LINK_LIBRARIES SnowDesktopBuildToolTestSupport
        COMMAND_ARGUMENTS "$<TARGET_FILE:snowwidget>" "$<TARGET_FILE:SnowDesktop>"
        LABELS authoring integration performance widget host-runtime)
    add_dependencies(SnowDesktopWidgetAuthorProfileCliTests snowwidget SnowDesktop)
    set_tests_properties(widget_author_profile_cli PROPERTIES TIMEOUT 150)
    set_target_properties(SnowDesktopWidgetAuthorProfileCliTests PROPERTIES VS_GLOBAL_UseCommonOutputDirectory "true")

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorPreviewCliTests
        widget_author_preview_cli
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/widget_author_preview_cli_tests.cpp
            tests/widget/authoring/widget_doc_recipe_tests.h
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 windowscodecs
        COMMAND_ARGUMENTS
            "$<TARGET_FILE:snowwidget>"
            "$<TARGET_FILE:SnowDesktop>"
            "${CMAKE_CURRENT_SOURCE_DIR}" native
        LABELS authoring integration lua preview render widget host-runtime)
    add_dependencies(SnowDesktopWidgetAuthorPreviewCliTests
        snowwidget SnowDesktop)
    # Both tests invoke the original host/tool paths supplied above. Copying
    # their referenced outputs into tests/ is unnecessary and makes parallel
    # MSBuild projects contend for the same SnowDesktop.pdb destination.
    set_target_properties(
        SnowDesktopThemeWorkflowTests SnowDesktopWidgetAuthorPreviewCliTests
        PROPERTIES VS_GLOBAL_UseCommonOutputDirectory "true")
    # One executable, five independently bounded scenes. Every child deadline
    # is below its 80-second suite budget and CTest's 90-second outer watchdog.
    set_tests_properties(widget_author_preview_cli PROPERTIES TIMEOUT 90)
    foreach(preview_suite background materials runtime validation)
        add_test(NAME widget_author_preview_cli_${preview_suite}
            COMMAND SnowDesktopWidgetAuthorPreviewCliTests
                "$<TARGET_FILE:snowwidget>" "$<TARGET_FILE:SnowDesktop>"
                "${CMAKE_CURRENT_SOURCE_DIR}" ${preview_suite})
        set_tests_properties(widget_author_preview_cli_${preview_suite} PROPERTIES
            TIMEOUT 90 LABELS "authoring;integration;lua;preview;render;widget;host-runtime"
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWidgetAuthorPreviewCliTests>")
    endforeach()

    snowdesktop_add_test(
        SnowDesktopWidgetViewTreeTests
        widget_view_tree
        SOURCES
            tests/widget_view_tree_tests.cpp
            src/widget/api/widget_view_contract.cpp
            src/widget/view/widget_view_tree.cpp
            src/widget/view/widget_view_lua.cpp
            src/widget/view/widget_interaction_region.cpp
            src/widget/runtime/widget_logical_slot.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS contract interaction layout lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetTextInputRulesTests
        widget_text_input_rules
        SOURCES
            tests/widget_text_input_rules_tests.cpp
            src/widget/view/widget_text_input_rules.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES dwrite
        LABELS contract interaction lua widget)

    snowdesktop_add_test(
        SnowDesktopTextInputWindowTests
        text_input_window
        SOURCES
            tests/text_input_window_tests.cpp
            tests/ui/text_input_clipboard_tests.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS ${CMAKE_SOURCE_DIR}
        LABELS core contract interaction accessibility security)

    snowdesktop_add_test(
        SnowDesktopWidgetStorageTransactionTests
        widget_storage_transaction
        SOURCES
            tests/widget_storage_transaction_tests.cpp
            src/widget/storage/widget_storage_transaction.cpp
            src/widget/storage/widget_storage_value.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetStorageWriteBudgetTests
        widget_storage_write_budget
        SOURCES
            tests/widget_storage_write_budget_tests.cpp
            src/widget/storage/widget_storage_write_budget.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSettingRulesTests
        widget_setting_rules
        SOURCES
            tests/widget_setting_rules_tests.cpp
            src/widget/settings/widget_setting_rules.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua settings widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSettingsModelTests
        widget_settings_model
        SOURCES
            tests/widget_settings_model_tests.cpp
            src/widget/settings/widget_setting_rules.cpp
        INCLUDE_DIRECTORIES src
        LABELS core contract lua settings widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSettingsServiceTests
        widget_settings_service
        SOURCES
            tests/widget_settings_service_tests.cpp
            src/widget/settings/widget_settings_service.cpp
            src/widget/settings/widget_setting_rules.cpp
        INCLUDE_DIRECTORIES src
        LABELS core contract lua security settings storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetEngineSettingsBackendTests
        widget_engine_settings_backend
        SOURCES
            tests/widget_engine_settings_backend_tests.cpp
            tests/widget/widget_host_control_projection_tests.cpp
            src/widget/api/widget_view_contract.cpp
            src/widget/view/widget_view_tree.cpp
            src/widget/view/widget_view_lua.cpp
            src/widget/view/widget_interaction_region.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/widget/view/widget_view_accessibility.cpp
            src/widget/settings/widget_setting_rules.cpp
            src/widget/storage/widget_storage_value.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract lua security settings storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSecretStoreTests
        widget_secret_store
        SOURCES
            tests/widget_secret_store_tests.cpp
            tests/widget/secret_store_legacy_fixture.cpp
            tests/widget/secret_store_legacy_fixture.h
            src/widget/storage/widget_secret_store.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt crypt32
        LABELS contract lua security storage widget)

    snowdesktop_add_test(
        SnowDesktopCommunityComponentRuntimeTests
        community_component_runtime
        SOURCES
            tests/widget/community_component_runtime_tests.cpp
            src/widget/runtime/widget_lua_lifecycle.cpp
            src/widget/runtime/lua_runtime.cpp
            src/widget/runtime/widget_runtime_scheduler.cpp
            src/widget/storage/widget_storage_transaction.cpp
            src/widget/storage/widget_storage_value.cpp
            src/widget/storage/widget_storage_write_budget.cpp
            src/widget/data/widget_calendar_event_query.cpp
            src/widget/api/widget_calendar_query_options.cpp
            src/system/calendar/calendar_service.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua ole32
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract lua calendar widget community)

    snowdesktop_add_test(
        SnowDesktopBuiltinWidgetSourceContractTests
        builtin_widget_source_contract
        SOURCES
            tests/builtin_widget_source_contract_tests.cpp
            tests/widget/rss_reader_tests.cpp
            tests/widget/builtin_interaction_timer_tests.cpp
            src/system/calendar/calendar_service.cpp
            src/widget/runtime/widget_runtime_scheduler.cpp
            src/widget/runtime/widget_lua_lifecycle.cpp
            src/widget/view/widget_interaction_region.cpp
            src/widget/runtime/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua ole32
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS core contract lua render storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetLogicalSlotTests
        widget_logical_slot
        SOURCES
            tests/widget_logical_slot_tests.cpp
            src/widget/runtime/widget_logical_slot.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopLuaLogicalSlotContainerTests
        lua_logical_slot_container
        SOURCES
            tests/lua_logical_slot_container_tests.cpp
            tests/drag_drop/lua_ingress_cases.cpp
            src/widgets/lua_logical_slot.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/core/slot.cpp
            src/core/container.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src src/core third_party/lua
        LINK_LIBRARIES d2d1 user32
        LABELS contract interaction lua slot widget)
    add_test(NAME lua_file_ingress_matrix
        COMMAND SnowDesktopLuaLogicalSlotContainerTests --file-ingress-tests)
    set_tests_properties(lua_file_ingress_matrix PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopLuaLogicalSlotContainerTests>"
        TIMEOUT 30 LABELS "integration;interaction;lua;slot;storage")

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
        SnowDesktopDesktopNamespaceRegistryTests
        desktop_namespace_registry
        SOURCES
            tests/desktop_namespace_registry_tests.cpp
            src/desktop/desktop_namespace_registry.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES advapi32 ole32 shell32 shlwapi
        LABELS core rules shell)

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
            src/navigation/navigation_settings.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopPinyinData
        LABELS rules navigation)

    snowdesktop_add_test(
        SnowDesktopQuickNavigationSearchAsyncTests
        quick_navigation_search_async
        SOURCES
            tests/quick_navigation_search_async_tests.cpp
            src/navigation/quick_navigation_search_async.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES
            user32
        LABELS rules navigation integration)

    snowdesktop_add_test(
        SnowDesktopShortcutApplicationRulesTests
        shortcut_application_rules
        SOURCES
            tests/shortcut_application_rules_tests.cpp
            tests/website_icon_tests.cpp
            src/icons/shortcut_icon_resource.cpp
            src/icons/website_icon.cpp
            src/platform/http_runtime.cpp
            src/data/atomic_file.cpp
            src/ui/preview/preview_png_writer.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES advapi32 ole32 shell32 shlwapi uuid windowscodecs winhttp ws2_32 xmllite
        LABELS rules shell)

    snowdesktop_add_test(
        SnowDesktopDesktopKeyboardRulesTests
        desktop_keyboard_rules
        SOURCES
            tests/desktop_keyboard_rules_tests.cpp
            tests/navigation/type_to_select_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopPinyinData
        LABELS contract interaction shell)

    snowdesktop_add_test(
        SnowDesktopIconRenderRulesTests
        icon_render_rules
        SOURCES
            tests/icon_render_rules_tests.cpp
            tests/large_icon_assets_tests.cpp
            tests/large_icon_rendering_tests.cpp
            src/icons/shortcut_icon_resource.cpp
            src/icons/large_icon_renderer.cpp
            src/icons/large_icon_assets.cpp
            src/icons/icon_image_codec.cpp
            src/icons/icon_package.cpp
            src/icons/icon_library.cpp
            src/icons/icon_pack_archive.cpp
            src/icons/android_icon_pack.cpp
            tests/icons/icon_library_tests.cpp
            tests/icons/icon_library_bitmap_cache_tests.cpp
            tests/icons/icon_pack_tests.cpp
            tests/icons/icon_workshop_storage_tests.cpp
            tests/icons/icon_workshop_workflow_tests.cpp
            src/icons/icon_workshop.cpp
            src/theme/theme_preview.cpp
            src/widget/preview/widget_preview_stage.cpp
            src/steam/steam_workshop_cache.cpp
            src/icons/icon_beautify.cpp
            src/ui/preview/preview_png_writer.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopWorkshopManagerSupport webp SnowDesktopPuff bcrypt ole32 shell32 shlwapi uuid windowscodecs gdi32 advapi32 user32 d2d1 dwrite d3d11 dxguid xmllite
        LABELS rules shell ui)

    add_test(NAME large_icon_shell_assets
        COMMAND SnowDesktopIconRenderRulesTests --large-icon-shell)
    set_tests_properties(large_icon_shell_assets PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopIconRenderRulesTests>"
        TIMEOUT 60 LABELS "integration;shell;icon;resource")

    add_test(NAME icon_library COMMAND SnowDesktopIconRenderRulesTests --icon-library)
    set_tests_properties(icon_library PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopIconRenderRulesTests>"
        TIMEOUT 90 LABELS "integration;icon;resource;rendering")

    add_test(NAME large_icon_rendering
        COMMAND SnowDesktopIconRenderRulesTests --large-icon-rendering)
    set_tests_properties(large_icon_rendering PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopIconRenderRulesTests>"
        TIMEOUT 60 LABELS "integration;icon;ui;rendering")

    snowdesktop_add_test(
        SnowDesktopDesktopPreviewRenderingTests
        desktop_preview_rendering
        SOURCES
            tests/drag_drop/desktop_preview_rendering_tests.cpp
            src/icons/large_icon_renderer.cpp
            src/ui/preview/preview_png_writer.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 uuid windowscodecs d2d1 dwrite dxguid
        LABELS integration icon interaction rendering)

    # Execute real native containers and DesktopApp routing without Run(),
    # desktop HWND creation or desktop automation. The virtual methods reference the
    # host graph, so link its production sources rather than fake container
    # methods. Keep XAML generation and runtime deployment in separate trees.
    set(SNOWDESKTOP_NATIVE_DRAG_SOURCES ${SNOWDESKTOP_SOURCES})
    list(REMOVE_ITEM SNOWDESKTOP_NATIVE_DRAG_SOURCES src/main.cpp)
    get_property(SNOWDESKTOP_TESTS_BEFORE_NATIVE_DRAG DIRECTORY PROPERTY TESTS)
    snowdesktop_add_test(
        SnowDesktopNativeDragIntegrationTests
        native_drag_target_integration
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/drag_drop/native_drag_integration_tests.cpp
            ${SNOWDESKTOP_NATIVE_DRAG_SOURCES}
        LABELS integration interaction slot widget)
    foreach(PROPERTY IN ITEMS
            INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
            PRECOMPILE_HEADERS LINK_LIBRARIES LINK_OPTIONS
            VS_PACKAGE_REFERENCES VS_USER_PROPS
            VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION
            VS_GLOBAL_ROOTNAMESPACE VS_GLOBAL_SnowDesktopSourceDirectory
            VS_GLOBAL_SnowDesktopWindowsAppSdkPackageVersion
            VS_GLOBAL_SnowDesktopCppWinRtPackageVersion
            VS_GLOBAL_AppContainerApplication VS_GLOBAL_AppxPackage
            VS_GLOBAL_CppWinRTOptimized VS_GLOBAL_CppWinRTRootNamespaceAutoMerge
            VS_GLOBAL_EnablePreviewMsixTooling
            VS_GLOBAL_TargetPlatformMinVersion VS_GLOBAL_TargetPlatformVersion
            VS_GLOBAL_UseCrtSDKReferenceStaticWarning VS_GLOBAL_UseWinUI
            VS_GLOBAL_WindowsAppSDKSelfContained
            VS_GLOBAL_WindowsAppSdkUndockedRegFreeWinRTInitialize
            VS_GLOBAL_WindowsPackageType)
        get_target_property(HOST_VALUE SnowDesktop ${PROPERTY})
        if (NOT HOST_VALUE STREQUAL "HOST_VALUE-NOTFOUND")
            string(REPLACE "generated/xaml/" "generated/native-drag-host/xaml/"
                HOST_VALUE "${HOST_VALUE}")
            set_property(TARGET SnowDesktopNativeDragIntegrationTests
                PROPERTY ${PROPERTY} "${HOST_VALUE}")
        endif()
    endforeach()
    set_target_properties(SnowDesktopNativeDragIntegrationTests PROPERTIES
        VS_GLOBAL_MSBuildProjectExtensionsPath "$(ProjectDir)obj\\$(MSBuildProjectName)\\"
        VS_GLOBAL_GeneratedFilesDir "generated\\native-drag-host\\winui\\$(Configuration)\\"
        VS_GLOBAL_XamlGeneratedOutputPath
            "$(ProjectDir)generated\\native-drag-host\\xaml\\$(Configuration)\\"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host")
    target_include_directories(SnowDesktopNativeDragIntegrationTests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    set_tests_properties(native_drag_target_integration PROPERTIES TIMEOUT 90)
    add_test(NAME native_drag_commit_storage
        COMMAND SnowDesktopNativeDragIntegrationTests --commit-storage)
    set_tests_properties(native_drag_commit_storage PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data")
    add_test(NAME native_async_layout_save
        COMMAND SnowDesktopNativeDragIntegrationTests --async-layout-save)
    set_tests_properties(native_async_layout_save PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data")
    add_test(NAME native_drag_reload_storage
        COMMAND SnowDesktopNativeDragIntegrationTests --reload-storage)
    set_tests_properties(native_drag_reload_storage PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data")
    add_test(NAME native_drag_async_reload
        COMMAND SnowDesktopNativeDragIntegrationTests --async-reload-storage)
    set_tests_properties(native_drag_async_reload PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    foreach(GESTURE IN ITEMS copy move alt_link ctrl_shift_link)
        add_test(NAME native_ole_files_${GESTURE}
            COMMAND SnowDesktopNativeDragIntegrationTests --ole-files ${GESTURE})
        set_tests_properties(native_ole_files_${GESTURE} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;data;shell;ole")
        add_test(NAME native_ole_gesture_files_${GESTURE}
            COMMAND SnowDesktopNativeDragIntegrationTests --ole-gesture-files ${GESTURE})
        set_tests_properties(native_ole_gesture_files_${GESTURE} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;data;shell;ole")
    endforeach()

    add_test(NAME native_ole_leave_lifecycle
        COMMAND SnowDesktopNativeDragIntegrationTests --ole-leave-lifecycle)
    set_tests_properties(native_ole_leave_lifecycle PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;data;shell;ole")
    foreach(RIGHT_DECISION IN ITEMS move copy link cancel dismiss)
        add_test(NAME native_right_drag_files_${RIGHT_DECISION}
            COMMAND SnowDesktopNativeDragIntegrationTests --right-drag-files ${RIGHT_DECISION})
        set_tests_properties(native_right_drag_files_${RIGHT_DECISION} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;data;shell;ole")
    endforeach()

    foreach(MEMBER_TYPE IN ITEMS collection files mapping)
        add_test(NAME native_member_order_${MEMBER_TYPE}
            COMMAND SnowDesktopNativeDragIntegrationTests --member-order ${MEMBER_TYPE})
        set_tests_properties(native_member_order_${MEMBER_TYPE} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    endforeach()
    foreach(INGRESS_TARGET IN ITEMS collection files mapping collection_group file_group_files file_group_mapping)
        foreach(INGRESS_VERB IN ITEMS copy move alt_link ctrl_shift_link right_move right_copy right_link right_cancel right_dismiss)
            add_test(NAME native_ole_widget_${INGRESS_TARGET}_${INGRESS_VERB}
                COMMAND SnowDesktopNativeDragIntegrationTests --ole-widget-files ${INGRESS_TARGET} ${INGRESS_VERB})
            set_tests_properties(native_ole_widget_${INGRESS_TARGET}_${INGRESS_VERB} PROPERTIES
                REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
                TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell;ole")
        endforeach()
    endforeach()
    foreach(DEFAULT_TARGET IN ITEMS desktop collection files mapping collection_group file_group_files file_group_mapping)
        foreach(VOLUME_RELATION IN ITEMS same cross mixed)
            add_test(NAME native_default_files_${DEFAULT_TARGET}_${VOLUME_RELATION}
                COMMAND SnowDesktopNativeDragIntegrationTests --ole-default-files ${DEFAULT_TARGET} ${VOLUME_RELATION})
            set_tests_properties(native_default_files_${DEFAULT_TARGET}_${VOLUME_RELATION} PROPERTIES
                REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
                TIMEOUT 180 SKIP_RETURN_CODE 77
                LABELS "integration;shell;interaction;slot;ole;data")
        endforeach()
    endforeach()
    foreach(DOCK_VERB IN ITEMS alt_link right_link right_cancel right_dismiss)
        add_test(NAME native_ole_dock_folders_${DOCK_VERB}
            COMMAND SnowDesktopNativeDragIntegrationTests --ole-dock-folders ${DOCK_VERB})
        set_tests_properties(native_ole_dock_folders_${DOCK_VERB} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 180
            LABELS "integration;shell;interaction;slot;ole;data;dock")
    endforeach()
    add_test(NAME native_folder_delivery_after_drag
        COMMAND SnowDesktopNativeDragIntegrationTests --folder-delivery-after-drag)
    set_tests_properties(native_folder_delivery_after_drag PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 30 LABELS "integration;interaction;slot;widget;data;shell")
    foreach(MIXED_TARGET IN ITEMS collection files)
        add_test(NAME native_mixed_selection_${MIXED_TARGET}
            COMMAND SnowDesktopNativeDragIntegrationTests --mixed-selection ${MIXED_TARGET})
        set_tests_properties(native_mixed_selection_${MIXED_TARGET} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
        foreach(MIXED_VERB IN ITEMS copy link)
            add_test(NAME native_mixed_file_${MIXED_VERB}_${MIXED_TARGET}
                COMMAND SnowDesktopNativeDragIntegrationTests --mixed-file-action ${MIXED_TARGET} ${MIXED_VERB})
            set_tests_properties(native_mixed_file_${MIXED_VERB}_${MIXED_TARGET} PROPERTIES
                REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
                TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
        endforeach()
    endforeach()
    add_test(NAME native_group_member_order
        COMMAND SnowDesktopNativeDragIntegrationTests --group-member-order)
    set_tests_properties(native_group_member_order PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    foreach(GROUP_MEMBER_TYPE IN ITEMS files mapping)
        add_test(NAME native_file_group_member_order_${GROUP_MEMBER_TYPE}
            COMMAND SnowDesktopNativeDragIntegrationTests --group-member-order ${GROUP_MEMBER_TYPE})
        set_tests_properties(native_file_group_member_order_${GROUP_MEMBER_TYPE} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    endforeach()

    add_test(NAME native_widget_pair_storage
        COMMAND SnowDesktopNativeDragIntegrationTests --widget-pair-storage)
    add_test(NAME native_group_lifecycle_storage
        COMMAND SnowDesktopNativeDragIntegrationTests --group-lifecycle-storage)
    add_test(NAME native_dock_file_returns
        COMMAND SnowDesktopNativeDragIntegrationTests --dock-file-returns)
    add_test(NAME native_right_menu_capture_storage
        COMMAND SnowDesktopNativeDragIntegrationTests --right-menu-capture)
    set_tests_properties(native_right_menu_capture_storage PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    set_tests_properties(native_dock_file_returns PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell;dock")
    set_tests_properties(native_widget_pair_storage native_group_lifecycle_storage PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;dock")

    foreach(GROUP_SURFACE_MODE IN ITEMS labels search date inactive)
        add_test(NAME native_group_surface_${GROUP_SURFACE_MODE}
            COMMAND SnowDesktopNativeDragIntegrationTests --group-surfaces ${GROUP_SURFACE_MODE})
        set_tests_properties(native_group_surface_${GROUP_SURFACE_MODE} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    endforeach()
    add_test(NAME native_self_ole_return
        COMMAND SnowDesktopNativeDragIntegrationTests --ole-return)
    add_test(NAME native_ole_completion_sequences
        COMMAND SnowDesktopNativeDragIntegrationTests --ole-sequences)
    add_test(NAME native_dock_generated_relations
        COMMAND SnowDesktopNativeDragIntegrationTests --dock-generated-relations)
    set_tests_properties(native_self_ole_return native_ole_completion_sequences native_dock_generated_relations PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell;dock")
    foreach(CONTENT_TARGET IN ITEMS desktop collection mapping dock)
        add_test(NAME native_owned_content_${CONTENT_TARGET}
            COMMAND SnowDesktopNativeDragIntegrationTests --owned-content ${CONTENT_TARGET})
        add_test(NAME native_owned_content_cancel_${CONTENT_TARGET}
            COMMAND SnowDesktopNativeDragIntegrationTests --owned-content-cancel ${CONTENT_TARGET})
        set_tests_properties(native_owned_content_${CONTENT_TARGET} native_owned_content_cancel_${CONTENT_TARGET} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell;dock")
    endforeach()

    foreach(GROUP_CONTENT_SURFACE IN ITEMS collection files mapping collection_group file_group_files file_group_mapping)
        add_test(NAME native_group_type_${GROUP_CONTENT_SURFACE}
            COMMAND SnowDesktopNativeDragIntegrationTests --group-type ${GROUP_CONTENT_SURFACE})
        set_tests_properties(native_group_type_${GROUP_CONTENT_SURFACE} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
        if(NOT GROUP_CONTENT_SURFACE STREQUAL "collection")
            add_test(NAME native_group_filter_${GROUP_CONTENT_SURFACE}
                COMMAND SnowDesktopNativeDragIntegrationTests --group-filter ${GROUP_CONTENT_SURFACE})
            set_tests_properties(native_group_filter_${GROUP_CONTENT_SURFACE} PROPERTIES
                REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
                TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
        endif()
    endforeach()
    foreach(NATIVE_ADDITIONAL_CASE IN ITEMS
        "native_group_search_type|--group-type-search"
        "native_group_tab_dwell|--group-dwell"
        "native_popup_request_lifecycle|--popup-request"
        "native_external_dwell_restart|--external-dwell"
        "native_ole_target_changes|--ole-target-changes"
        "native_ole_partial_batch|--ole-partial-batch"
        "native_dock_partition_storage|--dock-partitions"
        "native_dock_removal_animation|--dock-removal"
        "native_dock_running_pin_storage|--dock-running-pin-storage"
        "native_shortcut_final_targets|--shortcut-targets"
        "native_folder_shortcut_handoff|--folder-shortcut-handoff"
        "native_large_icon_storage_return|--large-icon-storage-return"
        "native_namespace_mapping_rejection|--namespace-rejection")
        string(REPLACE "|" ";" NATIVE_CASE_PARTS "${NATIVE_ADDITIONAL_CASE}")
        list(GET NATIVE_CASE_PARTS 0 NATIVE_CASE_NAME)
        list(GET NATIVE_CASE_PARTS 1 NATIVE_CASE_MODE)
        add_test(NAME ${NATIVE_CASE_NAME} COMMAND SnowDesktopNativeDragIntegrationTests ${NATIVE_CASE_MODE})
        set_tests_properties(${NATIVE_CASE_NAME} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 90 LABELS "integration;interaction;slot;widget;data;shell")
    endforeach()

    add_test(NAME native_icon_customization
        COMMAND SnowDesktopNativeDragIntegrationTests --icon-customization)
    set_tests_properties(native_icon_customization PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;icon;data;rendering")

    add_test(NAME native_list_title_hover
        COMMAND SnowDesktopNativeDragIntegrationTests --list-title-hover)
    add_test(NAME native_list_title_font_isolation
        COMMAND SnowDesktopNativeDragIntegrationTests --list-title-fonts)
    add_test(NAME native_popup_display_menu
        COMMAND SnowDesktopNativeDragIntegrationTests --popup-display-menu)
    add_test(NAME native_input_safety
        COMMAND SnowDesktopNativeDragIntegrationTests --input-safety)
    set_tests_properties(native_list_title_hover native_list_title_font_isolation native_popup_display_menu PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 90 LABELS "integration;interaction;widget;rendering")
    set_tests_properties(native_input_safety PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
        TIMEOUT 60 LABELS "integration;interaction;rendering")

    # Every mode in this fixture uses its own process, GUID storage/profile and
    # message-only HWND, except input safety uses native windows on an inactive
    # private Win32 desktop. None starts the desktop host or clipboard service;
    # Shell file operations touch only owned paths.
    # Derive the set from registrations so new modes inherit this isolation
    # contract without maintaining another list of test names.
    get_property(SNOWDESKTOP_NATIVE_DRAG_TESTS DIRECTORY PROPERTY TESTS)
    list(REMOVE_ITEM SNOWDESKTOP_NATIVE_DRAG_TESTS ${SNOWDESKTOP_TESTS_BEFORE_NATIVE_DRAG})
    set_property(TEST ${SNOWDESKTOP_NATIVE_DRAG_TESTS} APPEND PROPERTY LABELS isolated-native-model)

    add_test(NAME icon_bitmap_rendering
        COMMAND SnowDesktopIconRenderRulesTests --icon-bitmap-rendering)
    set_tests_properties(icon_bitmap_rendering PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopIconRenderRulesTests>"
        TIMEOUT 60 LABELS "integration;icon;ui;rendering")

    snowdesktop_add_test(
        SnowDesktopIconBeautifyTests
        icon_beautify
        SOURCES
            tests/icon_beautify_tests.cpp
            tests/icons/icon_backplate_tests.h
            tests/icons/icon_appearance_tests.cpp
            src/icons/icon_beautify.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules icon ui)

    snowdesktop_add_test(
        SnowDesktopItemLocationTests
        item_location
        SOURCES
            tests/item_location_tests.cpp
            src/layout/item_location.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 shlwapi uuid
        LABELS integration shell)

    snowdesktop_add_test(
        SnowDesktopVirtualFileDropTests
        virtual_file_drop
        SOURCES
            tests/virtual_file_drop_tests.cpp
            src/drag_drop/virtual_file_drop.cpp
            src/drag_drop/external_drop_content.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 user32
        LABELS contract interaction shell)

    snowdesktop_add_test(
        SnowDesktopUrlDropResourceTests
        url_drop_resource
        SOURCES
            tests/url_drop_resource_tests.cpp
            src/drag_drop/url_drop_resource.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction security)

    snowdesktop_add_test(
        SnowDesktopDropDataUrlTests
        drop_data_url
        SOURCES
            tests/drop_data_url_tests.cpp
            src/drag_drop/drop_data_url.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction security)

    snowdesktop_add_test(
        SnowDesktopDropTextRulesTests
        drop_text_rules
        SOURCES
            tests/drop_text_rules_tests.cpp
            src/drag_drop/drop_text_rules.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction security)

    snowdesktop_add_test(
        SnowDesktopDropImageDataTests
        drop_image_data
        SOURCES
            tests/drop_image_data_tests.cpp
            src/drag_drop/drop_image_data.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES gdi32 ole32 user32 windowscodecs
        LABELS contract interaction security)

    snowdesktop_add_test(
        SnowDesktopShellContextMenuInvokeTests
        shell_context_menu_invoke
        SOURCES tests/shell_context_menu_invoke_tests.cpp
            tests/shell/shell_catalogue_registry_handle_tests.cpp
            tests/shell/shell_context_menu_bootstrap_tests.cpp
            src/shell/shell_extension_menu.cpp
            src/shell/shell_extension_menu_cache.cpp
    src/shell/shell_extension_catalogue.cpp
    src/shell/shell_extension_service.cpp
            src/shell/shell_context_menu_site.cpp
            src/settings/settings_process.cpp
            src/settings/settings_ipc_channel.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 oleaut32 shell32 shlwapi user32 gdi32 uuid version dwmapi
        LABELS integration contract shell)

    # Keep slow inventory boundaries and real helper lifetimes independent of
    # the ordinary menu cases; every entry retains the shell resource lock below.
    add_test(NAME shell_menu_loading_deadlines
        COMMAND $<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests> --test-menu-loading-deadlines)
    add_test(NAME shell_extension_session_lifetime
        COMMAND $<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests> --test-extension-sessions)
    add_test(NAME shell_context_menu_bootstrap
        COMMAND $<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests> --test-menu-bootstrap)
    add_test(NAME shell_menu_refresh_control
        COMMAND $<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests> --test-menu-refresh)
    add_test(NAME shell_registry_catalogue
        COMMAND $<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests> --test-registry-catalogue)
    add_test(NAME shell_shortcut_query_recovery
        COMMAND $<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests> --test-shortcut-query-recovery)
    set_tests_properties(shell_menu_loading_deadlines shell_extension_session_lifetime shell_context_menu_bootstrap
        shell_menu_refresh_control shell_registry_catalogue shell_shortcut_query_recovery PROPERTIES
        TIMEOUT 60
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopShellContextMenuInvokeTests>"
        LABELS "integration;contract;shell")

    snowdesktop_add_test(
        SnowDesktopShellIntegrationContractTests
        shell_integration_contract
        SOURCES tests/shell_integration_contract_tests.cpp
        COMMAND_ARGUMENTS ${CMAKE_SOURCE_DIR}
        LABELS core contract shell settings)

    snowdesktop_add_test(
        SnowDesktopShellLaunchWorkerTests
        shell_launch_worker
        SOURCES
            tests/shell_launch_worker_tests.cpp
            src/shell/shell_launch_worker.cpp
            src/shell/shell_launch_process.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES advapi32 ole32 oleaut32 shell32 shlwapi user32 uuid
        LABELS integration shell interaction)

    snowdesktop_add_test(
        SnowDesktopShellFileOperationWorkerTests
        shell_file_operation_worker
        SOURCES
            tests/shell_file_operation_worker_tests.cpp
            src/app/drag_drop/drag_drop_controller.cpp
            src/core/drag_target_resolver.cpp
            src/core/container.cpp
            src/core/slot.cpp
            src/common/l10n.cpp
            src/layout/item_location.cpp
            tests/shell/shell_file_operation_worker_boundary.cpp
            src/drag_drop/external_drop_content.cpp
            src/data/layout_storage.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES ole32 shell32 shlwapi uuid netapi32 windowscodecs
        LABELS integration shell interaction)
    set_tests_properties(shell_file_operation_worker PROPERTIES
        SKIP_RETURN_CODE 77 LABELS "integration;shell;interaction;manual")
    add_test(NAME shell_folder_link_requests
        COMMAND SnowDesktopShellFileOperationWorkerTests --folder-link-request-tests)
    add_test(NAME shell_allowed_effect_requests
        COMMAND SnowDesktopShellFileOperationWorkerTests --allowed-effect-request-tests)
    add_test(NAME shell_async_slot_transfers
        COMMAND SnowDesktopShellFileOperationWorkerTests --async-slot-transfer-tests)
    add_test(NAME shell_drop_placement_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --drop-placement-tests)
    add_test(NAME shell_member_order_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --member-order-tests)
    add_test(NAME shell_right_drag_files
        COMMAND SnowDesktopShellFileOperationWorkerTests --right-drag-file-tests)
    add_test(NAME shell_drop_pipeline_decisions
        COMMAND SnowDesktopShellFileOperationWorkerTests --pipeline-decision-tests)
    add_test(NAME shell_group_membership_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --group-membership-tests)
    add_test(NAME shell_group_release_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --group-release-tests)
    add_test(NAME shell_widget_displacement_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --widget-displacement-tests)
    add_test(NAME shell_dock_model_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --dock-model-tests)
    add_test(NAME shell_dock_return_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --dock-return-tests)
    add_test(NAME shell_dock_file_actions
        COMMAND SnowDesktopShellFileOperationWorkerTests --dock-file-action-tests)
    add_test(NAME shell_page_mapping_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --page-mapping-tests)
    add_test(NAME shell_internal_desktop_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --internal-desktop-tests)
    add_test(NAME shell_file_page_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --file-page-tests)
    add_test(NAME shell_selected_desktop_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --selected-desktop-tests)
    add_test(NAME shell_desktop_request_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --desktop-request-tests)
    add_test(NAME shell_file_pointer_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --file-page-tests pointer)
    add_test(NAME shell_desktop_preview_cache_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --desktop-preview-cache-tests)
    add_test(NAME shell_file_preview_cache_storage
        COMMAND SnowDesktopShellFileOperationWorkerTests --file-page-tests cache)
    set_tests_properties(shell_folder_link_requests shell_allowed_effect_requests shell_async_slot_transfers shell_drop_placement_storage shell_member_order_storage shell_right_drag_files shell_drop_pipeline_decisions shell_group_membership_storage shell_group_release_storage shell_widget_displacement_storage shell_dock_model_storage shell_dock_return_storage shell_dock_file_actions shell_page_mapping_storage shell_internal_desktop_storage shell_file_page_storage shell_selected_desktop_storage shell_desktop_request_storage shell_file_pointer_storage shell_desktop_preview_cache_storage shell_file_preview_cache_storage PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopShellFileOperationWorkerTests>"
        TIMEOUT 90 LABELS "integration;shell;interaction;storage")
    add_test(NAME shell_file_operation_worker_network_preflight
        COMMAND SnowDesktopShellFileOperationWorkerTests --network-preflight-tests)
    set_tests_properties(shell_file_operation_worker_network_preflight PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopShellFileOperationWorkerTests>"
        TIMEOUT 20 LABELS "rules;shell;test-infrastructure")
    add_test(NAME shell_file_operation_feedback
        COMMAND SnowDesktopShellFileOperationWorkerTests --failure-feedback-tests)
    set_tests_properties(shell_file_operation_feedback PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopShellFileOperationWorkerTests>"
        TIMEOUT 20 LABELS "core;shell;storage")
    add_test(NAME shell_file_operation_path_alias
        COMMAND SnowDesktopShellFileOperationWorkerTests --tracked-path-alias-tests)
    set_tests_properties(shell_file_operation_path_alias PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopShellFileOperationWorkerTests>"
        TIMEOUT 90 LABELS "integration;shell;interaction;storage")
    # The automatic preflight regression shares the manual integration binary.
    # A cold fast build still needs it, although it never runs the integration.
    list(APPEND SNOWDESKTOP_FAST_TEST_TARGETS SnowDesktopShellFileOperationWorkerTests)

    snowdesktop_add_test(
        SnowDesktopDockAndWindowRulesTests
        dock_and_window_rules
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/dock_and_window_rules_tests.cpp
            tests/dock/start_menu_tests.cpp
            tests/desktop/explorer_icon_visibility_tests.cpp
            tests/dock/dock_window_pin_tests.cpp
            tests/dock/fullscreen_focus_tests.cpp
            tests/taskbar_native_tests.cpp
            tests/taskbar_symbol_resolver_tests.cpp
            src/taskbar_hook/taskbar_symbol_resolver.cpp
            src/taskbar_hook/taskbar_native.cpp
            src/taskbar_hook/taskbar_classic_surface.cpp
            ${MINHOOK64_SOURCES}
            tests/desktop_backdrop_compositor_tests.cpp
            src/data/atomic_file.cpp
            src/ui/render/native_tooltip.cpp
            src/theme/personalization.cpp
            tests/tray_model_tests.cpp
            tests/system/status_bar_input_method_profiles_cases.h
            tests/tray_live_tests.cpp
            src/system/tray/tray_model.cpp
            src/system/tray/tray_service.cpp
            src/system/tray/tray_menu_placement.cpp
            src/app/render/desktop_backdrop_compositor.cpp
            src/icons/icon_beautify.cpp
            src/common/l10n.cpp
            src/dock/dock_window_preview.cpp
            src/dock/dock_window_pin.cpp
            src/dock/dock_window_transition.cpp
            src/ui/render/ui_animation_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES advapi32 comctl32 d2d1 d3d11 dbghelp dcomp dwrite dwmapi dxgi dxguid gdi32 imm32 ole32 oleaut32 runtimeobject shell32 shcore shlwapi uiautomationcore user32 uuid windowsapp winhttp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS integration core rules window)
    target_include_directories(SnowDesktopDockAndWindowRulesTests PRIVATE third_party/minhook/include)
    snowdesktop_add_test(
        SnowDesktopDesktopLayerRecoveryTests
        desktop_layer_recovery
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/desktop/desktop_layer_recovery_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32
        LABELS integration core desktop window)
    snowdesktop_add_test(
        SnowDesktopDockWorkAreaReservationTests
        dock_work_area_reservation
        SOURCES tests/dock/dock_work_area_reservation_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32
        LABELS core dock rules)
    snowdesktop_add_test(
        SnowDesktopDockDocumentProxyTests
        dock_document_proxy
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/dock/dock_document_proxy_tests.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32
        LABELS integration core dock window)
    snowdesktop_add_test(
        SnowDesktopDockHookResourceTests
        dock_hook_resources
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/dock/dock_hook_resource_tests.cpp
            tests/dock/dock_minimize_target_cases.h
            src/dock/dock_external_minimize.cpp
            src/dock/dock_cloak_dispatcher.cpp
            src/dock/dock_minimize_hook.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32 dwmapi
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktopDockMinimizeHook>"
        LABELS integration core dock window)
    add_dependencies(SnowDesktopDockHookResourceTests SnowDesktopDockMinimizeHook)
    snowdesktop_add_test(
        SnowDesktopDockGenieRenderTests
        dock_genie_render
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/dock/dock_genie_render_tests.cpp
            src/dock/dock_window_transition.cpp src/ui/render/ui_animation_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES d2d1 d3d11 dxgi dcomp dwmapi ole32 user32 gdi32
        LABELS integration core dock window)
    add_dependencies(SnowDesktopDockAndWindowRulesTests SnowDesktopTaskbarHook)
    # Explicit opt-in: this real-Shell diagnostic registers only its own fixture
    # icon and broadcasts TaskbarCreated, never launching the desktop host.
    add_test(NAME tray_live_integration
        COMMAND SnowDesktopDockAndWindowRulesTests --tray-live "$<TARGET_FILE:SnowDesktopTaskbarHook>")
    set_tests_properties(tray_live_integration PROPERTIES
        TIMEOUT 90 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopDockAndWindowRulesTests>"
        LABELS "integration;shell;tray;manual")
    add_test(NAME tray_bootstrap_integration
        COMMAND SnowDesktopDockAndWindowRulesTests --tray-classic "$<TARGET_FILE:SnowDesktopTaskbarHook>")
    set_tests_properties(tray_bootstrap_integration PROPERTIES
        TIMEOUT 90 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopDockAndWindowRulesTests>"
        LABELS "integration;shell;tray;manual")

    snowdesktop_add_test(
        SnowDesktopDesktopDropCacheTests
        desktop_drop_cache
        SOURCES tests/desktop_drop_cache_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract interaction layout)

    snowdesktop_add_test(
        SnowDesktopUiAnimationSchedulerTests
        ui_animation_scheduler
        SOURCES
            tests/ui_animation_scheduler_tests.cpp
            src/ui/render/ui_animation_scheduler.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32
        LABELS rules animation)

    snowdesktop_add_test(
        SnowDesktopMenuIconRenderTests
        menu_icon_render
        SOURCES
            tests/menu_icon_render_tests.cpp
            src/ui/menu/menu_icon_render.cpp
            src/ui/menu/menu_builtin_icon.cpp
            src/resources/resource.rc
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES gdi32 ole32 user32 windowscodecs
        LABELS contract ui)

    snowdesktop_add_test(
        SnowDesktopModernMenuInteractionTests
        modern_menu_interaction
        SOURCES
            tests/modern_menu_interaction_tests.cpp
            tests/tray_focus_tests.cpp
            tests/system/tray/native_menu_layer_tests.cpp
            src/ui/menu/menu_icon_render.cpp
            src/ui/menu/menu_builtin_icon.cpp
            src/ui/menu/modern_menu.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES comctl32 dwmapi gdi32 imm32 ole32 user32 windowscodecs
        LABELS integration ui)

    snowdesktop_add_test(
        SnowDesktopComponentPreviewTests
        component_preview
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/component_preview_tests.cpp
            tests/widget_background_cache_tests.cpp
            tests/widget/widget_image_sampling_tests.cpp
            tests/widget_text_layout_cache_tests.cpp
            tests/scroll_content_clip_tests.cpp
            src/icons/icon_beautify.cpp
            src/widget/view/widget_background_cache.cpp
            src/widget/view/widget_image_sampling.cpp
            src/ui/preview/component_preview.cpp
            src/ui/menu/menu_icon_render.cpp
            src/ui/menu/menu_builtin_icon.cpp
            src/widget/preview/widget_preview_stage.cpp
            src/widget/preview/widget_preview_scene.cpp
            src/widget/preview/widget_preview_scene_model.cpp
        INCLUDE_DIRECTORIES src src/core
        LINK_LIBRARIES advapi32 d2d1 d3d11 dwrite dxgi dxguid gdi32 ole32 user32 uuid windowscodecs
        LABELS integration ui preview)

    snowdesktop_add_test(
        SnowDesktopWidgetPreviewStageTests
        widget_preview_stage
        SOURCES
            tests/widget_preview_stage_tests.cpp
            tests/widget/preview/wallpaper_pixels_tests.cpp
            src/widget/preview/widget_preview_stage.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES d2d1 d3d11 dxgi dxguid gdi32 ole32 windowscodecs
        LABELS contract render ui preview)

    snowdesktop_add_test(
        SnowDesktopWallpaperEngineCaptureTests
        wallpaper_engine_capture
        SOURCES
            tests/wallpaper_engine_capture_tests.cpp
            src/app/features/wallpaper_engine_capture.cpp
        INCLUDE_DIRECTORIES src src/app
        LINK_LIBRARIES d3d11 dxgi user32
        LABELS contract integration render preview)

    snowdesktop_add_test(
        SnowDesktopSteamWorkshopManagerTests
        steam_workshop_manager
        SOURCES
            tests/steam_workshop_manager_tests.cpp
            steam_bridge/src/preview_cache.cpp
            src/steam/steam_workshop_cache.cpp
            src/steam/steam_workshop_source.cpp
            src/steam/steam_workshop_sync.cpp
            src/widget/packages/widget_package.cpp
            src/data/atomic_file.cpp
            src/widget/packages/widget_package_paths.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
        INCLUDE_DIRECTORIES steam_bridge/src src
        LINK_LIBRARIES
            SnowDesktopWorkshopManagerSupport
            SnowDesktopSteamWorkshopCore
            SnowDesktopSteamAppIdentity
            advapi32
            bcrypt
            d3d11
            dxgi
            ole32
            user32
            windowscodecs
            winhttp
        COMMAND_ARGUMENTS
            "$<TARGET_FILE:snowwidget>"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS contract integration security steam sdk-free)
    add_dependencies(SnowDesktopSteamWorkshopManagerTests snowwidget)

    # Interactive visual smoke harness. It is excluded from normal builds and
    # CTest; run the target explicitly when reviewing the modern popup menu.
    add_executable(SnowDesktopModernMenuPreview WIN32 EXCLUDE_FROM_ALL
        tests/modern_menu_preview.cpp
        src/ui/menu/menu_icon_render.cpp
        src/ui/menu/menu_builtin_icon.cpp
        src/ui/menu/modern_menu.cpp
        src/resources/resource.rc)
    set_target_properties(SnowDesktopModernMenuPreview PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    target_compile_features(SnowDesktopModernMenuPreview PRIVATE cxx_std_20)
    target_include_directories(SnowDesktopModernMenuPreview PRIVATE src)
    target_link_libraries(SnowDesktopModernMenuPreview PRIVATE
        dwmapi gdi32 imm32 ole32 user32 windowscodecs)
    if (MSVC)
        set_property(TARGET SnowDesktopModernMenuPreview PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(SnowDesktopModernMenuPreview PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(SnowDesktopModernMenuPreview PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    # Fullscreen/focus diagnostic is built for delivery but is never launched
    # by CTest. Changing display modes is an explicit interactive operation.
    add_executable(SnowDesktopFullscreenProbe WIN32 tests/dock/fullscreen_probe.cpp)
    set_target_properties(SnowDesktopFullscreenProbe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    target_compile_features(SnowDesktopFullscreenProbe PRIVATE cxx_std_20)
    target_link_libraries(SnowDesktopFullscreenProbe PRIVATE d3d11 dxgi gdi32 shell32 user32)
    add_dependencies(SnowDesktopDockAndWindowRulesTests SnowDesktopFullscreenProbe)
    if (MSVC)
        set_property(TARGET SnowDesktopFullscreenProbe PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(SnowDesktopFullscreenProbe PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(SnowDesktopFullscreenProbe PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    snowdesktop_add_test(
        SnowDesktopPopupAnimationRulesTests
        popup_animation_rules
        SOURCES tests/popup_animation_rules_tests.cpp
        INCLUDE_DIRECTORIES src
        LABELS rules animation)

    include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.ToolTests.cmake")
    snowdesktop_add_native_tool_test(SnowDesktopBuildProductArtifactsTests build_product_artifacts
        build_tools/build_product_artifacts_tests.cpp "tools;integration;packaging" 120)
    # Shared desktop/Explorer/host resources are serialized. Isolated fixtures
    # and pure logic cases can execute alongside these tests.
    get_property(SNOWDESKTOP_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS SNOWDESKTOP_REGISTERED_TESTS)
        get_test_property(${test_name} LABELS test_labels)
        # These tests exercise a host graph or launch tool subprocesses. Budget
        # their CPU demand independently of the global CTest job count; running
        # 20 native hosts alongside tool workflows saturated the 24-core test
        # machine and caused timeouts, while eight heavy natives passed.
        if("isolated-native-model" IN_LIST test_labels OR "tools" IN_LIST test_labels)
            get_property(test_has_processor_budget TEST ${test_name} PROPERTY PROCESSORS SET)
            if(NOT test_has_processor_budget)
                set_tests_properties(${test_name} PROPERTIES PROCESSORS 3)
            endif()
        endif()
        if(("shell" IN_LIST test_labels OR "window" IN_LIST test_labels OR
            "ui" IN_LIST test_labels OR "host-runtime" IN_LIST test_labels)
            AND NOT "tools" IN_LIST test_labels
            AND NOT "isolated-native-model" IN_LIST test_labels)
            set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK snowdesktop_desktop)
        endif()
    endforeach()
    add_custom_target(
        SnowDesktopCoreTests
        DEPENDS ${SNOWDESKTOP_CORE_TEST_TARGETS})
    # Partial environment coverage must not be presented as a passing entry.
    # Failures take precedence over this distinct unavailable-environment code.
    set_tests_properties(application_data_lifecycle steam_runtime_update steam_workshop_manager
        PROPERTIES SKIP_RETURN_CODE 77)

    add_custom_target(
        SnowDesktopFastTests
        DEPENDS ${SNOWDESKTOP_FAST_TEST_TARGETS})
    add_custom_target(
        SnowDesktopBuildToolTests
        DEPENDS ${SNOWDESKTOP_TOOL_TEST_TARGETS})
    add_custom_target(
        SnowDesktopAppTests
        COMMAND "${CMAKE_COMMAND}" -E env "PSExecutionPolicyPreference=Bypass" "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile
            -File "${CMAKE_SOURCE_DIR}/scripts/arrange_build_output.ps1"
            -BuildOutput "${CMAKE_BINARY_DIR}/$<CONFIG>"
            -AllowMissingFirstPartyRuntime
        DEPENDS ${SNOWDESKTOP_APP_TEST_TARGETS})
    add_custom_target(
        SnowDesktopTests
        COMMAND "${CMAKE_COMMAND}" -E env "PSExecutionPolicyPreference=Bypass" "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile
            -File "${CMAKE_SOURCE_DIR}/scripts/arrange_build_output.ps1"
            -BuildOutput "${CMAKE_BINARY_DIR}/$<CONFIG>"
            -AllowMissingFirstPartyRuntime
        DEPENDS ${SNOWDESKTOP_TEST_TARGETS})
endif()


snowdesktop_install_shared_guard()
