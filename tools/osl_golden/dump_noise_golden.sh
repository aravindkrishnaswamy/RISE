#!/bin/sh
# Regenerate tests/data/osl_noise_golden.inc from Cycles' own kernels.
#
#   tools/osl_golden/dump_noise_golden.sh /path/to/blender [out.inc]
#
# The first argument is a Blender source checkout.  dump_noise_path.cpp is
# compiled three times against it, each into a namespace of its own, and
# all three are linked into the dumper, which writes every output once
# per build:
#   simd    Cycles' SIMD path (sse2neon, as Blender's arm64 build compiles
#           it), with Cycles' own math flags
#           (CYCLES_MATH_FLAGS in intern/cycles/CMakeLists.txt) -- the
#           arithmetic a Blender arm64 build runs, rebuilt here (not a
#           shipped binary);
#   scalar  its scalar "standard implementation", with the same flags;
#   ieee    the scalar path as strict IEEE single precision: no fused
#           multiply-add, no reciprocal substitution, no reassociation.
#           This is the arithmetic RISE's port reproduces on every
#           platform, so the test gates on it.
#
# Only noisetex.h's noise functions are needed, and its node entry point
# drags in the whole kernel-globals machinery, so the header is copied up
# to svm_node_tex_noise into a scratch directory.
set -eu
BLENDER=${1:?usage: dump_noise_golden.sh /path/to/blender [out.inc]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/tests/data/osl_noise_golden.inc}
CY=$BLENDER/intern/cycles
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sed -e '/#include "kernel\/svm\/util.h"/d' -e '/ccl_device_noinline int svm_node_tex_noise/,$d' \
    "$CY/kernel/svm/noisetex.h" > "$WORK/noisetex_core.h"
echo 'CCL_NAMESPACE_END' >> "$WORK/noisetex_core.h"

MATH="-fno-trapping-math -fno-math-errno -fno-signed-zeros -ffp-contract=on -freciprocal-math"
IEEE="-fno-math-errno -ffp-contract=off"
# arm64 only.  On x86-64, util/optimization.h enables Cycles' SSE path
# unconditionally, so neither the "scalar" nor the "ieee" build would be the
# scalar path -- and RISE's gate is against the scalar path as strict IEEE.
case "$(uname -m)" in
  arm64|aarch64) ;;
  *) echo "dump_noise_golden.sh must run on arm64: on x86-64 Cycles always takes its SSE path" >&2; exit 1 ;;
esac
SSE2NEON=$(find "$BLENDER/lib" -maxdepth 3 -name sse2neon.h 2>/dev/null | head -1 || true)
[ -n "$SSE2NEON" ] || { echo "sse2neon.h not found under $BLENDER/lib" >&2; exit 1; }
SIMD="-DWITH_SSE2NEON -I$(dirname "$SSE2NEON")"

# One prelude per path: Cycles' CMake defines its namespace macros on the
# command line; a header keeps the shell out of the quoting.
for path in simd scalar ieee; do
  cat > "$WORK/prelude_$path.h" <<H
#define CCL_NAMESPACE_BEGIN namespace ccl_$path {
#define CCL_NAMESPACE_END }
#define kernel_assert(x)
#define OSL_PATH $path
H
done
c++ -std=c++20 -O2 $MATH -include "$WORK/prelude_simd.h" -I"$WORK" -I"$CY" -I"$CY/.." $SIMD \
    -c "$HERE/dump_noise_path.cpp" -o "$WORK/path_simd.o"
c++ -std=c++20 -O2 $MATH -include "$WORK/prelude_scalar.h" -I"$WORK" -I"$CY" -I"$CY/.." \
    -c "$HERE/dump_noise_path.cpp" -o "$WORK/path_scalar.o"
c++ -std=c++20 -O2 $IEEE -include "$WORK/prelude_ieee.h" -I"$WORK" -I"$CY" -I"$CY/.." \
    -c "$HERE/dump_noise_path.cpp" -o "$WORK/path_ieee.o"
c++ -std=c++20 -O2 "$HERE/dump_noise_golden.cpp" "$WORK/path_simd.o" "$WORK/path_scalar.o" "$WORK/path_ieee.o" -o "$WORK/dump"

COMMIT=$(git -C "$BLENDER" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
{
  echo "// Blender commit $COMMIT."
  "$WORK/dump"
} > "$OUT"
echo "wrote $OUT"
