#!/usr/bin/env python3
"""Independent Fraction controls for the caller binary64 chemical reference."""
from fractions import Fraction
import importlib.util
import json
from pathlib import Path
import random
import struct
import sys


def main():
    output = Path(sys.argv[1]); output.mkdir(parents=True, exist_ok=False)
    helper = Path(__file__).with_name('r248_chemical_binary64.py')
    spec = importlib.util.spec_from_file_location('chemical', helper)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    rng = random.Random(24864); checks = 0; fixtures = 0
    def independent(bits):
        return Fraction.from_float(struct.unpack('<d', struct.pack('<Q', bits))[0])
    def run(a, b, coords):
        nonlocal checks, fixtures
        raw = [struct.pack('<' + 'Q' * len(a), *x) for x in (a, b)]
        raw.append(struct.pack('<' + 'I' * len(coords), *coords))
        out = m.reduce(*raw); fixtures += 1
        leaves = [independent(x) + independent(y) for x, y in zip(a, b)]
        for order, row in enumerate(out['rows']):
            values = [v * Fraction(c, 2) ** order for v, c in zip(leaves, coords)]
            p = sum((max(v, 0) for v in values), Fraction(0))
            n = sum((max(-v, 0) for v in values), Fraction(0))
            scale = 1 << (1074 + order)
            assert row['positive'] == p * scale and row['negative'] == n * scale
            assert (row['count'], row['positive_count'], row['negative_count'], row['zero_count']) == (len(values), sum(v > 0 for v in values), sum(v < 0 for v in values), sum(v == 0 for v in values))
            checks += 6
        support = [(v, Fraction(c, 2)) for v, c in zip(leaves, coords) if v > 0]
        if not support: assert out['positive_geometry'] is None
        else:
            total = sum(v for v, c in support)
            center = sum(v * c for v, c in support) / total
            variance = sum(v * (c - center) ** 2 for v, c in support) / total
            assert out['positive_geometry'] == {'centroid': center, 'variance': variance}
        checks += 1
        return raw, out
    for exponent in range(2047):
        for sign in (0, 1):
            a = (sign << 63) | (exponent << 52) | ((1 << 52) - 1)
            run([a], [1], [0xffffffff])
    run([], [], [])
    run([0, 1 << 63, 0x3ff0000000000000, 0x7fefffffffffffff], [0, 0, 0xbfefffffffffffff, 1], [1, 3, 159, 0xffffffff])
    # Difference below binary32 precision survives exact binary64 composition.
    raw, out = run([0x3ff0000000000001], [0xbff0000000000000], [3])
    assert out['rows'][0]['positive'] == 1 << (1074 - 52)
    for i in range(100):
        a = []; b = []
        for j in range(17):
            a.append((rng.randrange(2) << 63) | (rng.randrange(2047) << 52) | rng.getrandbits(52))
            b.append((rng.randrange(2) << 63) | (rng.randrange(2047) << 52) | rng.getrandbits(52))
        raw, out = run(a, b, [rng.getrandbits(31) * 2 + 1 for j in range(17)])
        before = m.reduce(*raw)
        mutable = [bytearray(x) for x in raw]; captured = m.capture(*mutable)
        for x in mutable: x[:] = bytes(len(x))
        assert m.reduce(*captured) == before
        checks += 1
    red = []
    def refuse(name, fn):
        try: fn()
        except (ValueError, TypeError): red.append(name)
        else: raise AssertionError(name)
    for name, bits in [('nan', 0x7ff8000000000000), ('inf', 0x7ff0000000000000), ('negative_inf', 0xfff0000000000000), ('bool_bits', True), ('negative_bits', -1), ('wide_bits', 1 << 64)]:
        refuse(name, lambda bits=bits: m.weight(bits))
    for name, args in [('unaligned', (b'x', b'x', b'')), ('unpaired', (bytes(8), b'', bytes(4))), ('coordinate_count', (bytes(8), bytes(8), b'')), ('memoryview', (memoryview(bytes(8)), bytes(8), bytes(4)))]:
        refuse(name, lambda args=args: m.reduce(*args))
    for c in (0, 2): refuse('coordinate_' + str(c), lambda c=c: m.reduce(bytes(8), bytes(8), struct.pack('<I', c)))
    r = m.record(positive=1, count=1, positive_count=1)
    refuse('count_overflow', lambda: m.add(dict(r, count=0xffffffff, positive_count=0xffffffff, positive=0xffffffff), r))
    refuse('wrong_order', lambda: m.add(r, dict(r, order=1)))
    refuse('impossible_magnitude', lambda: m.record(positive=0, count=1, positive_count=1))
    refuse('noncanonical_keys', lambda: m.validate(dict(r, extra=0)))
    assert m.contract()['worst_aggregate_bits'] == 2195
    assert 2195 <= m.LIMBS * 16
    # Budget refusal is exercised without allocating a multi-GiB fixture.
    old = m.CAPTURE_LIMIT; m.CAPTURE_LIMIT = 19
    refuse('combined_cap', lambda: m.capture(bytes(8), bytes(8), bytes(4)))
    m.CAPTURE_LIMIT = old
    report = dict(fixtures=fixtures, independent_checks=checks, reds=red, contract=m.contract(),
                  native_fields=0, new_simulation=False, native_authentication=False,
                  rounded_finalization=False, measured_memory=False)
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__': main()
