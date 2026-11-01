set(SNOWDESKTOP_WIDGET_SKILL_MANIFEST
    "${CMAKE_SOURCE_DIR}/widgets/snowdesktop-lua-widget/skill.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SNOWDESKTOP_WIDGET_SKILL_MANIFEST}")
file(READ "${SNOWDESKTOP_WIDGET_SKILL_MANIFEST}"
    SNOWDESKTOP_WIDGET_SKILL_MANIFEST_JSON)
string(JSON SNOWDESKTOP_WIDGET_SKILL_REVISION GET
    "${SNOWDESKTOP_WIDGET_SKILL_MANIFEST_JSON}" revision)

add_executable(snowwidget
    tools/snowwidget/main.cpp
    tools/snowwidget/gpu_diagnostics.cpp
    src/widget/data/widget_gpu_sampler.cpp
    src/widget/authoring/widget_author_lint.cpp
    src/widget/authoring/widget_author_permissions.cpp
    src/widget/authoring/widget_author_test.cpp
    src/widget/api/widget_api_contract_json.cpp
    src/widget/api/widget_api_registry.cpp
    src/widget/runtime/widget_logical_slot.cpp
    src/widget/runtime/lua_runtime.cpp
    src/widget/packages/widget_package.cpp
    src/data/atomic_file.cpp
    src/widget/permissions/widget_permission_broker.cpp
    src/widget/permissions/widget_permission_state.cpp
    src/widget/api/widget_system_contract_json.cpp
    src/widget/api/widget_view_contract.cpp
    src/widget/api/widget_view_contract_json.cpp)
target_compile_features(snowwidget PRIVATE cxx_std_20)
snowdesktop_add_executable_version_resource(snowwidget "SnowDesktop widget authoring tool")
target_include_directories(snowwidget PRIVATE src)
target_link_libraries(snowwidget PRIVATE SnowDesktopLua SnowDesktopPerfetto bcrypt ole32 pdh dxgi)
target_compile_definitions(snowwidget PRIVATE
    SNOWDESKTOP_VERSION="${SNOWDESKTOP_VERSION}"
    SNOWDESKTOP_WIDGET_SKILL_REVISION=${SNOWDESKTOP_WIDGET_SKILL_REVISION})
if (MSVC)
    set_property(TARGET snowwidget PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(snowwidget PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(snowwidget PRIVATE
        _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(snowwidget PRIVATE -Wall -Wextra -pedantic)
endif()
