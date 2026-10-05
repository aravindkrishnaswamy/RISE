#!/usr/bin/env python3
"""Bounded host exact quotient/RN-even reference for caller reduction records.

No native record authentication, reduction replay, allocation/parity or run.
Empty means unavailable; overflow unavailable; signed underflow zeros explicit.
"""
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire')
HELPERS={
 'f32':('r248_resident_sum_contract.py','c79be0c3bb79b3939d5c9015ebcee2908abc33a2a15175cb6eac5e57557a1ec2'),
 'product':('r248_resident_product_contract.py','f0095248c77c46ac8a67bed3accb0314599c90933e6f91c1da231e19fb470c93'),
 'compound':('r248_resident_compound_contract.py','66abfa13a6590c81f673e9ed62ee5110407e8e847044dc207f0da7ef81a0b828')}
OPERAND_BITS=2048
WORK_BITS=4096


def require(ok,message):
    if not ok:raise ValueError(message)


def context(family):
    require(type(family) is str and family in HELPERS,'exact reduction family')
    name,sha=HELPERS[family];p=ROOT/name;raw=p.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==sha,'accepted reduction helper hash')
    c={'__name__':'captured_reduction'};exec(compile(raw,str(p),'exec'),c);return c,sha


def quotient_round(n,d,shift):
    if shift>=0:n<<=shift
    else:d<<=-shift
    require(n.bit_length()<=WORK_BITS and d.bit_length()<=WORK_BITS,'bounded integer division workspace')
    q,r=divmod(n,d);increment=(2*r>d or (2*r==d and q%2==1))
    return q+int(increment),r!=0


def round_rational(n,d):
    require(type(n) is int and type(d) is int and d>0 and
            abs(n).bit_length()<=OPERAND_BITS and d.bit_length()<=OPERAND_BITS,'bounded signed numerator/positive denominator')
    if n==0:return dict(available=True,binary64_bits=0,status='exact_zero',inexact=False,underflow=False,rounded_nonzero_to_zero=False)
    sign=(1<<63) if n<0 else 0;n=abs(n)
    e=n.bit_length()-d.bit_length()
    if (n<(d<<e)) if e>=0 else ((n<<-e)<d):e-=1
    if e>1023:return dict(available=False,binary64_bits=None,status='overflow',inexact=True,underflow=False,rounded_nonzero_to_zero=False)
    if e<-1022:
        q,inexact=quotient_round(n,d,1074)
        require(q<=1<<52,'subnormal rounding bound')
        return dict(available=True,binary64_bits=sign|q,status='rounded_zero' if q==0 else ('subnormal' if q<1<<52 else 'normal'),
                    inexact=inexact,underflow=inexact and q<1<<52,rounded_nonzero_to_zero=q==0)
    q,inexact=quotient_round(n,d,52-e)
    if q==1<<53:q>>=1;e+=1
    if e>1023:return dict(available=False,binary64_bits=None,status='overflow',inexact=True,underflow=False,rounded_nonzero_to_zero=False)
    require(1<<52<=q<1<<53,'normal significand bound')
    return dict(available=True,binary64_bits=sign|((e+1023)<<52)|(q-(1<<52)),status='normal',
                inexact=inexact,underflow=False,rounded_nonzero_to_zero=False)


def positive_measure(bits,width):
    require(type(width) is int and width in (32,64),'exact binary measure width')
    require(type(bits) is int and 0<=bits<1<<width,'typed measure bits')
    value=struct.unpack('<f' if width==32 else '<d',struct.pack('<I' if width==32 else '<Q',bits))[0]
    # Fraction refuses NaN/inf; check exponent explicitly before conversion.
    exponent=(bits>>23)&255 if width==32 else (bits>>52)&2047
    require(exponent<(255 if width==32 else 2047) and value>0,'positive finite represented measure')
    return Fraction.from_float(value)


def finalize(raw,family,mode='sum',dt_binary32_bits=None,multiplier_binary64_bits=None):
    require(type(raw) is bytes,'immutable canonical caller record')
    require(type(mode) is str and mode in ('sum','mean'),'exact sum/mean mode')
    c,sha=context(family);r=c['decode'](raw);totals=c['exact_totals'](r)
    divisor=positive_measure(dt_binary32_bits,32) if dt_binary32_bits is not None else Fraction(1)
    multiplier=positive_measure(multiplier_binary64_bits,64) if multiplier_binary64_bits is not None else Fraction(1)
    if mode=='mean' and r['count']==0:
        values={k:dict(available=False,binary64_bits=None,status='empty_mean',inexact=False,underflow=False,
                       rounded_nonzero_to_zero=False) for k in totals}
    else:
        if mode=='mean':divisor*=r['count']
        values={}
        for k,v in totals.items():
            q=v*multiplier/divisor;values[k]=round_rational(q.numerator,q.denominator)
    return dict(schema='rise.fire.r248.exact-finalization.v1',family=family,mode=mode,
                source_record_sha256=hashlib.sha256(raw).hexdigest(),captured_helper_sha256=sha,values=values,
                dt_binary32_bits=dt_binary32_bits,multiplier_binary64_bits=multiplier_binary64_bits,
                native_record_authenticated=False,measure_semantics_authenticated=False,
                native_operation_replayed=False,native_allocation_accepted=False,physical_acceptance=False)


