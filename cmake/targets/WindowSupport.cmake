add_library(SnowDesktopWindowSupport STATIC
    src/platform/window_support/window_support_installation.cpp
    src/platform/window_support/window_support_host_identity.cpp
    src/platform/window_support/window_support_runtime.cpp
    src/platform/window_support/window_support_security.cpp
    src/platform/window_support/window_support_authorization.cpp)
target_include_directories(SnowDesktopWindowSupport PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(SnowDesktopWindowSupport PUBLIC cxx_std_20)
target_compile_definitions(SnowDesktopWindowSupport PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SnowDesktopWindowSupport PRIVATE /W4 /permissive- /EHsc /utf-8)
set_property(TARGET SnowDesktopWindowSupport PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(SnowDesktopWindowSupport PUBLIC advapi32 bcrypt ole32 oleaut32 shell32 taskschd user32)

# A small, independently authorized HWND broker. No desktop composition root,
# Lua engine, Shell extension loader or arbitrary command executor is linked.
add_executable(SnowDesktopWindowHelper WIN32
    src/platform/window_support/window_helper_main.cpp
    src/dock/dock_window_pin.cpp
    src/dock/dock_external_minimize.cpp
    src/dock/dock_window_helper_animation.cpp
    src/dock/dock_cloak_dispatcher.cpp
    src/dock/dock_window_transition.cpp
    src/ui/render/ui_animation_scheduler.cpp)
target_include_directories(SnowDesktopWindowHelper PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_features(SnowDesktopWindowHelper PRIVATE cxx_std_20)
target_compile_definitions(SnowDesktopWindowHelper PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SnowDesktopWindowHelper PRIVATE /W4 /permissive- /EHsc /utf-8)
set_property(TARGET SnowDesktopWindowHelper PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(SnowDesktopWindowHelper PRIVATE SnowDesktopWindowSupport SnowDesktopPerfetto d2d1 dwmapi dcomp dxgi d3d11)
snowdesktop_add_executable_version_resource(SnowDesktopWindowHelper "SnowDesktop administrator window support")
snowdesktop_enable_native_window_compatibility(SnowDesktopWindowHelper)
add_dependencies(SnowDesktop SnowDesktopWindowHelper)
