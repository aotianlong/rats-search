# Compiler-rules override injected into the Manticore sub-build (CMAKE_USER_MAKE_RULES_OVERRIDE).
#
# Manticore declares C++17 (`SET(CMAKE_CXX_STANDARD 17)` in its top-level CMakeLists) but its
# sources use designated initializers — a C++20 feature that GCC/Clang accept as an extension in
# C++17 mode, while MSVC rejects it outright (error C7555). Upstream never hits this because the
# official Windows binaries are cross-compiled with clang-cl; a native cl.exe build does.
#
# Passing -DCMAKE_CXX_STANDARD=20 would not help: Manticore's plain set() shadows the cache entry.
# Adding /std:c++20 to CMAKE_CXX_FLAGS is generator-dependent (whether it lands before or after the
# standard flag decides which one MSVC honours). Redefining what "C++17" expands to for MSVC is the
# one place that works everywhere and touches nothing else.

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX17_STANDARD_COMPILE_OPTION "-std:c++20")
    set(CMAKE_CXX17_EXTENSION_COMPILE_OPTION "-std:c++20")
endif()
