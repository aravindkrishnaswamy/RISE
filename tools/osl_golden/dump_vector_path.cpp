// Compiled three times by dump_vector_golden.sh (OSL_PATH = simd / scalar / ieee),
// running Cycles' real float3 ops and svm_node_vector_rotate.
#include "util/defines.h"
#include "util/math.h"
#include "util/transform.h"
#include "util/types.h"
#include "kernel/svm/types.h"

#include "vector_rotate_core.h"

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

void OSL_CAT(VecOp, OSL_PATH)(int op, const float *a, const float *b, const float *c, float *out)
{
  using namespace OSL_NS;
  const float3 va = make_float3(a[0], a[1], a[2]);
  const float3 vb = make_float3(b[0], b[1], b[2]);
  const float3 vc = make_float3(c[0], c[1], c[2]);
  float3 r = zero_float3();
  switch (op) {
    case 0: r = min(va, vb); break;
    case 1: r = max(va, vb); break;
    case 2: r = fabs(va); break;
    case 3: r = floor(va); break;
    case 4: r = va - floor(va); break;
    case 5: r = safe_pow(va, vb); break;
    case 6: r = clamp(va, vb, vc); break;
  }
  out[0] = r.x;
  out[1] = r.y;
  out[2] = r.z;
}

void OSL_CAT(VecRotate, OSL_PATH)(
    int type,
    int invert,
    const float *v,
    const float *center,
    const float *axis,
    float angle,
    const float *euler,
    float *out)
{
  using namespace OSL_NS;
  float stack[24] = {};
  stack[0] = v[0]; stack[1] = v[1]; stack[2] = v[2];
  stack[3] = euler[0]; stack[4] = euler[1]; stack[5] = euler[2];
  stack[6] = center[0]; stack[7] = center[1]; stack[8] = center[2];
  stack[9] = axis[0]; stack[10] = axis[1]; stack[11] = axis[2];
  stack[12] = angle;
  svm_node_vector_rotate(
      stack,
      PackU4((uint)type, 0, 3, (uint)invert),
      PackU3(6, 9, 12),
      13);
  out[0] = stack[13];
  out[1] = stack[14];
  out[2] = stack[15];
}
