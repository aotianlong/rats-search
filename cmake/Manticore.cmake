# Manticore.cmake — build Manticore Search from sources as part of the RatsSearch build.
#
# By default the project uses the prebuilt daemons shipped in the imports/ submodule.
# With -DRATS_SEARCH_BUILD_MANTICORE=ON the daemon is instead compiled from the upstream
# sources pinned by RATS_MANTICORE_TAG with the very same toolchain (compiler, generator,
# sysroot, cross-compilation settings) RatsSearch itself is built with, so the produced
# searchd always matches the target platform of the application binary.
#
# The built binaries land in ${CMAKE_BINARY_DIR}/imports/<platform>/<arch>/, which mirrors
# the layout of the imports/ submodule. data::Manticore::findSearchdPath() already probes
# "<appdir>/../imports/<platform>/<arch>/searchd", so both build/bin/RatsSearch and the test
# binaries in build/tests pick the freshly built daemon up ahead of the prebuilt one.
#
# Prerequisites of the source build (checked at configure time, never installed by us):
#   * bison and flex — Manticore generates its SQL/expression parsers at build time,
#     the tarball carries no pre-generated grammar files;
#   * Boost >= 1.71 with the context and filesystem components;
#   * network access — Manticore's own CMake downloads and builds its bundled dependencies
#     (ICU, cctz, xxHash, nlohmann/json, uni-algo, RoaringBitmap, columnar API headers). ICU alone
#     is the bulk of the build time; RATS_MANTICORE_CACHE_DIR keeps all of it across clean builds.

include_guard(GLOBAL)
include(ExternalProject)

