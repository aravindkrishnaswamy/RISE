// Compiled three times by dump_math_golden.sh (OSL_PATH = simd / scalar / ieee),
// running Cycles' real svm_node_math, svm_node_vector_math<float3>, svm_node_clamp,
// svm_node_map_range, and svm_node_vector_map_range entry points.
#include "util/defines.h"
#include "util/math.h"
#include "util/types.h"

#include "math_core.h"
#include "clamp_core.h"
#include "map_range_core.h"

#define OSL_CAT2(a, b) a##b
#define OSL_CAT(a, b) OSL_CAT2(a, b)
#define OSL_NS OSL_CAT(ccl_, OSL_PATH)

static uint32_t PackU3(uint32_t x, uint32_t y, uint32_t z)
{
  return (x & 0xFFu) | ((y & 0xFFu) << 8) | ((z & 0xFFu) << 16);
}

static uint32_t PackU4(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
  return (x & 0xFFu) | ((y & 0xFFu) << 8) | ((z & 0xFFu) << 16) | ((w & 0xFFu) << 24);
}

float OSL_CAT(Math, OSL_PATH)(int op, float a, float b, float c)
{
  using namespace OSL_NS;
  float stack[16] = {};
  stack[0] = a;
  stack[1] = b;
  stack[2] = c;
  svm_node_math(stack, (uint)op, PackU3(0, 1, 2), 3);
  return stack[3];
}

void OSL_CAT(VectorMath, OSL_PATH)(
    int op, const float *a, const float *b, const float *c, float scale, float *outVal, float *outVec)
{
  using namespace OSL_NS;
  float stack[24] = {};
  stack[0] = a[0]; stack[1] = a[1]; stack[2] = a[2];
  stack[3] = b[0]; stack[4] = b[1]; stack[5] = b[2];
  stack[6] = c[0]; stack[7] = c[1]; stack[8] = c[2];
  stack[9] = scale;
  const uint4 extra[1] = { make_uint4(6, 0, 0, 0) };
  int offset = 0;
  svm_node_vector_math<float3>(extra, stack, (uint)op, PackU3(0, 3, 9), PackU3(10, 11, 0), offset);
  *outVal = stack[10];
  outVec[0] = stack[11];
  outVec[1] = stack[12];
  outVec[2] = stack[13];
}

float OSL_CAT(Clamp, OSL_PATH)(int type, float v, float mn, float mx)
{
  using namespace OSL_NS;
  float stack[16] = {};
  stack[0] = v;
  stack[1] = mn;
  stack[2] = mx;
  const uint4 defaults[1] = { make_uint4(0, 0, 0, 0) };
  int offset = 0;
  svm_node_clamp(defaults, stack, 0, PackU3(1, 2, (uint)type), 3, offset);
  return stack[3];
}

float OSL_CAT(MapRange, OSL_PATH)(
    int type, int clampResult, float v, float fmin, float fmax, float tmin, float tmax, float steps)
{
  using namespace OSL_NS;
  float stack[16] = {};
  stack[0] = v;
  stack[1] = fmin;
  stack[2] = fmax;
  stack[3] = tmin;
  stack[4] = tmax;
  stack[5] = steps;
  const uint4 defaults[2] = { make_uint4(0, 0, 0, 0), make_uint4(0, 0, 0, 0) };
  int offset = 0;
  svm_node_map_range(defaults, stack, 0, PackU4(1, 2, 3, 4), PackU3((uint)type, 5, 6), offset);
  if (clampResult && type != NODE_MAP_RANGE_SMOOTHSTEP && type != NODE_MAP_RANGE_SMOOTHERSTEP) {
    int clampOff = 0;
    svm_node_clamp(defaults, stack, 6, PackU3(3, 4, (uint)NODE_CLAMP_RANGE), 6, clampOff);
  }
  return stack[6];
}

void OSL_CAT(VectorMapRange, OSL_PATH)(
    int type,
    int clampResult,
    const float *v,
    const float *fmin,
    const float *fmax,
    const float *tmin,
    const float *tmax,
    const float *steps,
    float *out)
{
  using namespace OSL_NS;
  float stack[32] = {};
  for (int i = 0; i < 3; ++i) {
    stack[0 + i] = v[i];
    stack[3 + i] = fmin[i];
    stack[6 + i] = fmax[i];
    stack[9 + i] = tmin[i];
    stack[12 + i] = tmax[i];
    stack[15 + i] = steps[i];
  }
  svm_node_vector_map_range(
      stack, 0, PackU4(3, 6, 9, 12), PackU4(15, (uint)clampResult, (uint)type, 18), 0);
  out[0] = stack[18];
  out[1] = stack[19];
  out[2] = stack[20];
}
