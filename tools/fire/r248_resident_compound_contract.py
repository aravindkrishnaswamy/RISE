#!/usr/bin/env python3
"""Pure exact compound leaf/sign/moment oracle. Native kernels/auth unavailable.

gas+soot or low+alpha*anti are composed before sign splitting at2^-298 scale.
This is an unrounded dyadic reference, not a materialized native f32/FMA replay.
"""
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct

PRODUCT=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_resident_product_contract.py')
PRODUCT_SHA='f0095248c77c46ac8a67bed3accb0314599c90933e6f91c1da231e19fb470c93'
UINT32_MAX=2**32-1
LIMBS=33
RECORD_BYTES=288
MAX_F32_WEIGHT=((2**24-1)<<253)
MAX_COMPOUND=2*MAX_F32_WEIGHT*(1<<149)
KINDS=('gas_plus_soot','low_plus_alpha_anti')


def require(ok,message):
    if not ok:raise ValueError(message)


def context():
    raw=PRODUCT.read_bytes();require(hashlib.sha256(raw).hexdigest()==PRODUCT_SHA,'accepted product helper hash')
    c={'__name__':'captured_product_primitive'};exec(compile(raw,str(PRODUCT),'exec'),c);return c


def u32(x):require(type(x) is int and 0<=x<=UINT32_MAX,'typed uint32');return x


def kind_order(kind,power):
    require(type(kind) is str and kind in KINDS,'typed compound kind')
    require(type(power) is int and power in (0,1,2),'typed compound coordinate order')


def signed_weight(c,bits):
    n=c['factor_weight'](bits);return -n if bits>>31 else n


def compound(c,kind,first,second,third):
    kind_order(kind,0)
    a=signed_weight(c,first);b=signed_weight(c,second)
    if kind==KINDS[0]:
        require(type(third) is int and third==0,'canonical unused gas+soot third slot')
        return (a+b)<<149
    alpha=signed_weight(c,third);require(0<=alpha<=1<<149,'represented alpha in0..1')
    return (a<<149)+b*alpha


def digits(n):
    require(type(n) is int and 0<=n<2**(16*LIMBS),'bounded compound integer magnitude')
    return tuple((n>>(16*i))&65535 for i in range(LIMBS))


def number(d):
    require(type(d) is tuple and len(d)==LIMBS,'exact compound digit tuple')
    for x in d:require(type(x) is int and 0<=x<=65535,'canonical low16 digit')
    return sum(x<<(16*i) for i,x in enumerate(d))


def empty(kind,power):
    kind_order(kind,power);return dict(kind=kind,power=power,positive=digits(0),negative=digits(0),count=0,positive_count=0,negative_count=0,zero_count=0)


def validate(r):
    require(type(r) is dict and set(r)=={'kind','power','positive','negative','count','positive_count','negative_count','zero_count'},'exact compound record')
    kind_order(r['kind'],r['power'])
    for k in ('count','positive_count','negative_count','zero_count'):u32(r[k])
    require(r['count']==r['positive_count']+r['negative_count']+r['zero_count'],'compound count partition')
    p,n=number(r['positive']),number(r['negative']);maximum=MAX_COMPOUND*UINT32_MAX**r['power']
    for value,count in ((p,r['positive_count']),(n,r['negative_count'])):
        minimum=(1<<149) if r['kind']==KINDS[0] else 1
        require(count*minimum<=value<=count*maximum if count else value==0,'necessary compound magnitude bound')
        if r['kind']==KINDS[0]:require(value%(1<<149)==0,'exact gas+soot lattice at common scale')
    return p,n


def leaf_with_context(c,kind,first,second,third,coordinate2,power):
    kind_order(kind,power);u32(coordinate2)
    n=compound(c,kind,first,second,third)*coordinate2**power
    r=empty(kind,power);r['count']=1
    if not n:r['zero_count']=1
    elif n>0:r['positive']=digits(n);r['positive_count']=1
    else:r['negative']=digits(-n);r['negative_count']=1
    return r


def leaf(kind,first,second,third=0,coordinate2=0,power=0):
    return leaf_with_context(context(),kind,first,second,third,coordinate2,power)


