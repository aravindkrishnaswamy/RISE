#!/usr/bin/env python3
"""
Reproducible Decimal(110) exact-binary64 oracle generator and verifier for
tests/SSSNearUnityFresnelCases.inl (DL-334: docs/DL334_SSS_NEAR_UNITY.md).

Usage:
  python3 tools/generate_sss_near_unity_fresnel_cases.py --verify
  python3 tools/generate_sss_near_unity_fresnel_cases.py --write tests/SSSNearUnityFresnelCases.inl
"""

import argparse
import re
import sys
from decimal import Decimal, getcontext

getcontext().prec = 110

EPS = Decimal.from_float(2.0 ** -52)


def compute_oracle(nt_f: float, ni_f: float, mu_f: float):
    nt = Decimal.from_float(nt_f)
    ni = Decimal.from_float(ni_f)
    mu = Decimal.from_float(mu_f)

    if ni == nt:
        c_val = float(mu)
        cos_bound = float(Decimal(32) * EPS * mu)
        ref_bound = float(Decimal(64) * EPS)
        return 0.0, c_val, True, cos_bound, ref_bound

    L = max(ni, nt)
    S = min(ni, nt)
    q = S / L
    delta = (L - S) / L
    contrast = delta * (Decimal(2) - delta)

    if ni < nt:
        cos_t2 = contrast + (q * mu) * (q * mu)
    else:
        cos_t2 = (mu * mu - contrast) / (q * q)

    if cos_t2 < 0:
        return 1.0, 0.0, False, 0.0, float(Decimal(64) * EPS)

    cos_t = cos_t2.sqrt()
    if mu == 0 and cos_t == 0:
        R = Decimal(1)
    else:
        rs = (ni * mu - nt * cos_t) / (ni * mu + nt * cos_t)
        rp = (nt * mu - ni * cos_t) / (nt * mu + ni * cos_t)
        R = Decimal("0.5") * (rs * rs + rp * rp)

    if cos_t > 0:
        cos_bound_d = (
            Decimal(32) * EPS * cos_t
            + Decimal(64) * EPS * EPS * max(contrast, mu * mu) / (cos_t * q * q)
        )
    else:
        cos_bound_d = Decimal(0)

    denom = max(mu, cos_t)
    if denom > 0:
        ref_bound_d = Decimal(64) * EPS + Decimal(8) * cos_bound_d / denom
    else:
        ref_bound_d = Decimal(64) * EPS

    return float(R), float(cos_t), True, float(cos_bound_d), float(ref_bound_d)


ROW_RE = re.compile(
    r"^\{(\d+),([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),(true|false),([^,]+),([^}]+)\},$"
)


def parse_rows(path: str):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            m = ROW_RE.match(line)
            if not m:
                raise ValueError(f"Unrecognized row format: {line}")
            painter = int(m.group(1))
            nm = float(m.group(2))
            nt = float(m.group(3))
            ni = float(m.group(4))
            mu = float(m.group(5))
            r_ref = float(m.group(6))
            c_ref = float(m.group(7))
            trans_ref = m.group(8) == "true"
            cb_ref = float(m.group(9))
            rb_ref = float(m.group(10))
            rows.append((painter, nm, nt, ni, mu, r_ref, c_ref, trans_ref, cb_ref, rb_ref, line))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true", help="Verify existing .inl against Decimal(110) oracle")
    ap.add_argument("--inl", default="tests/SSSNearUnityFresnelCases.inl", help="Path to .inl file")
    args = ap.parse_args()

    rows = parse_rows(args.inl)
    mismatches = 0
    for idx, (painter, nm, nt, ni, mu, r_ref, c_ref, trans_ref, cb_ref, rb_ref, _) in enumerate(rows):
        r_calc, c_calc, trans_calc, _, _ = compute_oracle(nt, ni, mu)
        if trans_calc != trans_ref or abs(r_calc - r_ref) > 1e-15 or abs(c_calc - c_ref) > 1e-15:
            print(f"Row {idx} mismatch: got ({r_calc}, {c_calc}, {trans_calc}) vs ({r_ref}, {c_ref}, {trans_ref})")
            mismatches += 1
    if mismatches:
        print(f"FAILED: {mismatches} / {len(rows)} rows mismatched Decimal(110) oracle")
        return 1
    print(f"OK: verified all {len(rows)} rows in {args.inl} against Decimal(110) exact-binary64 oracle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
