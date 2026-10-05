#include <metal_stdlib>
using namespace metal;
struct Record { uint pos[20]; uint neg[20]; uint count; uint positive; uint negative; uint zero; };
struct Params { uint n; uint groups; };
inline Record clear_record(){Record r;for(uint i=0;i<20;++i){r.pos[i]=0;r.neg[i]=0;}r.count=0;r.positive=0;r.negative=0;r.zero=0;return r;}
inline Record add_record(Record a,Record b,device atomic_uint* failure){
 if(b.count>0xffffffffu-a.count){atomic_fetch_or_explicit(failure,2u,memory_order_relaxed);return clear_record();}
 uint cp=0,cn=0;
 for(uint i=0;i<20;++i){uint p=a.pos[i]+b.pos[i]+cp,n=a.neg[i]+b.neg[i]+cn;
  a.pos[i]=p&65535u;a.neg[i]=n&65535u;cp=p>>16;cn=n>>16;}
 if(cp||cn)atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);
 a.count+=b.count;a.positive+=b.positive;a.negative+=b.negative;a.zero+=b.zero;return a;
}
inline Record make_leaf(uint bits,device atomic_uint* failure){
 Record r=clear_record();uint e=(bits>>23)&255u,m=bits&0x7fffffu;
 if(e==255u){atomic_fetch_or_explicit(failure,1u,memory_order_relaxed);return r;}
 if(e)m|=1u<<23;r.count=1u;
 if(m==0u){r.zero=1u;return r;}
 uint shift=e?e-1u:0u;bool negative=(bits>>31)!=0u;
 r.positive=negative?0u:1u;r.negative=negative?1u:0u;
 for(uint i=0u;i<20u;++i){uint start=16u*i,digit=0u;
  if(start>=shift){uint offset=start-shift;if(offset<24u)digit=(m>>offset)&65535u;}
  else{uint offset=shift-start;if(offset<16u)digit=(m<<offset)&65535u;}
  if(negative)r.neg[i]=digit;else r.pos[i]=digit;}
 return r;
}
inline void finish_group(Record r,device Record* output,device atomic_uint* failure,
 threadgroup Record* scratch,uint lane,uint group){
 scratch[lane]=r;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint stride=8u;stride>0u;stride>>=1u){if(lane<stride)scratch[lane]=add_record(scratch[lane],scratch[lane+stride],failure);
  threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(lane==0u)output[group]=scratch[0];
}
kernel void reduce_leaves(device const uint* input [[buffer(0)]],device Record* output [[buffer(1)]],
 device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],
 uint lane [[thread_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){
 Record r=clear_record();for(uint offset=lane;offset<256u;offset+=16u){ulong index=ulong(group.x)*256ul+offset;
  if(index<ulong(p.n))r=add_record(r,make_leaf(input[index],failure),failure);}
 finish_group(r,output,failure,scratch,lane,group.x);
}
kernel void reduce_partials(device const Record* input [[buffer(0)]],device Record* output [[buffer(1)]],
 device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],
 uint lane [[thread_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){
 Record r=clear_record();for(uint offset=lane;offset<256u;offset+=16u){ulong index=ulong(group.x)*256ul+offset;
  if(index<ulong(p.n))r=add_record(r,input[index],failure);}
 finish_group(r,output,failure,scratch,lane,group.x);
}
