function(snowdesktop_add_test TEST_TARGET TEST_NAME)
    set(multiValueArgs
        SOURCES
        INCLUDE_DIRECTORIES
        LINK_LIBRARIES
        COMMAND_ARGUMENTS
        LABELS)
    cmake_parse_arguments(
        SNOW_TEST "NATIVE_WINDOW_COMPATIBILITY;OBSERVE_RUNTIME_MODULES" "" "${multiValueArgs}" ${ARGN})

    if (NOT SNOW_TEST_SOURCES)
        message(FATAL_ERROR
            "${TEST_TARGET} must declare at least one source file")
    endif()

    add_executable(${TEST_TARGET} ${SNOW_TEST_SOURCES})
    target_link_libraries(${TEST_TARGET} PRIVATE SnowDesktopTextInput)
    if ("src/dock/dock_window_pin.cpp" IN_LIST SNOW_TEST_SOURCES OR "src/dock/dock_window_transition.cpp" IN_LIST SNOW_TEST_SOURCES)
        target_link_libraries(${TEST_TARGET} PRIVATE SnowDesktopWindowSupport)
    endif()
    foreach(probe_source IN LISTS SNOW_TEST_SOURCES)
        if(probe_source IN_LIST SNOWDESKTOP_PERFETTO_PROBE_SOURCES)
            target_link_libraries(${TEST_TARGET} PRIVATE SnowDesktopPerfetto)
            target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}/tests")
            break()
        endif()
    endforeach()
    if (SNOW_TEST_NATIVE_WINDOW_COMPATIBILITY)
        snowdesktop_enable_native_window_compatibility(${TEST_TARGET})
    endif()
    # Keep every CTest binary, symbol file, and import library outside the
    # runnable product root. Generator expressions prevent Visual Studio
    # from appending a second configuration directory.
    set_target_properties(${TEST_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        ARCHIVE_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        PDB_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
        COMPILE_PDB_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
    target_compile_features(${TEST_TARGET} PRIVATE cxx_std_20)
    if (SNOW_TEST_INCLUDE_DIRECTORIES)
        target_include_directories(
            ${TEST_TARGET} PRIVATE
            ${SNOW_TEST_INCLUDE_DIRECTORIES})
    endif()
    if (SNOW_TEST_LINK_LIBRARIES)
        target_link_libraries(
            ${TEST_TARGET} PRIVATE
            ${SNOW_TEST_LINK_LIBRARIES})
    endif()

    if (MSVC)
        set_property(
            TARGET ${TEST_TARGET}
            PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            ${TEST_TARGET} PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(
            ${TEST_TARGET} PRIVATE
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(
            ${TEST_TARGET} PRIVATE
            -Wall -Wextra -pedantic)
    endif()

    if(SNOW_TEST_OBSERVE_RUNTIME_MODULES)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
            get_target_property(existing_launcher ${TEST_TARGET} TEST_LAUNCHER)
            if(existing_launcher)
                message(STATUS "Runtime module observation unavailable with an existing test launcher; evidence remains unclosed")
            else()
                set_property(TARGET ${TEST_TARGET} PROPERTY TEST_LAUNCHER
                    "$<TARGET_FILE:SnowDesktopTestRuntimeObserver>;--test;${TEST_NAME};--")
                add_dependencies(${TEST_TARGET} SnowDesktopTestRuntimeObserver)
            endif()
        else()
            message(STATUS "Runtime module observation unavailable before CMake 3.29; evidence remains unclosed")
        endif()
    endif()
    add_test(
        NAME ${TEST_NAME}
        COMMAND ${TEST_TARGET}
            ${SNOW_TEST_COMMAND_ARGUMENTS})
    set_tests_properties(
        ${TEST_NAME} PROPERTIES
        TIMEOUT 60
        REQUIRED_FILES "$<TARGET_FILE:${TEST_TARGET}>")
    if (SNOW_TEST_LABELS)
        set_tests_properties(
            ${TEST_NAME} PROPERTIES
            LABELS "${SNOW_TEST_LABELS}")
    endif()

    list(APPEND SNOWDESKTOP_TEST_TARGETS ${TEST_TARGET})
    set(SNOWDESKTOP_TEST_TARGETS
        "${SNOWDESKTOP_TEST_TARGETS}" PARENT_SCOPE)
    if ("tools" IN_LIST SNOW_TEST_LABELS)
        list(APPEND SNOWDESKTOP_TOOL_TEST_TARGETS ${TEST_TARGET})
        set(SNOWDESKTOP_TOOL_TEST_TARGETS
            "${SNOWDESKTOP_TOOL_TEST_TARGETS}" PARENT_SCOPE)
    else()
        list(APPEND SNOWDESKTOP_APP_TEST_TARGETS ${TEST_TARGET})
        set(SNOWDESKTOP_APP_TEST_TARGETS
            "${SNOWDESKTOP_APP_TEST_TARGETS}" PARENT_SCOPE)
    endif()
    if ("core" IN_LIST SNOW_TEST_LABELS AND NOT "tools" IN_LIST SNOW_TEST_LABELS)
        list(APPEND SNOWDESKTOP_CORE_TEST_TARGETS ${TEST_TARGET})
        set(SNOWDESKTOP_CORE_TEST_TARGETS
            "${SNOWDESKTOP_CORE_TEST_TARGETS}" PARENT_SCOPE)
    endif()
    if (NOT "integration" IN_LIST SNOW_TEST_LABELS AND NOT "tools" IN_LIST SNOW_TEST_LABELS)
        list(APPEND SNOWDESKTOP_FAST_TEST_TARGETS ${TEST_TARGET})
        set(SNOWDESKTOP_FAST_TEST_TARGETS
            "${SNOWDESKTOP_FAST_TEST_TARGETS}" PARENT_SCOPE)
    endif()
endfunction()

function(snowdesktop_configure_native_test_host target)
    foreach(PROPERTY IN ITEMS
            INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
            PRECOMPILE_HEADERS LINK_LIBRARIES LINK_OPTIONS
            VS_PACKAGE_REFERENCES VS_USER_PROPS
            VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION
            VS_GLOBAL_ROOTNAMESPACE VS_GLOBAL_SnowDesktopSourceDirectory
            VS_GLOBAL_SnowDesktopWindowsAppSdkPackageVersion
            VS_GLOBAL_SnowDesktopCppWinRtPackageVersion
            VS_GLOBAL_AppContainerApplication VS_GLOBAL_AppxPackage
            VS_GLOBAL_CppWinRTOptimized VS_GLOBAL_CppWinRTRootNamespaceAutoMerge
            VS_GLOBAL_EnablePreviewMsixTooling
            VS_GLOBAL_TargetPlatformMinVersion VS_GLOBAL_TargetPlatformVersion
            VS_GLOBAL_UseCrtSDKReferenceStaticWarning VS_GLOBAL_UseWinUI
            VS_GLOBAL_WindowsAppSDKSelfContained
            VS_GLOBAL_WindowsAppSdkUndockedRegFreeWinRTInitialize
            VS_GLOBAL_WindowsPackageType)
        get_target_property(HOST_VALUE SnowDesktop ${PROPERTY})
        if (NOT HOST_VALUE STREQUAL "HOST_VALUE-NOTFOUND")
            string(REPLACE "generated/xaml/" "generated/native-drag-host/xaml/"
                HOST_VALUE "${HOST_VALUE}")
            set_property(TARGET ${target}
                PROPERTY ${PROPERTY} "${HOST_VALUE}")
        endif()
    endforeach()
    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_MSBuildProjectExtensionsPath "$(ProjectDir)obj\\$(MSBuildProjectName)\\"
        VS_GLOBAL_GeneratedFilesDir "generated\\native-drag-host\\winui\\$(Configuration)\\"
        VS_GLOBAL_XamlGeneratedOutputPath
            "$(ProjectDir)generated\\native-drag-host\\xaml\\$(Configuration)\\"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests/native-host")
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/tests")
endfunction()
