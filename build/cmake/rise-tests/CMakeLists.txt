# -----------------------------------------------------------------------------
# CMakeLists.txt - RISE test runner build (Windows / MSVC)
#
# Purpose:
#   Compile every standalone test under tests/*.cpp into its own .exe, linked
#   against the existing RISE static library produced by the VS2022 Library
#   project. Generates a .sln + per-test .vcxproj that msbuild builds natively.
#
# Why a separate CMake project (vs. extending the .sln by hand):
#   The RISE library on Windows is built by build/VS2022/Library/Library.vcxproj
#   and produces bin/RISE.lib (Release) / dbin/RISE.lib (Debug). Every test is a
#   single .cpp linked against that static lib. Hand-authoring 50+ .vcxproj
#   files duplicates compile/link settings; this CMake script generates them
#   from one source list.
#
# Prerequisites (one-time, done outside this script):
#   1. Build the Library and CLI from build/VS2022/RISE.sln in both Debug and
#      Release. This populates:
#        - bin/RISE.lib, dbin/RISE.lib
#        - bin/*.dll, dbin/*.dll (OpenEXR + OIDN runtime)
#        - build/VS2022/vcpkg_installed/x64-windows/x64-windows/{include,lib,...}
#   2. Have OIDN built into extlib/oidn/install/ via
#      `extlib/oidn/build.ps1` (Windows) or `extlib/oidn/build.sh`
#      (macOS/Linux). The build matches the version that
#      build/VS2022/Library/Library.vcxproj links against.  Override
#      with -DOIDN_DIR=<path> if you have a third install elsewhere.
#
# Usage (from this directory):
#   cmake -B _out -G "Visual Studio 17 2022" -A x64
#   cmake --build _out --config Release          # builds all tests
#   cmake --build _out --config Release --target Math3DTest   # one test
#
# Test outputs:
#   bin/tests/<TestName>.exe   (Release)
#   dbin/tests/<TestName>.exe  (Debug)
#
# When a single test fails to compile or link, msbuild reports the failure but
# continues to the next target by default in --build mode (unlike `make`, which
# stops on first error). To force-continue across the whole suite, pass
# `--` `/p:ContinueOnError=true` to `cmake --build`.
#
# Hard constraint: nothing under ../../../src/Library/** is modified, and the
# existing VS2022 .sln is not touched. This script consumes RISE.lib as a
# pre-built dependency.
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)
project(rise_tests CXX)

if(NOT MSVC)
    message(FATAL_ERROR
        "rise-tests is Windows/MSVC-only. On Linux/macOS use the Makefile: "
        "make -C build/make/rise tests")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the CRT linkage used by Library.vcxproj (MultiThreadedDLL / MultiThreadedDebugDLL).
# Mismatched CRT between RISE.lib and the test exes produces inscrutable LNK2038 errors.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------