def add_digits(a,b):
    number(a);number(b);out=[];carry=0
    for x,y in zip(a,b):
        total=x+y+carry;require(total<=131071,'compound uint32 carry bound')
        out.append(total&65535);carry=total>>16
    require(carry==0,'compound final carry overflow');return tuple(out)


def merge(a,b):
    validate(a);validate(b);require((a['kind'],a['power'])==(b['kind'],b['power']),'matching compound kind/order')
    require(a['count']+b['count']<=UINT32_MAX,'compound count overflow')
    r=empty(a['kind'],a['power']);r['positive']=add_digits(a['positive'],b['positive']);r['negative']=add_digits(a['negative'],b['negative'])
    for k in ('count','positive_count','negative_count','zero_count'):r[k]=a[k]+b[k]
    validate(r);return r


def reduce_entries(entries,kind,power=0):
    kind_order(kind,power);require(type(entries) is tuple and len(entries)<=UINT32_MAX,'immutable bounded compound entries')
    c=context();r=empty(kind,power)
    for x in entries:
        require(type(x) is tuple and len(x)==4,'exact compound factor/coordinate entry')
        r=merge(r,leaf_with_context(c,kind,*x,power))
    return r


def exact_totals(r):
    p,n=validate(r);s=Fraction(1,2**(298+r['power']))
    return dict(signed=(p-n)*s,absolute=(p+n)*s,positive=p*s,negative=-n*s)


def positive_moments(entries,kind):
    kind_order(kind,0);require(type(entries) is tuple and len(entries)<=UINT32_MAX,'bounded same-leaf spatial witness')
    c=context();r=[empty(kind,p) for p in (0,1,2)]
    for x in entries:
        require(type(x) is tuple and len(x)==4,'exact moment witness entry')
        a,b,d,coord=x;u32(coord);require(coord>0 and coord%2==1,'caller native cell centre coordinate2 oddpositive')
        n=compound(c,kind,a,b,d)
        if n<=0:continue
        for p in (0,1,2):r[p]=merge(r[p],leaf_with_context(c,kind,a,b,d,coord,p))
    p,m1,m2=(exact_totals(v)['positive'] for v in r)
    if not p:return dict(positive_count=0,positive_weight=p,centroid=None,variance=None,joint_caller_witness=True,native_geometry_authenticated=False)
    mean=m1/p;variance=m2/p-mean**2
    require(variance>=0 and len({v['positive_count'] for v in r})==1,'joint samepositiveleaf moments')
    return dict(positive_count=r[0]['positive_count'],positive_weight=p,centroid=mean,variance=variance,
                joint_caller_witness=True,native_geometry_authenticated=False)


def encode(r):
    validate(r);return struct.pack('<72I',KINDS.index(r['kind']),r['power'],*r['positive'],*r['negative'],
                                  *(r[k] for k in ('count','positive_count','negative_count','zero_count')))


def decode(raw):
    require(type(raw) is bytes and len(raw)==RECORD_BYTES,'exact immutable compound bytes')
    x=struct.unpack('<72I',raw);require(x[0]<2,'exact compound kind code')
    r=dict(kind=KINDS[x[0]],power=x[1],positive=tuple(x[2:35]),negative=tuple(x[35:68]),count=x[68],positive_count=x[69],negative_count=x[70],zero_count=x[71])
    validate(r);return r


def plan(entries,channels,threadgroup_limit):
    u32(entries);require(entries>0,'nonempty compound bound')
    require(type(channels) is int and 0<channels<=64,'bounded compound batch')
    scratch=16*RECORD_BYTES+32;require(type(threadgroup_limit) is int and threadgroup_limit>=scratch,'caller compound scratch limit')
    a=(entries+255)//256;b=(a+255)//256
    return dict(schema='rise.fire.r248.compound-plan.v1',entries=entries,channels=channels,record_bytes=RECORD_BYTES,
                observer_partial_buffer_bytes=(a+b)*channels*RECORD_BYTES,threadgroup_scratch_bytes=scratch,
                device_limit_authenticated=False,native_kernel_implemented=False,native_allocation_accepted=False,
                measured_current_RSS=None,measured_peak_RSS=None,new_simulation_launched=False)


