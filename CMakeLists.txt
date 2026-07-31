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
set(SNOWDESKTOP_EXE_VERSION_RESOURCE
    "${SNOWDESKTOP_VERSION_RESOURCE_DIR}/SnowDesktop.version.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.rc.in"
    "${SNOWDESKTOP_EXE_VERSION_RESOURCE}"
    @ONLY)

set(SNOWDESKTOP_FILE_DESCRIPTION "SnowDesktop taskbar hook")
set(SNOWDESKTOP_ORIGINAL_FILENAME "SnowDesktopTaskbarHook.dll")
set(SNOWDESKTOP_FILE_TYPE "VFT_DLL")
set(SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE
    "${SNOWDESKTOP_VERSION_RESOURCE_DIR}/SnowDesktopTaskbarHook.version.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.rc.in"
    "${SNOWDESKTOP_TASKBAR_HOOK_VERSION_RESOURCE}"
    @ONLY)

message(STATUS "SnowDesktop version: ${SNOWDESKTOP_VERSION}")

set(SNOWDESKTOP_SOURCES
    src/main.cpp
    src/app/app.cpp
    src/app/desktop_backdrop_compositor.cpp
    src/dock_window_preview.cpp
    src/dock_window_transition.cpp
    src/taskbar_dynamic/search_visibility_detector.cpp
    src/core/item.cpp
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
    src/personalization.cpp
    src/dock_settings.cpp
    src/navigation_settings.cpp
    src/general_settings.cpp
    src/category_settings.cpp
    src/l10n.cpp
    src/item_location.cpp
    src/search_match.cpp
    src/everything_search.cpp
    src/widget_engine.cpp
    src/widget_package.cpp
    src/widget_package_paths.cpp
    src/system_snapshot.cpp
    src/http_runtime.cpp
    src/widgets/widget_base.cpp
    src/widgets/collection.cpp
    src/widgets/collection_group.cpp
    src/widgets/file_group.cpp
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

add_library(SnowDesktopImgui STATIC ${IMGUI_SOURCES})
target_compile_features(SnowDesktopImgui PRIVATE cxx_std_20)
target_include_directories(SnowDesktopImgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
if(MSVC)
    set_property(TARGET SnowDesktopImgui PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopImgui PRIVATE /MP /W4 /permissive- /EHsc /utf-8 /Z7)
    target_compile_definitions(SnowDesktopImgui PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(SnowDesktopLua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(SnowDesktopLua PUBLIC third_party/lua)
if(MSVC)
    set_property(TARGET SnowDesktopLua PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopLua PRIVATE /MP
        /wd4244 /wd4310 /wd4324 /wd4701 /wd4702)
endif()

add_library(SnowDesktopEverything STATIC ${EVERYTHING_SOURCES})
set_source_files_properties(${EVERYTHING_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(SnowDesktopEverything PUBLIC
    third_party/everything/include)
if(MSVC)
    set_property(TARGET SnowDesktopEverything PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopEverything PRIVATE /MP /wd4100 /wd4133 /wd4996)
endif()

add_library(SnowDesktopPinyinData STATIC ${PINYIN_DATA_SOURCES})
target_compile_features(SnowDesktopPinyinData PRIVATE cxx_std_20)
target_include_directories(SnowDesktopPinyinData PUBLIC
    third_party/pinyin-data)
if(MSVC)
    set_property(TARGET SnowDesktopPinyinData PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopPinyinData PRIVATE /MP)
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
    third_party/spdlog/include
)

if (MSVC)
    set_property(TARGET SnowDesktop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktop PRIVATE /MP /W4 /permissive- /EHsc /utf-8 /Z7)
    target_compile_definitions(SnowDesktop PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        WINVER=0x0A00 _WIN32_WINNT=0x0A00
        SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
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
        SNOWDESKTOP_STORE_ID="${SNOWDESKTOP_STORE_ID}"
        SNOWDESKTOP_PACKAGE_FAMILY_NAME="${SNOWDESKTOP_PACKAGE_FAMILY_NAME}")
endif()

target_link_libraries(SnowDesktop PRIVATE
    SnowDesktopImgui SnowDesktopLua SnowDesktopEverything SnowDesktopPinyinData
    comctl32 d2d1 d3d11 dbghelp dcomp dwmapi dwrite dxgi dxguid gdi32 msimg32
    advapi32 bcrypt imm32 iphlpapi ole32 pdh propsys runtimeobject shcore shell32 shlwapi urlmon user32 windowsapp windowscodecs uuid winhttp ws2_32
    version wbemuuid
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
add_custom_command(TARGET SnowDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/lang"
        "$<TARGET_FILE_DIR:SnowDesktop>/lang"
    COMMENT "Copying language files to output (SnowDesktop)..."
)

add_executable(snowwidget
    tools/snowwidget/main.cpp
    src/widget_package.cpp)
target_compile_features(snowwidget PRIVATE cxx_std_20)
target_include_directories(snowwidget PRIVATE src)
target_link_libraries(snowwidget PRIVATE bcrypt ole32)
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

include(CTest)
if (BUILD_TESTING)
    add_executable(SnowDesktopApplicationDataLifecycleTests
        tests/application_data_lifecycle_tests.cpp
        src/full_data_backup.cpp
        src/widget_package.cpp
        src/portable_data_migration.cpp
        src/single_instance.cpp)
    target_compile_features(
        SnowDesktopApplicationDataLifecycleTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopApplicationDataLifecycleTests PRIVATE src)
    target_link_libraries(
        SnowDesktopApplicationDataLifecycleTests PRIVATE
        bcrypt ole32 shell32 user32 version)
    if (MSVC)
        set_property(
            TARGET SnowDesktopApplicationDataLifecycleTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopApplicationDataLifecycleTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopApplicationDataLifecycleTests PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopApplicationDataLifecycleTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME application_data_lifecycle
        COMMAND SnowDesktopApplicationDataLifecycleTests)

    add_executable(
        SnowDesktopHttpSecurityTests
        tests/http_security_tests.cpp
        src/http_runtime.cpp)
    target_compile_features(
        SnowDesktopHttpSecurityTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopHttpSecurityTests PRIVATE src)
    target_link_libraries(
        SnowDesktopHttpSecurityTests PRIVATE
        shlwapi winhttp ws2_32)
    if (MSVC)
        set_property(
            TARGET SnowDesktopHttpSecurityTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopHttpSecurityTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopHttpSecurityTests PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopHttpSecurityTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME http_security
        COMMAND SnowDesktopHttpSecurityTests)

    add_executable(
        SnowDesktopCalendarServiceTests
        tests/calendar_service_tests.cpp
        src/calendar_service.cpp)
    target_compile_features(
        SnowDesktopCalendarServiceTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopCalendarServiceTests PRIVATE src)
    target_link_libraries(
        SnowDesktopCalendarServiceTests PRIVATE ole32)
    if (MSVC)
        set_property(
            TARGET SnowDesktopCalendarServiceTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopCalendarServiceTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopCalendarServiceTests PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopCalendarServiceTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME calendar_service
        COMMAND SnowDesktopCalendarServiceTests)

    add_executable(
        SnowDesktopLocalizationContractTests
        tests/localization_contract_tests.cpp
        src/l10n.cpp)
    target_compile_features(
        SnowDesktopLocalizationContractTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopLocalizationContractTests PRIVATE src)
    if (MSVC)
        set_property(
            TARGET SnowDesktopLocalizationContractTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopLocalizationContractTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopLocalizationContractTests PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopLocalizationContractTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME localization_contract
        COMMAND SnowDesktopLocalizationContractTests
            "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(SnowDesktopSlotContractMatrixTests
        tests/slot_contract_matrix_tests.cpp)
    target_compile_features(
        SnowDesktopSlotContractMatrixTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopSlotContractMatrixTests PRIVATE src)

    add_executable(SnowDesktopSlotRuntimeContractTests
        tests/slot_runtime_contract_tests.cpp
        src/core/slot.cpp
        src/core/container.cpp
        src/l10n.cpp)
    target_compile_features(
        SnowDesktopSlotRuntimeContractTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopSlotRuntimeContractTests PRIVATE
        src src/core)
    target_link_libraries(
        SnowDesktopSlotRuntimeContractTests PRIVATE
        d2d1 user32)

    foreach(SLOT_TEST_TARGET
        SnowDesktopSlotContractMatrixTests
        SnowDesktopSlotRuntimeContractTests)
        if (MSVC)
            set_property(
                TARGET ${SLOT_TEST_TARGET}
                PROPERTY MSVC_RUNTIME_LIBRARY
                    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            target_compile_options(
                ${SLOT_TEST_TARGET} PRIVATE
                /W4 /permissive- /EHsc /utf-8)
            target_compile_definitions(
                ${SLOT_TEST_TARGET} PRIVATE
                _UNICODE UNICODE NOMINMAX
                    WIN32_LEAN_AND_MEAN)
        else()
            target_compile_options(
                ${SLOT_TEST_TARGET} PRIVATE
                -Wall -Wextra -pedantic)
        endif()
    endforeach()
    add_test(
        NAME slot_contract_matrix
        COMMAND SnowDesktopSlotContractMatrixTests)
    add_test(
        NAME slot_runtime_contract
        COMMAND SnowDesktopSlotRuntimeContractTests)

    add_executable(SnowDesktopWidgetInteractionRulesTests
        tests/widget_interaction_rules_tests.cpp)
    target_compile_features(
        SnowDesktopWidgetInteractionRulesTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopWidgetInteractionRulesTests PRIVATE src)
    if (MSVC)
        set_property(
            TARGET SnowDesktopWidgetInteractionRulesTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopWidgetInteractionRulesTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(
            SnowDesktopWidgetInteractionRulesTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME widget_interaction_rules
        COMMAND SnowDesktopWidgetInteractionRulesTests)

    add_executable(
        SnowDesktopQuickNavigationRulesTests
        tests/quick_navigation_rules_tests.cpp
        src/navigation_settings.cpp)
    target_compile_features(
        SnowDesktopQuickNavigationRulesTests
        PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopQuickNavigationRulesTests
        PRIVATE src)
    target_link_libraries(
        SnowDesktopQuickNavigationRulesTests
        PRIVATE SnowDesktopPinyinData)
    if (MSVC)
        set_property(
            TARGET SnowDesktopQuickNavigationRulesTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopQuickNavigationRulesTests
            PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopQuickNavigationRulesTests
            PRIVATE _UNICODE UNICODE NOMINMAX
                WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopQuickNavigationRulesTests
            PRIVATE -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME quick_navigation_rules
        COMMAND SnowDesktopQuickNavigationRulesTests)

    add_executable(
        SnowDesktopItemLocationTests
        tests/item_location_tests.cpp
        src/item_location.cpp)
    target_compile_features(
        SnowDesktopItemLocationTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopItemLocationTests PRIVATE src)
    target_link_libraries(
        SnowDesktopItemLocationTests
        PRIVATE ole32 shell32 shlwapi uuid)
    if (MSVC)
        set_property(
            TARGET SnowDesktopItemLocationTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopItemLocationTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopItemLocationTests PRIVATE
            _UNICODE UNICODE NOMINMAX
                WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopItemLocationTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME item_location
        COMMAND SnowDesktopItemLocationTests)

    add_executable(
        SnowDesktopDockAndWindowRulesTests
        tests/dock_and_window_rules_tests.cpp
        src/dock_window_preview.cpp
        src/dock_window_transition.cpp)
    target_compile_features(
        SnowDesktopDockAndWindowRulesTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopDockAndWindowRulesTests PRIVATE src)
    target_link_libraries(
        SnowDesktopDockAndWindowRulesTests PRIVATE
        d2d1 dwrite dwmapi gdi32 shcore user32)
    if (MSVC)
        set_property(
            TARGET SnowDesktopDockAndWindowRulesTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopDockAndWindowRulesTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopDockAndWindowRulesTests PRIVATE
            _UNICODE UNICODE NOMINMAX
                WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopDockAndWindowRulesTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME dock_and_window_rules
        COMMAND SnowDesktopDockAndWindowRulesTests)

    add_executable(
        SnowDesktopPopupAnimationRulesTests
        tests/popup_animation_rules_tests.cpp)
    target_compile_features(
        SnowDesktopPopupAnimationRulesTests PRIVATE cxx_std_20)
    target_include_directories(
        SnowDesktopPopupAnimationRulesTests PRIVATE src)
    if (MSVC)
        set_property(
            TARGET SnowDesktopPopupAnimationRulesTests
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            SnowDesktopPopupAnimationRulesTests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            SnowDesktopPopupAnimationRulesTests PRIVATE
            _UNICODE UNICODE NOMINMAX
                WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            SnowDesktopPopupAnimationRulesTests PRIVATE
            -Wall -Wextra -pedantic)
    endif()
    add_test(
        NAME popup_animation_rules
        COMMAND SnowDesktopPopupAnimationRulesTests)

endif()


