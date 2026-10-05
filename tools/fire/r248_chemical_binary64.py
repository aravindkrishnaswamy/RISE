#!/usr/bin/env python3
"""Exact caller binary64 chemical compound moments. No native authentication.

Gas+soot is composed before sign at common2^-1074. Capture bytes once for
orders0/1/2. Returned exact rationals are not a bounded rounded finalizer or
an admitted standalone summary. Logical limb bounds are not measured memory.
"""
from fractions import Fraction
import hashlib
import struct

UINT32_MAX = (1 << 32) - 1
MAX_WEIGHT = ((1 << 53) - 1) << 2045
LIMBS = 138
CAPTURE_LIMIT = 2 << 30  # unchanged public fixture cap, combined caller bytes


def require(ok, message):
    if not ok:
        raise ValueError(message)


def weight(bits):
    require(type(bits) is int and 0 <= bits < 1 << 64, 'typed binary64 bits')
    exponent = (bits >> 52) & 2047
    fraction = bits & ((1 << 52) - 1)
    require(exponent != 2047, 'finite binary64 chemical operand')
    value = fraction if exponent == 0 else ((1 << 52) | fraction) << (exponent - 1)
    return -value if bits >> 63 else value


def record(positive=0, negative=0, count=0, positive_count=0, negative_count=0, zero_count=0, order=0):
    r = dict(positive=positive, negative=negative, count=count, positive_count=positive_count,
             negative_count=negative_count, zero_count=zero_count, order=order)
    validate(r)
    return r


def validate(r):
    require(type(r) is dict and set(r) == {'positive', 'negative', 'count', 'positive_count',
                                         'negative_count', 'zero_count', 'order'}, 'exact record keys')
    require(type(r['order']) is int and r['order'] in (0, 1, 2), 'coordinate order')
    for k in ('count', 'positive_count', 'negative_count', 'zero_count'):
        require(type(r[k]) is int and 0 <= r[k] <= UINT32_MAX, 'typed bounded counts')
    require(r['count'] == r['positive_count'] + r['negative_count'] + r['zero_count'], 'count partition')
    maximum = 2 * MAX_WEIGHT * UINT32_MAX ** r['order']
    for k, ck in (('positive', 'positive_count'), ('negative', 'negative_count')):
        n, count = r[k], r[ck]
        require(type(n) is int and 0 <= n < 1 << (16 * LIMBS), 'bounded magnitude')
        require(count <= n <= count * maximum if count else n == 0, 'necessary support magnitude')
    return r


def add(a, b):
    validate(a); validate(b)
    require(a['order'] == b['order'], 'same coordinate order')
    return record(**{k: a[k] + b[k] if k != 'order' else a[k] for k in a})


def capture(gas, soot, coordinate2):
    # Restrict to byte buffers with known length; no arbitrary coercion/iterator.
    for field in (gas, soot, coordinate2):
        require(type(field) in (bytes, bytearray), 'byte buffer capture only')
    require(len(gas) == len(soot) and len(gas) % 8 == 0, 'aligned paired binary64 fields')
    count = len(gas) // 8
    require(count <= UINT32_MAX and len(coordinate2) == 4 * count, 'bounded coordinate count')
    require(len(gas) + len(soot) + len(coordinate2) <= CAPTURE_LIMIT, 'combined public capture cap')
    return bytes(gas), bytes(soot), bytes(coordinate2)


def reduce(gas, soot, coordinate2):
    fields = capture(gas, soot, coordinate2)
    rows = [record(order=j) for j in range(3)]
    for i in range(len(fields[0]) // 8):
        a = struct.unpack_from('<Q', fields[0], 8 * i)[0]
        b = struct.unpack_from('<Q', fields[1], 8 * i)[0]
        c = struct.unpack_from('<I', fields[2], 4 * i)[0]
        require(c > 0 and c & 1, 'positive odd caller cell-center coordinate2')
        n = weight(a) + weight(b)
        # SAME captured composed leaf and sign support for all three moments.
        for j in range(3):
            v = n * c ** j
            leaf = record(positive=max(v, 0), negative=max(-v, 0), count=1,
                          positive_count=int(v > 0), negative_count=int(v < 0),
                          zero_count=int(v == 0), order=j)
            rows[j] = add(rows[j], leaf)
    p, m1, m2 = (Fraction(r['positive'], 1 << (1074 + r['order'])) for r in rows)
    geometry = None if p == 0 else dict(centroid=m1 / p, variance=m2 / p - (m1 / p) ** 2)
    if geometry is not None:
        require(geometry['variance'] >= 0, 'joint exact nonnegative variance')
    return dict(rows=tuple(rows), positive_geometry=geometry,
                capture_sha256=tuple(hashlib.sha256(x).hexdigest() for x in fields),
                native_authenticated=False, bounded_rounded_finalization_accepted=False,
                measured_allocation_accepted=False, physical_acceptance=False)


def contract():
    return dict(scale_exponent=-1074, limb_radix_bits=16, limbs=LIMBS,
                worst_aggregate_bits=(2 * MAX_WEIGHT * UINT32_MAX ** 3).bit_length(),
                capture_combined_byte_cap=CAPTURE_LIMIT,
                capture_plus_working_memory_measured=False,
                scratch='Three positive/negative Python integers plus transient exact rationals; logical bound only',
                reference='Exact represented binary64 gas+soot before sign, caller odd coordinates',
                standalone_summary_admission=False, native_source_or_geometry_authentication=False,
                rounded_finalization=False, new_simulation=False)
