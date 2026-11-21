//////////////////////////////////////////////////////////////////////
//
//  svm_stack_stubs.h - The SVM stack helpers svm_node_tex_white_noise
//    uses, copied from intern/cycles/kernel/svm/util.h (Apache-2.0,
//    Blender Foundation) for the golden dumper.  util.h itself includes
//    the kernel-globals machinery, which this harness does not build; the
//    node needs only these six helpers.  Their bodies are util.h's, minus
//    the kernel_assert bounds checks (the dumper defines kernel_assert
//    empty), with stack_store_float3's copy_v3_v3 written out.
//
//  Tabs: 2 (matches the Cycles sources it accompanies)
//
//////////////////////////////////////////////////////////////////////

#pragma once

CCL_NAMESPACE_BEGIN

ccl_device_inline float3 stack_load_float3(const ccl_private float *stack, const uint a)
{
  const ccl_private float *stack_a = stack + a;
  return make_float3(stack_a[0], stack_a[1], stack_a[2]);
}

ccl_device_inline float stack_load_float(const ccl_private float *stack, const uint a)
{
  return stack[a];
}

ccl_device_inline void stack_store_float3(ccl_private float *stack, const uint a, const float3 f)
{
  ccl_private float *stack_a = stack + a;
  stack_a[0] = f.x;
  stack_a[1] = f.y;
  stack_a[2] = f.z;
}

ccl_device_inline void stack_store_float(ccl_private float *stack, const uint a, const float f)
{
  stack[a] = f;
}

ccl_device_inline bool stack_valid(const uint a)
{
  return a != (uint)SVM_STACK_INVALID;
}

ccl_device_forceinline void svm_unpack_node_uchar2(const uint i,
                                                   ccl_private uint *x,
                                                   ccl_private uint *y)
{
  *x = (i & 0xFF);
  *y = ((i >> 8) & 0xFF);
}

CCL_NAMESPACE_END
