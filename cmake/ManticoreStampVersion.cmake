# Script-mode helper for Manticore.cmake: stamps the version number into the freshly cloned sources.
#
# Manticore keeps its version as a literal in src/sphinxversion.h.in (`#define VERNUMBERS "0.0.0"`)
# and rewrites that line during the release build — the tagged tree itself still says 0.0.0, and the
# CMake variable of the same name only feeds CPack. Without this, a source-built daemon reports
# "Manticore 0.0.0" in its log and to clients. Idempotent: rewrites the line whatever it holds.
#
# Invoked as:
#   cmake -DMANTICORE_SOURCE_DIR=... -DMANTICORE_VERSION=17.5.1 -P ManticoreStampVersion.cmake

if(NOT MANTICORE_SOURCE_DIR OR NOT MANTICORE_VERSION)
    message(FATAL_ERROR "ManticoreStampVersion: MANTICORE_SOURCE_DIR and MANTICORE_VERSION are required")
endif()

set(_header "${MANTICORE_SOURCE_DIR}/src/sphinxversion.h.in")
if(NOT EXISTS "${_header}")
    message(WARNING "ManticoreStampVersion: ${_header} not found, leaving the version alone")
    return()
endif()

file(READ "${_header}" _content)
string(REGEX REPLACE "#define[ \t]+VERNUMBERS[ \t]+\"[^\"]*\""
    "#define VERNUMBERS \"${MANTICORE_VERSION}\"" _patched "${_content}")

if(NOT _patched STREQUAL _content)
    file(WRITE "${_header}" "${_patched}")
    message(STATUS "Manticore: stamped version ${MANTICORE_VERSION} into sphinxversion.h.in")
endif()
