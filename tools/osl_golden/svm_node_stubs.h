//////////////////////////////////////////////////////////////////////
//
//  svm_node_stubs.h - What svm_node_tex_voronoi needs beyond
//    svm_stack_stubs.h, for the golden dumper: a KernelGlobals that is
//    a plain array of SVM nodes, read_node over it, and the
//    stack_load_float_default / svm_unpack_node_uchar3 /
//    svm_unpack_node_uchar4 helpers, copied from
//    intern/cycles/kernel/svm/util.h (Apache-2.0, Blender Foundation).
//    The real KernelGlobals and kernel_data_fetch belong to the
//    kernel-globals machinery this harness does not build.
//
//  The copied code's notice, as util.h carries it:
//    SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//    SPDX-License-Identifier: Apache-2.0
//  (licenses/Apache-2.0.txt).
//
//  Tabs: 2 (matches the Cycles sources it accompanies)
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "kernel/features.h"
#include "svm_stack_stubs.h"

CCL_NAMESPACE_BEGIN

// The dumper's stand-in for the kernel globals: the node's own uint4s.
typedef const uint4 *KernelGlobals;

ccl_device_inline uint4 read_node(KernelGlobals kg, ccl_private int *const offset)
{
  const uint4 node = kg[*offset];
  (*offset)++;
  return node;
}

ccl_device_inline float stack_load_float_default(const ccl_private float *stack,
                                                 const uint a,
                                                 const uint value)
{
  return (a == (uint)SVM_STACK_INVALID) ? __uint_as_float(value) : stack_load_float(stack, a);
}

ccl_device_forceinline void svm_unpack_node_uchar3(const uint i,
                                                   ccl_private uint *x,
                                                   ccl_private uint *y,
                                                   ccl_private uint *z)
{
  *x = (i & 0xFF);
  *y = ((i >> 8) & 0xFF);
  *z = ((i >> 16) & 0xFF);
}

ccl_device_forceinline void svm_unpack_node_uchar4(const uint i,
                                                   ccl_private uint *x,
                                                   ccl_private uint *y,
                                                   ccl_private uint *z,
                                                   ccl_private uint *w)
{
  *x = (i & 0xFF);
  *y = ((i >> 8) & 0xFF);
  *z = ((i >> 16) & 0xFF);
  *w = ((i >> 24) & 0xFF);
}

CCL_NAMESPACE_END
