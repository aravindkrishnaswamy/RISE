#!/usr/bin/env python3
"""Independent exact-rational oracles for C++ compensated film controls.

One observation of 1 and N-1 observations of zero; append k observations of
an exact binary64 x. Formula uses exact Fraction sums and squared sums, not
Welford or the implementation's recurrence. Print correctly rounded binary64
hex constants using Python Fraction-to-float conversion.
"""
from fractions import Fraction

STEPS = (1, 2, 4, 16, 64, 1024)
COUNTS = (1_000_000, 1_000_000_000, (1 << 53) - 1024)


def hex_fraction(value):
    return float(value).hex()


def initial_state(n):
    mean = Fraction(1, n)
    m2 = 1 - mean
    return tuple(hex_fraction(v) for v in (
        mean, mean - Fraction.from_float(float(mean)),
        m2, m2 - Fraction.from_float(float(m2)),
    ))


def moments(n, k, x):
    x = Fraction.from_float(x)
    total = n + k
    first = 1 + k * x
    second = 1 + k * x * x
    mean = first / total
    m2 = second - first * first / total
    variance_mean = m2 / (total * (total - 1))
    return tuple(hex_fraction(v) for v in (mean, m2, variance_mean))


if __name__ == "__main__":
    for n in COUNTS:
        print("initial", n, *initial_state(n))
        for x in (0.0, 2e-9):
            for k in STEPS:
                print("append", x.hex(), k, *moments(n, k, x))
    for x in (0.0, 1e-20):
        print("actual", x.hex(), 100001, *moments(1, 100000, x))

    # Mixed-scale tests use a normal total M2 with a tiny auxiliary increment.
    for tiny in (1e-200, float.fromhex("0x1p-600"), float.fromhex("0x1p-1074")):
        t = Fraction.from_float(tiny)
        for n in (3, 32):
            mean = Fraction(1, 2) + t / n
            m2 = Fraction(1, 2) - t + (1 - Fraction(1, n)) * t * t
            sample_variance = m2 / (n - 1)
            mean_variance = sample_variance / n
            print("mixed", tiny.hex(), n, *(hex_fraction(v) for v in
                (mean, m2, sample_variance, mean_variance)))
