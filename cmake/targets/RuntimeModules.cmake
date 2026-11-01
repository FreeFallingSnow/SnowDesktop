# Production modules shared by the host, authoring tool and native tests.
# Keep Windows/XAML composition roots on their executable targets; these
# modules declare their own inputs and do not consume executable XAML headers.
function(snowdesktop_add_runtime_module target)
    cmake_parse_arguments(MODULE "" "" "SOURCES;LINK_LIBRARIES" ${ARGN})
    add_library(${target} STATIC ${MODULE_SOURCES})
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${CMAKE_SOURCE_DIR}/src")
    if(MODULE_LINK_LIBRARIES)
        target_link_libraries(${target} PUBLIC ${MODULE_LINK_LIBRARIES})
    endif()
    if(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # Preserve the host's Release crash/debug symbols in the shared objects.
        # Debug keeps CMake's own information format without overriding /Zi.
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8
            $<$<CONFIG:Release>:/Z7>)
        target_compile_definitions(${target} PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -pedantic)
    endif()
    set_property(GLOBAL APPEND PROPERTY SNOWDESKTOP_RUNTIME_MODULES ${target})
endfunction()

snowdesktop_add_runtime_module(SnowDesktopAtomicFile
    SOURCES src/data/atomic_file.cpp)
snowdesktop_add_runtime_module(SnowDesktopWidgetPermissions
    SOURCES src/widget/permissions/widget_permission_broker.cpp
        src/widget/permissions/widget_permission_state.cpp
    LINK_LIBRARIES bcrypt)
snowdesktop_add_runtime_module(SnowDesktopWidgetLuaRuntime
    SOURCES src/widget/runtime/lua_runtime.cpp
        src/widget/runtime/widget_lua_lifecycle.cpp
    LINK_LIBRARIES SnowDesktopLua SnowDesktopPerfetto)
snowdesktop_add_runtime_module(SnowDesktopWidgetPackages
    SOURCES src/widget/packages/widget_package.cpp
        src/widget/runtime/widget_logical_slot.cpp
    LINK_LIBRARIES SnowDesktopAtomicFile SnowDesktopWidgetPermissions bcrypt ole32)
snowdesktop_add_runtime_module(SnowDesktopWidgetApiRegistry
    SOURCES src/widget/api/widget_api_registry.cpp
    LINK_LIBRARIES SnowDesktopLua)
snowdesktop_add_runtime_module(SnowDesktopWidgetAuthoring
    SOURCES src/widget/authoring/widget_author_permissions.cpp
        src/widget/authoring/widget_author_test.cpp
    LINK_LIBRARIES SnowDesktopWidgetPackages SnowDesktopWidgetLuaRuntime)
snowdesktop_add_runtime_module(SnowDesktopWidgetStorage
    SOURCES src/widget/storage/widget_storage_transaction.cpp
        src/widget/storage/widget_storage_value.cpp
        src/widget/storage/widget_secret_store.cpp
    LINK_LIBRARIES bcrypt crypt32)

# Shared native services used by the host and module-level behavior scenarios.
# Data paths, deployment and diagnostic logging currently form one dependency
# cycle; keep that runtime environment together, separate from app composition.
snowdesktop_add_runtime_module(SnowDesktopSteamRuntimeIdentity
    SOURCES src/steam/steam_runtime_context.cpp)
snowdesktop_add_runtime_module(SnowDesktopRuntimeEnvironment
    SOURCES src/data/data_paths.cpp src/data/portable_data_migration.cpp
        src/diagnostics/debug_profile.cpp src/diagnostics/diagnostic_log.cpp
        src/platform/deployment_context.cpp src/platform/hook_entry_deployment.cpp
    LINK_LIBRARIES SnowDesktopAtomicFile SnowDesktopSteamRuntimeIdentity
        advapi32 bcrypt ole32 runtimeobject shell32 shlwapi windowsapp)
