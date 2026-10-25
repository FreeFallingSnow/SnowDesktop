# Fixed upstream input, offline and byte-verified. Keep upstream amalgamations
# unchanged; reproducibly adapt their two known MSVC narrowing sites below.
set(SNOWDESKTOP_PERFETTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/perfetto")
file(SHA256 "${SNOWDESKTOP_PERFETTO_ROOT}/perfetto.h" SNOWDESKTOP_PERFETTO_HEADER_SHA256)
file(SHA256 "${SNOWDESKTOP_PERFETTO_ROOT}/perfetto.cc" SNOWDESKTOP_PERFETTO_SOURCE_SHA256)
if(NOT SNOWDESKTOP_PERFETTO_HEADER_SHA256 STREQUAL "df97880d56c765a4c51ffe7fd2c71ce4fbb3164677d81185e8488c1516e87470" OR
   NOT SNOWDESKTOP_PERFETTO_SOURCE_SHA256 STREQUAL "cfd4d552272f37da310d43c96c418788567d12174017e2fce061797297391e4c")
    message(FATAL_ERROR "Perfetto v58.2 SDK source integrity check failed")
endif()

file(READ "${SNOWDESKTOP_PERFETTO_ROOT}/perfetto.cc" SNOWDESKTOP_PERFETTO_COMPILER_SOURCE)
set(SNOWDESKTOP_PERFETTO_INDEX_INPUT
    "size_t page_idx;\n  uint8_t chunk_idx;\n  std::tie(page_idx, chunk_idx) = shmem_abi_.GetPageAndChunkIndex(chunk);")
set(SNOWDESKTOP_PERFETTO_LENGTH_INPUT
    "const size_t size_field_len = static_cast<size_t>(*out - size_field_start);")
string(FIND "${SNOWDESKTOP_PERFETTO_COMPILER_SOURCE}" "${SNOWDESKTOP_PERFETTO_INDEX_INPUT}" SNOWDESKTOP_PERFETTO_INDEX_POSITION)
string(FIND "${SNOWDESKTOP_PERFETTO_COMPILER_SOURCE}" "${SNOWDESKTOP_PERFETTO_LENGTH_INPUT}" SNOWDESKTOP_PERFETTO_LENGTH_POSITION)
if(SNOWDESKTOP_PERFETTO_INDEX_POSITION LESS 0 OR SNOWDESKTOP_PERFETTO_LENGTH_POSITION LESS 0)
    message(FATAL_ERROR "Perfetto v58.2 compatibility inputs changed; review before updating")
endif()
# GetPageAndChunkIndex returns pair<size_t,size_t>, and GetChunkState accepts
# size_t for both indexes. Retain the actual index type instead of narrowing it.
string(REPLACE "${SNOWDESKTOP_PERFETTO_INDEX_INPUT}"
    "size_t page_idx;\n  size_t chunk_idx;\n  std::tie(page_idx, chunk_idx) = shmem_abi_.GetPageAndChunkIndex(chunk);"
    SNOWDESKTOP_PERFETTO_COMPILER_SOURCE "${SNOWDESKTOP_PERFETTO_COMPILER_SOURCE}")
# AppendLenDelim writes a uint32_t length as a varint (at most five bytes).
# Express that bounded length in the uint32_t type returned by this function.
string(REPLACE "${SNOWDESKTOP_PERFETTO_LENGTH_INPUT}"
    "const auto size_field_len = static_cast<uint32_t>(*out - size_field_start);"
    SNOWDESKTOP_PERFETTO_COMPILER_SOURCE "${SNOWDESKTOP_PERFETTO_COMPILER_SOURCE}")
set(SNOWDESKTOP_PERFETTO_COMPILER_FILE "${CMAKE_BINARY_DIR}/generated/perfetto/perfetto.cc")
set(SNOWDESKTOP_PERFETTO_EXISTING_SOURCE "")
if(EXISTS "${SNOWDESKTOP_PERFETTO_COMPILER_FILE}")
    file(READ "${SNOWDESKTOP_PERFETTO_COMPILER_FILE}" SNOWDESKTOP_PERFETTO_EXISTING_SOURCE)
endif()
if(NOT SNOWDESKTOP_PERFETTO_EXISTING_SOURCE STREQUAL SNOWDESKTOP_PERFETTO_COMPILER_SOURCE)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/perfetto")
    file(WRITE "${SNOWDESKTOP_PERFETTO_COMPILER_FILE}" "${SNOWDESKTOP_PERFETTO_COMPILER_SOURCE}")
endif()

add_library(SnowDesktopPerfettoSdk STATIC "${SNOWDESKTOP_PERFETTO_COMPILER_FILE}")
target_include_directories(SnowDesktopPerfettoSdk PUBLIC "${SNOWDESKTOP_PERFETTO_ROOT}")
target_compile_features(SnowDesktopPerfettoSdk PUBLIC cxx_std_17)
target_compile_definitions(SnowDesktopPerfettoSdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(SnowDesktopPerfettoSdk PUBLIC ws2_32)
add_library(SnowDesktopPerfetto STATIC
    src/diagnostics/perfetto_categories.cpp
    src/diagnostics/perfetto_session.cpp
    src/diagnostics/performance_trace.cpp)
target_include_directories(SnowDesktopPerfetto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SnowDesktopPerfetto PUBLIC cxx_std_20)
target_link_libraries(SnowDesktopPerfetto PUBLIC SnowDesktopPerfettoSdk)
if(MSVC)
    set_property(TARGET SnowDesktopPerfettoSdk SnowDesktopPerfetto PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SnowDesktopPerfettoSdk PRIVATE /bigobj /permissive- /EHsc /utf-8)
    target_compile_options(SnowDesktopPerfetto PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(SnowDesktopPerfetto PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Actual translation units that emit probes. Only targets consuming these
# sources link the SDK; unrelated unit tests keep their existing dependencies.
set(SNOWDESKTOP_PERFETTO_PROBE_SOURCES
    src/app/dock/app_dock_window_tracking.cpp
    src/app/dock/app_floating_dock_render.cpp
    src/app/dock/app_floating_dock_window.cpp
    src/app/lifecycle/app_message_dispatch.cpp
    src/app/lifecycle/app_run.cpp
    src/app/popup/app_floating_popup_window.cpp
    src/app/popup/app_popup_geometry.cpp
    src/app/popup/app_popup_lifecycle.cpp
    src/app/render/app_composition_animation_overlay.cpp
    src/app/render/app_icon_loader.cpp
    src/app/render/app_paint.cpp
    src/app/render/app_scene_render.cpp
    src/app/render/desktop_backdrop_compositor.cpp
    src/app/shell/app_desktop_reload.cpp
    src/app/shell/app_shell_file_operation.cpp
    src/app/widgets/app_widget_composition.cpp
    src/diagnostics/performance_capture.cpp
    src/diagnostics/performance_control_pipe.cpp
    src/dock/dock_window_transition.cpp
    src/shell/shell_extension_catalogue.cpp
    src/shell/shell_extension_service.cpp
    src/system/status_bar/status_bar.cpp
    src/widget/data/widget_audio_analysis_provider.cpp
    src/widget/data/widget_system_data_provider.cpp
    src/widget/runtime/lua_runtime.cpp
    src/widget/runtime/widget_engine.cpp
    src/widget/runtime/widget_lua_lifecycle.cpp
    src/widget/view/widget_view_lua.cpp
    src/winui/settings_window_host.cpp
)