def contract_layout():
    return dict(schema='rise.fire.r248.exact-finalization-contract.v1',operand_magnitude_bits=OPERAND_BITS,
                division_work_magnitude_bits=WORK_BITS,rounding='binary64 round-to-nearest ties-to-even; integer arithmetic',
                denominator='positive exact rational represented binary32 dt and optional uint32 record count',
                multiplier='one positive finite represented binary64 multiplier; not inferred dx/area/volume',
                empty_mean='unavailable; no defaultzero',overflow='unavailable; no finite fabricated value',
                underflow='IEEE signed rounded zero/subnormal with explicit inexact/tininess-after-rounding flags',
                total_live_memory_measured=False,native_kernel_implemented=False,native_record_authenticated=False,
                new_simulation_launched=False)


def controls():
    # Analytic exact halfway-to-even and binade carry; no epsilon.
    for n,d,bits in ((1,1,0x3ff0000000000000),((1<<53)+1,1<<53,0x3ff0000000000000),
                     ((1<<53)+3,1<<53,0x3ff0000000000002),((1<<54)-1,1<<53,0x4000000000000000),
                     (1,1<<1074,1),(1,1<<1075,0),(-1,1<<1075,1<<63),(3,1<<1075,2)):
        require(round_rational(n,d)['binary64_bits']==bits,'analytic RN-even exact rounding')
    require(round_rational((1<<53)-1,1<<1075)['binary64_bits']==1<<52,'subnormal-to-normal carry')
    require(not round_rational(1<<1024,1)['available'] and round_rational(1<<1024,1)['status']=='overflow','overflow unavailable')
    require(round_rational(1,1<<1075)['underflow'] and round_rational(-1,1<<1075)['rounded_nonzero_to_zero'],'explicit signed underflow')
    c,_=context('f32');r=c['reduce_bits']((0x3f800000,0x40000000,0xbf800000));raw=c['encode'](r)
    a=finalize(raw,'f32','mean',0x3f000000,0x4000000000000000)
    require(a['values']['signed']['binary64_bits']==struct.unpack('<Q',struct.pack('<d',8/3))[0],'represented dt/count/multiplier finalization')
    require(finalize(c['encode'](c['empty']()),'f32','mean')['values']['signed']['status']=='empty_mean','empty mean unavailable')
    require(finalize(c['encode'](c['empty']()),'f32')['values']['signed']['status']=='exact_zero','empty sum knownzero')
    for family in ('product','compound'):
        c,_=context(family)
        r=c['leaf'](1,1,1,0) if family=='product' else c['leaf']('low_plus_alpha_anti',0,1,1)
        out=finalize(c['encode'](r),family,dt_binary32_bits=1)
        require(out['values']['signed']['binary64_bits']==struct.unpack('<Q',struct.pack('<d',2.**-149))[0],'exact fine-lattice dt quotient')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing exact finalization refusal')
    for args in ((True,1),(1,True),(1,0),(1,-1),(1<<2048,1),(1,1<<2048)):
        refuse(lambda args=args:round_rational(*args))
    for bits in (True,-1,2**32,0,0x80000000,0xbf800000,0x7f800000,0x7fc00000):
        refuse(lambda bits=bits:finalize(raw,'f32',dt_binary32_bits=bits))
    for bits in (0,0x8000000000000000,0xbff0000000000000,0x7ff0000000000000,0x7ff8000000000000):
        refuse(lambda bits=bits:finalize(raw,'f32',multiplier_binary64_bits=bits))
    refuse(lambda:finalize(bytearray(raw),'f32'));refuse(lambda:finalize(raw,'unknown'));refuse(lambda:finalize(raw,'f32','unknown'))
    require(red==22,'finalization targeted RED count')
    print(json.dumps(dict(status='pure_exact_quotient_RNeven_host_finalization_only',red=red,
                         operand_bits=OPERAND_BITS,work_bits=WORK_BITS,native_fields_read=0,
                         native_record_authenticated=False,native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
