#!/bin/sh
# Regenerate tests/data/osl_vector_golden.inc from Cycles' own float3
# componentwise ops and svm_node_vector_rotate.
#
#   tools/osl_golden/dump_vector_golden.sh /path/to/blender [out.inc]
set -eu
BLENDER=${1:?usage: dump_vector_golden.sh /path/to/blender [out.inc]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/tests/data/osl_vector_golden.inc}
CY=$BLENDER/intern/cycles
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sed -e 's|#include "kernel/svm/util.h"|#include "svm_node_stubs.h"|' \
    "$CY/kernel/svm/vector_rotate.h" > "$WORK/vector_rotate_core.h"
grep -q '#include "svm_node_stubs.h"' "$WORK/vector_rotate_core.h" || {
  echo "vector_rotate.h no longer includes kernel/svm/util.h as expected" >&2; exit 1; }

cp "$HERE/svm_stack_stubs.h" "$HERE/svm_node_stubs.h" "$WORK/"

MATH="-fno-trapping-math -fno-math-errno -fno-signed-zeros -ffp-contract=on -freciprocal-math"
IEEE="-fno-math-errno -ffp-contract=off"
case "$(uname -m)" in
  arm64|aarch64) ;;
  *) echo "dump_vector_golden.sh must run on arm64" >&2; exit 1 ;;
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
    -c "$HERE/dump_vector_path.cpp" -o "$WORK/path_simd.o"
c++ -std=c++20 -O2 $MATH -include "$WORK/prelude_scalar.h" -I"$WORK" -I"$CY" -I"$CY/.." \
    -c "$HERE/dump_vector_path.cpp" -o "$WORK/path_scalar.o"
c++ -std=c++20 -O2 $IEEE -include "$WORK/prelude_ieee.h" -I"$WORK" -I"$CY" -I"$CY/.." \
    -c "$HERE/dump_vector_path.cpp" -o "$WORK/path_ieee.o"
c++ -std=c++20 -O2 "$HERE/dump_vector_golden.cpp" "$WORK/path_simd.o" "$WORK/path_scalar.o" "$WORK/path_ieee.o" -o "$WORK/dump"

COMMIT=$(git -C "$BLENDER" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
{
  echo "// Blender commit $COMMIT."
  "$WORK/dump"
} > "$OUT"
echo "wrote $OUT"
