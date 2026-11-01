# Ship a deterministic WebP decoder, independent of installed WIC extensions.
foreach(WEBP_TOOL ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
    set(WEBP_BUILD_${WEBP_TOOL} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
option(SNOWDESKTOP_CACHE_WEBP_PROBES "Reuse unchanged native MSVC WebP SIMD checks" ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowDesktop.WebPProbeCache.cmake")
add_subdirectory(third_party/libwebp EXCLUDE_FROM_ALL)
if(MSVC)
    # The host and native tests use the static CRT. Apply it to the upstream
    # object libraries too; changing only webpdecoder/webp leaves mixed CRTs.
    get_property(SNOWDESKTOP_WEBP_TARGETS DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(SNOWDESKTOP_WEBP_TARGET IN LISTS SNOWDESKTOP_WEBP_TARGETS)
        set_property(TARGET ${SNOWDESKTOP_WEBP_TARGET} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()
add_library(SnowDesktopPuff STATIC third_party/puff/puff.c)
target_include_directories(SnowDesktopPuff PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/puff")
if(MSVC)
    set_property(TARGET SnowDesktopPuff PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowDesktop.Perfetto.cmake")
# Decoder-only upstream target exports its root; expose the public API directory.
target_include_directories(webpdecoder INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp/src>")
