# Libical core for the Windows host. The upstream Perl generators are run
# when updating the vendored sources, so ordinary builds are offline.
set(HAVE_INTTYPES_H 1)
set(HAVE_SIZEOF_INTPTR_T 1)
set(HAVE_SIZEOF_SIZE_T 1)
set(HAVE_SIZEOF_WINT_T 1)
set(HAVE_SNPRINTF 1)
set(HAVE_ISWSPACE 1)
set(HAVE_WCTYPE_H 1)
set(HAVE__STRICMP 1)
set(HAVE__STRNICMP 1)
set(HAVE_STAT 1)
set(HAVE_FSTAT 1)
set(HAVE__STRDUP 1)
set(HAVE__ACCESS 1)
set(HAVE__GETPID 1)
set(HAVE__MKDIR 1)
set(HAVE__OPEN 1)
set(HAVE__READ 1)
set(HAVE__WRITE 1)
set(HAVE__UNLINK 1)
set(HAVE_FCNTL_H 1)
set(SIZEOF_INT 4)
set(SIZEOF_TIME_T 8)
set(SIZEOF_ICALTIME_T 8)
set(USE_BUILTIN_TZDATA 1)
set(LIBICAL_DEVMODE_SYNCMODE_THREADLOCAL 1)
configure_file(config.h.cmake config.h)

add_library(SnowDesktopLibIcal STATIC
    icalderivedproperty.c
    icalderivedparameter.c
    icalderivedvalue.c
    icalrestriction.c
    icalarray.c
    icalattach.c
    icalcomponent.c
    icalenumarray.c
    icalenums.c
    icalerror_p.c
    icalerror.c
    icallimits.c
    icalmemory.c
    icalparameter.c
    icalparser.c
    icalproperty.c
    icalrecur.c
    icalstrarray.c
    icaltime_p.c
    icaltime.c
    icaltimezone_p.c
    icaltimezone.c
    icalduration.c
    icalperiod.c
    icaltypes.c
    icalvalue.c
    icalpvl_p.c
    qsort_gen.c
    icallangbind_p.c
    icaldate_p.c
    byref.c
)
target_include_directories(SnowDesktopLibIcal PUBLIC "."
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
target_compile_definitions(SnowDesktopLibIcal PUBLIC LIBICAL_ICAL_STATIC_DEFINE
    PRIVATE HAVE_CONFIG_H _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
    WIN32_LEAN_AND_MEAN NOMINMAX PACKAGE_DATA_DIR="")
set_target_properties(SnowDesktopLibIcal PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
