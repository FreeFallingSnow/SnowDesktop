# The vendored CPU check still runs its real compile/link check on a miss.
# Keep separate slots for the default and additional-flag attempts. Imported
# libraries and custom toolchain hooks retain upstream's uncached behavior.
function(snowdesktop_webp_probe_key output simd)
    set(${output} "WEBP_HAVE_FLAG_${simd}" PARENT_SCOPE)
    if(NOT SNOWDESKTOP_CACHE_WEBP_PROBES OR NOT WIN32 OR CMAKE_CROSSCOMPILING OR
       NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
       CMAKE_TOOLCHAIN_FILE OR CMAKE_USER_MAKE_RULES_OVERRIDE OR CMAKE_USER_MAKE_RULES_OVERRIDE_C OR
       CMAKE_PROJECT_TOP_LEVEL_INCLUDES OR CMAKE_REQUIRED_LIBRARIES OR
       CMAKE_REQUIRED_LINK_OPTIONS OR CMAKE_REQUIRED_LINK_DIRECTORIES OR
       "${CMAKE_C_FLAGS} ${CMAKE_REQUIRED_FLAGS}" MATCHES "(^| )(@|/FI|-include)")
        return()
    endif()

    string(SHA256 source_id "${CMAKE_CURRENT_SOURCE_DIR}")
    get_property(files_key GLOBAL PROPERTY "SNOWDESKTOP_WEBP_PROBE_FILES_${source_id}")
    if(NOT files_key)
        file(GLOB_RECURSE headers CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/sharpyuv/*.h")
        list(APPEND headers "${CMAKE_CURRENT_LIST_FILE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpu.cmake"
            "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" "${CMAKE_C_COMPILER}" "${CMAKE_LINKER}"
            "${CMAKE_AR}" "${CMAKE_MT}" "${CMAKE_RC_COMPILER}"
            "${CMAKE_ROOT}/Modules/CheckCSourceCompiles.cmake"
            "${CMAKE_ROOT}/Modules/Internal/CheckSourceCompiles.cmake")
        list(SORT headers)
        set(material "webp-native-msvc-probe-v1")
        foreach(path IN LISTS headers)
            if(EXISTS "${path}")
                file(SHA256 "${path}" digest)
                string(APPEND material "\n${path}:${digest}")
            endif()
        endforeach()
        string(SHA256 files_key "${material}")
        set_property(GLOBAL PROPERTY "SNOWDESKTOP_WEBP_PROBE_FILES_${source_id}" "${files_key}")
    endif()

    set(variables CMAKE_VERSION CMAKE_GENERATOR CMAKE_GENERATOR_INSTANCE CMAKE_GENERATOR_PLATFORM
        CMAKE_GENERATOR_TOOLSET CMAKE_VS_PLATFORM_NAME CMAKE_VS_PLATFORM_TOOLSET
        CMAKE_VS_PLATFORM_TOOLSET_VERSION CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE
        CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION
        CMAKE_SYSTEM_PROCESSOR CMAKE_SIZEOF_VOID_P CMAKE_C_COMPILER CMAKE_C_COMPILER_VERSION
        CMAKE_C_COMPILER_FRONTEND_VARIANT CMAKE_C_COMPILER_TARGET CMAKE_C_COMPILER_LAUNCHER
        CMAKE_C_STANDARD CMAKE_C_STANDARD_REQUIRED CMAKE_C_EXTENSIONS CMAKE_C_FLAGS
        CMAKE_C_STANDARD_LIBRARIES CMAKE_EXE_LINKER_FLAGS CMAKE_LINKER CMAKE_AR CMAKE_MT
        CMAKE_RC_COMPILER CMAKE_RC_FLAGS CMAKE_BUILD_TYPE
        CMAKE_CONFIGURATION_TYPES CMAKE_TRY_COMPILE_CONFIGURATION CMAKE_TRY_COMPILE_TARGET_TYPE
        CMAKE_TRY_COMPILE_NO_PLATFORM_VARIABLES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
        CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        CMAKE_POSITION_INDEPENDENT_CODE CMAKE_REQUIRED_FLAGS CMAKE_REQUIRED_DEFINITIONS
        CMAKE_REQUIRED_INCLUDES ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES})
    foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE} ${CMAKE_TRY_COMPILE_CONFIGURATION})
        string(TOUPPER "${config}" config)
        list(APPEND variables "CMAKE_C_FLAGS_${config}" "CMAKE_EXE_LINKER_FLAGS_${config}" "CMAKE_RC_FLAGS_${config}")
    endforeach()
    list(REMOVE_DUPLICATES variables)
    set(material "${files_key}")
    foreach(name IN LISTS variables)
        string(HEX "${${name}}" value)
        string(APPEND material "\n${name}:${value}")
    endforeach()
    foreach(name IN ITEMS INCLUDE LIB LIBPATH PATH CL _CL_ CFLAGS LDFLAGS VCINSTALLDIR
            VCToolsInstallDir VCToolsVersion WindowsSdkDir WindowsSDKVersion UCRTVersion)
        string(HEX "$ENV{${name}}" value)
        string(APPEND material "\nenv-${name}:${value}")
    endforeach()
    # A generated configuration header can change between CMake invocations.
    set(config_header "${CMAKE_CURRENT_BINARY_DIR}/src/webp/config.h")
    if(EXISTS "${config_header}")
        file(SHA256 "${config_header}" digest)
        string(APPEND material "\nconfig-header:${digest}")
    endif()
    string(SHA256 context "${material}")
    string(SHA256 flags "${CMAKE_REQUIRED_FLAGS}")
    set(slot "SNOWDESKTOP_WEBP_PROBE_${simd}_${flags}")
    if(NOT "${${slot}_CONTEXT}" STREQUAL "${context}")
        unset("${slot}" CACHE)
        set("${slot}_CONTEXT" "${context}" CACHE INTERNAL "WebP SIMD probe inputs" FORCE)
    endif()
    set(${output} "${slot}" PARENT_SCOPE)
endfunction()
