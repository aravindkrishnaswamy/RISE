#!/usr/bin/env python3
"""Independent Fraction-to-binary64 oracle for new chemical finalization."""
from fractions import Fraction
import importlib.util
import json
from pathlib import Path
import random
import struct
import sys


def main():
    output = Path(sys.argv[1]); output.mkdir(parents=True, exist_ok=False)
    p = Path(__file__).with_name('r248_chemical_finalization.py')
    spec = importlib.util.spec_from_file_location('chemical_round', p)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    rng = random.Random(2484608); checks = 0; reds = []
    def oracle(q):
        nonlocal checks
        got = m.round_rational(q.numerator, q.denominator)
        try: expected = struct.unpack('<Q', struct.pack('<d', float(q)))[0]
        except OverflowError: assert not got['available'] and got['status'] == 'overflow'
        else: assert got['available'] and got['binary64_bits'] == expected
        checks += 1
    for e in range(-1074, 1024):
        x = Fraction(2 ** e) if e >= 0 else Fraction(1, 2 ** -e)
        for q in (x, -x, x * Fraction(3, 2), -x * Fraction(3, 2), x * Fraction(1, 2), -x * Fraction(1, 2)):
            oracle(q)
    # Rounded extrema, signed halfway underflow and carry across binades.
    for q in (Fraction(1, 1 << 1075), Fraction(-1, 1 << 1075),
              Fraction((1 << 54) - 1) * (1 << 970), Fraction((1 << 53) - 1) * (1 << 971)):
        oracle(q)
    for i in range(3000):
        n = rng.getrandbits(rng.randrange(1, 4609)); d = rng.getrandbits(rng.randrange(1, 4609)) or 1
        oracle(Fraction(n if i & 1 else -n, d))
    def finite(bits): return Fraction.from_float(struct.unpack('<d', struct.pack('<Q', bits))[0])
    for i in range(100):
        aa = [(rng.randrange(2) << 63) | (rng.randrange(2047) << 52) | rng.getrandbits(52) for j in range(13)]
        bb = [(rng.randrange(2) << 63) | (rng.randrange(2047) << 52) | rng.getrandbits(52) for j in range(13)]
        cc = [2 * rng.getrandbits(31) + 1 for j in range(13)]
        measure = (rng.randrange(2047) << 52) | rng.getrandbits(52) or 1
        args = (struct.pack('<13Q', *aa), struct.pack('<13Q', *bb), struct.pack('<13I', *cc))
        got = m.finalize(*args, multiplier_binary64_bits=measure)
        leaves = [finite(a) + finite(b) for a, b in zip(aa, bb)]
        pos = sum((max(q, 0) for q in leaves), Fraction(0)); neg = sum((max(-q, 0) for q in leaves), Fraction(0))
        values = {'signed': pos - neg, 'absolute': pos + neg, 'positive': pos, 'negative_magnitude': neg}
        for k, q in values.items():
            expected = m.round_rational((q * finite(measure)).numerator, (q * finite(measure)).denominator)
            oracle(q * finite(measure)); assert got['values'][k] == expected
        if pos:
            support = [(q, Fraction(c, 2)) for q, c in zip(leaves, cc) if q > 0]
            center = sum(q * c for q, c in support) / pos
            variance = sum(q * (c - center) ** 2 for q, c in support) / pos
            for k, q in [('centroid', center), ('variance', variance)]:
                oracle(q); assert got['geometry_cell_units'][k] == m.round_rational(q.numerator, q.denominator)
        else: assert not got['geometry_cell_units']['centroid']['available']
    empty = m.finalize(b'', b'', b'')
    assert all(v['available'] and v['binary64_bits'] == 0 for v in empty['values'].values())
    assert all(not v['available'] for v in empty['geometry_cell_units'].values())
    tiny = m.round_rational(-1, 1 << 1075)
    assert tiny['rounded_nonzero_to_zero'] and tiny['underflow'] and tiny['inexact'] and tiny['binary64_bits'] == 1 << 63
    def refuse(name, fn):
        try: fn()
        except ValueError: reds.append(name)
        else: raise AssertionError(name)
    for name, n, d in [('bool', True, 1), ('zero_denominator', 1, 0), ('negative_denominator', 1, -1),
                       ('numerator_bound', 1 << 4608, 1), ('negative_numerator_bound', -(1 << 4608), 1), ('denominator_bound', 1, 1 << 4608)]:
        refuse(name, lambda n=n, d=d: m.round_rational(n, d))
    for bits in (0, 1 << 63, 0xbff0000000000000, 0x7ff0000000000000, 0x7ff8000000000000, True, -1, 1 << 64):
        refuse('measure_' + str(bits), lambda bits=bits: m.positive_measure(bits))
    refuse('workspace', lambda: m.quotient_round(1, 1, 6144))
    for name, args in [('huge_shift', (1, 1, 1 << 64)), ('negative_huge_shift', (1, 1, -(1 << 64))), ('bool_shift', (1, 1, True)), ('zero_divisor', (1, 0, 0)), ('negative_dividend', (-1, 1, 0)), ('oversized_dividend', (1 << 6144, 1, 0)), ('negative_workspace', (1, 1 << 6143, -1))]:
        refuse(name, lambda args=args: m.quotient_round(*args))
    refuse('source_nan', lambda: m.finalize(struct.pack('<Q', 0x7ff8000000000000), bytes(8), struct.pack('<I', 1)))
    refuse('even_coordinate', lambda: m.finalize(bytes(8), bytes(8), struct.pack('<I', 2)))
    report = {'independent_rounding_checks': checks, 'paired_field_fixtures': 100, 'reds': reds,
              'contract': m.contract(), 'native_source_authentication': False, 'measured_memory': False,
              'new_simulation': False}
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n'); print(json.dumps(report))


if __name__ == '__main__': main()
