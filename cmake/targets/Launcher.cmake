# Error messages must survive a missing or damaged distribution/lang tree.
# Keep lang/*.json as the only translation source, embedded at configure time.
file(GLOB SNOWDESKTOP_LAUNCHER_CATALOGS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lang/*.json")
set(SNOWDESKTOP_LAUNCHER_MESSAGES "#pragma once\nstruct LauncherMessage { const char* language; const wchar_t* message; const wchar_t* pending; const wchar_t* warning; const wchar_t* startup; const wchar_t* securitySoftwareHint; const wchar_t* startupPrivileges; const wchar_t* startAnyway; const wchar_t* cancel; };\ninline constexpr LauncherMessage kLauncherMessages[] = {\n")
foreach(catalog IN LISTS SNOWDESKTOP_LAUNCHER_CATALOGS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${catalog}")
    get_filename_component(language "${catalog}" NAME_WE)
    file(READ "${catalog}" catalog_json)
    string(JSON message GET "${catalog_json}" "app.launcher.failed")
    string(JSON pending GET "${catalog_json}" "app.launcher.pending")
    string(JSON warning GET "${catalog_json}" "app.launcher.warning")
    string(JSON startup GET "${catalog_json}" "app.operation.startFailed")
    string(JSON security_hint GET "${catalog_json}" "app.launcher.securitySoftwareHint")
    string(JSON startup_privileges GET "${catalog_json}" "app.startup.privilegesFailed")
    string(JSON start_anyway GET "${catalog_json}" "app.startup.startAnyway")
    string(JSON cancel GET "${catalog_json}" "settings.dialog.cancel")
    string(APPEND SNOWDESKTOP_LAUNCHER_MESSAGES "{\"${language}\", LR\"LAUNCHER(${message})LAUNCHER\", LR\"LAUNCHER(${pending})LAUNCHER\", LR\"LAUNCHER(${warning})LAUNCHER\", LR\"LAUNCHER(${startup})LAUNCHER\", LR\"LAUNCHER(${security_hint})LAUNCHER\", LR\"LAUNCHER(${startup_privileges})LAUNCHER\", LR\"LAUNCHER(${start_anyway})LAUNCHER\", LR\"LAUNCHER(${cancel})LAUNCHER\"},\n")
endforeach()
string(APPEND SNOWDESKTOP_LAUNCHER_MESSAGES "};\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_messages.h"
    CONTENT "${SNOWDESKTOP_LAUNCHER_MESSAGES}" @ONLY)

add_executable(SnowDesktopLauncher WIN32
    src/steam/steam_launcher_main.cpp
    src/steam/steam_runtime_context.cpp
    src/steam/steam_runtime_manager.cpp
    src/steam/steam_runtime_manager.h
    src/steam/steam_runtime_context.h
    src/resources/app_icon.rc
    "${SNOWDESKTOP_LAUNCHER_VERSION_RESOURCE}")
target_compile_features(SnowDesktopLauncher PRIVATE cxx_std_20)
target_include_directories(SnowDesktopLauncher PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(SnowDesktopLauncher PRIVATE bcrypt shell32 user32 ole32 oleaut32 advapi32 comctl32)
if (MSVC)
    target_link_options(SnowDesktopLauncher PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    set_property(TARGET SnowDesktopLauncher PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopLauncher PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(SnowDesktopLauncher PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(SnowDesktopLauncher PRIVATE
        -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktopLauncher PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
