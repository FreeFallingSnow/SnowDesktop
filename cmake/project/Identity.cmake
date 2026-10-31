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
