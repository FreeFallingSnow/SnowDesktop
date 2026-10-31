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
