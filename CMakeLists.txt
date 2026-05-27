cmake_minimum_required(VERSION 3.24)

project(SnowDesktop LANGUAGES CXX)

add_executable(SnowDesktop WIN32
    src/main.cpp
    src/app.cpp
    src/utils.cpp
    src/settings_window.cpp
    src/resource.rc
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)

target_compile_features(SnowDesktop PRIVATE cxx_std_20)

target_include_directories(SnowDesktop PRIVATE
    third_party/spdlog/include
    third_party/imgui
    third_party/imgui/backends
)

add_custom_command(TARGET SnowDesktop PRE_BUILD
    COMMAND cmd /c "taskkill /f /im SnowDesktop.exe 2>nul & exit 0"
    COMMENT "Stopping running SnowDesktop.exe before build..."
)

if (MSVC)
    target_compile_options(SnowDesktop PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(SnowDesktop PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_options(SnowDesktop PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
else()
    target_compile_options(SnowDesktop PRIVATE -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktop PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(SnowDesktop PRIVATE
    comctl32
    d2d1
    d3d11
    dcomp
    dwrite
    dxgi
    gdi32
    msimg32
    ole32
    shell32
    shlwapi
    user32
    windowscodecs
    uuid
)
