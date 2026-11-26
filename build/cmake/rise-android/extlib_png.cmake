# -----------------------------------------------------------------------------
# extlib_png.cmake
#
# Builds the vendored libpng (extlib/libpng) and zlib (extlib/zlib) as static
# libraries for the Android NDK toolchain.
#
# These are both pure C and don't have the C++17 compat issues that block the
# vendored extlib/openexr — see the plan file's "OpenEXR note". We deliberately
# exclude libpng's x86-asm accelerated TUs (pnggccrd.c, pngvcrd.c) — they won't
# compile under arm64 and the reference C fallback is used instead.
# -----------------------------------------------------------------------------

set(EXTLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../extlib")

# ---- zlib ----
set(ZLIB_DIR "${EXTLIB_DIR}/zlib")
set(ZLIB_SOURCES
    "${ZLIB_DIR}/adler32.c"
    "${ZLIB_DIR}/compress.c"
    "${ZLIB_DIR}/crc32.c"
    "${ZLIB_DIR}/deflate.c"
    "${ZLIB_DIR}/gzio.c"
    "${ZLIB_DIR}/infback.c"
    "${ZLIB_DIR}/inffast.c"
    "${ZLIB_DIR}/inflate.c"
    "${ZLIB_DIR}/inftrees.c"
    "${ZLIB_DIR}/trees.c"
    "${ZLIB_DIR}/uncompr.c"
    "${ZLIB_DIR}/zutil.c"
)

add_library(z_local STATIC ${ZLIB_SOURCES})
target_include_directories(z_local PUBLIC "${ZLIB_DIR}")
target_compile_options(z_local PRIVATE
    -w                                # vendored K&R C emits a flood of warnings; silence
    -Wno-implicit-function-declaration
    -Wno-deprecated-non-prototype
    -Wno-deprecated-declarations
)
set_target_properties(z_local PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- libpng (1.2-vintage) ----
set(PNG_DIR "${EXTLIB_DIR}/libpng")
set(PNG_SOURCES
    "${PNG_DIR}/png.c"
    "${PNG_DIR}/pngerror.c"
    "${PNG_DIR}/pngget.c"
    "${PNG_DIR}/pngmem.c"
    "${PNG_DIR}/pngpread.c"
    "${PNG_DIR}/pngread.c"
    "${PNG_DIR}/pngrio.c"
    "${PNG_DIR}/pngrtran.c"
    "${PNG_DIR}/pngrutil.c"
    "${PNG_DIR}/pngset.c"
    "${PNG_DIR}/pngtrans.c"
    "${PNG_DIR}/pngwio.c"
    "${PNG_DIR}/pngwrite.c"
    "${PNG_DIR}/pngwtran.c"
    "${PNG_DIR}/pngwutil.c"
    # Deliberately excluded: pnggccrd.c, pngvcrd.c (x86/AMD64 asm accelerators)
)

add_library(png_local STATIC ${PNG_SOURCES})
target_include_directories(png_local PUBLIC "${PNG_DIR}")
target_link_libraries(png_local PUBLIC z_local)
target_compile_options(png_local PRIVATE
    -w
    -Wno-implicit-function-declaration
    -Wno-deprecated-non-prototype
    -Wno-deprecated-declarations
)
set_target_properties(png_local PROPERTIES POSITION_INDEPENDENT_CODE ON)
