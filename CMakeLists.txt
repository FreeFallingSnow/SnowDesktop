cmake_minimum_required(VERSION 3.24)

project(SnowDesktop LANGUAGES CXX)

add_executable(SnowDesktop WIN32
    src/main.cpp
)

target_compile_features(SnowDesktop PRIVATE cxx_std_20)

if (MSVC)
    target_compile_options(SnowDesktop PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(SnowDesktop PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(SnowDesktop PRIVATE -Wall -Wextra -pedantic)
    target_compile_definitions(SnowDesktop PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_link_libraries(SnowDesktop PRIVATE
    comctl32
    gdi32
    msimg32
    ole32
    shell32
    shlwapi
    user32
    uuid
)
