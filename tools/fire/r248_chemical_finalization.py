#!/usr/bin/env python3
"""Separate bounded integer binary64 chemical caller finalization reference.

No native source/lifetime/geometry authentication or measured resource bound.
Only immutable paired witnesses; no summary-only admission or power*dt again.
"""
from fractions import Fraction
import hashlib
from pathlib import Path
import struct

OPERAND_BITS = 4608
WORK_BITS = 6144
CHEMICAL = Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_chemical_binary64.py')
CHEMICAL_SHA = '80b1644433fd84390c128ccc24db98660194450d7d506fe532967ffa07e77f15'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def quotient_round(n,d,shift):
    require(type(n) is int and n>=0 and type(d) is int and d>0 and
            type(shift) is int and -WORK_BITS<=shift<=WORK_BITS,
            'typed bounded division operands and shift')
    require(n.bit_length()<=WORK_BITS and d.bit_length()<=WORK_BITS,
            'bounded division input workspace')
    require((n.bit_length()+shift<=WORK_BITS if shift>=0 else
             d.bit_length()-shift<=WORK_BITS), 'division workspace before shift')
    if shift>=0:n<<=shift
    else:d<<=-shift
    require(n.bit_length()<=WORK_BITS and d.bit_length()<=WORK_BITS,'bounded integer division workspace')
    q,r=divmod(n,d);increment=(2*r>d or (2*r==d and q%2==1))
    return q+int(increment),r!=0


def round_rational(n,d):
    require(type(n) is int and type(d) is int and d>0 and
            n.bit_length()<=OPERAND_BITS and d.bit_length()<=OPERAND_BITS,'bounded signed numerator/positive denominator')
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


def context():
    raw = CHEMICAL.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == CHEMICAL_SHA, 'accepted binary64 chemical helper')
    c = {'__name__': 'captured_chemical_reference'}
    exec(compile(raw, str(CHEMICAL), 'exec'), c)
    return c


def positive_measure(bits):
    require(type(bits) is int and 0 <= bits < 1 << 64, 'typed represented binary64 measure')
    require(bits >> 63 == 0 and ((bits >> 52) & 2047) != 2047 and bits != 0,
            'positive finite represented measure')
    return Fraction.from_float(struct.unpack('<d', struct.pack('<Q', bits))[0])


def finalize(gas, soot, coordinate2, multiplier_binary64_bits=None):
    # One helper capture feeds moments and hashes; do not recapture caller inputs.
    c = context(); exact = c['reduce'](gas, soot, coordinate2)
    multiplier = positive_measure(multiplier_binary64_bits) if multiplier_binary64_bits is not None else Fraction(1)
    row = exact['rows'][0]; scale = 1 << 1074
    p, n = Fraction(row['positive'], scale), Fraction(row['negative'], scale)
    values = {'signed': p - n, 'absolute': p + n, 'positive': p, 'negative_magnitude': n}
    rounded = {}
    for name, value in values.items():
        q = value * multiplier
        rounded[name] = round_rational(q.numerator, q.denominator)
    geometry = exact['positive_geometry']
    if geometry is None:
        rounded_geometry = {name: {'available': False, 'binary64_bits': None, 'status': 'empty_positive_support',
                                   'inexact': False, 'underflow': False, 'rounded_nonzero_to_zero': False}
                            for name in ('centroid', 'variance')}
    else:
        rounded_geometry = {name: round_rational(value.numerator, value.denominator)
                            for name, value in geometry.items()}
    return dict(values=rounded, geometry_cell_units=rounded_geometry, count=row['count'],
                positive_count=row['positive_count'], capture_sha256=exact['capture_sha256'],
                helper_sha256=CHEMICAL_SHA, multiplier_binary64_bits=multiplier_binary64_bits,
                measure_semantics_authenticated=False, native_source_authenticated=False,
                measured_allocation_accepted=False, physical_acceptance=False)


def contract():
    return dict(operand_magnitude_bits=OPERAND_BITS, division_work_magnitude_bits=WORK_BITS,
                twice_remainder_comparison_max_bits=WORK_BITS + 1,
                conservative_sum_multiplier_numerator_bits=4230,
                conservative_sum_multiplier_denominator_bits=2149,
                conservative_joint_variance_numerator_bits=4327,
                conservative_joint_variance_denominator_bits=4265,
                generic_max_operand_plus_subnormal_shift_bits=OPERAND_BITS + 1074,
                conversion='One exact represented binary64 positive multiplier for sum quantities only',
                geometry='Centroid in caller cell units; variance in caller cell units squared; no dx conversion',
                rounded='One integer RN-even per exact rational; unavailable overflow/empty geometry',
                original_2048_4096_finalizer_unchanged=True,
                measured_memory=False, source_authentication=False, new_simulation=False)
