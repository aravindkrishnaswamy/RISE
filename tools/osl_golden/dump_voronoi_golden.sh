#!/bin/sh
# Regenerate tests/data/osl_voronoi_golden.inc from Cycles' own Voronoi node.
#
#   tools/osl_golden/dump_voronoi_golden.sh /path/to/blender [out.inc]
#
# The first argument is a Blender source checkout.  As dump_noise_golden.sh
# does for the noise, dump_voronoi_path.cpp is compiled three times against
# it, each into a namespace of its own, and all three are linked into the
# dumper, which writes every node output once per build (the PCG hash rows
# once, after checking the builds agree):
#   simd    Cycles' SIMD path (sse2neon, as Blender's arm64 build compiles
#           it), with Cycles' own math flags (CYCLES_MATH_FLAGS in
#           intern/cycles/CMakeLists.txt) -- rebuilt here, not a shipped
#           binary;
#   scalar  its scalar "standard implementation", with the same flags;
#   ieee    the scalar path as strict IEEE single precision: no fused
#           multiply-add, no reciprocal substitution, no reassociation.
#           This is the arithmetic RISE's port reproduces, so the test
#           gates on it.
#
# voronoi.h is used WHOLE, node entry point included: only its
# kernel/svm/util.h include goes, replaced by svm_node_stubs.h (a
# KernelGlobals that is the node's own uint4 array, read_node over it, and
# the stack helpers the node calls, copied from util.h).
set -eu
BLENDER=${1:?usage: dump_voronoi_golden.sh /path/to/blender [out.inc]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/tests/data/osl_voronoi_golden.inc}
CY=$BLENDER/intern/cycles
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sed -e 's|#include "kernel/svm/util.h"|#include "svm_node_stubs.h"|' \
    "$CY/kernel/svm/voronoi.h" > "$WORK/voronoi_core.h"
grep -q '#include "svm_node_stubs.h"' "$WORK/voronoi_core.h" || {
  echo "voronoi.h no longer includes kernel/svm/util.h as expected; update the sed above" >&2; exit 1; }
cp "$HERE/svm_stack_stubs.h" "$HERE/svm_node_stubs.h" "$WORK/"

MATH="-fno-trapping-math -fno-math-errno -fno-signed-zeros -ffp-contract=on -freciprocal-math"
IEEE="-fno-math-errno -ffp-contract=off"
# arm64 only, for the reason dump_noise_golden.sh gives: on x86-64 Cycles
# always takes its SSE path, so there would be no scalar build to gate on.
case "$(uname -m)" in
  arm64|aarch64) ;;
  *) echo "dump_voronoi_golden.sh must run on arm64: on x86-64 Cycles always takes its SSE path" >&2; exit 1 ;;
esac
SSE2NEON=$(find "$BLENDER/lib" -maxdepth 3 -name sse2neon.h 2>/dev/null | head -1 || true)
[ -n "$SSE2NEON" ] || { echo "sse2neon.h not found under $BLENDER/lib" >&2; exit 1; }
SIMD="-DWITH_SSE2NEON -I$(dirname "$SSE2NEON")"

for path in simd scalar ieee; do
  cat > "$WORK/prelude_$path.h" <<H
#define CCL_NAMESPACE_BEGIN namespace ccl_$path {
#define CCL_NAMESPACE_END }
#define kernel_assert(x)
#define OSL_PATH $path
H
done
c++ -std=c++20 -O2 $MATH -include "$WORK/prelude_simd.h" -I"$WORK" -I"$CY" -I"$CY/.." $SIMD \
    -c "$HERE/dump_voronoi_path.cpp" -o "$WORK/path_simd.o"
c++ -std=c++20 -O2 $MATH -include "$WORK/prelude_scalar.h" -I"$WORK" -I"$CY" -I"$CY/.." \
    -c "$HERE/dump_voronoi_path.cpp" -o "$WORK/path_scalar.o"
c++ -std=c++20 -O2 $IEEE -include "$WORK/prelude_ieee.h" -I"$WORK" -I"$CY" -I"$CY/.." \
    -c "$HERE/dump_voronoi_path.cpp" -o "$WORK/path_ieee.o"
c++ -std=c++20 -O2 "$HERE/dump_voronoi_golden.cpp" "$WORK/path_simd.o" "$WORK/path_scalar.o" "$WORK/path_ieee.o" -o "$WORK/dump"

COMMIT=$(git -C "$BLENDER" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
{
  echo "// Blender commit $COMMIT."
  "$WORK/dump"
} > "$OUT"
echo "wrote $OUT"
