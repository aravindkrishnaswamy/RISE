#!/usr/bin/env python3
"""Pure exact f32-sum limb oracle/planner; resident kernels NOT implemented.

One represented f32 leaf per selected entry. Products, spatial weighted moments,
derived rate divisions and family-specific authentication are separate contracts.
"""
from fractions import Fraction
import json
import struct

RADIX=2**16
LIMBS=20
UINT32_MAX=2**32-1
RECORD_BYTES=176
CHANNELS_MAX=64
TILE=256
LANES=16
THREADGROUP_BYTES=LANES*RECORD_BYTES+32


def require(ok,message):
    if not ok:raise ValueError(message)


def u32(x):
    require(type(x) is int and 0<=x<=UINT32_MAX,'typed uint32');return x


def digits(x):
    require(type(x) is int and 0<=x<2**(16*LIMBS),'bounded unsigned exact accumulator')
    return tuple((x>>(16*i))&65535 for i in range(LIMBS))


def number(d):
    require(type(d) is tuple and len(d)==LIMBS,'exact limb tuple')
    for x in d:require(type(x) is int and 0<=x<65536,'canonical low16 limb in uint32')
    return sum(x<<(16*i) for i,x in enumerate(d))


def empty():return dict(positive=digits(0),negative=digits(0),count=0,positive_count=0,negative_count=0,zero_count=0)


def validate(r):
    require(type(r) is dict and set(r)=={'positive','negative','count','positive_count','negative_count','zero_count'},'exact sum record')
    for k in ('count','positive_count','negative_count','zero_count'):u32(r[k])
    require(r['count']==r['positive_count']+r['negative_count']+r['zero_count'],'count partition')
    p,n=number(r['positive']),number(r['negative'])
    # f32 minimum magnitude has integer weight1 at common scale2^-149.
    maximum=((2**24-1)<<253)
    for value,count in ((p,r['positive_count']),(n,r['negative_count'])):
        require(count<=value<=count*maximum if count else value==0,'finite f32 support magnitude bounds')
        if count==1 and value.bit_length()>24:
            require(value%(1<<(value.bit_length()-24))==0,'single leaf exact binary32 significand')
    return p,n


def leaf(bits):
    u32(bits);e=(bits>>23)&255;mantissa=bits&0x7fffff
    require(e<255,'finite binary32 leaf bits')
    if e:mantissa|=1<<23
    weight=mantissa<<(e-1 if e else 0)
    r=empty();r['count']=1
    if not weight:r['zero_count']=1
    elif bits>>31:r['negative']=digits(weight);r['negative_count']=1
    else:r['positive']=digits(weight);r['positive_count']=1
    return r


def add_digits(a,b):
    number(a);number(b);out=[];carry=0
    for x,y in zip(a,b):
        total=x+y+carry
        require(total<=131071,'uint32 carry bound')
        out.append(total&65535);carry=total>>16
    require(carry==0,'accumulator carry overflow')
    return tuple(out)


def merge(a,b):
    validate(a);validate(b)
    require(a['count']+b['count']<=UINT32_MAX,'selected entry count overflow')
    r=dict(positive=add_digits(a['positive'],b['positive']),negative=add_digits(a['negative'],b['negative']))
    for k in ('count','positive_count','negative_count','zero_count'):r[k]=a[k]+b[k]
    validate(r);return r


def reduce_bits(bits):
    require(type(bits) is tuple,'exact immutable caller leaf tuple')
    r=empty()
    for x in bits:r=merge(r,leaf(x))
    return r


def exact_totals(r):
    p,n=validate(r);scale=Fraction(1,2**149)
    return dict(signed=(p-n)*scale,absolute=(p+n)*scale,positive=p*scale,negative=-n*scale)


def encode(r):
    validate(r)
    return struct.pack('<44I',*r['positive'],*r['negative'],*(r[k] for k in ('count','positive_count','negative_count','zero_count')))


def decode(raw):
    require(type(raw) is bytes and len(raw)==RECORD_BYTES,'exact immutable limb record bytes')
    x=struct.unpack('<44I',raw)
    r=dict(positive=tuple(x[:20]),negative=tuple(x[20:40]),count=x[40],positive_count=x[41],negative_count=x[42],zero_count=x[43])
    validate(r);return r


