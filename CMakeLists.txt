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

foreach(f main.cpp etw_monitor.cpp log.cpp app_state.cpp tree_view.cpp
          ai_prompt.cpp process_names.cpp
          etw_monitor.h dir_index.h ring_buffer.h log.h app_state.h
          format.h colors.h tree_view.h ai_prompt.h process_names.h)
    if (NOT EXISTS "${SRC_DIR}/${f}")
        message(FATAL_ERROR "Missing ${f} in ${SRC_DIR}")
    endif()
endforeach()

set(SOURCES
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/etw_monitor.cpp"
    "${SRC_DIR}/log.cpp"
    "${SRC_DIR}/app_state.cpp"
    "${SRC_DIR}/tree_view.cpp"
    "${SRC_DIR}/ai_prompt.cpp"
    "${SRC_DIR}/process_names.cpp"
    "${SRC_DIR}/etw_monitor.h"
    "${SRC_DIR}/dir_index.h"
    "${SRC_DIR}/ring_buffer.h"
    "${SRC_DIR}/log.h"
    "${SRC_DIR}/app_state.h"
    "${SRC_DIR}/format.h"
    "${SRC_DIR}/colors.h"
    "${SRC_DIR}/tree_view.h"
    "${SRC_DIR}/ai_prompt.h"
    "${SRC_DIR}/process_names.h"
)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
else()
    message(WARNING "app.manifest not found - the exe will not request elevation")
endif()

# Application icon.
#
# The file is located by listing directories and matching the name, rather than
# by globbing for "*.ico" or testing EXISTS on a composed path. Both of those
# fail on a name that only looks right: a trailing space, an uppercase
# extension, or a second extension are all invisible in Explorer, and a
# trailing space in particular cannot be opened by the name you can see,
# because Windows strips it while parsing the path.
#
# Whatever is found is then COPIED to a clean name in the build tree, and the
# .rc points at that copy. So the resource compiler never has to deal with the
# original name at all.
set(VIGILES_ICON_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/assets")

set(VIGILES_ICON "")      # preferred: the coloured one
set(VIGILES_FALLBACK "")  # any .ico at all, used if that is all there is
set(VIGILES_SEEN "")

foreach(dir ${VIGILES_ICON_DIRS})
    if (IS_DIRECTORY "${dir}")
        file(GLOB entries "${dir}/*")
        foreach(e ${entries})
            if (NOT IS_DIRECTORY "${e}")
                get_filename_component(n "${e}" NAME)
                string(TOLOWER "${n}" nlow)
                if (nlow MATCHES "\\.ico")
                    list(APPEND VIGILES_SEEN "${e}")
                    if (NOT VIGILES_FALLBACK)
                        set(VIGILES_FALLBACK "${e}")
                    endif()
                    # Prefer the coloured icon: it carries its own background
                    # and so stays visible on a dark taskbar.
                    if (NOT VIGILES_ICON AND NOT nlow MATCHES "mono")
                        set(VIGILES_ICON "${e}")
                    endif()
                endif()
            endif()
        endforeach()
    endif()
endforeach()

# Only a monochrome icon around: better than none.
if (NOT VIGILES_ICON AND VIGILES_FALLBACK)
    set(VIGILES_ICON "${VIGILES_FALLBACK}")
endif()

# Nothing in the usual places: sweep the project. The build tree is excluded by
# path, not by name, so a copy left by an earlier configure cannot be picked up
# and mistaken for the real source asset.
if (NOT VIGILES_ICON)
    file(GLOB_RECURSE swept "${CMAKE_CURRENT_SOURCE_DIR}/*")
    foreach(e ${swept})
        get_filename_component(n "${e}" NAME)
        string(TOLOWER "${n}" nlow)
        string(FIND "${e}" "${CMAKE_CURRENT_BINARY_DIR}" in_build)
        if (NOT VIGILES_ICON AND nlow MATCHES "\\.ico" AND NOT in_build EQUAL 0
            AND NOT e MATCHES "/(build|out|\\.git)/")
            set(VIGILES_ICON "${e}")
            message(STATUS "Found an icon by sweeping the project tree")
        endif()
    endforeach()
endif()

if (VIGILES_ICON)
    if (MSVC)
        enable_language(RC)
    endif()

    # Normalise: copy the icon to a name and location we control, so neither a
    # surprising filename nor a surprising directory can reach rc.exe.
    set(VIGILES_ICON_CLEAN "${CMAKE_CURRENT_BINARY_DIR}/vigiles_app.ico")
    configure_file("${VIGILES_ICON}" "${VIGILES_ICON_CLEAN}" COPYONLY)

    # The .rc is written here rather than read from a template file on disk.
    # A template is one more file that can end up in the wrong directory, and
    # this one is three lines. The icon path is absolute because rc.exe
    # resolves relative paths against its working directory - the build tree
    # under MSBuild, not the directory holding the .rc - and a path that does
    # not resolve gives no icon and no error.
    file(TO_NATIVE_PATH "${VIGILES_ICON_CLEAN}" VIGILES_ICON_NATIVE)
    string(REPLACE "\\" "\\\\" VIGILES_ICON_NATIVE "${VIGILES_ICON_NATIVE}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vigiles.rc"
"// Generated by CMake - edits here are overwritten.\n"
"// 101 must match IDI_APPICON in resource.h.\n"
"101 ICON \"${VIGILES_ICON_NATIVE}\"\n")

    list(APPEND SOURCES "${CMAKE_CURRENT_BINARY_DIR}/vigiles.rc")
    message(STATUS "Vigiles icon: ${VIGILES_ICON}")
else()
    if (NOT VIGILES_SEEN)
        set(VIGILES_SEEN "none")
    endif()
    message(WARNING
        "No icon file found - building WITHOUT an application icon.\n"
        "  Searched: ${VIGILES_ICON_DIRS}\n"
        "  .ico-like files seen: ${VIGILES_SEEN}")
endif()

add_executable(Vigiles WIN32 ${SOURCES})
target_include_directories(Vigiles PRIVATE "${SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

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
