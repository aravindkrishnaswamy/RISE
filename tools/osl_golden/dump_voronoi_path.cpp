//////////////////////////////////////////////////////////////////////
//
//  dump_voronoi_path.cpp - One build of Cycles' Voronoi Texture node,
//    behind plain functions.  dump_voronoi_golden.sh compiles this file
//    three times, as dump_noise_golden.sh compiles dump_noise_path.cpp:
//    Cycles' SIMD path (sse2neon on arm64) into namespace ccl_simd, its
//    scalar "standard implementation" into ccl_scalar, and that scalar
//    path as strict IEEE into ccl_ieee.  The functions' names carry the
//    build (OSL_PATH), so all three link into one dumper.
//
//  Tabs: 2 (matches the Cycles sources it includes)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "util/types.h"
#include "kernel/svm/types.h"
#include "voronoi_core.h"  // voronoi.h whole, its util.h include swapped for svm_node_stubs.h

#define OSL_CAT2(a, b) a##b
#define OSL_CAT(a, b) OSL_CAT2(a, b)
#define OSL_FN(name) OSL_CAT(name, OSL_PATH)

using namespace OSL_CAT(ccl_, OSL_PATH);

// The PCG cell hashes, as Voronoi's feature points use them.
void OSL_FN(HashInt2)(const int *k, float *out)
{
  const float2 f = hash_int2_to_float2(make_int2(k[0], k[1]));
  out[0] = f.x;
  out[1] = f.y;
}
void OSL_FN(HashInt3)(const int *k, float *out)
{
  const float3 f = hash_int3_to_float3(make_int3(k[0], k[1], k[2]));
  out[0] = f.x;
  out[1] = f.y;
  out[2] = f.z;
}
void OSL_FN(HashInt4)(const int *k, float *out)
{
  const float4 f = hash_int4_to_float4(make_int4(k[0], k[1], k[2], k[3]));
  out[0] = f.x;
  out[1] = f.y;
  out[2] = f.z;
  out[3] = f.w;
}

// The Voronoi node itself: svm_node_tex_voronoi on a small SVM stack,
// with every input linked (none from the node's defaults).  c holds the
// coordinates in RISE's order (1D: w; 4D: x, y, z, w) BEFORE the node's
// Scale.  Stack: vector 0..2, w 3, scale 4, detail 5, roughness 6,
// lacunarity 7, smoothness 8, exponent 9, randomness 10; outputs
// distance 11, color 12..14, position 15..17, w 18, radius 19.
// in = {scale, detail, roughness, lacunarity, smoothness, exponent,
// randomness}; out = {distance, color.rgb, position.xyz, w, radius}.
void OSL_FN(Voronoi)(int dims, const float *c, int feature, int metric, const float *in,
                     bool normalize, float *out)
{
  float stack[24];
  for (int k = 0; k < 24; ++k) {
    stack[k] = 0.0f;
  }
  if (dims == 1) {
    stack[3] = c[0];
  }
  else {
    stack[0] = c[0];
    stack[1] = c[1];
    stack[2] = c[2];
    stack[3] = c[3];
  }
  for (int k = 0; k < 7; ++k) {
    stack[4 + k] = in[k];
  }
  uint4 nodes[3];
  nodes[0] = make_uint4(0u | (3u << 8) | (4u << 16) | (5u << 24),
                        6u | (7u << 8) | (8u << 16) | (9u << 24),
                        10u | ((normalize ? 1u : 0u) << 8) | (11u << 16) | (12u << 24),
                        15u | (18u << 8) | (19u << 16));
  nodes[1] = make_uint4(0u, 0u, 0u, 0u);  // defaults, unread: every input is on the stack
  nodes[2] = make_uint4(0u, 0u, 0u, 0u);
  svm_node_tex_voronoi<KERNEL_FEATURE_NODE_MASK_SURFACE>(
      nodes, stack, uint(dims), uint(feature), uint(metric), 0);
  out[0] = stack[11];
  for (int k = 0; k < 3; ++k) {
    out[1 + k] = stack[12 + k];
    out[4 + k] = stack[15 + k];
  }
  out[7] = stack[18];
  out[8] = stack[19];
}
