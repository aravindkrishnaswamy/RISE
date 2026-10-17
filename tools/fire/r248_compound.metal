#include <metal_stdlib>
using namespace metal;
struct Entry {uint first,second,third,coordinate2;};
struct Record {uint kind,power;uint pos[33],neg[33];uint count,positive,negative,zero;};
struct Params {uint n,groups,power,kind;};
inline Record clear_record(uint kind,uint power){Record r;r.kind=kind;r.power=power;for(uint i=0;i<33;++i){r.pos[i]=0;r.neg[i]=0;}r.count=0;r.positive=0;r.negative=0;r.zero=0;return r;}
inline Record add_record(Record a,Record b,device atomic_uint* failure){
 if(a.kind!=b.kind||a.power!=b.power){atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return clear_record(a.kind,a.power);}
 if(b.count>0xffffffffu-a.count){atomic_fetch_or_explicit(failure,2u,memory_order_relaxed);return clear_record(a.kind,a.power);}
 uint cp=0,cn=0;for(uint i=0;i<33;++i){uint p=a.pos[i]+b.pos[i]+cp,n=a.neg[i]+b.neg[i]+cn;a.pos[i]=p&65535u;a.neg[i]=n&65535u;cp=p>>16;cn=n>>16;}
 if(cp||cn)atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);
 a.count+=b.count;a.positive+=b.positive;a.negative+=b.negative;a.zero+=b.zero;return a;
}
// Base-2^16 schoolbook multiply by a uint32 factor, with no floating operations.
inline void multiply_small(thread uint* value,uint factor,device atomic_uint* failure){
 uint out[33];for(uint i=0;i<33;++i)out[i]=0;
 for(uint j=0;j<2;++j){uint digit=(factor>>(16u*j))&65535u,carry=0;
  for(uint i=0;i<33-j;++i){uint k=i+j,z=value[i]*digit+out[k]+carry;out[k]=z&65535u;carry=z>>16;}
  if(carry||(j==1u&&value[32]!=0u&&digit!=0u))atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);}
 for(uint i=0;i<33;++i)value[i]=out[i];
}
inline bool factor(uint bits,thread uint* value,device atomic_uint* failure){
 uint e=(bits>>23)&255u,m=bits&0x7fffffu;for(uint i=0;i<33;++i)value[i]=0u;
 if(e==255u){atomic_fetch_or_explicit(failure,1u,memory_order_relaxed);return false;}
 if(e)m|=1u<<23;uint shift=e?e-1u:0u;
 for(uint i=0;i<33;++i){uint start=i*16u;if(start>=shift){uint offset=start-shift;if(offset<24u)value[i]=(m>>offset)&65535u;}else{uint offset=shift-start;if(offset<16u)value[i]=(m<<offset)&65535u;}}return true;
}
inline void shift_value(thread uint* v,uint shift){
 uint out[33],word=shift/16u,bits=shift%16u;for(uint i=0;i<33;++i){uint d=0;if(i>=word){uint k=i-word;d=(v[k]<<bits)&65535u;if(bits!=0u&&k>0u)d|=v[k-1u]>>(16u-bits);}out[i]=d;}for(uint i=0;i<33;++i)v[i]=out[i];
}
inline Record make_leaf(Entry e,uint kind,uint power,device atomic_uint* failure){
 Record r=clear_record(kind,power);uint a[33],b[33];
 if(kind>1u||power>2u||!factor(e.first,a,failure)||!factor(e.second,b,failure)){atomic_fetch_or_explicit(failure,1u,memory_order_relaxed);return r;}
 shift_value(a,149u);
 if(kind==0u){if(e.third!=0u){atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return r;}shift_value(b,149u);}
 else{uint alpha[33];if(!factor(e.third,alpha,failure))return r;
  uint absolute=e.third&0x7fffffffu;if(absolute>0x3f800000u||((e.third>>31)!=0u&&absolute!=0u)){atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return r;}
  uint exponent=(e.third>>23)&255u,m=e.third&0x7fffffu;if(exponent)m|=1u<<23;
  multiply_small(b,m,failure);shift_value(b,exponent?exponent-1u:0u);}
 bool negative=(e.first>>31)!=0u,secondNegative=(e.second>>31)!=0u;
 uint v[33];if(negative==secondNegative){uint carry=0u;for(uint i=0;i<33;++i){uint z=a[i]+b[i]+carry;v[i]=z&65535u;carry=z>>16;}if(carry)atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);}
 else{bool firstLarger=true;for(uint i=33u;i>0u;--i){if(a[i-1u]!=b[i-1u]){firstLarger=a[i-1u]>b[i-1u];break;}}
  uint borrow=0u;for(uint i=0;i<33;++i){uint x=firstLarger?a[i]:b[i],y=firstLarger?b[i]:a[i],next=y+borrow;v[i]=(x+65536u-next)&65535u;borrow=x<next?1u:0u;}negative=firstLarger?negative:secondNegative;}
 for(uint j=0;j<power;++j)multiply_small(v,e.coordinate2,failure);
 bool zero=true;for(uint i=0;i<33;++i)if(v[i]!=0u)zero=false;
 r.count=1u;if(zero){r.zero=1u;return r;}r.positive=negative?0u:1u;r.negative=negative?1u:0u;
 for(uint i=0;i<33;++i){if(negative)r.neg[i]=v[i];else r.pos[i]=v[i];}return r;
}
inline void finish_group(Record r,device Record* output,device atomic_uint* failure,threadgroup Record* scratch,uint lane,uint group){
 scratch[lane]=r;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint stride=8u;stride>0u;stride>>=1u){if(lane<stride)scratch[lane]=add_record(scratch[lane],scratch[lane+stride],failure);threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(lane==0u)output[group]=scratch[0];
}
kernel void reduce_leaves(device const Entry* input [[buffer(0)]],device Record* output [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],uint lane [[thread_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){
 Record r=clear_record(p.kind,p.power);for(uint offset=lane;offset<256u;offset+=16u){ulong index=ulong(group.x)*256ul+offset;if(index<ulong(p.n))r=add_record(r,make_leaf(input[index],p.kind,p.power,failure),failure);}finish_group(r,output,failure,scratch,lane,group.x);
}
kernel void reduce_partials(device const Record* input [[buffer(0)]],device Record* output [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],uint lane [[thread_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){
 Record r=clear_record(p.kind,p.power);for(uint offset=lane;offset<256u;offset+=16u){ulong index=ulong(group.x)*256ul+offset;if(index<ulong(p.n))r=add_record(r,input[index],failure);}finish_group(r,output,failure,scratch,lane,group.x);
}
// Explicit synthetic primitive controls; never dispatched for ordinary entries.
kernel void control_add(device const Entry* input [[buffer(0)]],device Record* output [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],constant Params& p [[buffer(3)]],threadgroup Record* scratch [[threadgroup(0)]],uint lane [[thread_index_in_threadgroup]]){
 if(lane!=0u)return;Record a=clear_record(p.kind,p.power),b=clear_record(p.kind,p.power);
 if(input[0].first==1u){a.count=0xffffffffu;a.zero=a.count;b.count=1u;b.zero=1u;}
 else if(input[0].first==2u){a.count=1u;a.positive=1u;b.count=1u;b.positive=1u;for(uint i=0;i<33;++i)a.pos[i]=65535u;b.pos[0]=1u;}
 else{atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return;}
 scratch[0]=add_record(a,b,failure);output[0]=scratch[0];
}
