#!/usr/bin/env python3
"""Compile the private production FMA and check exact nearest-53/ties-even.

Run: python3 tests/PolynomialWideArithmeticTest.py --output-dir /absolute/evidence
No library API/test hook is added. The generated TU includes Polynomial.cpp and
uses the make release arithmetic flags; all generated files stay outside source.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import subprocess


def nearest53(value):
    """Exact rational reference, including midpoint parity; no float decision."""
    if not value:
        return Fraction(0)
    sign = -1 if value < 0 else 1
    value = abs(value)
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if value < Fraction(2) ** exponent:
        exponent -= 1
    unit = Fraction(2) ** (exponent - 52)
    scaled = value / unit
    integer, remainder = divmod(scaled.numerator, scaled.denominator)
    twice = 2 * remainder
    if twice > scaled.denominator or (twice == scaled.denominator and integer % 2):
        integer += 1
    return sign * integer * unit


PROBE = r'''
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <random>
#include "@SOURCE@"
static void emit(const char* family, oqs_wide a, oqs_wide b, oqs_wide c) {
    const oqs_wide r=oqs_fma(a,b,c);
    std::printf("%s %a %lld %a %lld %a %lld %a %lld\n",family,
        a.m,(long long)a.e,b.m,(long long)b.e,c.m,(long long)c.e,r.m,(long long)r.e);
}
int main() {
    // Odd i*j makes the product exactly halfway; i*j mod 4 varies tie parity.
    // The same exact arithmetic is shifted far beyond native exponent range.
    for(int shift : {-2000,0,2000}) for(int i : {1,3,5,7}) for(int j : {1,3})
    for(int productSign : {-1,1}) for(int addendSign : {-1,0,1})
    for(int gap : {-2000,-1100,-1076,-1074,-1073,-1024,-112,-110,-108,-107,-106,-54,1100}) {
        const oqs_wide a=oqs_wide::scaled(productSign*(1+i*0x1p-27),shift);
        const oqs_wide b(1+j*0x1p-26);
        const oqs_wide c=oqs_wide::scaled(addendSign,gap+shift);
        emit("midpoint",a,b,c);
        // Adjacent significands create non-midpoint controls with the same c.
        emit("neighbor",oqs_wide::scaled(std::nextafter(a.m,0.0),a.e),b,c);
    }
    for(int shift : {-2000,0,2000}) for(int sign : {-1,1}) {
        emit("cancel",oqs_wide::scaled(sign*(1+0x1p-27),shift),
            oqs_wide(1-0x1p-27),oqs_wide::scaled(-sign,shift));
        emit("zero_product",oqs_wide(0),oqs_wide::scaled(1,shift),oqs_wide::scaled(sign,shift));
        emit("power_boundary",oqs_wide::scaled(sign,shift),oqs_wide(1),oqs_wide::scaled(-sign,shift-1100));
    }
    // Full-width mantissas, cancellation, and exponents spanning both branches.
    std::mt19937_64 rng(274275);
    for(int i=0;i<2000;++i) {
        auto sample=[&]() { const double m=std::scalbn(static_cast<double>((rng()>>11)|(UINT64_C(1)<<52)),-53);
            return oqs_wide::scaled((rng()&1)?m:-m,static_cast<int>(rng()%10001)-5000); };
        const oqs_wide a=sample(),b=sample();
        const oqs_wide c=(i%3==0) ? -(a*b) : sample();
        emit("random",a,b,c);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[1] / 'src/Library/Functions/Polynomial.cpp'
    probe = out / 'probe.cpp'
    probe.write_text(PROBE.replace('@SOURCE@', str(source)))
    command = [args.compiler, '-O3', '-flto', '-ffast-math', '-fno-finite-math-only',
               '-funroll-loops', '-std=gnu++17', '-Wall', '-pedantic',
               '-Wno-c++11-long-long', str(probe), '-o', str(out/'probe')]
    build = subprocess.run(command, capture_output=True, text=True)
    (out/'build.log').write_text(build.stdout + build.stderr)
    report = dict(command=command, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  build_rc=build.returncode, warnings=(build.stdout+build.stderr).count('warning:'))
    if build.returncode:
        (out/'result.json').write_text(json.dumps(report, indent=2))
        return build.returncode
    run = subprocess.run([str(out/'probe')], capture_output=True, text=True)
    (out/'raw.log').write_text(run.stdout)
    (out/'run.stderr').write_text(run.stderr)
    failures, counts = [], {}
    for index, line in enumerate(run.stdout.splitlines()):
        family, *fields = line.split()
        values = [Fraction(float.fromhex(fields[i])) * Fraction(2)**int(fields[i+1]) for i in range(0,8,2)]
        a,b,c,actual = values
        exact = a*b+c
        expected = nearest53(exact)
        counts[family] = counts.get(family, 0) + 1
        if actual != expected:
            failures.append(dict(index=index, raw=line, actual=str(actual),
                                 expected=str(expected), exact_error=str(abs(actual-exact))))
    report.update(run_rc=run.returncode, counts=counts, checks=sum(counts.values()), failures=failures)
    (out/'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k != 'failures'}, indent=2))
    print(f"Exact nearest-53 checks: {sum(counts.values())}, failures: {len(failures)}")
    return int(bool(run.returncode or failures or not counts or report['warnings']))


if __name__ == '__main__':
    raise SystemExit(main())