def controls():
    one=0x3f800000;two=0x40000000;negone=0xbf800000
    gas=((one,negone,0,1),(two,negone,0,3),(1,0,0,5))
    flux=((negone,two,0x3f000000,1),(one,negone,one,3),(one,one,0x80000000,5))
    f=lambda x:Fraction.from_float(struct.unpack('<f',struct.pack('<I',x))[0])
    for kind,entries in zip(KINDS,(gas,flux)):
        for p in (0,1,2):
            values=[(f(a)+f(b) if kind==KINDS[0] else f(a)+f(b)*f(d))*Fraction(coord,2)**p for a,b,d,coord in entries]
            r=reduce_entries(entries,kind,p);t=exact_totals(r)
            require(t['signed']==sum(values) and t['absolute']==sum(abs(v) for v in values),'compose-before-sign exact oracle')
            require(decode(encode(r))==r and reduce_entries(tuple(reversed(entries)),kind,p)==r,'order/codec exact')
        require(reduce_entries(entries,kind)['positive_count']==(2 if kind==KINDS[0] else 1),'cancelled support not separatepositiveparts')
    m=positive_moments(((one,0,0,1),(0x40400000,0,0,5)),KINDS[0])
    require(m['centroid']==2 and m['variance']==Fraction(3,4) and m['positive_count']==2,'joint weighted moment exact')
    m=positive_moments(((0x7f7fffff,0,0,1),(1,0,0,5)),KINDS[0]);require(m['variance']>0,'maxsubnormal spread survives exact cancellation')
    z=positive_moments(((one,negone,0,1),),KINDS[0]);require(z['centroid'] is None and z['variance'] is None,'empty composedpositive support unavailable')
    maximum=MAX_COMPOUND*UINT32_MAX**3
    r=empty(KINDS[0],2);r.update(positive=digits(maximum),count=UINT32_MAX,positive_count=UINT32_MAX)
    require(maximum.bit_length()==523 and decode(encode(r))==r,'523bit maximum fits528')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing compound refusal')
    for bits in (True,-1,2**32,0x7f800000,0x7fc00000):refuse(lambda bits=bits:leaf(KINDS[0],bits,0))
    refuse(lambda:leaf(KINDS[0],one,0,0x80000000))
    for alpha in (negone,0x3f800001,0x7f800000):refuse(lambda alpha=alpha:leaf(KINDS[1],one,one,alpha))
    for p in (True,-1,3):refuse(lambda p=p:leaf(KINDS[0],0,0,0,0,p))
    refuse(lambda:merge(empty(KINDS[0],0),empty(KINDS[1],0)));refuse(lambda:merge(r,leaf(KINDS[0],0,0,0,0,2)))
    for coord in (0,2,True,2**32):refuse(lambda coord=coord:positive_moments(((one,0,0,coord),),KINDS[0]))
    bad=bytearray(encode(empty(KINDS[0],0)));struct.pack_into('<I',bad,8,65536);refuse(lambda:decode(bytes(bad)))
    bad=bytearray(encode(empty(KINDS[0],0)));struct.pack_into('<I',bad,272,1);refuse(lambda:decode(bytes(bad)))
    refuse(lambda:decode(bytes(RECORD_BYTES-1)));refuse(lambda:plan(1,65,4640));refuse(lambda:plan(1,1,4639))
    bad=empty(KINDS[0],0);bad.update(positive=digits((1<<149)+1),count=1,positive_count=1);refuse(lambda:validate(bad))
    bad=empty(KINDS[0],0);bad.update(positive=digits(1<<149),count=2,positive_count=2);refuse(lambda:validate(bad))
    require(red==25,'compound targeted RED count')
    p=plan(105*104*160,64,4640)
    print(json.dumps(dict(status='pure_compound_sign_and_joint_positive_moment_contract',red=red,record_bytes=RECORD_BYTES,
        max_magnitude_bits=523,example_partial_bank_bytes=p['observer_partial_buffer_bytes'],threadgroup_bytes=4640,
        native_fields_read=0,native_kernel_implemented=False,native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
