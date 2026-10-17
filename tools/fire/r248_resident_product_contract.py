#!/usr/bin/env python3
"""Exact two-f32 product with coordinate weights; pure integer oracle/design.

No native multiply/reduction replay, gas+soot sign composition, rate division,
geometry authentication, Metal kernel or allocation qualification.
"""
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct

BASE=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_resident_sum_contract.py')
BASE_SHA='c79be0c3bb79b3939d5c9015ebcee2908abc33a2a15175cb6eac5e57557a1ec2'
UINT32_MAX=2**32-1
LIMBS=41
RECORD_BYTES=352
TILE=256
LANES=16
THREADGROUP_BYTES=LANES*RECORD_BYTES+32
MAX_WEIGHT=((2**24-1)<<253)


def require(ok,message):
    if not ok:raise ValueError(message)


def base_context():
    raw=BASE.read_bytes();require(hashlib.sha256(raw).hexdigest()==BASE_SHA,'accepted sum primitive digest')
    c={'__name__':'captured_sum_primitive'};exec(compile(raw,str(BASE),'exec'),c);return c


def u32(x):
    require(type(x) is int and 0<=x<=UINT32_MAX,'typed uint32');return x


def order(p):require(type(p) is int and p in (0,1,2),'typed coordinate moment order');return p


def digits(n):
    require(type(n) is int and 0<=n<2**(16*LIMBS),'bounded exact product accumulator')
    return tuple((n>>(16*i))&65535 for i in range(LIMBS))


def number(d):
    require(type(d) is tuple and len(d)==LIMBS,'exact product digit tuple')
    for x in d:require(type(x) is int and 0<=x<=65535,'canonical low16 uint32 digit')
    return sum(x<<(16*i) for i,x in enumerate(d))


def empty(power):
    order(power);return dict(power=power,positive=digits(0),negative=digits(0),count=0,positive_count=0,negative_count=0,zero_count=0)


def validate(r):
    require(type(r) is dict and set(r)=={'power','positive','negative','count','positive_count','negative_count','zero_count'},'exact weighted product record')
    order(r['power'])
    for k in ('count','positive_count','negative_count','zero_count'):u32(r[k])
    require(r['count']==r['positive_count']+r['negative_count']+r['zero_count'],'product count partition')
    p,n=number(r['positive']),number(r['negative']);maximum=MAX_WEIGHT**2*UINT32_MAX**r['power']
    for value,count in ((p,r['positive_count']),(n,r['negative_count'])):
        require(count<=value<=count*maximum if count else value==0,'exact product necessary support bounds')
    return p,n


def factor_weight(bits):
    u32(bits);e=(bits>>23)&255;f=bits&0x7fffff;require(e<255,'finite represented factor')
    return ((f|(1<<23))<<(e-1)) if e else f


def leaf(first,second,coordinate2=0,power=0):
    order(power);u32(coordinate2)
    w=factor_weight(first)*factor_weight(second)*coordinate2**power
    r=empty(power);r['count']=1
    if not w:r['zero_count']=1
    elif (first^second)>>31:r['negative']=digits(w);r['negative_count']=1
    else:r['positive']=digits(w);r['positive_count']=1
    return r


def add_digits(a,b):
    number(a);number(b);carry=0;out=[]
    for x,y in zip(a,b):
        z=x+y+carry;require(z<=131071,'uint32 digit-add bound');out.append(z&65535);carry=z>>16
    require(carry==0,'product carry overflow');return tuple(out)


def merge(a,b):
    validate(a);validate(b);require(a['power']==b['power'],'same coordinate moment order')
    require(a['count']+b['count']<=UINT32_MAX,'product entry count overflow')
    r=empty(a['power']);r['positive']=add_digits(a['positive'],b['positive']);r['negative']=add_digits(a['negative'],b['negative'])
    for k in ('count','positive_count','negative_count','zero_count'):r[k]=a[k]+b[k]
    validate(r);return r


def reduce_entries(entries,power):
    require(type(entries) is tuple,'immutable factor/coordinate witness tuple');r=empty(power)
    for x in entries:
        require(type(x) is tuple and len(x)==3,'exact pair/coordinate entry')
        r=merge(r,leaf(*x,power))
    return r


def exact_totals(r):
    p,n=validate(r);scale=Fraction(1,2**(298+r['power']))
    return dict(signed=(p-n)*scale,absolute=(p+n)*scale,positive=p*scale,negative=-n*scale)


def encode(r):
    validate(r);return struct.pack('<88I',r['power'],0,*r['positive'],*r['negative'],
          *(r[k] for k in ('count','positive_count','negative_count','zero_count')))


