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

function(snowdesktop_configure_winui target)
    # Pin the complete WinUI toolchain on the executable so NuGet restore,
    # C++/WinRT projection generation, XAML compilation, and self-contained
    # deployment all happen in the CMake-generated MSBuild project.
    set_property(TARGET ${target} PROPERTY VS_PACKAGE_REFERENCES
        "Microsoft.WindowsAppSDK_${SNOWDESKTOP_WINDOWSAPPSDK_PACKAGE_VERSION};Microsoft.Windows.CppWinRT_${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}")
    set_property(TARGET ${target} PROPERTY VS_USER_PROPS
        "${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.WinUI.props")
    set_property(TARGET ${target} PROPERTY
        VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.19041.0")
    set_target_properties(${target} PROPERTIES
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
endfunction()