target_compile_definitions(SnowDesktopRuntimeEnvironment PRIVATE
    SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
    SNOWDESKTOP_STORE_ID="${SNOWDESKTOP_STORE_ID}"
    SNOWDESKTOP_PACKAGE_FAMILY_NAME="${SNOWDESKTOP_PACKAGE_FAMILY_NAME}")
# Match the host's C++/WinRT projection package, without WinUI, XAML or its
# generated application types. NuGet restore remains target-local.
set_target_properties(SnowDesktopRuntimeEnvironment PROPERTIES
    VS_PACKAGE_REFERENCES "Microsoft.Windows.CppWinRT_${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}")
snowdesktop_add_runtime_module(SnowDesktopNativeUtilities
    SOURCES src/common/utils.cpp src/icons/shortcut_icon_resource.cpp
    LINK_LIBRARIES SnowDesktopRuntimeEnvironment
        comctl32 d2d1 dwrite gdi32 ole32 shell32 shcore shlwapi windowscodecs xmllite)
snowdesktop_add_runtime_module(SnowDesktopLocalization
    SOURCES src/common/l10n.cpp)
snowdesktop_add_runtime_module(SnowDesktopSettingsPersistence
    SOURCES src/settings/settings_controller.cpp src/settings/settings_route.cpp
        src/settings/settings_store_native.cpp src/settings/general_settings.cpp
        src/settings/category_settings.cpp src/dock/dock_settings.cpp
        src/navigation/navigation_settings.cpp src/theme/personalization.cpp
        src/theme/personalization_path.cpp
    LINK_LIBRARIES SnowDesktopRuntimeEnvironment SnowDesktopNativeUtilities
        SnowDesktopLocalization SnowDesktopPerfetto dwmapi)
snowdesktop_add_runtime_module(SnowDesktopLayoutStorage
    SOURCES src/data/layout_storage.cpp
    LINK_LIBRARIES SnowDesktopAtomicFile)
snowdesktop_add_runtime_module(SnowDesktopWidgetTasks
    SOURCES src/widget/tasks/widget_task_broker.cpp
    LINK_LIBRARIES SnowDesktopPerfetto)
snowdesktop_add_runtime_module(SnowDesktopDockWindowPin
    SOURCES src/dock/dock_window_pin.cpp
    LINK_LIBRARIES SnowDesktopWindowSupport d2d1 dwmapi)
if(MSVC)
    target_compile_options(SnowDesktopRuntimeEnvironment PRIVATE /bigobj)
    foreach(module IN ITEMS SnowDesktopSteamRuntimeIdentity SnowDesktopRuntimeEnvironment
            SnowDesktopNativeUtilities SnowDesktopLocalization SnowDesktopSettingsPersistence
            SnowDesktopLayoutStorage SnowDesktopWidgetTasks SnowDesktopDockWindowPin)
        # These sources previously used the host's Windows 10 platform and CRT
        # compatibility definitions. Keep them explicit on their new owners.
        target_compile_definitions(${module} PRIVATE
            _CRT_SECURE_NO_WARNINGS WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    endforeach()
endif()

# Replace matching production translation units with their owning module.
# Existing source inventories remain the selection input, so adding a consumer
# cannot silently leave a second copy of a promoted source in that target.
function(snowdesktop_use_runtime_modules target)
    get_target_property(sources ${target} SOURCES)
    get_property(modules GLOBAL PROPERTY SNOWDESKTOP_RUNTIME_MODULES)
    foreach(module IN LISTS modules)
        get_target_property(module_sources ${module} SOURCES)
        set(uses_module FALSE)
        foreach(source IN LISTS module_sources)
            if(source IN_LIST sources)
                list(REMOVE_ITEM sources "${source}")
                set(uses_module TRUE)
            endif()
        endforeach()
        if(uses_module)
            target_link_libraries(${target} PRIVATE ${module})
        endif()
    endforeach()
    set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
endfunction()

snowdesktop_use_runtime_modules(SnowDesktop)
snowdesktop_use_runtime_modules(snowwidget)
snowdesktop_use_runtime_modules(SnowDesktopLauncher)
# The independent window helper supplies its own platform service providers;
# retain that build variant instead of linking the host runtime environment.
