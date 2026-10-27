include_guard(GLOBAL)

option(SNOWDESKTOP_SOURCE_DEPENDENCIES
    "Emit per-source MSVC dependency reports for build/test evidence" ON)

function(snowdesktop_dependency_source target source)
    if(source MATCHES "\\$<")
        # Unresolved conditional sources remain visibly incomplete in the
        # observer; a generated path is not guessed across configurations.
        return()
    endif()
    get_target_property(source_directory "${target}" SOURCE_DIR)
    get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${source_directory}")
    get_filename_component(extension "${absolute}" LAST_EXT)
    string(TOLOWER "${extension}" extension)
    if(NOT extension MATCHES "^\\.(c|cc|cpp|cxx|c\\+\\+|ixx|cppm)$")
        return()
    endif()
    cmake_path(NORMAL_PATH absolute)
    string(TOLOWER "${absolute}" identity)
    string(SHA256 scope "${source_directory}|${identity}")
    get_property(configured GLOBAL PROPERTY "SNOWDESKTOP_DEPENDENCY_CONFIGURED_${scope}")
    if(configured)
        return()
    endif()
    # Preserve CL's /MP batching: ordinary sources share the object directory.
    # CMake already separates object directories for duplicate basenames.
    # APPEND retains existing XAML DependentUpon/SubType metadata.
    set_property(SOURCE "${absolute}" TARGET_DIRECTORY "${target}" APPEND PROPERTY VS_SETTINGS
        "GenerateSourceDependencies=true"
        "SourceDependenciesFile=$([System.IO.Path]::GetDirectoryName('%(ClCompile.ObjectFileName)'))"
        # MSBuild can skip an unchanged object even when this auxiliary JSON
        # is missing. Reassess only that source; existing reports keep ordinary
        # tracking. ProjectDir binds child projects independently of process cwd.
        "MinimalRebuildFromTracking=$([System.IO.File]::Exists('$([System.IO.Path]::Combine('$(ProjectDir)', '$([System.IO.Path]::GetDirectoryName('%(ClCompile.ObjectFileName)'))', '%(ClCompile.Filename)%(ClCompile.Extension).json'))'))")
    set_property(GLOBAL PROPERTY "SNOWDESKTOP_DEPENDENCY_CONFIGURED_${scope}" TRUE)
endfunction()

function(snowdesktop_dependency_directory directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type "${target}" TYPE)
        if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        get_target_property(sources "${target}" SOURCES)
        foreach(source IN LISTS sources)
            snowdesktop_dependency_source("${target}" "${source}")
        endforeach()
        # CMake creates these after deferred calls. Assign metadata to their
        # known generated source paths too; unused C/C++ variants add no item.
        get_target_property(binary_directory "${target}" BINARY_DIR)
        foreach(extension c cxx)
            snowdesktop_dependency_source("${target}"
                "${binary_directory}/CMakeFiles/${target}.dir/cmake_pch.${extension}")
        endforeach()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        snowdesktop_dependency_directory("${child}")
    endforeach()
endfunction()

function(snowdesktop_enable_source_dependency_reports)
    if(NOT SNOWDESKTOP_SOURCE_DEPENDENCIES)
        return()
    endif()
    if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio " OR
       NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR MSVC_VERSION LESS 1927)
        message(STATUS "Source dependency reports unavailable for this toolchain; evidence remains incomplete")
        return()
    endif()
    snowdesktop_dependency_directory("${CMAKE_SOURCE_DIR}")
endfunction()

cmake_language(DEFER CALL snowdesktop_enable_source_dependency_reports)
