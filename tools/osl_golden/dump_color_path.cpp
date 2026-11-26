//////////////////////////////////////////////////////////////////////
//
//  dump_color_path.cpp - One build of Cycles' colour conversions and Mix
//    Color node, behind plain functions.  dump_color_golden.sh compiles
//    this file three times, as dump_voronoi_golden.sh compiles
//    dump_voronoi_path.cpp: Cycles' SIMD path (sse2neon on arm64) into
//    namespace ccl_simd, its scalar "standard implementation" into
//    ccl_scalar, and that scalar path as strict IEEE into ccl_ieee.  The
//    functions' names carry the build (OSL_PATH), so all three link into
//    one dumper.
//
//  Tabs: 2 (matches the Cycles sources it includes)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "util/types.h"
#include "kernel/svm/types.h"
#include "mix_core.h"            // mix.h whole, its util.h include swapped for svm_node_stubs.h
#include "sepcomb_color_core.h"  // sepcomb_color.h whole, same swap

#define OSL_CAT2(a, b) a##b
#define OSL_CAT(a, b) OSL_CAT2(a, b)
#define OSL_FN(name) OSL_CAT(name, OSL_PATH)

using namespace OSL_CAT(ccl_, OSL_PATH);

// op: 0 = rgb_to_hsv (svm_node_separate_color, HSV)
//     1 = hsv_to_rgb (svm_node_combine_color,  HSV)
//     2 = rgb_to_hsl (svm_node_separate_color, HSL)
//     3 = hsl_to_rgb (svm_node_combine_color,  HSL)
void OSL_FN(ColorConv)(int op, const float *in, float *out)
{
  float stack[8] = {};
  stack[0] = in[0];
  stack[1] = in[1];
  stack[2] = in[2];
  if (op == 0 || op == 2) {
    const uint color_type = (op == 0) ? (uint)NODE_COMBSEP_COLOR_HSV :
                                        (uint)NODE_COMBSEP_COLOR_HSL;
    const uint results_offsets = 3u | (4u << 8) | (5u << 16);
    svm_node_separate_color(stack, color_type, 0u, results_offsets);
    out[0] = stack[3];
    out[1] = stack[4];
    out[2] = stack[5];
  }
  else {
    const uint color_type = (op == 1) ? (uint)NODE_COMBSEP_COLOR_HSV :
                                        (uint)NODE_COMBSEP_COLOR_HSL;
    const uint inputs_offsets = 0u | (1u << 8) | (2u << 16);
    svm_node_combine_color(stack, color_type, inputs_offsets, 3u);
    out[0] = stack[3];
    out[1] = stack[4];
    out[2] = stack[5];
  }
}

// svm_node_mix_color: stack has t at 0, a at 1..3, b at 4..6, result at 7..9.
void OSL_FN(MixColor)(int mode,
                      int clampFactor,
                      int clampResult,
                      float t,
                      const float *a,
                      const float *b,
                      float *out)
{
  float stack[12] = {};
  stack[0] = t;
  stack[1] = a[0];
  stack[2] = a[1];
  stack[3] = a[2];
  stack[4] = b[0];
  stack[5] = b[1];
  stack[6] = b[2];
  const uint options = (clampFactor ? 1u : 0u) | (uint(mode) << 8) |
                       ((clampResult ? 1u : 0u) << 16);
  const uint input_offset = 0u | (1u << 8) | (4u << 16);
  svm_node_mix_color(stack, options, input_offset, 7u);
  out[0] = stack[7];
  out[1] = stack[8];
  out[2] = stack[9];
}

// Legacy svm_node_mix (always clamps factor; optional NODE_MIX_CLAMP follow-up).
void OSL_FN(MixLegacy)(int mode,
                       int clampResult,
                       float t,
                       const float *a,
                       const float *b,
                       float *out)
{
  float stack[12] = {};
  stack[0] = t;
  stack[1] = a[0];
  stack[2] = a[1];
  stack[3] = a[2];
  stack[4] = b[0];
  stack[5] = b[1];
  stack[6] = b[2];
  uint4 nodes[2];
  nodes[0] = make_uint4(0u, uint(mode), 7u, 0u);
  nodes[1] = make_uint4(0u, uint(NODE_MIX_CLAMP), 7u, 0u);
  svm_node_mix(nodes, stack, 0u, 1u, 4u, 0);
  if (clampResult) {
    svm_node_mix(nodes, stack, 0u, 7u, 0u, 1);
  }
  out[0] = stack[7];
  out[1] = stack[8];
  out[2] = stack[9];
}
