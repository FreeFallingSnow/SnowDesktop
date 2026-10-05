# Test drivers and assertions are native C++. External tools retain their
# implementation languages; no Python discovery/driver enters CTest.
include("${CMAKE_CURRENT_LIST_DIR}/SnowDesktop.PowerShell.cmake")
if(BUILD_TESTING AND WIN32)
    add_library(SnowDesktopBuildToolTestSupport STATIC tests/build_tool_test_support.cpp)
    target_include_directories(SnowDesktopBuildToolTestSupport PUBLIC tests src)
    target_compile_features(SnowDesktopBuildToolTestSupport PUBLIC cxx_std_20)
    target_compile_definitions(SnowDesktopBuildToolTestSupport PUBLIC _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(SnowDesktopBuildToolTestSupport PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    if(MSVC)
        set_property(TARGET SnowDesktopBuildToolTestSupport PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(SnowDesktopBuildToolTestSupport PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_executable(SnowDesktopBuildToolTestStandIn tests/build_tool_test_standin.cpp)
    target_link_libraries(SnowDesktopBuildToolTestStandIn PRIVATE SnowDesktopBuildToolTestSupport)
    if(MSVC)
        set_property(TARGET SnowDesktopBuildToolTestStandIn PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(SnowDesktopBuildToolTestStandIn PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    set_target_properties(SnowDesktopBuildToolTestStandIn PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    macro(snowdesktop_add_native_tool_test target name source labels timeout)
        snowdesktop_add_test(${target} ${name}
            SOURCES "tests/${source}" ${ARGN}
            LINK_LIBRARIES SnowDesktopBuildToolTestSupport winhttp ws2_32
            COMMAND_ARGUMENTS "${CMAKE_CURRENT_SOURCE_DIR}"
            LABELS ${labels})
        add_dependencies(${target} SnowDesktopBuildToolTestStandIn)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${timeout}
            ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass")
    endmacro()
    target_sources(SnowDesktopDeploymentPackagingContractTests PRIVATE tests/release_publication_tests.cpp)
    target_link_libraries(SnowDesktopDeploymentPackagingContractTests PRIVATE SnowDesktopBuildToolTestSupport)
    snowdesktop_add_native_tool_test(SnowDesktopSteamLocalDeployContractTests steam_local_deploy_contract
        steam_local_deploy_contract_tests.cpp "contract;deployment;integration;steam" 60)
    snowdesktop_add_native_tool_test(SnowDesktopDeploymentManifestIntegrationTests deployment_manifest_integration
        deployment_manifest_integration.cpp "build;integration;packaging;winui" 60)
    snowdesktop_add_native_tool_test(SnowDesktopBuildWorkflowTests build_workflow
        build_workflow_tests.cpp "integration;tools" 300
        tests/build_entry_tests.cpp tests/build_shell_recovery_tests.cpp)
    snowdesktop_add_native_tool_test(SnowDesktopBuildPlanExecutionTests build_plan_execution
        build_plan_execution_tests.cpp "tools;core" 120)
    snowdesktop_add_native_tool_test(SnowDesktopBuildDashboardTests build_dashboard
        build_dashboard_tests.cpp "tools;core;retry-isolated-resource" 60)
    add_test(NAME build_dashboard_browser COMMAND SnowDesktopBuildDashboardTests "${CMAKE_CURRENT_SOURCE_DIR}" --browser)
    set_tests_properties(build_dashboard_browser PROPERTIES TIMEOUT 180
        LABELS "manual;integration;tools;retry-isolated-resource"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildDashboardTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildWaitRetryTests build_wait_retry
        build_wait_retry_tests.cpp "integration;tools" 240)
    snowdesktop_add_native_tool_test(SnowDesktopBuildSharedResourcesTests build_shared_resources
        build_shared_resources_tests.cpp "tools;core" 90)
    snowdesktop_add_native_tool_test(SnowDesktopBuildForegroundWaitTests build_foreground_wait
        build_foreground_wait_tests.cpp "tools;integration" 90)
    snowdesktop_add_native_tool_test(SnowDesktopPowerShellRuntimeTests powershell_runtime
        powershell_runtime_tests.cpp "tools;core" 90)
    snowdesktop_add_native_tool_test(SnowDesktopTestSelectionTests test_selection
        test_selection_tests.cpp "core;contract;build" 30)
    snowdesktop_add_native_tool_test(SnowDesktopBuildCollaborationTests build_collaboration
        build_collaboration_tests.cpp "core;contract;build" 180)
endif()
