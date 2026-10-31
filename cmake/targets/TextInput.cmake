add_library(SnowDesktopPointerInput STATIC src/ui/input/private_pointer.cpp src/ui/input/private_pointer.h)
source_group(TREE "${CMAKE_SOURCE_DIR}/src" PREFIX "Source" FILES
    src/ui/input/private_pointer.cpp src/ui/input/private_pointer.h)
target_include_directories(SnowDesktopPointerInput PUBLIC src)
target_compile_features(SnowDesktopPointerInput PUBLIC cxx_std_20)
target_compile_definitions(SnowDesktopPointerInput PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SnowDesktopPointerInput PUBLIC user32)
if (MSVC)
    set_property(TARGET SnowDesktopPointerInput PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopPointerInput PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_library(SnowDesktopTextInput STATIC src/ui/input/text_input_window.cpp src/ui/input/text_input_accessibility.cpp)
target_include_directories(SnowDesktopTextInput PUBLIC src)
target_compile_features(SnowDesktopTextInput PUBLIC cxx_std_20)
target_compile_definitions(SnowDesktopTextInput PRIVATE
    _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SnowDesktopTextInput PUBLIC SnowDesktopPointerInput d2d1 dwrite gdi32 imm32 user32 ole32 oleaut32 uuid uiautomationcore)
if (MSVC)
    set_property(TARGET SnowDesktopTextInput PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopTextInput PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(SnowDesktop PRIVATE SnowDesktopTextInput SnowDesktopPerfetto bcrypt)