def plan(entries,channels,threadgroup_limit):
    u32(entries);require(entries>0,'nonempty selected bound')
    require(type(channels) is int and 0<channels<=CHANNELS_MAX,'bounded channels per batch')
    require(type(threadgroup_limit) is int and threadgroup_limit>=THREADGROUP_BYTES,'caller threadgroup limit admits design')
    tiles=(entries+TILE-1)//TILE;next_tiles=(tiles+TILE-1)//TILE
    return dict(schema='rise.fire.r248.resident-f32-sum-plan.v1',selected_entry_bound=entries,channels_per_batch=channels,
                tile_entries=TILE,lanes=LANES,limbs_per_magnitude=LIMBS,radix=RADIX,record_bytes=RECORD_BYTES,
                bank_a_records=tiles*channels,bank_b_records=next_tiles*channels,
                observer_partial_buffer_bytes=(tiles+next_tiles)*channels*RECORD_BYTES,
                threadgroup_scratch_bytes=THREADGROUP_BYTES,caller_threadgroup_limit=threadgroup_limit,
                device_limit_authenticated=False,device_buffers_allocated=False,measured_current_RSS=None,measured_peak_RSS=None,
                total_owner_live_allocation_accepted=False,source34GiB_ceiling_is_reader_peak=False,
                native_kernel_implemented=False,native_parity_accepted=False,new_simulation_launched=False)


def controls():
    fixtures=((0,0x80000000,1,0x80000001,0x7f7fffff,0xff7fffff),
              (0x3f800000,0x40000000,0xbf800000,0x00800000,0x807fffff))
    for bits in fixtures:
        r=reduce_bits(bits);t=exact_totals(r)
        expected=[Fraction.from_float(struct.unpack('<f',struct.pack('<I',x))[0]) for x in bits]
        require(t['signed']==sum(expected) and t['absolute']==sum(abs(x) for x in expected),'exact f32 oracle')
        require(merge(reduce_bits(bits[:2]),reduce_bits(bits[2:]))==r and reduce_bits(tuple(reversed(bits)))==r,'merge/order independent exact integer result')
        require(decode(encode(r))==r,'exact canonical limb roundtrip')
    # Analytic maximum count proof without allocating UINT32_MAX leaves.
    maximum=((2**24-1)<<253)
    r=empty();r.update(positive=digits(maximum*UINT32_MAX),count=UINT32_MAX,positive_count=UINT32_MAX)
    require(decode(encode(r))==r,'maximum uint32 count fits320bits')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing limb/planner refusal')
    for x in (True,-1,2**32,0x7f800000,0xff800000,0x7fc00000):refuse(lambda x=x:leaf(x))
    refuse(lambda:merge(r,leaf(0)));refuse(lambda:decode(bytes(RECORD_BYTES-1)))
    bad=bytearray(encode(empty()));struct.pack_into('<I',bad,0,65536);refuse(lambda:decode(bytes(bad)))
    bad=bytearray(encode(empty()));struct.pack_into('<I',bad,160,1);refuse(lambda:decode(bytes(bad)))
    for args in ((0,1,THREADGROUP_BYTES),(2**32,1,THREADGROUP_BYTES),(1,True,THREADGROUP_BYTES),(1,65,THREADGROUP_BYTES),(1,1,THREADGROUP_BYTES-1)):
        refuse(lambda args=args:plan(*args))
    bad=empty();bad.update(positive=digits((1<<25)+1),count=1,positive_count=1);refuse(lambda:validate(bad))
    require(red==16,'targeted primitive/planner REDs')
    p=plan(105*104*160,64,THREADGROUP_BYTES)
    print(json.dumps(dict(status='pure_f32_integer_sum_primitive_and_allocation_plan_only',red=red,
        example_observer_partial_buffer_bytes=p['observer_partial_buffer_bytes'],threadgroup_bytes=THREADGROUP_BYTES,
        measured_memory_bytes=None,native_kernel_implemented=False,native_fields_read=0,new_simulation_launched=False)))


if __name__=='__main__':controls()
