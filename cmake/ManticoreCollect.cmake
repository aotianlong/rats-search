# Script-mode helper for Manticore.cmake: lifts the freshly built Manticore executables out
# of the ExternalProject build tree into the imports-style directory RatsSearch looks in.
#
# Invoked as:
#   cmake -DMANTICORE_BUILD_DIR=... -DMANTICORE_DEST_DIR=... -DMANTICORE_CONFIG=...
#         -DMANTICORE_EXE_SUFFIX=... -DMANTICORE_TARGETS=searchd|indexer -P ManticoreCollect.cmake
#
# MANTICORE_TARGETS is pipe-separated because -D cannot carry a semicolon-separated list.

if(NOT MANTICORE_BUILD_DIR OR NOT MANTICORE_DEST_DIR)
    message(FATAL_ERROR "ManticoreCollect: MANTICORE_BUILD_DIR and MANTICORE_DEST_DIR are required")
endif()

string(REPLACE "|" ";" _targets "${MANTICORE_TARGETS}")
if(NOT _targets)
    set(_targets searchd)
endif()

file(MAKE_DIRECTORY "${MANTICORE_DEST_DIR}")

foreach(_target IN LISTS _targets)
    set(_exe "${_target}${MANTICORE_EXE_SUFFIX}")

    # Single-config generators put the binary in <build>/src, multi-config ones in <build>/src/<Config>.
    set(_found "")
    foreach(_candidate
            "${MANTICORE_BUILD_DIR}/src/${MANTICORE_CONFIG}/${_exe}"
            "${MANTICORE_BUILD_DIR}/src/${_exe}")
        if(EXISTS "${_candidate}")
            set(_found "${_candidate}")
            break()
        endif()
    endforeach()

    # Fall back to a search over the whole build tree in case upstream moves things around.
    if(NOT _found)
        file(GLOB_RECURSE _hits "${MANTICORE_BUILD_DIR}/${_exe}")
        if(_hits)
            list(GET _hits 0 _found)
        endif()
    endif()

    if(NOT _found)
        message(FATAL_ERROR "ManticoreCollect: ${_exe} not found under ${MANTICORE_BUILD_DIR}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_found}" "${MANTICORE_DEST_DIR}/${_exe}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "ManticoreCollect: failed to copy ${_found} to ${MANTICORE_DEST_DIR}")
    endif()
    message(STATUS "Manticore: ${_found} -> ${MANTICORE_DEST_DIR}/${_exe}")

    # Runtime libraries Manticore drops next to the executable (Windows: OpenSSL/zlib/... DLLs
    # when the corresponding WITH_* features are enabled).
    get_filename_component(_bin_dir "${_found}" DIRECTORY)
    file(GLOB _runtime "${_bin_dir}/*.dll" "${_bin_dir}/*.so" "${_bin_dir}/*.so.*" "${_bin_dir}/*.dylib")
    foreach(_lib IN LISTS _runtime)
        get_filename_component(_lib_name "${_lib}" NAME)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_lib}" "${MANTICORE_DEST_DIR}/${_lib_name}")
    endforeach()

    list(APPEND _collected "${MANTICORE_DEST_DIR}/${_exe}")
endforeach()

# Shared libraries the daemon was linked against but that live elsewhere — most notably a Boost
# built as DLLs (vcpkg's default triplet). Without them searchd dies at startup with
# STATUS_DLL_NOT_FOUND, so resolve and copy them next to the binary. Best-effort: a failure here
# only means the daemon needs those libraries on its search path.
string(REPLACE "|" ";" _search_dirs "${MANTICORE_SEARCH_DIRS}")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${_collected}
    DIRECTORIES ${_search_dirs}
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[Ss]ystem32" "/usr/lib" "/lib64" "^/System/"
)

foreach(_lib IN LISTS _resolved)
    get_filename_component(_lib_name "${_lib}" NAME)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_lib}" "${MANTICORE_DEST_DIR}/${_lib_name}")
    message(STATUS "Manticore: runtime dependency ${_lib_name}")
endforeach()

if(_unresolved)
    message(STATUS "Manticore: unresolved runtime dependencies (expected to come from the system): ${_unresolved}")
endif()