# Directory of this file — CMAKE_CURRENT_LIST_DIR changes inside function bodies.
set(_RATS_MANTICORE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

option(RATS_SEARCH_BUILD_MANTICORE
    "Build Manticore Search (searchd) from sources instead of using the prebuilt imports/ binaries" OFF)
option(RATS_MANTICORE_BUILD_TOOLS
    "Also build the indexer/indextool/spelldump/wordbreaker helpers (RatsSearch only needs searchd)" OFF)
option(RATS_MANTICORE_MINIMAL
    "Build a minimal daemon: disable the optional Manticore dependencies RatsSearch does not use (ICU \
excepted — 17.5.x does not link without it)" ON)

set(RATS_MANTICORE_TAG "release-17.5.1" CACHE STRING
    "Manticore Search git tag to build (release-17.5.1 is what imports/ currently ships)")
set(RATS_MANTICORE_REPOSITORY "https://github.com/manticoresoftware/manticoresearch.git" CACHE STRING
    "Manticore Search git repository to clone")
set(RATS_MANTICORE_SOURCE_DIR "" CACHE PATH
    "Local Manticore source tree to build instead of cloning (offline builds)")
set(RATS_MANTICORE_CMAKE_ARGS "" CACHE STRING
    "Extra -D arguments forwarded to the Manticore build, e.g. -DWITH_SSL=ON")
set(RATS_MANTICORE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
    "Build type of the Manticore sub-build. Manticore builds its bundled dependencies for \
RelWithDebInfo and Debug only, so asking for Release makes the imported dependency targets fall back to \
the Debug variant and the daemon fails to link (MSVC: RuntimeLibrary MDd_DynamicDebug vs MD_DynamicRelease)")
set(RATS_MANTICORE_CACHE_DIR "" CACHE PATH
    "Directory where Manticore keeps its downloaded and prebuilt dependencies (its CACHEB). \
Point it outside the build tree to survive a clean build; empty means Manticore's own default")

# imports/<platform>/<arch> naming, kept in sync with data::Manticore::findSearchdPath().
function(rats_manticore_platform_dir OUT_VAR)
    if(WIN32)
        set(_os "win")
    elseif(APPLE)
        set(_os "darwin")
    else()
        set(_os "linux")
    endif()

    # Prefer the target processor over the host one so cross builds land in the right place.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
    if(_proc MATCHES "^(arm64|aarch64|armv8)" OR (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64"))
        set(_arch "arm64")
    elseif(_proc MATCHES "^(x86_64|amd64|x64)$" OR (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8))
        set(_arch "x64")
    elseif(_proc MATCHES "^(i[3-6]86|x86)$")
        set(_arch "ia32")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch "x64")
    else()
        set(_arch "ia32")
    endif()

    set(${OUT_VAR} "${_os}/${_arch}" PARENT_SCOPE)
endfunction()

# Fails the configure step with an actionable message when a build tool is missing.
function(_rats_manticore_check_prerequisites)
    find_program(RATS_MANTICORE_BISON NAMES bison win_bison
        DOC "bison executable used to generate the Manticore SQL grammars")
    find_program(RATS_MANTICORE_FLEX NAMES flex win_flex
        DOC "flex executable used to generate the Manticore lexers")

    set(_missing "")
    if(NOT RATS_MANTICORE_BISON)
        list(APPEND _missing "bison")
    endif()
    if(NOT RATS_MANTICORE_FLEX)
        list(APPEND _missing "flex")
    endif()

    # Boost is a hard requirement of the daemon (context + filesystem).
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 NEW) # BoostConfig.cmake instead of the removed FindBoost
    endif()
    find_package(Boost 1.71 QUIET COMPONENTS context filesystem)
    if(NOT Boost_FOUND)
        list(APPEND _missing "Boost >= 1.71 (context, filesystem)")
    endif()

    if(_missing)
        list(JOIN _missing ", " _missing_str)
        if(WIN32)
            set(_hint "  choco install winflexbison3        (or set -DRATS_MANTICORE_BISON=/path/to/win_bison.exe)\n  vcpkg install boost-context boost-filesystem, then configure with\n      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
        elseif(APPLE)
            set(_hint "  brew install bison flex boost\n  (Apple's own bison is too old — put the brew one first in PATH)")
        else()
            set(_hint "  apt install bison flex libboost-context-dev libboost-filesystem-dev\n  dnf install bison flex boost-devel")
        endif()
        message(FATAL_ERROR
            "RATS_SEARCH_BUILD_MANTICORE=ON but the Manticore build prerequisites are missing: ${_missing_str}\n"
            "Install them:\n${_hint}\n"
            "Or build against the prebuilt daemons instead: -DRATS_SEARCH_BUILD_MANTICORE=OFF")
    endif()

    set(RATS_MANTICORE_BOOST_DIR "${Boost_DIR}" CACHE INTERNAL "BoostConfig.cmake location forwarded to Manticore")
endfunction()

# Declares the `manticore` target: an ExternalProject that configures, builds and collects
# searchd (plus the optional tools) into ${CMAKE_BINARY_DIR}/imports/<platform>/<arch>/.
# Sets RATS_MANTICORE_BINARIES in the caller's scope (absolute paths of the collected files).
function(rats_add_manticore)
    _rats_manticore_check_prerequisites()

    rats_manticore_platform_dir(_platform_dir)
    set(_out_dir "${CMAKE_BINARY_DIR}/imports/${_platform_dir}")

    set(_targets searchd)
    if(RATS_MANTICORE_BUILD_TOOLS)
        list(APPEND _targets indexer indextool spelldump wordbreaker)
    endif()

    set(_binaries "")
    foreach(_target IN LISTS _targets)
        list(APPEND _binaries "${_out_dir}/${_target}${CMAKE_EXECUTABLE_SUFFIX}")
    endforeach()

    # The daemon is a separate process, so its build type is independent of the application's —
    # and it has to be, see RATS_MANTICORE_BUILD_TYPE. Multi-config generators select it with
    # --config at build time, single-config ones with CMAKE_BUILD_TYPE at configure time.
    get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_multi_config)
        set(_build_type_arg "")
    else()
        set(_build_type_arg "-DCMAKE_BUILD_TYPE:STRING=${RATS_MANTICORE_BUILD_TYPE}")
    endif()

    set(_args
        ${_build_type_arg}
        -DBISON_EXECUTABLE:FILEPATH=${RATS_MANTICORE_BISON}
        -DFLEX_EXECUTABLE:FILEPATH=${RATS_MANTICORE_FLEX}
        -DBUILD_TESTING:BOOL=OFF
        -DSPLIT_SYMBOLS:BOOL=OFF
        # Imported dependencies (vcpkg Boost, Manticore's own dependency cache) rarely carry a
        # RelWithDebInfo variant. Without a mapping CMake silently picks the first configuration
        # they do carry — Debug — and the daemon ends up linked against debug DLLs and a release
        # CRT: it then fails to start with STATUS_DLL_NOT_FOUND.
        "-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO:STRING=RelWithDebInfo$<SEMICOLON>Release$<SEMICOLON>"
        # Teaches the sub-build that "C++17" means /std:c++20 under MSVC — see the module for why.
        "-DCMAKE_USER_MAKE_RULES_OVERRIDE:FILEPATH=${_RATS_MANTICORE_MODULE_DIR}/ManticoreMsvcStd.cmake"
    )

    # Same toolchain as RatsSearch — that is the whole point of building from sources.
    set(_forwarded
        CMAKE_TOOLCHAIN_FILE CMAKE_PREFIX_PATH CMAKE_MAKE_PROGRAM
        CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT
        VCPKG_TARGET_TRIPLET)
    if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
        # ExternalProject already forwards the generator, platform and toolset. With Visual Studio
        # those fully define the compiler, and pinning cl.exe on top of them breaks the nested
        # dependency builds Manticore spawns: they run outside the VS environment and cannot
        # execute a bare cl.exe. Other generators do need the compiler spelled out.
        list(APPEND _forwarded CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
    endif()
    if(CMAKE_CROSSCOMPILING)
        # Only meaningful when actually cross-compiling: setting these switches the sub-build
        # into cross-compilation mode as well.
        list(APPEND _forwarded CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR CMAKE_SYSROOT)
    endif()
    foreach(_var IN LISTS _forwarded)
        if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
            # CMAKE_ARGS is a list itself, so a list-valued setting (CMAKE_PREFIX_PATH,
            # CMAKE_OSX_ARCHITECTURES) has to keep its separators escaped.
            string(REPLACE ";" "$<SEMICOLON>" _value "${${_var}}")
            list(APPEND _args "-D${_var}:STRING=${_value}")
        endif()
    endforeach()
    if(RATS_MANTICORE_BOOST_DIR)
        list(APPEND _args "-DBoost_DIR:PATH=${RATS_MANTICORE_BOOST_DIR}")
    endif()
    if(RATS_MANTICORE_CACHE_DIR)
        list(APPEND _args "-DCACHEB:PATH=${RATS_MANTICORE_CACHE_DIR}")
    endif()
    # src/sphinxversion.h.in carries a placeholder 0.0.0 that upstream rewrites at release time.
    # Recover the number from the tag so the daemon reports its actual version; VERNUMBERS itself
    # only feeds CPack, hence the patch step below (git checkouts only — never a user's own tree).
    set(_patch_args "")
    if(NOT RATS_MANTICORE_SOURCE_DIR AND RATS_MANTICORE_TAG MATCHES "([0-9]+\\.[0-9]+\\.[0-9]+)")
        list(APPEND _args "-DVERNUMBERS:STRING=${CMAKE_MATCH_1}")
        set(_patch_args PATCH_COMMAND ${CMAKE_COMMAND}
            "-DMANTICORE_SOURCE_DIR=<SOURCE_DIR>"
            "-DMANTICORE_VERSION=${CMAKE_MATCH_1}"
            -P "${_RATS_MANTICORE_MODULE_DIR}/ManticoreStampVersion.cmake")
    endif()

    # RatsSearch drives Manticore over the local MySQL protocol with plain RT indexes and no
    # morphology, so every optional dependency below is dead weight: turning them off keeps the
    # build self-contained (no system OpenSSL/zlib/expat/MySQL/ODBC headers needed) and cuts the
    # build time roughly in half. Re-enable individually via RATS_MANTICORE_CMAKE_ARGS.
    # WITH_ICU stays on deliberately: in 17.5.x the calls into icu.cpp (sphCheckConfigICU,
    # sphSpawnFilterICU) are not #if-guarded at their call sites, so a WITH_ICU=OFF build compiles
    # and then fails to link. Manticore builds ICU from source, which is the bulk of the build time.
    # Every value is stated explicitly (ON ones included): dropping an argument would leave the
    # previous value behind in the sub-build's CMake cache instead of restoring the default.
    if(RATS_MANTICORE_MINIMAL)
        list(APPEND _args
            -DWITH_ICU:BOOL=ON         # mandatory, see above
            -DWITH_JIEBA:BOOL=OFF      # Chinese segmentation
            -DWITH_RE2:BOOL=OFF        # regexp_filter
            -DWITH_STEMMER:BOOL=OFF    # Snowball morphology
            -DWITH_GALERA:BOOL=OFF     # cluster replication
            -DWITH_SSL:BOOL=OFF        # encrypted client connections
            -DWITH_ZLIB:BOOL=OFF       # compressed MySQL protocol
            -DWITH_ZSTD:BOOL=OFF       # compressed networking
            -DWITH_CURL:BOOL=OFF       # remote fetches
            -DWITH_ODBC:BOOL=OFF       # indexer sources
            -DWITH_EXPAT:BOOL=OFF      # xmlpipe indexer sources
            -DWITH_ICONV:BOOL=OFF      # xmlpipe encodings
            -DWITH_MYSQL:BOOL=OFF      # mysql indexer sources
            -DWITH_POSTGRESQL:BOOL=OFF # pgsql indexer sources
        )
    endif()

    list(APPEND _args ${RATS_MANTICORE_CMAKE_ARGS})

    if(_multi_config)
        set(_config_arg --config ${RATS_MANTICORE_BUILD_TYPE})
    else()
        set(_config_arg "")
    endif()

    # Deliberately terse directory names. Manticore builds its dependencies through nested
    # ExternalProjects, and on Windows the resulting paths (…/mtc/b/_deps/<dep>-build/CMakeFiles/
    # CMakeScratch/TryCompile-xxxxxx/…) run into MAX_PATH — MSBuild's file tracker then fails with
    # FTK1011 and the dependency build dies with a bogus "no CMAKE_CXX_COMPILER" error.
    set(_prefix "${CMAKE_BINARY_DIR}/mtc")

    # Where the sources come from: a local tree (offline / development) or the pinned upstream tag.
    if(RATS_MANTICORE_SOURCE_DIR)
        if(NOT EXISTS "${RATS_MANTICORE_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "RATS_MANTICORE_SOURCE_DIR=${RATS_MANTICORE_SOURCE_DIR} is not a Manticore source tree")
        endif()
        set(_source_args SOURCE_DIR "${RATS_MANTICORE_SOURCE_DIR}" DOWNLOAD_COMMAND "")
        set(_source_desc "${RATS_MANTICORE_SOURCE_DIR} (local)")
    else()
        set(_source_args
            SOURCE_DIR "${_prefix}/s"
            GIT_REPOSITORY "${RATS_MANTICORE_REPOSITORY}"
            GIT_TAG "${RATS_MANTICORE_TAG}"
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            # Manticore's manual/ tree busts MAX_PATH on Windows without this.
            GIT_CONFIG core.longpaths=true
            UPDATE_DISCONNECTED TRUE
        )
        set(_source_desc "${RATS_MANTICORE_REPOSITORY}@${RATS_MANTICORE_TAG}")
    endif()

    string(REPLACE ";" "|" _targets_arg "${_targets}")

    # Where to look for the shared libraries the daemon ended up linked against (vcpkg's bin
    # directory, Manticore's own dependency cache, …) so they can travel next to the binary.
    set(_dep_dirs "")
    foreach(_dir IN LISTS CMAKE_PREFIX_PATH ITEMS "${RATS_MANTICORE_CACHE_DIR}")
        if(_dir)
            list(APPEND _dep_dirs "${_dir}" "${_dir}/bin")
        endif()
    endforeach()
    string(REPLACE ";" "|" _dep_dirs "${_dep_dirs}")

    ExternalProject_Add(manticore
        PREFIX "${_prefix}"
        BINARY_DIR "${_prefix}/b"
        STAMP_DIR "${_prefix}/t"
        TMP_DIR "${_prefix}/m"
        DOWNLOAD_DIR "${_prefix}/d"
        ${_source_args}
        ${_patch_args}
        CMAKE_ARGS ${_args}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> ${_config_arg} --target ${_targets} --parallel
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${_binaries}
        USES_TERMINAL_DOWNLOAD TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
    )

    # searchd sits in <build>/src (or <build>/src/<Config>); lift it into the imports layout.
    ExternalProject_Add_Step(manticore collect
        COMMENT "Collecting Manticore binaries into ${_out_dir}"
        DEPENDEES build
        COMMAND ${CMAKE_COMMAND}
            "-DMANTICORE_BUILD_DIR=<BINARY_DIR>"
            "-DMANTICORE_DEST_DIR=${_out_dir}"
            "-DMANTICORE_CONFIG=${RATS_MANTICORE_BUILD_TYPE}"
            "-DMANTICORE_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            "-DMANTICORE_TARGETS=${_targets_arg}"
            "-DMANTICORE_SEARCH_DIRS=${_dep_dirs}"
            -P "${_RATS_MANTICORE_MODULE_DIR}/ManticoreCollect.cmake"
        BYPRODUCTS ${_binaries}
        ALWAYS TRUE
        USES_TERMINAL TRUE
    )

    set(RATS_MANTICORE_BINARIES "${_binaries}" PARENT_SCOPE)
    set(RATS_MANTICORE_OUTPUT_DIR "${_out_dir}" PARENT_SCOPE)
    set(RATS_MANTICORE_SOURCE_DESC "${_source_desc}" PARENT_SCOPE)
endfunction()