set(RISE_ROOT       "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(RISE_VS2022_DIR "${RISE_ROOT}/build/VS2022")
set(RISE_TESTS_DIR  "${RISE_ROOT}/tests")
set(RISE_LIB_INCLUDE "${RISE_ROOT}/src/Library")

# vcpkg-installed dependency tree (manifest-mode populates this when the
# VS2022 solution is built). The double "x64-windows" mirrors what the old
# GeometrySurfaceDerivativesTest.vcxproj used.
set(VCPKG_INSTALLED  "${RISE_VS2022_DIR}/vcpkg_installed/x64-windows/x64-windows")
set(VCPKG_INCLUDE    "${VCPKG_INSTALLED}/include")
set(VCPKG_LIB_RELEASE "${VCPKG_INSTALLED}/lib")
set(VCPKG_LIB_DEBUG   "${VCPKG_INSTALLED}/debug/lib")

# OIDN install — prefer the in-tree extlib build (built via
# extlib/oidn/build.sh on macOS / Linux or extlib/oidn/build.ps1 on
# Windows), fall back to a hand-installed location if the user has
# pre-existing infrastructure pointed elsewhere.  Override on the
# command line with -DOIDN_DIR=<path> if needed.
if(NOT DEFINED OIDN_DIR)
    set(_oidn_extlib "${RISE_ROOT}/extlib/oidn/install")
    if(EXISTS "${_oidn_extlib}/lib/OpenImageDenoise.lib"
       OR EXISTS "${_oidn_extlib}/lib/libOpenImageDenoise.dylib"
       OR EXISTS "${_oidn_extlib}/lib/libOpenImageDenoise.so")
        set(OIDN_DIR "${_oidn_extlib}")
    else()
        set(OIDN_DIR "C:/Dev/oidn-2.4.1.x64.windows")
    endif()
endif()

# OpenPGL install — must match the path baked into Library.vcxproj /
# RISE-CLI.vcxproj (currently C:/Dev/openpgl-install).  RISE.lib is
# compiled with RISE_ENABLE_OPENPGL on the VS2022 side, so every test
# .exe must link openpgl.lib to resolve the pgl* symbols pulled in
# from RISE.lib.  Override with -DOPENPGL_DIR=... at configure time
# if installed elsewhere.
if(NOT DEFINED OPENPGL_DIR)
    set(OPENPGL_DIR "C:/Dev/openpgl-install")
endif()

# RISE static library produced by Library.vcxproj.
set(RISE_LIB_RELEASE "${RISE_ROOT}/bin/RISE.lib")
set(RISE_LIB_DEBUG   "${RISE_ROOT}/dbin/RISE.lib")

# -----------------------------------------------------------------------------
# Sanity checks - fail loudly if prerequisites are missing
# -----------------------------------------------------------------------------

if(NOT EXISTS "${VCPKG_INCLUDE}")
    message(FATAL_ERROR
        "vcpkg headers not found at ${VCPKG_INCLUDE}. "
        "Build build/VS2022/RISE.sln first so vcpkg manifest-mode populates "
        "build/VS2022/vcpkg_installed/.")
endif()

if(NOT EXISTS "${OIDN_DIR}/lib/OpenImageDenoise.lib")
    message(FATAL_ERROR
        "OIDN not found at ${OIDN_DIR}. Pass -DOIDN_DIR=<path> at configure time.")
endif()

if(NOT EXISTS "${OPENPGL_DIR}/lib/openpgl.lib")
    message(FATAL_ERROR
        "OpenPGL not found at ${OPENPGL_DIR}/lib/openpgl.lib. "
        "Pass -DOPENPGL_DIR=<path> at configure time. RISE.lib is "
        "compiled with RISE_ENABLE_OPENPGL so test exes must link "
        "openpgl.lib to resolve pgl* symbols.")
endif()

# -----------------------------------------------------------------------------
# Library dependency lists (per-config)
# -----------------------------------------------------------------------------

set(OPENEXR_LIBS_RELEASE
    "${VCPKG_LIB_RELEASE}/OpenEXR-3_4.lib"
    "${VCPKG_LIB_RELEASE}/OpenEXRCore-3_4.lib"
    "${VCPKG_LIB_RELEASE}/OpenEXRUtil-3_4.lib"
    "${VCPKG_LIB_RELEASE}/IlmThread-3_4.lib"
    "${VCPKG_LIB_RELEASE}/Iex-3_4.lib"
    "${VCPKG_LIB_RELEASE}/Imath-3_2.lib"
)
set(OPENEXR_LIBS_DEBUG
    "${VCPKG_LIB_DEBUG}/OpenEXR-3_4_d.lib"
    "${VCPKG_LIB_DEBUG}/OpenEXRCore-3_4_d.lib"
    "${VCPKG_LIB_DEBUG}/OpenEXRUtil-3_4_d.lib"
    "${VCPKG_LIB_DEBUG}/IlmThread-3_4_d.lib"
    "${VCPKG_LIB_DEBUG}/Iex-3_4_d.lib"
    "${VCPKG_LIB_DEBUG}/Imath-3_2_d.lib"
)

# libpng + zlib are pulled in transitively via RISE.lib's
# PNGReader / PNGWriter.  The VS2022 RISE-CLI link doesn't list them
# explicitly because vcpkg's libpng.h emits `#pragma comment(lib,
# "libpng16.lib")` which the compiler auto-injects when png.h is
# included — but test .cpp files don't include png.h, so those
# pragmas never fire and the linker can't find the symbols.  Listing
# the libs explicitly here mirrors what auto-linking would have done.
set(LIBPNG_LIBS_RELEASE
    "${VCPKG_LIB_RELEASE}/libpng16.lib"
    "${VCPKG_LIB_RELEASE}/zlib.lib"
)
set(LIBPNG_LIBS_DEBUG
    "${VCPKG_LIB_DEBUG}/libpng16d.lib"
    "${VCPKG_LIB_DEBUG}/zlibd.lib"
)

set(OIDN_LIBS
    "${OIDN_DIR}/lib/OpenImageDenoise.lib"
    "${OIDN_DIR}/lib/OpenImageDenoise_core.lib"
)

# OpenPGL ships a single library (no _d suffix) used for both Debug
# and Release configs — same pattern as OIDN.  Mirrors the link line
# in build/VS2022/RISE-CLI/RISE-CLI.vcxproj.
set(OPENPGL_LIBS
    "${OPENPGL_DIR}/lib/openpgl.lib"
)

# Runtime DLLs to deploy alongside test exes. bin/ and dbin/ are populated by
# Library.vcxproj's vcpkg integration (and by manual OIDN copy in the existing
# VS2022 setup); we just rebroadcast them into bin/tests and dbin/tests.
#
# Build parallel lists pairing each Release DLL with its Debug counterpart.
# OpenEXR debug DLLs use the _d suffix; OIDN ships only one variant, so debug
# falls back to the same filename in dbin/. The per-test post-build step picks
# from the right list via a $<IF:$<CONFIG:Debug>,...> generator expression.
file(GLOB RUNTIME_DLLS_RELEASE
    "${RISE_ROOT}/bin/OpenEXR*.dll"
    "${RISE_ROOT}/bin/IlmThread*.dll"
    "${RISE_ROOT}/bin/Iex*.dll"
    "${RISE_ROOT}/bin/Imath*.dll"
    "${RISE_ROOT}/bin/OpenImageDenoise*.dll"
    "${RISE_ROOT}/bin/openpgl*.dll"
    "${RISE_ROOT}/bin/tbb*.dll"
    "${RISE_ROOT}/bin/libpng*.dll"
    "${RISE_ROOT}/bin/zlib*.dll"
    "${RISE_ROOT}/bin/deflate*.dll"
)

# Build per-config DLL lists independently.  Older logic dropped a
# Release DLL when no Debug counterpart existed, which silently broke
# the Release deployment (e.g. openpgl.dll has no Debug variant in
# dbin/, so the dropping logic skipped it from bin/tests/ too).
# We keep them separate: a Debug build only needs Debug DLLs, a
# Release build only needs Release DLLs, and the per-test post-build
# COPY uses a config-conditional source picked at build time.
set(RUNTIME_DLLS_PAIRED_RELEASE "")
set(RUNTIME_DLLS_PAIRED_DEBUG   "")
foreach(_rel_dll ${RUNTIME_DLLS_RELEASE})
    get_filename_component(_rel_name "${_rel_dll}" NAME)

    # Locate a Debug-side counterpart.  Try a few naming conventions
    # in order of specificity so vcpkg's mixed scheme works:
    #   "<name>_d.dll"   — OpenEXR-style debug suffix
    #   "<name>d.dll"    — vcpkg libpng / zlib style ("libpng16d.dll")
    #   "<name>.dll"     — OIDN-style same-name in both trees
    string(REGEX REPLACE "\\.dll$" "_d.dll"  _dbg_underscore "${_rel_name}")
    string(REGEX REPLACE "\\.dll$" "d.dll"   _dbg_appended    "${_rel_name}")
    string(REGEX REPLACE "1\\.dll$" "d1.dll" _dbg_zlib        "${_rel_name}")
    set(_dbg_path "")
    foreach(_candidate "${_dbg_underscore}" "${_dbg_appended}" "${_dbg_zlib}" "${_rel_name}")
        if(EXISTS "${RISE_ROOT}/dbin/${_candidate}")
            set(_dbg_path "${RISE_ROOT}/dbin/${_candidate}")
            break()
        endif()
    endforeach()

    # Always carry the Release DLL — tests in bin/tests/ need it
    # regardless of whether dbin/ has been populated.
    list(APPEND RUNTIME_DLLS_PAIRED_RELEASE "${_rel_dll}")

    if(_dbg_path STREQUAL "")
        message(STATUS
            "rise-tests: no Debug DLL for ${_rel_name} in dbin/ — "
            "Debug builds will be missing this runtime dependency. "
            "Release builds in bin/tests/ are unaffected.")
        # Pad with the Release path so list lengths stay aligned;
        # the IF generator expression below routes Debug builds away
        # from this entry by virtue of dest being dbin/tests, so the
        # Release DLL just lands in dbin/tests too — no functional
        # harm, since Debug isn't the configured runtime.
        list(APPEND RUNTIME_DLLS_PAIRED_DEBUG   "${_rel_dll}")
    else()
        list(APPEND RUNTIME_DLLS_PAIRED_DEBUG   "${_dbg_path}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Discover test sources
#
# CONFIGURE_DEPENDS asks CMake to re-glob on every build. This matches the
# Makefile's `wildcard tests/*.cpp` so adding a new test doesn't require
# re-running cmake by hand.
# -----------------------------------------------------------------------------

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${RISE_TESTS_DIR}/*.cpp")

if(NOT TEST_SOURCES)
    message(FATAL_ERROR "No test sources found under ${RISE_TESTS_DIR}")
endif()

# -----------------------------------------------------------------------------
# Per-test executable definition
# -----------------------------------------------------------------------------

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name "${test_src}" NAME_WE)

    add_executable(${test_name} "${test_src}")

    target_include_directories(${test_name} PRIVATE
        "${RISE_LIB_INCLUDE}"
        "${RISE_TESTS_DIR}"        # ProceduralTestHelpers.h, StandaloneTestReporting.h, etc.
        "${VCPKG_INCLUDE}"
    )

    # Defines mirror Library.vcxproj + the legacy test .vcxproj.
    target_compile_definitions(${test_name} PRIVATE
        COLORS_RGB
        MERSENNE53
        NO_TIFF_SUPPORT
        WIN32
        _CRT_SECURE_NO_WARNINGS
    )

    target_compile_options(${test_name} PRIVATE
        /Zc:__cplusplus
        /permissive-
        /wd4250                    # 'inherits via dominance' - benign with diamond inheritance
    )

    # Per-config libraries via generator expressions.
    target_link_libraries(${test_name} PRIVATE
        $<$<CONFIG:Debug>:${RISE_LIB_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${RISE_LIB_RELEASE}>
        $<$<CONFIG:Debug>:${OPENEXR_LIBS_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${OPENEXR_LIBS_RELEASE}>
        $<$<CONFIG:Debug>:${LIBPNG_LIBS_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${LIBPNG_LIBS_RELEASE}>
        ${OIDN_LIBS}
        ${OPENPGL_LIBS}
        user32
        gdi32
    )

    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${RISE_ROOT}/dbin/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${RISE_ROOT}/bin/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RISE_ROOT}/bin/tests"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${RISE_ROOT}/bin/tests"
    )

    # Stage runtime DLLs next to each test exe so it can launch without a
    # PATH / SetDllDirectory dance. The source is config-selected via a
    # generator expression; the dest ($<TARGET_FILE_DIR:...>) is already
    # config-specific (bin/tests vs dbin/tests). copy_if_different is a no-op
    # when the dest is already up to date.
    list(LENGTH RUNTIME_DLLS_PAIRED_RELEASE _num_dlls)
    if(_num_dlls GREATER 0)
        math(EXPR _last_dll "${_num_dlls} - 1")
        foreach(_i RANGE 0 ${_last_dll})
            list(GET RUNTIME_DLLS_PAIRED_RELEASE ${_i} _rel)
            list(GET RUNTIME_DLLS_PAIRED_DEBUG   ${_i} _dbg)
            add_custom_command(TARGET ${test_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${_dbg},${_rel}>"
                    "$<TARGET_FILE_DIR:${test_name}>"
                VERBATIM)
        endforeach()
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Convenience aggregate target: `cmake --build _out --target rise_all_tests`
# -----------------------------------------------------------------------------

add_custom_target(rise_all_tests)
foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name "${test_src}" NAME_WE)
    add_dependencies(rise_all_tests ${test_name})
endforeach()

list(LENGTH TEST_SOURCES _num_tests)
message(STATUS "rise-tests: configured ${_num_tests} test executables from ${RISE_TESTS_DIR}")
