//////////////////////////////////////////////////////////////////////
//
//  dump_noise_path.cpp - One of Cycles' two CPU code paths for the noise
//    kernels, behind plain functions.  dump_noise_golden.sh compiles this
//    file three times: Cycles' SIMD path (sse2neon on arm64, as Blender's
//    arm64 build compiles it) into namespace ccl_simd, its scalar
//    "standard implementation" into ccl_scalar, and that scalar path as
//    strict IEEE into ccl_ieee.  The functions' names carry the build
//    (OSL_PATH), so all three link into one dumper.
//
//  Tabs: 2 (matches the Cycles sources it includes)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "util/types.h"
#include "kernel/svm/types.h"
#include "noisetex_core.h"  // noisetex.h up to svm_node_tex_noise; see dump_noise_golden.sh

#define OSL_CAT2(a, b) a##b
#define OSL_CAT(a, b) OSL_CAT2(a, b)
#define OSL_FN(name) OSL_CAT(name, OSL_PATH)

using namespace OSL_CAT(ccl_, OSL_PATH);

unsigned OSL_FN(HashUint)(unsigned a) { return hash_uint(a); }
unsigned OSL_FN(HashUint2)(unsigned a, unsigned b) { return hash_uint2(a, b); }
unsigned OSL_FN(HashUint3)(unsigned a, unsigned b, unsigned c) { return hash_uint3(a, b, c); }
unsigned OSL_FN(HashUint4)(unsigned a, unsigned b, unsigned c, unsigned d) { return hash_uint4(a, b, c, d); }
float OSL_FN(HashFloat)(float a) { return hash_float_to_float(a); }
float OSL_FN(HashFloat2)(float a, float b) { return hash_float2_to_float(make_float2(a, b)); }

float OSL_FN(SNoise)(int dims, const float *c)
{
  switch (dims) {
    case 1: return snoise_1d(c[0]);
    case 2: return snoise_2d(make_float2(c[0], c[1]));
    case 3: return snoise_3d(make_float3(c[0], c[1], c[2]));
    default: return snoise_4d(make_float4(c[0], c[1], c[2], c[3]));
  }
}

float OSL_FN(Fractal)(int dims, const float *c, int type, float detail, float roughness,
                      float lacunarity, float offset, float gain, bool normalize)
{
  switch (dims) {
    case 1:
      return noise_select(c[0], detail, roughness, lacunarity, offset, gain, type, normalize);
    case 2:
      return noise_select(make_float2(c[0], c[1]), detail, roughness, lacunarity, offset, gain, type, normalize);
    case 3:
      return noise_select(make_float3(c[0], c[1], c[2]), detail, roughness, lacunarity, offset, gain, type, normalize);
    default:
      return noise_select(make_float4(c[0], c[1], c[2], c[3]), detail, roughness, lacunarity, offset, gain, type, normalize);
  }
}

// svm_node_tex_noise's two clamps, then noise_texture_Nd.  out = {Fac, Color.r, .g, .b}.
void OSL_FN(Texture)(int dims, const float *c, int type, float detail, float roughness,
                     float lacunarity, float offset, float gain, float distortion, bool normalize,
                     float *out)
{
  detail = clamp(detail, 0.0f, 15.0f);
  roughness = fmaxf(roughness, 0.0f);
  float fac;
  float3 color;
  switch (dims) {
    case 1:
      noise_texture_1d(c[0], detail, roughness, lacunarity, offset, gain, distortion, type, normalize, true, &fac, &color);
      break;
    case 2:
      noise_texture_2d(make_float2(c[0], c[1]), detail, roughness, lacunarity, offset, gain, distortion, type, normalize, true, &fac, &color);
      break;
    case 3:
      noise_texture_3d(make_float3(c[0], c[1], c[2]), detail, roughness, lacunarity, offset, gain, distortion, type, normalize, true, &fac, &color);
      break;
    default:
      noise_texture_4d(make_float4(c[0], c[1], c[2], c[3]), detail, roughness, lacunarity, offset, gain, distortion, type, normalize, true, &fac, &color);
      break;
  }
  out[0] = fac;
  out[1] = color.x;
  out[2] = color.y;
  out[3] = color.z;
}
