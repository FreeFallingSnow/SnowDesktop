include(CTest)
if (BUILD_TESTING)
    set(SNOWDESKTOP_TEST_TARGETS)
    set(SNOWDESKTOP_CORE_TEST_TARGETS)
    set(SNOWDESKTOP_FAST_TEST_TARGETS)
    set(SNOWDESKTOP_APP_TEST_TARGETS)
    set(SNOWDESKTOP_TOOL_TEST_TARGETS)

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/testing/Support.cmake")

    # Admitted module contracts complement the host behavior matrix. Each
    # target is isolated because it owns a language runtime, store, pipe server,
    # or external tool process; no helper-per-program expansion is retained.
    snowdesktop_add_test(
        SnowDesktopHttpSecurityTests
        http_security
        SOURCES
            tests/http_security_tests.cpp
            src/platform/http_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES shlwapi winhttp ws2_32
        LABELS core contract security)

    snowdesktop_add_test(
        SnowDesktopLuaRuntimeTests
        lua_runtime
        SOURCES
            tests/lua_runtime_tests.cpp
            src/widget/runtime/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS core contract lua widget)

    snowdesktop_add_test(
        SnowDesktopWidgetFilesystemHandleStoreTests
        widget_filesystem_handle_store
        SOURCES
            tests/widget_filesystem_handle_store_tests.cpp
            tests/drag_drop/lua_file_delivery_cases.cpp
            src/widget/tasks/widget_filesystem_handle_store.cpp
            src/widget/view/widget_interaction_region.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
            src/widget/runtime/widget_lua_lifecycle.cpp
            src/widget/runtime/lua_runtime.cpp
            src/data/atomic_file.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt SnowDesktopLua
        LABELS contract filesystem security task lua widget)

    snowdesktop_add_test(
        SnowDesktopLocalizationContractTests
        localization_contract
        SOURCES
            tests/localization_contract_tests.cpp
            src/common/l10n.cpp
        INCLUDE_DIRECTORIES src
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS core contract l10n)

    snowdesktop_add_test(
        SnowDesktopApplicationControlTests
        application_control
        SOURCES tests/platform/app_control_ipc_tests.cpp
            tests/platform/control_elements_tests.cpp
            src/platform/control/control_protocol.cpp
            src/platform/control/control_server.cpp
            src/platform/control/control_elements.cpp
            src/ui/input/control_pointer.cpp
            src/ui/render/demo_cursor.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32 advapi32 gdi32 bcrypt
        LABELS contract integration ipc)

    snowdesktop_add_test(
        SnowDesktopDeploymentPackagingContractTests
        deployment_packaging_contract
        SOURCES tests/deployment_packaging_contract_tests.cpp
        COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS tools integration packaging)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorTestRunnerTests
        widget_author_test_runner
        SOURCES
            tests/widget_author_test_tests.cpp
            src/widget/authoring/widget_author_test.cpp
            src/widget/runtime/lua_runtime.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua
        LABELS authoring contract lua security widget)

    snowdesktop_add_test(
        SnowDesktopWidgetAuthorToolsTests
        widget_author_tools
        SOURCES
            tests/widget_author_tools_tests.cpp
            tests/widget_package_review_tests.cpp
            src/widget/authoring/widget_author_permissions.cpp
            src/widget/api/widget_api_registry.cpp
            src/widget/runtime/widget_logical_slot.cpp
            src/widget/packages/widget_package.cpp
            src/data/atomic_file.cpp
            src/widget/permissions/widget_permission_broker.cpp
            src/widget/permissions/widget_permission_state.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES SnowDesktopLua bcrypt ole32
        LABELS authoring contract data permission security widget)

    snowdesktop_add_test(
        SnowDesktopWidgetStorageTransactionTests
        widget_storage_transaction
        SOURCES
            tests/widget_storage_transaction_tests.cpp
            src/widget/storage/widget_storage_transaction.cpp
            src/widget/storage/widget_storage_value.cpp
        INCLUDE_DIRECTORIES src
        LABELS contract lua storage widget)

    snowdesktop_add_test(
        SnowDesktopWidgetSecretStoreTests
        widget_secret_store
        SOURCES
            tests/widget_secret_store_tests.cpp
            tests/widget/secret_store_legacy_fixture.cpp
            tests/widget/secret_store_legacy_fixture.h
            src/widget/storage/widget_secret_store.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES bcrypt crypt32
        LABELS contract lua security storage widget)

    # Real loopback transport protects cancellation/retry/cache boundaries that
    # URL admission alone cannot exercise; the origin owns an ephemeral port.
    add_test(NAME http_network_integration
        COMMAND SnowDesktopHttpSecurityTests --network)
    set_tests_properties(http_network_integration PROPERTIES
        TIMEOUT 60 REQUIRED_FILES "$<TARGET_FILE:SnowDesktopHttpSecurityTests>"
        LABELS "integration;network;security")
    add_test(NAME lua_file_delivery_contract
        COMMAND SnowDesktopWidgetFilesystemHandleStoreTests --file-drop-delivery-tests)
    set_tests_properties(lua_file_delivery_contract PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWidgetFilesystemHandleStoreTests>"
        TIMEOUT 30 LABELS "integration;filesystem;permission;lua;widget")
    add_test(NAME widget_package_file_export
        COMMAND SnowDesktopWidgetAuthorToolsTests --package-export)
    set_tests_properties(widget_package_file_export PROPERTIES
        TIMEOUT 30 LABELS "authoring;contract;data;security;widget"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopWidgetAuthorToolsTests>")

    foreach(marker 1 2)
        add_library(SnowDesktopHookEntryFixture${marker} SHARED tests/platform/hook_entry_fixture.cpp)
        target_compile_features(SnowDesktopHookEntryFixture${marker} PRIVATE cxx_std_20)
        target_compile_definitions(SnowDesktopHookEntryFixture${marker} PRIVATE HOOK_ENTRY_FIXTURE_MARKER=${marker} UNICODE _UNICODE)
        target_compile_options(SnowDesktopHookEntryFixture${marker} PRIVATE /W4 /EHsc /utf-8)
        target_link_libraries(SnowDesktopHookEntryFixture${marker} PRIVATE user32)
        set_property(TARGET SnowDesktopHookEntryFixture${marker} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        set_target_properties(SnowDesktopHookEntryFixture${marker} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    endforeach()
    snowdesktop_add_test(
        SnowDesktopHookEntryTests
        hook_stable_entry
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES tests/platform/hook_entry_tests.cpp src/platform/hook_entry_deployment.cpp
        INCLUDE_DIRECTORIES src
        LINK_LIBRARIES user32 bcrypt
        COMMAND_ARGUMENTS "$<TARGET_FILE:SnowDesktopHookEntry>"
            "$<TARGET_FILE:SnowDesktopHookEntryFixture1>" "$<TARGET_FILE:SnowDesktopHookEntryFixture2>"
        LABELS core integration deployment)
    add_dependencies(SnowDesktopHookEntryTests SnowDesktopHookEntry SnowDesktopHookEntryFixture1 SnowDesktopHookEntryFixture2)
    add_dependencies(SnowDesktopHookEntryTests SnowDesktopDockMinimize32)
    add_test(NAME hook_stable_entry32 COMMAND SnowDesktopHookEntryTests --run32
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryTests32.exe"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/SnowDesktop.Runtime/SnowDesktopHookEntry32.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_1.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_2.dll")
    set_tests_properties(hook_stable_entry32 PROPERTIES LABELS "core;integration;deployment" TIMEOUT 30
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopHookEntryTests>")
    add_test(NAME hook_stable_entry_cross32 COMMAND SnowDesktopHookEntryTests --cross32
        "${CMAKE_BINARY_DIR}/$<CONFIG>/SnowDesktop.Runtime/SnowDesktopHookEntry32.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_1.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryFixture32_2.dll"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/SnowDesktopHookEntryTests32.exe")
    set_tests_properties(hook_stable_entry_cross32 PROPERTIES LABELS "core;integration;deployment" TIMEOUT 30
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopHookEntryTests>")


    # Execute real native containers and DesktopApp routing without Run(),
    # desktop HWND creation or desktop automation. The virtual methods reference the
    # host graph, so link its production sources rather than fake container
    # methods. Keep XAML generation and runtime deployment in separate trees.
    set(SNOWDESKTOP_NATIVE_DRAG_SOURCES ${SNOWDESKTOP_SOURCES})
    list(REMOVE_ITEM SNOWDESKTOP_NATIVE_DRAG_SOURCES src/main.cpp)
    snowdesktop_add_test(
        SnowDesktopNativeDragIntegrationTests
        behavior_sync_copy
        NATIVE_WINDOW_COMPATIBILITY
        SOURCES
            tests/behavior/core_interactions_tests.cpp
            ${SNOWDESKTOP_NATIVE_DRAG_SOURCES}
        COMMAND_ARGUMENTS sync-copy
        LABELS core integration isolated-native-model interaction)
    snowdesktop_configure_native_test_host(SnowDesktopNativeDragIntegrationTests)
    foreach(row IN ITEMS copy move default-move link batch unsupported cancel interleaved failed-first settings recovery search widget dock
        settings-host manifest group-type-contract
        container-collection container-files container-mapping container-collection-group container-file-group container-mapping-group
        member-collection member-files member-reorder right-copy right-cancel target-replaced target-remapped)
        add_test(NAME behavior_${row} COMMAND SnowDesktopNativeDragIntegrationTests ${row})
        set_tests_properties(behavior_${row} PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopNativeDragIntegrationTests>"
            TIMEOUT 60 PROCESSORS 3 LABELS "core;integration;isolated-native-model;behavior")
    endforeach()
    set_tests_properties(behavior_sync_copy PROPERTIES TIMEOUT 60 PROCESSORS 3)
    set_tests_properties(behavior_dock PROPERTIES RESOURCE_LOCK snowdesktop_desktop)

    include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.ToolTests.cmake")
    snowdesktop_add_native_tool_test(SnowDesktopBuildProductArtifactsTests build_product_artifacts
        build_tools/build_product_artifacts_tests.cpp "tools;integration;packaging" 120)
    # Shared desktop/Explorer/host resources are serialized. Isolated fixtures
    # and pure logic cases can execute alongside these tests.
    get_property(SNOWDESKTOP_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS SNOWDESKTOP_REGISTERED_TESTS)
        get_test_property(${test_name} LABELS test_labels)
        # These tests exercise a host graph or launch tool subprocesses. Budget
        # their CPU demand independently of the global CTest job count; running
        # 20 native hosts alongside tool workflows saturated the 24-core test
        # machine and caused timeouts, while eight heavy natives passed.
        if("isolated-native-model" IN_LIST test_labels OR "tools" IN_LIST test_labels)
            get_property(test_has_processor_budget TEST ${test_name} PROPERTY PROCESSORS SET)
            if(NOT test_has_processor_budget)
                set_tests_properties(${test_name} PROPERTIES PROCESSORS 3)
            endif()
        endif()
        if(("shell" IN_LIST test_labels OR "window" IN_LIST test_labels OR
            "ui" IN_LIST test_labels OR "host-runtime" IN_LIST test_labels)
            AND NOT "tools" IN_LIST test_labels
            AND NOT "isolated-native-model" IN_LIST test_labels)
            set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK snowdesktop_desktop)
        endif()
    endforeach()
    add_custom_target(
        SnowDesktopCoreTests
        DEPENDS ${SNOWDESKTOP_CORE_TEST_TARGETS})

    add_custom_target(
        SnowDesktopFastTests
        DEPENDS ${SNOWDESKTOP_FAST_TEST_TARGETS})
    add_custom_target(
        SnowDesktopBuildToolTests
        DEPENDS ${SNOWDESKTOP_TOOL_TEST_TARGETS})
    add_custom_target(
        SnowDesktopAppTests
        COMMAND "${CMAKE_COMMAND}" -E env "PSExecutionPolicyPreference=Bypass" "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile
            -File "${CMAKE_SOURCE_DIR}/scripts/arrange_build_output.ps1"
            -BuildOutput "${CMAKE_BINARY_DIR}/$<CONFIG>"
            -AllowMissingFirstPartyRuntime
        DEPENDS ${SNOWDESKTOP_APP_TEST_TARGETS})
    add_custom_target(
        SnowDesktopTests
        COMMAND "${CMAKE_COMMAND}" -E env "PSExecutionPolicyPreference=Bypass" "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile
            -File "${CMAKE_SOURCE_DIR}/scripts/arrange_build_output.ps1"
            -BuildOutput "${CMAKE_BINARY_DIR}/$<CONFIG>"
            -AllowMissingFirstPartyRuntime
        DEPENDS ${SNOWDESKTOP_TEST_TARGETS})
endif()