def decode(raw):
    require(type(raw) is bytes and len(raw)==RECORD_BYTES,'exact immutable weighted record bytes')
    x=struct.unpack('<88I',raw);require(x[1]==0,'canonical reserved word')
    r=dict(power=x[0],positive=tuple(x[2:43]),negative=tuple(x[43:84]),count=x[84],positive_count=x[85],negative_count=x[86],zero_count=x[87])
    validate(r);return r


def plan(entries,channels,threadgroup_limit):
    u32(entries);require(entries>0,'nonempty product bound')
    require(type(channels) is int and 0<channels<=64,'bounded product batch')
    require(type(threadgroup_limit) is int and threadgroup_limit>=THREADGROUP_BYTES,'caller product threadgroup reservation')
    a=(entries+255)//256;b=(a+255)//256
    return dict(schema='rise.fire.r248.resident-product-plan.v1',entries=entries,channels=channels,
                record_bytes=RECORD_BYTES,limbs=LIMBS,tile=TILE,lanes=LANES,
                observer_partial_buffer_bytes=(a+b)*channels*RECORD_BYTES,
                threadgroup_scratch_bytes=THREADGROUP_BYTES,caller_threadgroup_limit=threadgroup_limit,
                device_limit_authenticated=False,native_kernel_implemented=False,native_allocation_accepted=False,
                measured_current_RSS=None,measured_peak_RSS=None,new_simulation_launched=False)


def controls():
    base=base_context()
    samples=((1,1,1),(0x7f7fffff,0x7f7fffff,UINT32_MAX),(0xff7fffff,1,3),
             (0x3f800000,0xbf800000,5),(0x80000000,0xff7fffff,7),(0x3fc00000,0x40000000,9))
    f=lambda bits:Fraction.from_float(struct.unpack('<f',struct.pack('<I',bits))[0])
    for power in (0,1,2):
        r=reduce_entries(samples,power);expected=[f(a)*f(b)*Fraction(c,2)**power for a,b,c in samples]
        require(exact_totals(r)['signed']==sum(expected) and exact_totals(r)['absolute']==sum(abs(x) for x in expected),'exact unrounded products/coordinate moments')
        require(merge(reduce_entries(samples[:2],power),reduce_entries(samples[2:],power))==r and
                reduce_entries(tuple(reversed(samples)),power)==r and decode(encode(r))==r,'partition/order/codec')
    # At power0, multiplying by represented1 agrees exactly with captured f32 sum.
    bits=(1,0x7f7fffff,0xff7fffff,0x80000000)
    require(exact_totals(reduce_entries(tuple((x,0x3f800000,0) for x in bits),0))==base['exact_totals'](base['reduce_bits'](bits)),'one-factor exact reduction equivalence')
    maximum=MAX_WEIGHT**2*UINT32_MAX**3
    r=empty(2);r.update(positive=digits(maximum),count=UINT32_MAX,positive_count=UINT32_MAX)
    require(maximum.bit_length()==650 and decode(encode(r))==r,'650bit maxcount-squarecoordinate capacity')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing weighted product refusal')
    for bits in (True,-1,2**32,0x7f800000,0x7fc00000):refuse(lambda bits=bits:leaf(bits,0))
    for coord in (True,-1,2**32):refuse(lambda coord=coord:leaf(0,0,coord,1))
    for power in (True,-1,3):refuse(lambda power=power:leaf(0,0,0,power))
    refuse(lambda:merge(empty(0),empty(1)));refuse(lambda:merge(r,leaf(0,0,0,2)))
    refuse(lambda:decode(bytes(RECORD_BYTES-1)))
    bad=bytearray(encode(empty(0)));struct.pack_into('<I',bad,4,1);refuse(lambda:decode(bytes(bad)))
    bad=bytearray(encode(empty(0)));struct.pack_into('<I',bad,8,65536);refuse(lambda:decode(bytes(bad)))
    bad=bytearray(encode(empty(0)));struct.pack_into('<I',bad,336,1);refuse(lambda:decode(bytes(bad)))
    for args in ((0,1,THREADGROUP_BYTES),(1,65,THREADGROUP_BYTES),(1,1,THREADGROUP_BYTES-1)):
        refuse(lambda args=args:plan(*args))
    require(red==20,'product control RED count')
    p=plan(105*104*160,64,THREADGROUP_BYTES)
    print(json.dumps(dict(status='pure_two_f32_product_coordinate_integer_contract_only',red=red,record_bytes=RECORD_BYTES,
       maximum_magnitude_bits=650,example_partial_bank_bytes=p['observer_partial_buffer_bytes'],threadgroup_bytes=THREADGROUP_BYTES,
       native_fields_read=0,native_kernel_implemented=False,native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
