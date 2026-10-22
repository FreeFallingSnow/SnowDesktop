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
    snowdesktop_add_native_tool_test(SnowDesktopTestDependencyTests test_dependencies
        build_tools/build_test_dependency_tests.cpp "tools;core;integration" 120)
    target_sources(SnowDesktopTestDependencyTests PRIVATE tests/build_tools/build_source_report_tests.cpp)
    add_test(NAME test_dependency_reports COMMAND SnowDesktopTestDependencyTests "${CMAKE_CURRENT_SOURCE_DIR}" --source-reports)
    set_tests_properties(test_dependency_reports PROPERTIES TIMEOUT 180 LABELS "tools;core;integration"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopTestDependencyTests>")
    add_test(NAME build_input_identity COMMAND SnowDesktopBuildWorkflowTests "${CMAKE_CURRENT_SOURCE_DIR}" --input-identity)
    set_tests_properties(build_input_identity PROPERTIES TIMEOUT 45 LABELS "tools;core;contract"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildWorkflowTests>")
    add_test(NAME build_commit_lifecycle COMMAND SnowDesktopBuildWorkflowTests "${CMAKE_CURRENT_SOURCE_DIR}" --commit-lifecycle)
    set_tests_properties(build_commit_lifecycle PROPERTIES TIMEOUT 120 LABELS "tools;core;integration"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildWorkflowTests>")
    add_test(NAME build_wait_progress COMMAND SnowDesktopBuildWorkflowTests "${CMAKE_CURRENT_SOURCE_DIR}" --wait-progress)
    set_tests_properties(build_wait_progress PROPERTIES TIMEOUT 90 LABELS "tools;integration"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildWorkflowTests>")
    add_test(NAME build_git_wait COMMAND SnowDesktopBuildWorkflowTests "${CMAKE_CURRENT_SOURCE_DIR}" --git-wait)
    set_tests_properties(build_git_wait PROPERTIES TIMEOUT 90 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildWorkflowTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildPlanExecutionTests build_plan_execution
        build_plan_execution_tests.cpp "tools;core" 120)
    target_sources(SnowDesktopBuildPlanExecutionTests PRIVATE tests/build_tools/build_records_tests.cpp tests/build_tools/build_verification_tests.cpp)
    foreach(record IN ITEMS evidence acceptance)
        add_test(NAME build_${record} COMMAND SnowDesktopBuildPlanExecutionTests
            "${CMAKE_CURRENT_SOURCE_DIR}" --${record})
        set_tests_properties(build_${record} PROPERTIES TIMEOUT 180 LABELS "tools;integration"
            ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildPlanExecutionTests>")
    endforeach()
    add_test(NAME build_verification COMMAND SnowDesktopBuildPlanExecutionTests "${CMAKE_CURRENT_SOURCE_DIR}" --verification)
    set_tests_properties(build_verification PROPERTIES TIMEOUT 240 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildPlanExecutionTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildDashboardTests build_dashboard
        build_dashboard_tests.cpp "tools;core;retry-isolated-resource" 60)
    add_test(NAME build_dashboard_startup COMMAND SnowDesktopBuildDashboardTests "${CMAKE_CURRENT_SOURCE_DIR}" --startup)
    set_tests_properties(build_dashboard_startup PROPERTIES TIMEOUT 60 LABELS "tools;core;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildDashboardTests>")
    add_test(NAME build_dashboard_browser COMMAND SnowDesktopBuildDashboardTests "${CMAKE_CURRENT_SOURCE_DIR}" --browser)
    set_tests_properties(build_dashboard_browser PROPERTIES TIMEOUT 180
        LABELS "manual;integration;tools;retry-isolated-resource"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildDashboardTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildWaitRetryTests build_wait_retry
        build_wait_retry_tests.cpp "integration;tools" 240)
    add_test(NAME build_retry_ctest_selection COMMAND SnowDesktopBuildWaitRetryTests "${CMAKE_CURRENT_SOURCE_DIR}" --ctest-selection)
    set_tests_properties(build_retry_ctest_selection PROPERTIES TIMEOUT 90 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildWaitRetryTests>")
    add_test(NAME build_retry_repairs COMMAND SnowDesktopBuildWaitRetryTests "${CMAKE_CURRENT_SOURCE_DIR}" --repairs)
    set_tests_properties(build_retry_repairs PROPERTIES TIMEOUT 180 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildWaitRetryTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildSharedResourcesTests build_shared_resources
        build_shared_resources_tests.cpp "tools;core" 90)
    snowdesktop_add_native_tool_test(SnowDesktopBuildForegroundWaitTests build_foreground_wait
        build_foreground_wait_tests.cpp "tools;integration" 90)
    snowdesktop_add_native_tool_test(SnowDesktopPowerShellRuntimeTests powershell_runtime
        powershell_runtime_tests.cpp "tools;core" 90)
    snowdesktop_add_native_tool_test(SnowDesktopTestSelectionTests test_selection
        test_selection_tests.cpp "tools;core;contract;build" 30)
    snowdesktop_add_native_tool_test(SnowDesktopBuildCollaborationTests build_collaboration
        build_collaboration_tests.cpp "tools;core;contract;build" 180)
    # Three independent fixture groups run concurrently within this one test.
    # Reserve their combined budget rather than multiplying hidden parallelism.
    set_tests_properties(build_collaboration PROPERTIES PROCESSORS 9)
    snowdesktop_add_native_tool_test(SnowDesktopDshCollaborationTests dsh_collaboration
        build_tools/dsh_collaboration_tests.cpp "tools;core;integration" 90)
    add_test(NAME dsh_installed_sdk COMMAND SnowDesktopDshCollaborationTests "${CMAKE_CURRENT_SOURCE_DIR}" --installed-sdk)
    set_tests_properties(dsh_installed_sdk PROPERTIES TIMEOUT 60 LABELS "manual;integration;tools"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopDshCollaborationTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildRunTests build_run
        build_tools/build_run_tests.cpp "tools;core;integration" 180)
    foreach(run_case IN ITEMS plans batch-lifetime default-entry process-lifetime)
        string(REPLACE "-" "_" run_name "${run_case}")
        add_test(NAME build_run_${run_name} COMMAND SnowDesktopBuildRunTests "${CMAKE_CURRENT_SOURCE_DIR}" --${run_case})
        set_tests_properties(build_run_${run_name} PROPERTIES TIMEOUT 120 LABELS "tools;core;integration"
            ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildRunTests>")
    endforeach()
    snowdesktop_add_native_tool_test(SnowDesktopBuildResourceBudgetTests build_resource_budget
        build_tools/build_resource_budget_tests.cpp "tools;integration" 240)
    set_tests_properties(build_resource_budget PROPERTIES PROCESSORS 6)
    add_test(NAME build_resource_fairness COMMAND SnowDesktopBuildResourceBudgetTests "${CMAKE_CURRENT_SOURCE_DIR}" --fairness)
    set_tests_properties(build_resource_fairness PROPERTIES TIMEOUT 180 PROCESSORS 6 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildResourceBudgetTests>")
    add_test(NAME build_resource_budget_compile COMMAND SnowDesktopBuildResourceBudgetTests "${CMAKE_CURRENT_SOURCE_DIR}" --compile)
    set_tests_properties(build_resource_budget_compile PROPERTIES TIMEOUT 240 PROCESSORS 9 LABELS "manual;tools;integration"
        RESOURCE_LOCK snowdesktop_compiler_budget
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildResourceBudgetTests>")
    add_test(NAME build_project_resources COMMAND SnowDesktopBuildRunTests "${CMAKE_CURRENT_SOURCE_DIR}" --project-resources)
    set_tests_properties(build_project_resources PROPERTIES TIMEOUT 90 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildRunTests>")
    add_test(NAME build_run_observer COMMAND SnowDesktopBuildRunTests "${CMAKE_CURRENT_SOURCE_DIR}" --observer-snapshot)
    set_tests_properties(build_run_observer PROPERTIES TIMEOUT 90 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildRunTests>")
    add_test(NAME build_request_publication COMMAND SnowDesktopBuildRunTests
        "${CMAKE_CURRENT_SOURCE_DIR}" --request-publication)
    set_tests_properties(build_request_publication PROPERTIES TIMEOUT 90
        LABELS "tools;core;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildRunTests>")
    snowdesktop_add_native_tool_test(SnowDesktopBuildStateTransactionTests build_state_transaction
        build_tools/build_state_transaction_tests.cpp "tools;core;integration" 60)
    snowdesktop_add_native_tool_test(SnowDesktopBuildTelemetryTests build_telemetry
        build_tools/build_telemetry_tests.cpp "tools;integration" 180)
    snowdesktop_add_native_tool_test(SnowDesktopBuildNoopTests build_noop_outputs
        build_tools/build_noop_tests.cpp "tools;integration" 240)
    set_tests_properties(build_noop_outputs PROPERTIES PROCESSORS 4)
    snowdesktop_add_native_tool_test(SnowDesktopBuildCandidateTests build_candidate
        build_tools/build_candidate_tests.cpp "tools;integration" 180)
    set_tests_properties(build_candidate PROPERTIES PROCESSORS 4)
    snowdesktop_add_native_tool_test(SnowDesktopBuildActivationTests build_activation
        build_tools/build_activation_tests.cpp "tools;integration" 120)
    target_link_libraries(SnowDesktopBuildActivationTests PRIVATE user32)
    snowdesktop_add_native_tool_test(SnowDesktopBuildCandidateExecutionTests build_candidate_execution
        build_tools/build_candidate_execution_tests.cpp "tools;integration" 300)
    set_tests_properties(build_candidate_execution PROPERTIES PROCESSORS 4)
    foreach(control checkout_lock source_lock)
        string(REPLACE "_" "-" control_option "${control}")
        add_test(NAME build_candidate_${control} COMMAND SnowDesktopBuildCandidateExecutionTests "${CMAKE_CURRENT_SOURCE_DIR}" --verify-${control_option})
        set_tests_properties(build_candidate_${control} PROPERTIES TIMEOUT 100 LABELS "tools;integration" PROCESSORS 4
            ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
            REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildCandidateExecutionTests>")
    endforeach()
    add_test(NAME build_candidate_batches COMMAND SnowDesktopBuildCandidateExecutionTests "${CMAKE_CURRENT_SOURCE_DIR}" --batches)
    add_test(NAME build_candidate_tool_verification COMMAND SnowDesktopBuildCandidateExecutionTests "${CMAKE_CURRENT_SOURCE_DIR}" --tool-verification)
    set_tests_properties(build_candidate_tool_verification PROPERTIES TIMEOUT 180 LABELS "tools;integration" PROCESSORS 4
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildCandidateExecutionTests>")
    set_tests_properties(build_candidate_batches PROPERTIES TIMEOUT 240 LABELS "tools;integration" PROCESSORS 4
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildCandidateExecutionTests>")
    add_test(NAME build_candidate_artifacts COMMAND SnowDesktopBuildCandidateExecutionTests "${CMAKE_CURRENT_SOURCE_DIR}" --artifacts)
    set_tests_properties(build_candidate_artifacts PROPERTIES TIMEOUT 240 LABELS "tools;integration" PROCESSORS 4
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildCandidateExecutionTests>")
    add_test(NAME build_candidate_preflight COMMAND SnowDesktopBuildCandidateExecutionTests "${CMAKE_CURRENT_SOURCE_DIR}" --preflight)
    set_tests_properties(build_candidate_preflight PROPERTIES TIMEOUT 90 LABELS "tools;integration" PROCESSORS 3
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildCandidateExecutionTests>")
    target_sources(SnowDesktopBuildTelemetryTests PRIVATE
        tests/build_tools/build_configuration_tests.cpp
        tests/build_tools/build_compile_budget_tests.cpp
        tests/build_tools/build_resource_publication_tests.cpp)
    add_test(NAME build_compile_budget COMMAND SnowDesktopBuildTelemetryTests "${CMAKE_CURRENT_SOURCE_DIR}" --compile-budget)
    set_tests_properties(build_compile_budget PROPERTIES TIMEOUT 240 LABELS "manual;tools;integration" PROCESSORS 9
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        RESOURCE_LOCK snowdesktop_compiler_budget
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildTelemetryTests>")
    add_test(NAME build_resource_publication COMMAND SnowDesktopBuildTelemetryTests
        "${CMAKE_CURRENT_SOURCE_DIR}" --resource-publication)
    set_tests_properties(build_resource_publication PROPERTIES TIMEOUT 90 LABELS "tools;integration"
        ENVIRONMENT "SNOWDESKTOP_ENTRY_POWERSHELL=${SNOWDESKTOP_POWERSHELL_EXECUTABLE};PSExecutionPolicyPreference=Bypass"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildTelemetryTests>")
    add_test(NAME build_configuration COMMAND SnowDesktopBuildTelemetryTests "${CMAKE_CURRENT_SOURCE_DIR}" --configuration-cache)
    set_tests_properties(build_configuration PROPERTIES TIMEOUT 180 LABELS "tools;integration"
        REQUIRED_FILES "$<TARGET_FILE:SnowDesktopBuildTelemetryTests>")
endif()
