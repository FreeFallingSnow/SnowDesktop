add_executable(SnowDesktop WIN32
    ${SNOWDESKTOP_SOURCES}
    src/resources/resource.rc
    "${SNOWDESKTOP_EXE_VERSION_RESOURCE}"
)
source_group(TREE "${CMAKE_SOURCE_DIR}/src" PREFIX "Source" FILES
    ${SNOWDESKTOP_SOURCES}
    src/resources/resource.rc)
snowdesktop_enable_native_window_compatibility(SnowDesktop)

function(snowdesktop_configure_host target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE
        src
        "${CMAKE_BINARY_DIR}/generated"
        src/core
        src/app
        src/winui
        "${CMAKE_BINARY_DIR}/generated/xaml/$<CONFIG>"
    )

    if (MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /MP${SNOWDESKTOP_COMPILE_JOBS} /bigobj /W4 /permissive- /EHsc /utf-8 /Z7)
        target_compile_definitions(${target} PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
            WINVER=0x0A00 _WIN32_WINNT=0x0A00
            DISABLE_XAML_GENERATED_MAIN
            SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
            SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
            SNOWDESKTOP_STORE_ID="${SNOWDESKTOP_STORE_ID}"
            SNOWDESKTOP_PACKAGE_FAMILY_NAME="${SNOWDESKTOP_PACKAGE_FAMILY_NAME}")
        target_precompile_headers(${target} PRIVATE
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
        target_link_options(${target} PRIVATE
            "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
            /DELAYLOAD:Microsoft.UI.Windowing.Core.dll
            /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -pedantic)
        target_compile_definitions(${target} PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
            SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
            SNOWDESKTOP_STEAM_APP_ID=${SNOWDESKTOP_STEAM_APP_ID}
            SNOWDESKTOP_STORE_ID="${SNOWDESKTOP_STORE_ID}"
            SNOWDESKTOP_PACKAGE_FAMILY_NAME="${SNOWDESKTOP_PACKAGE_FAMILY_NAME}")
    endif()

    target_link_libraries(${target} PRIVATE
        webpdecoder SnowDesktopPuff SnowDesktopWindowSupport
        ${SNOWDESKTOP_SYSTEM_CONTROL_LIBRARIES}
        SnowDesktopLua SnowDesktopEverything SnowDesktopPinyinData
        SnowDesktopSearchVisibility
        comctl32 d2d1 d3d11 dbghelp dcomp dwmapi dwrite dxgi dxguid gdi32 msimg32
        advapi32 bcrypt crypt32 delayimp imm32 iphlpapi mfuuid ole32 oleacc oleaut32 pdh propsys runtimeobject shcore shell32 shlwapi uiautomationcore urlmon user32 windowsapp windowscodecs uuid winhttp ws2_32 xmllite
        version wbemuuid taskschd
    )

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/widgets"
            "$<TARGET_FILE_DIR:SnowDesktop>/widgets"
        COMMENT "Copying widget scripts to output (SnowDesktop)..."
    )
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/lang"
            "$<TARGET_FILE_DIR:SnowDesktop>/lang"
        COMMENT "Copying language files to output (SnowDesktop)..."
    )
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SnowDesktop>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/libwebp/COPYING" "$<TARGET_FILE_DIR:SnowDesktop>/licenses/libwebp-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/libwebp/PATENTS" "$<TARGET_FILE_DIR:SnowDesktop>/licenses/libwebp-PATENTS.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/puff/puff.h" "$<TARGET_FILE_DIR:SnowDesktop>/licenses/puff-LICENSE.txt")
endfunction()
