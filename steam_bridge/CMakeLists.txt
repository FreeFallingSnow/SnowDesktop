# SPDX-FileCopyrightText: 2026 SnowDesktop contributors
# SPDX-License-Identifier: MIT

set(SNOWDESKTOP_STEAMWORKS_SDK_ROOT
    "$ENV{SNOWDESKTOP_STEAMWORKS_SDK_ROOT}" CACHE PATH
    "External Steamworks SDK root; leave empty for an SDK-free bridge build")

add_library(SnowDesktopSteamAppIdentity INTERFACE)
target_compile_features(SnowDesktopSteamAppIdentity INTERFACE cxx_std_20)
target_include_directories(SnowDesktopSteamAppIdentity INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(SnowDesktopSteamAppIdentity INTERFACE
    SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
    SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID=${SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID})

add_library(SnowDesktopSteamWorkshopCore STATIC
    src/steam_workshop_core.cpp)
target_compile_features(SnowDesktopSteamWorkshopCore PUBLIC cxx_std_20)
target_include_directories(SnowDesktopSteamWorkshopCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(SnowDesktopSteamWorkshopCore PUBLIC
    SnowDesktopSteamAppIdentity)

add_library(SnowDesktopWorkshopManagerSupport STATIC
    src/authoring_toolchain.cpp
    src/component_workshop_publish.cpp
    src/manager_localization.cpp
    src/package_tool.cpp
    src/workshop_localization.cpp
    src/workshop_project.cpp)
target_compile_features(SnowDesktopWorkshopManagerSupport PUBLIC cxx_std_20)
target_include_directories(SnowDesktopWorkshopManagerSupport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(SnowDesktopWorkshopManagerSupport PUBLIC
    ole32 shell32)

add_executable(SnowDesktopSteamBridge src/main.cpp)
target_link_libraries(SnowDesktopSteamBridge PRIVATE
    SnowDesktopSteamWorkshopCore)

add_executable(SnowDesktopWorkshopManager WIN32
    src/manager_main.cpp
    src/preview_cache.cpp)
target_link_libraries(SnowDesktopWorkshopManager PRIVATE
    SnowDesktopSteamWorkshopCore
    SnowDesktopWorkshopManagerSupport
    SnowDesktopWorkshopImgui
    d3d11 dxgi ole32 shell32 shlwapi user32 windowscodecs winhttp)

if (MSVC)
    set(SNOWDESKTOP_STEAM_BRIDGE_VERSION_RESOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/SnowDesktopSteamBridge.version.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
        "${SNOWDESKTOP_STEAM_BRIDGE_VERSION_RESOURCE}" @ONLY)
    target_sources(SnowDesktopSteamBridge PRIVATE
        "${SNOWDESKTOP_STEAM_BRIDGE_VERSION_RESOURCE}")

    set(SNOWDESKTOP_WORKSHOP_MANAGER_VERSION_RESOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/SnowDesktopWorkshopManager.version.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/manager_version.rc.in"
        "${SNOWDESKTOP_WORKSHOP_MANAGER_VERSION_RESOURCE}" @ONLY)
    target_sources(SnowDesktopWorkshopManager PRIVATE
        "${SNOWDESKTOP_WORKSHOP_MANAGER_VERSION_RESOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager_resources.rc")
endif()

foreach(STEAM_TARGET IN ITEMS
        SnowDesktopSteamWorkshopCore
        SnowDesktopWorkshopManagerSupport
        SnowDesktopSteamBridge
        SnowDesktopWorkshopManager)
    target_compile_definitions(${STEAM_TARGET} PRIVATE
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if (MSVC)
        set_property(TARGET ${STEAM_TARGET} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${STEAM_TARGET} PRIVATE
            /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${STEAM_TARGET} PRIVATE
            -Wall -Wextra -pedantic)
    endif()
endforeach()

set_target_properties(
    SnowDesktopSteamBridge SnowDesktopWorkshopManager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_custom_command(TARGET SnowDesktopWorkshopManager POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/lang"
        "$<TARGET_FILE_DIR:SnowDesktopWorkshopManager>/lang"
    COMMENT "Copying shared SnowDesktop language catalogs...")

add_custom_command(TARGET SnowDesktopWorkshopManager POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/widgets/snowdesktop-lua-widget"
        "$<TARGET_FILE_DIR:SnowDesktopWorkshopManager>/widgets/snowdesktop-lua-widget"
    COMMENT "Copying the SnowDesktop authoring Skill...")

add_dependencies(SnowDesktopWorkshopManager snowwidget)
add_custom_command(TARGET SnowDesktopWorkshopManager POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:SnowDesktopWorkshopManager>/widgets/snowdesktop-lua-widget/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:snowwidget>"
        "$<TARGET_FILE_DIR:SnowDesktopWorkshopManager>/widgets/snowdesktop-lua-widget/bin/snowwidget.exe"
    COMMENT "Embedding snowwidget CLI in the Workshop Manager Agent Skill...")

if (SNOWDESKTOP_STEAMWORKS_SDK_ROOT)
    if (NOT WIN32)
        message(FATAL_ERROR
            "The SnowDesktop Steam bridge currently supports Steamworks on Windows only")
    endif()
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" SNOWDESKTOP_SOURCE_ROOT_REAL)
    file(REAL_PATH "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT}"
        SNOWDESKTOP_STEAMWORKS_SDK_ROOT_REAL)
    cmake_path(IS_PREFIX SNOWDESKTOP_SOURCE_ROOT_REAL
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT_REAL}" NORMALIZE
        SNOWDESKTOP_STEAMWORKS_SDK_IS_IN_SOURCE)
    if (SNOWDESKTOP_STEAMWORKS_SDK_IS_IN_SOURCE)
        message(FATAL_ERROR
            "Steamworks SDK must be kept outside the SnowDesktop source tree")
    endif()
    set(STEAMWORKS_HEADER
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT_REAL}/public/steam/steam_api.h")
    set(STEAMWORKS_IMPORT_LIBRARY
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT_REAL}/redistributable_bin/win64/steam_api64.lib")
    set(STEAMWORKS_REDISTRIBUTABLE
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT_REAL}/redistributable_bin/win64/steam_api64.dll")
    foreach(REQUIRED_STEAMWORKS_FILE IN ITEMS
            "${STEAMWORKS_HEADER}"
            "${STEAMWORKS_IMPORT_LIBRARY}"
            "${STEAMWORKS_REDISTRIBUTABLE}")
        if (NOT EXISTS "${REQUIRED_STEAMWORKS_FILE}")
            message(FATAL_ERROR
                "Required external Steamworks SDK file was not found: ${REQUIRED_STEAMWORKS_FILE}")
        endif()
    endforeach()
    target_compile_definitions(SnowDesktopSteamWorkshopCore PUBLIC
        SNOWDESKTOP_HAS_STEAMWORKS=1)
    target_include_directories(SnowDesktopSteamWorkshopCore SYSTEM PUBLIC
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT_REAL}/public")
    target_link_libraries(SnowDesktopSteamWorkshopCore PUBLIC
        "${STEAMWORKS_IMPORT_LIBRARY}")
    foreach(STEAM_EXE IN ITEMS
            SnowDesktopSteamBridge SnowDesktopWorkshopManager)
        add_custom_command(TARGET ${STEAM_EXE} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${STEAMWORKS_REDISTRIBUTABLE}"
                "$<TARGET_FILE_DIR:${STEAM_EXE}>/steam_api64.dll"
            COMMENT "Copying the permitted Steamworks redistributable...")
    endforeach()
    message(STATUS "SnowDesktop Steam tools: Steamworks support enabled")
else()
    target_compile_definitions(SnowDesktopSteamWorkshopCore PUBLIC
        SNOWDESKTOP_HAS_STEAMWORKS=0)
    message(STATUS
        "SnowDesktop Steam tools: SDK-free build (set SNOWDESKTOP_STEAMWORKS_SDK_ROOT to enable Steamworks)")
endif()

foreach(STEAM_EXE IN ITEMS
        SnowDesktopSteamBridge SnowDesktopWorkshopManager)
    add_custom_command(TARGET ${STEAM_EXE} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:${STEAM_EXE}>/SnowDesktopSteamBridge-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:${STEAM_EXE}>/SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md"
        COMMENT "Copying Steam bridge license notices...")
endforeach()
