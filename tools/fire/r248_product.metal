#include <metal_stdlib>
using namespace metal;
struct Entry {uint first,second,coordinate2;};
struct Record {uint power,reserved;uint pos[41],neg[41];uint count,positive,negative,zero;};
struct Params {uint n,groups,power;};
inline Record clear_record(uint power){Record r;r.power=power;r.reserved=0;for(uint i=0;i<41;++i){r.pos[i]=0;r.neg[i]=0;}r.count=0;r.positive=0;r.negative=0;r.zero=0;return r;}
inline Record add_record(Record a,Record b,device atomic_uint* failure){
 if(a.power!=b.power){atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return clear_record(a.power);}
 if(b.count>0xffffffffu-a.count){atomic_fetch_or_explicit(failure,2u,memory_order_relaxed);return clear_record(a.power);}
 uint cp=0,cn=0;for(uint i=0;i<41;++i){uint p=a.pos[i]+b.pos[i]+cp,n=a.neg[i]+b.neg[i]+cn;a.pos[i]=p&65535u;a.neg[i]=n&65535u;cp=p>>16;cn=n>>16;}
 if(cp||cn)atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);
 a.count+=b.count;a.positive+=b.positive;a.negative+=b.negative;a.zero+=b.zero;return a;
}
// Base-2^16 schoolbook multiply by a uint32 factor, with no floating operations.
inline void multiply_small(thread uint* value,uint factor,device atomic_uint* failure){
 uint out[41];for(uint i=0;i<41;++i)out[i]=0;
 for(uint j=0;j<2;++j){uint digit=(factor>>(16u*j))&65535u,carry=0;
  for(uint i=0;i<41-j;++i){uint k=i+j,z=value[i]*digit+out[k]+carry;out[k]=z&65535u;carry=z>>16;}
  if(carry||(j==1u&&value[40]!=0u&&digit!=0u))atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);}
 for(uint i=0;i<41;++i)value[i]=out[i];
}
inline Record make_leaf(Entry e,uint power,device atomic_uint* failure){
 Record r=clear_record(power);uint ea=(e.first>>23)&255u,eb=(e.second>>23)&255u;
 if(ea==255u||eb==255u||power>2u){atomic_fetch_or_explicit(failure,1u,memory_order_relaxed);return r;}
 uint a=e.first&0x7fffffu,b=e.second&0x7fffffu;if(ea)a|=1u<<23;if(eb)b|=1u<<23;
 r.count=1u;if(a==0u||b==0u||(power!=0u&&e.coordinate2==0u)){r.zero=1u;return r;}
 uint v[41];for(uint i=0;i<41;++i)v[i]=0;v[0]=a&65535u;v[1]=a>>16;
 multiply_small(v,b,failure);uint shift=(ea?ea-1u:0u)+(eb?eb-1u:0u),word=shift/16u,bits=shift%16u;
 uint shifted[41];for(uint i=0;i<41;++i){uint d=0u;if(i>=word){uint k=i-word;d=(v[k]<<bits)&65535u;if(bits!=0u&&k>0u)d|=v[k-1u]>>(16u-bits);}shifted[i]=d;}
 for(uint i=0;i<41;++i)v[i]=shifted[i];for(uint j=0;j<power;++j)multiply_small(v,e.coordinate2,failure);
 bool negative=((e.first^e.second)>>31)!=0u;r.positive=negative?0u:1u;r.negative=negative?1u:0u;
 for(uint i=0;i<41;++i){if(negative)r.neg[i]=v[i];else r.pos[i]=v[i];}return r;
}
inline void finish_group(Record r,device Record* output,device atomic_uint* failure,threadgroup Record* scratch,uint lane,uint group){
 scratch[lane]=r;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint stride=8u;stride>0u;stride>>=1u){if(lane<stride)scratch[lane]=add_record(scratch[lane],scratch[lane+stride],failure);threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(lane==0u)output[group]=scratch[0];
}
kernel void reduce_leaves(device const Entry* input [[buffer(0)]],device Record* output [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],uint lane [[thread_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){
 Record r=clear_record(p.power);for(uint offset=lane;offset<256u;offset+=16u){ulong index=ulong(group.x)*256ul+offset;if(index<ulong(p.n))r=add_record(r,make_leaf(input[index],p.power,failure),failure);}finish_group(r,output,failure,scratch,lane,group.x);
}
kernel void reduce_partials(device const Record* input [[buffer(0)]],device Record* output [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],uint lane [[thread_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){
 Record r=clear_record(p.power);for(uint offset=lane;offset<256u;offset+=16u){ulong index=ulong(group.x)*256ul+offset;if(index<ulong(p.n))r=add_record(r,input[index],failure);}finish_group(r,output,failure,scratch,lane,group.x);
}
// Explicit synthetic primitive controls; never dispatched for ordinary entries.
kernel void control_add(device const Entry* input [[buffer(0)]],device Record* output [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],uint lane [[thread_index_in_threadgroup]]){
 if(lane!=0u)return;Record a=clear_record(p.power),b=clear_record(p.power);
 if(input[0].first==1u){a.count=0xffffffffu;a.zero=a.count;b.count=1u;b.zero=1u;}
 else if(input[0].first==2u){a.count=1u;a.positive=1u;b.count=1u;b.positive=1u;for(uint i=0;i<41;++i)a.pos[i]=65535u;b.pos[0]=1u;}
 else{atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return;}
 scratch[0]=add_record(a,b,failure);output[0]=scratch[0];
}
