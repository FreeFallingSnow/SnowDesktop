cmake_minimum_required(VERSION 3.24)

include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.SharedBuildGuard.cmake")
snowdesktop_check_shared_output()

project(SnowDesktop LANGUAGES C CXX)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.CompileParallelism.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.DependencyEvidence.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.ProductBuild.cmake")

include(cmake/project/MediaDependencies.cmake)

# WinUI 3 XAML compilation and NuGet PackageReference integration are MSBuild
# features. Keeping this requirement explicit avoids silently generating a
# project that can compile the native host but cannot generate the XAML/WinRT
# sources required by the settings UI.
if (NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
    message(FATAL_ERROR
        "SnowDesktop WinUI 3 requires a Visual Studio/MSBuild generator")
endif()

# Included modules retain the root directory scope for paths and generated projects.
include(cmake/project/Identity.cmake)
include(cmake/sources/Host.cmake)
include(cmake/project/RuntimeLibraries.cmake)
include(cmake/project/WindowsHooks.cmake)
include(cmake/project/WinUI.cmake)

include(cmake/targets/Host.cmake)
include(cmake/targets/WindowSupport.cmake)
include(cmake/targets/Launcher.cmake)
snowdesktop_configure_winui(SnowDesktop)

if (TARGET SnowDesktopTaskbarHook)
    add_dependencies(SnowDesktop SnowDesktopTaskbarHook)
endif()
if (TARGET SnowDesktopWallpaperHook)
    add_dependencies(SnowDesktop SnowDesktopWallpaperHook)
    target_compile_definitions(SnowDesktop PRIVATE
        SNOWDESKTOP_ENABLE_WALLPAPER_ENGINE_CAPTURE=1)
endif()

include(cmake/targets/ShellHooks.cmake)
snowdesktop_configure_host(SnowDesktop)

include(cmake/targets/WidgetTool.cmake)
include(cmake/targets/Distribution.cmake)
include(cmake/targets/TextInput.cmake)
include(cmake/testing/Tests.cmake)

snowdesktop_install_shared_guard()
