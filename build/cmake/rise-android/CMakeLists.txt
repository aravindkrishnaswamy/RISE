# -----------------------------------------------------------------------------
# CMakeLists.txt — RISE Android NDK build
#
# Builds:
#   - z_local      (static, vendored zlib)
#   - png_local    (static, vendored libpng, links against z_local)
#   - rise         (static, the full RISE library — same source list as the
#                   Makefile's SRCLIB, transcribed verbatim into rise_sources.cmake)
#   - rise_jni     (shared, the JNI bridge; depends on everything above)
#
# This file is invoked by the Android Gradle plugin via externalNativeBuild.
# It is also buildable standalone with the NDK toolchain file, which is how
# we smoke-test step 1 of the implementation plan before touching the Android
# project itself.
#
# Standalone invocation example (from this directory):
#   cmake -B _out \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a \
#     -DANDROID_PLATFORM=android-29 \
#     -DANDROID_STL=c++_shared \
#     -DRISE_JNI_SOURCES_DIR=<absolute path to android/app/src/main/cpp>
#   cmake --build _out --target rise
#
# Hard constraint: nothing under ../../../src/Library/** is modified. All
# adaptation is in this directory and in <RISE_JNI_SOURCES_DIR>.
# -----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22.1)
project(rise_android CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Visibility hidden by default; JNI entry points mark themselves visible.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Reusable paths
set(RISE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

# Pull in the source lists
include("${CMAKE_CURRENT_LIST_DIR}/rise_sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/extlib_png.cmake")

# -----------------------------------------------------------------------------
# librise.a — the RISE library as a static archive
# -----------------------------------------------------------------------------

add_library(rise STATIC ${RISE_LIB_SOURCES})

target_include_directories(rise PUBLIC
    "${RISE_ROOT}/src/Library"
    "${RISE_ROOT}/src/Library/Interfaces"
)

target_compile_definitions(rise PUBLIC
    COLORS_RGB                  # match Config.common
    MERSENNE53
)
target_compile_definitions(rise PRIVATE
    RISE_ENABLE_MAILBOXING      # match Config.OSX DEF_MAILBOXING
    NO_EXR_SUPPORT              # extlib/openexr is pre-C++17; see plan file
    NO_TIFF_SUPPORT             # extlib/libtiff similarly not ported
)

target_compile_options(rise PRIVATE
    -O3
    -ffast-math
    -fno-finite-math-only       # infs/nans are used in intersection math
    -funroll-loops
    -fexceptions                # library throws std::runtime_error in a few paths
    -frtti                      # dynamic_cast usage in scene management
    -ffunction-sections
    -fdata-sections
    # The library is old C++ code; silence noisy but non-fatal clang warnings.
    -Wno-deprecated-declarations
    -Wno-deprecated-non-prototype
    -Wno-deprecated-register
    -Wno-register
    -Wno-writable-strings
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-private-field
    -Wno-c++11-narrowing
    -Wno-sign-compare
    -Wno-undefined-var-template
    -Wno-delete-non-abstract-non-virtual-dtor
    -Wno-non-virtual-dtor
    -Wno-overloaded-virtual
    -Wno-reorder
    -Wno-switch
    -Wno-parentheses
    -Wno-logical-op-parentheses
    -Wno-dangling-else
    -Wno-format
    -Wno-format-security
    -Wno-tautological-compare
    -Wno-misleading-indentation
)

# Link rise against vendored libpng + zlib so PNG reader/writer resolve.
target_link_libraries(rise PUBLIC png_local z_local)

set_target_properties(rise PROPERTIES POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# librise_jni.so — the JNI bridge shared library (only built if the sources
# directory was passed in; the Android Gradle plugin always passes it, and
# the standalone library smoke-test does not).
# -----------------------------------------------------------------------------

if(DEFINED RISE_JNI_SOURCES_DIR AND EXISTS "${RISE_JNI_SOURCES_DIR}/rise_jni.cpp")
    add_library(rise_jni SHARED
        "${RISE_JNI_SOURCES_DIR}/rise_jni.cpp"
        "${RISE_JNI_SOURCES_DIR}/RiseBridge.cpp"
        "${RISE_JNI_SOURCES_DIR}/RiseCallbacks.cpp"
        "${RISE_JNI_SOURCES_DIR}/RiseLogPrinter.cpp"
        "${RISE_JNI_SOURCES_DIR}/jni_util.cpp"
    )

    target_include_directories(rise_jni PRIVATE
        "${RISE_JNI_SOURCES_DIR}"
        "${RISE_ROOT}/src/Library"
        "${RISE_ROOT}/src/Library/Interfaces"
    )

    target_compile_options(rise_jni PRIVATE
        -O3
        -fexceptions
        -frtti
        -ffunction-sections
        -fdata-sections
        -Wall
    )

    target_link_libraries(rise_jni PRIVATE
        rise
        png_local
        z_local
        android
        log
    )

    # 16 KB page alignment is mandatory for Play Store apps targeting Android
    # 15+ as of November 2025. --gc-sections / --icf=all shave 15-20% of .so
    # size on arm64.
    target_link_options(rise_jni PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--icf=all"
        "-Wl,-z,max-page-size=16384"
    )
endif()
