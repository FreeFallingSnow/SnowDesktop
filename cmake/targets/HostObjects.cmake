# Reuse compatible native host implementations across the product and its
# integration driver. XAML/activation sources remain on their executables.
# Existing small runtime libraries keep their independent ownership/config.
get_target_property(host_sources SnowDesktop SOURCES)
get_target_property(host_links SnowDesktop LINK_LIBRARIES)
get_target_property(host_headers SnowDesktop PRECOMPILE_HEADERS)

# One SDK consumer owns platform/reference projections. Native object groups
# only include its headers, so they neither restore NuGet nor generate XAML.
set(host_bindings_directory "${CMAKE_BINARY_DIR}/generated/host-bindings")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/generated/host_bindings_anchor.cpp"
    CONTENT "namespace snowdesktop::build { void HostBindingsAnchor() {} }\n")
add_library(SnowDesktopHostBindings STATIC
    "${CMAKE_BINARY_DIR}/generated/host_bindings_anchor.cpp")
target_compile_features(SnowDesktopHostBindings PRIVATE cxx_std_20)
target_compile_options(SnowDesktopHostBindings PRIVATE /W4 /permissive- /EHsc /utf-8 /Z7)
set_target_properties(SnowDesktopHostBindings PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    VS_PACKAGE_REFERENCES "Microsoft.WindowsAppSDK_${SNOWDESKTOP_WINDOWSAPPSDK_PACKAGE_VERSION};Microsoft.Windows.CppWinRT_${SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION}"
    VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.19041.0"
    VS_GLOBAL_MSBuildProjectExtensionsPath "$(ProjectDir)obj\\$(MSBuildProjectName)\\"
    VS_GLOBAL_GeneratedFilesDir "generated\\host-bindings\\$(Configuration)\\"
    VS_GLOBAL_RootNamespace "SnowDesktopHostBindings"
    VS_GLOBAL_TargetPlatformMinVersion "10.0.19041.0"
    VS_GLOBAL_TargetPlatformVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    VS_GLOBAL_UseWinUI "true"
    VS_GLOBAL_WindowsAppSDKSelfContained "false"
    VS_GLOBAL_WindowsPackageType "None"
    VS_GLOBAL_AppContainerApplication "false"
    VS_GLOBAL_AppxPackage "false"
    VS_GLOBAL_EnablePreviewMsixTooling "false"
    VS_GLOBAL_CppWinRTOptimized "true"
    VS_GLOBAL_CppWinRTRootNamespaceAutoMerge "true"
    VS_GLOBAL_CppWinRTGenerateWindowsMetadata "false"
    VS_GLOBAL_CppWinRTEnableComponentProjection "false")

# The PCH and object groups consume exactly one native compile contract. Do not
# reuse the SDK producer's PCH: NuGet adds target-local compiler metadata.
add_library(SnowDesktopHostBuildSettings INTERFACE)
target_compile_features(SnowDesktopHostBuildSettings INTERFACE cxx_std_20)
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
    get_target_property(value SnowDesktop ${property})
    if(property STREQUAL "INCLUDE_DIRECTORIES")
        list(FILTER value EXCLUDE REGEX "/generated/xaml/")
    endif()
    set_property(TARGET SnowDesktopHostBuildSettings PROPERTY "INTERFACE_${property}" "${value}")
endforeach()
target_include_directories(SnowDesktopHostBuildSettings INTERFACE
    "${host_bindings_directory}/$<CONFIG>")
# Match the GUI host's native definitions and C++/WinRT XAML heap-enforcement
# opt-out, which its SDK targets otherwise append only to XAML projects.
target_compile_definitions(SnowDesktopHostBuildSettings INTERFACE
    _WINDOWS WINRT_NO_MAKE_DETECTION)
target_link_libraries(SnowDesktopHostBuildSettings INTERFACE
    ${host_links} SnowDesktopHostBindings)

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/generated/host_pch_anchor.cpp"
    CONTENT "namespace snowdesktop::build { void HostPchAnchor() {} }\n")
add_library(SnowDesktopHostPch STATIC "${CMAKE_BINARY_DIR}/generated/host_pch_anchor.cpp")
set_property(TARGET SnowDesktopHostPch PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(SnowDesktopHostPch PRIVATE SnowDesktopHostBuildSettings)
target_precompile_headers(SnowDesktopHostPch PRIVATE ${host_headers})

# Preserve existing source ownership. Smaller groups stay on the executable;
# these substantial native groups share compiled objects, not copied lists.
foreach(group IN ITEMS app widget system settings ui platform icons shell widgets core drag_drop)
    set(sources)
    foreach(source IN LISTS host_sources)
        if(source MATCHES "^src/${group}/.*\\.cpp$")
            list(APPEND sources "${source}")
        endif()
    endforeach()
    if(NOT sources)
        message(FATAL_ERROR "Missing native host source group: ${group}")
    endif()
    set(target "SnowDesktopHost_${group}")
    add_library(${target} OBJECT ${sources})
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(${target} PRIVATE SnowDesktopHostBuildSettings SnowDesktopHostPch)
    target_precompile_headers(${target} REUSE_FROM SnowDesktopHostPch)
    source_group(TREE "${CMAKE_SOURCE_DIR}/src" PREFIX "Source" FILES ${sources})
    set_property(GLOBAL APPEND PROPERTY SNOWDESKTOP_HOST_OBJECT_GROUPS "${target}")
endforeach()

function(snowdesktop_use_host_objects target)
    get_target_property(sources ${target} SOURCES)
    get_target_property(links ${target} LINK_LIBRARIES)
    get_property(groups GLOBAL PROPERTY SNOWDESKTOP_HOST_OBJECT_GROUPS)
    foreach(group IN LISTS groups)
        get_target_property(group_sources ${group} SOURCES)
        set(used FALSE)
        foreach(source IN LISTS group_sources)
            if(source IN_LIST sources)
                list(REMOVE_ITEM sources "${source}")
                set(used TRUE)
            endif()
        endforeach()
        if(used AND NOT group IN_LIST links)
            target_link_libraries(${target} PRIVATE ${group})
        endif()
    endforeach()
    set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
endfunction()
snowdesktop_use_host_objects(SnowDesktop)
