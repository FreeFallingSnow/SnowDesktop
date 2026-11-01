# Production modules shared by the host, authoring tool and native tests.
# Keep Windows/XAML composition roots on their executable targets; these
# modules have no target-specific generated headers or application defines.
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
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
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
