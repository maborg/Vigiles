cmake_minimum_required(VERSION 3.20)
project(Vigiles CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Works whether the sources sit in src/ or next to this file.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
else()
    set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
message(STATUS "Vigiles sources: ${SRC_DIR}")

foreach(f main.cpp etw_monitor.cpp log.cpp etw_monitor.h dir_index.h ring_buffer.h log.h)
    if (NOT EXISTS "${SRC_DIR}/${f}")
        message(FATAL_ERROR "Missing ${f} in ${SRC_DIR}")
    endif()
endforeach()

set(SOURCES
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/etw_monitor.cpp"
    "${SRC_DIR}/log.cpp"
    "${SRC_DIR}/etw_monitor.h"
    "${SRC_DIR}/dir_index.h"
    "${SRC_DIR}/ring_buffer.h"
    "${SRC_DIR}/log.h"
)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
else()
    message(WARNING "app.manifest not found - the exe will not request elevation")
endif()

add_executable(Vigiles WIN32 ${SOURCES})
target_include_directories(Vigiles PRIVATE "${SRC_DIR}")

target_compile_definitions(Vigiles PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(Vigiles PRIVATE
    advapi32 tdh comctl32 shlwapi dbghelp)

if (MSVC)
    # /EHa is required: OnEvent mixes __try/__except with C++ exception handling
    # so that nothing escapes into ProcessTrace.
    target_compile_options(Vigiles PRIVATE /W4 /permissive- /utf-8 /EHa)

    # Keep symbols in Release too, otherwise the minidump is useless.
    target_compile_options(Vigiles PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(Vigiles PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

    # The linker emits the <trustInfo> section itself. app.manifest must NOT
    # also contain a requestedExecutionLevel, or mt.exe fails with c1010001
    # ("Values of attribute \"level\" not equal in different manifest snippets").
    set_target_properties(Vigiles PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
endif()
