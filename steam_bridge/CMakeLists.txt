# SPDX-FileCopyrightText: 2026 SnowDesktop contributors
# SPDX-License-Identifier: MIT

set(SNOWDESKTOP_STEAMWORKS_SDK_ROOT "" CACHE PATH
    "External Steamworks SDK root; leave empty for an SDK-free bridge build")

add_executable(SnowDesktopSteamBridge
    src/main.cpp)

if (MSVC)
    set(SNOWDESKTOP_STEAM_BRIDGE_VERSION_RESOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/SnowDesktopSteamBridge.version.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
        "${SNOWDESKTOP_STEAM_BRIDGE_VERSION_RESOURCE}"
        @ONLY)
    target_sources(SnowDesktopSteamBridge PRIVATE
        "${SNOWDESKTOP_STEAM_BRIDGE_VERSION_RESOURCE}")
endif()

set_target_properties(SnowDesktopSteamBridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

target_compile_features(SnowDesktopSteamBridge PRIVATE cxx_std_20)
target_compile_definitions(SnowDesktopSteamBridge PRIVATE
    SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}")

if (MSVC)
    set_property(TARGET SnowDesktopSteamBridge PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopSteamBridge PRIVATE
        /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(SnowDesktopSteamBridge PRIVATE
        -Wall -Wextra -pedantic)
endif()

if (SNOWDESKTOP_STEAMWORKS_SDK_ROOT)
    if (NOT WIN32)
        message(FATAL_ERROR
            "The SnowDesktop Steam bridge currently supports Steamworks on Windows only")
    endif()

    set(STEAMWORKS_HEADER
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT}/public/steam/steam_api.h")
    set(STEAMWORKS_IMPORT_LIBRARY
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib")
    set(STEAMWORKS_REDISTRIBUTABLE
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll")

    foreach(REQUIRED_STEAMWORKS_FILE IN ITEMS
            "${STEAMWORKS_HEADER}"
            "${STEAMWORKS_IMPORT_LIBRARY}"
            "${STEAMWORKS_REDISTRIBUTABLE}")
        if (NOT EXISTS "${REQUIRED_STEAMWORKS_FILE}")
            message(FATAL_ERROR
                "Required external Steamworks SDK file was not found: ${REQUIRED_STEAMWORKS_FILE}")
        endif()
    endforeach()

    target_compile_definitions(SnowDesktopSteamBridge PRIVATE
        SNOWDESKTOP_HAS_STEAMWORKS=1)
    target_include_directories(SnowDesktopSteamBridge SYSTEM PRIVATE
        "${SNOWDESKTOP_STEAMWORKS_SDK_ROOT}/public")
    target_link_libraries(SnowDesktopSteamBridge PRIVATE
        "${STEAMWORKS_IMPORT_LIBRARY}")
    add_custom_command(TARGET SnowDesktopSteamBridge POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${STEAMWORKS_REDISTRIBUTABLE}"
            "$<TARGET_FILE_DIR:SnowDesktopSteamBridge>/steam_api64.dll"
        COMMENT "Copying the permitted Steamworks redistributable...")
    message(STATUS "SnowDesktop Steam bridge: Steamworks support enabled")
else()
    target_compile_definitions(SnowDesktopSteamBridge PRIVATE
        SNOWDESKTOP_HAS_STEAMWORKS=0)
    message(STATUS
        "SnowDesktop Steam bridge: SDK-free build (set SNOWDESKTOP_STEAMWORKS_SDK_ROOT to enable Steamworks)")
endif()

add_custom_command(TARGET SnowDesktopSteamBridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:SnowDesktopSteamBridge>/SnowDesktopSteamBridge-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "$<TARGET_FILE_DIR:SnowDesktopSteamBridge>/SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md"
    COMMENT "Copying Steam bridge license notices...")
