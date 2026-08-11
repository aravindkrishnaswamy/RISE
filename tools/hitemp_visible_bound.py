#!/usr/bin/env python3
"""Compute the 380-780 nm absorption upper bound for the CO2/H2O gas-opacity
record (FIRE_SMOKE_DESIGN.md §12 item 5).

The bound MUST be computed from the UN-PRUNED histogram: pruning only removes
absorption, so a bound taken from pruned data would understate it.  Reports,
per species and over the certified temperature domain, the fraction of the
Planck-mean absorption coefficient contributed by lines inside the renderer's
380-780 nm band, plus the absolute visible-band kappa_P itself.

Usage:
  hitemp_visible_bound.py <hist> <tips_dir> [--tlo 300] [--thi 2500] [--tstep 100]
"""
import sys, os, math

C2 = 1.4387769
TREF = 296.0
N_ATM = 7.3389965e21          # molec/cm^3 at 1 atm, times 1/T[K]
VIS_LO_WN = 1.0e7 / 780.0     # 12820.5 cm^-1
VIS_HI_WN = 1.0e7 / 380.0     # 26315.8 cm^-1


def load_tips(tips_dir):
    q = {}
    for iso in range(1, 14):
        p = os.path.join(tips_dir, f"q_iso{iso}.txt")
        if not os.path.exists(p):
            continue
        ts, qs = [], []
        with open(p) as f:
            for line in f:
                a, b = line.split()
                ts.append(float(a)); qs.append(float(b))
        q[iso] = (ts, qs)
    return q


def qval(tips, iso, T):
    ts, qs = tips[iso]
    if T <= ts[0]:
        return qs[0]
    if T >= ts[-1]:
        return qs[-1]
    lo, hi = 0, len(ts) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if ts[m] <= T: lo = m
        else: hi = m
    f = (T - ts[lo]) / (ts[hi] - ts[lo])
    return qs[lo] + f * (qs[hi] - qs[lo])


def load_cells(path):
    cells = []
    meta = {}
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                parts = line[1:].split()
                if len(parts) >= 2:
                    meta[parts[0]] = ' '.join(parts[1:])
                continue
            p = line.split()
            if len(p) != 8:
                continue
            cells.append((int(p[0]), float(p[3]), float(p[4]), float(p[5]), float(p[6])))
    return cells, meta


def planck_w(nu, Tr):
    x = C2 * nu / Tr
    if x > 700.0:
        return 0.0
    return nu * nu * nu / math.expm1(x)


def planck_norm(Tr):
    t = Tr / C2
    return t ** 4 * (math.pi ** 4 / 15.0)


def main():
    hist, tips_dir = sys.argv[1], sys.argv[2]
    tlo, thi, tstep = 300.0, 2500.0, 100.0
    args = sys.argv[3:]
    for i, a in enumerate(args):
        if a == '--tlo': tlo = float(args[i + 1])
        elif a == '--thi': thi = float(args[i + 1])
        elif a == '--tstep': tstep = float(args[i + 1])

    tips = load_tips(tips_dir)
    cells, meta = load_cells(hist)
    print(f"# cells {len(cells)}  (un-pruned: {meta.get('cells','?')})")
    print(f"# visible band {VIS_LO_WN:.1f}-{VIS_HI_WN:.1f} cm^-1 (380-780 nm)")
    print(f"# source records_read {meta.get('records_read','?')}")
    print()
    print(f"{'T_gas':>7} {'T_rad':>7} {'kP_total':>13} {'kP_visible':>13} {'vis_fraction':>13}")

    worst_frac = 0.0
    worst_at = None
    max_vis_abs = 0.0
    T = tlo
    temps = []
    while T <= thi + 1e-9:
        temps.append(T); T += tstep

    for Tg in temps:
        st = []
        for iso, sumA, meanE, meanE2, meanNu in cells:
            if iso not in tips:
                st.append(0.0); continue
            varE = max(0.0, meanE2 - meanE * meanE)
            x = C2 / Tg
            corr = 1.0 + 0.5 * x * x * varE
            s = (sumA * (qval(tips, iso, TREF) / qval(tips, iso, Tg))
                 * math.exp(-x * meanE) * (1.0 - math.exp(-x * meanNu)) * corr)
            st.append(s)
        for Tr in temps:
            norm = planck_norm(Tr)
            tot = 0.0; vis = 0.0
            for (iso, sumA, meanE, meanE2, meanNu), s in zip(cells, st):
                w = s * planck_w(meanNu, Tr)
                tot += w
                if VIS_LO_WN <= meanNu <= VIS_HI_WN:
                    vis += w
            tot /= norm; vis /= norm
            n1 = N_ATM / Tg
            frac = (vis / tot) if tot > 0 else 0.0
            if frac > worst_frac:
                worst_frac = frac; worst_at = (Tg, Tr)
            max_vis_abs = max(max_vis_abs, vis * n1 * 100.0)
            if Tr in (temps[0], temps[len(temps) // 2], temps[-1]):
                print(f"{Tg:7.0f} {Tr:7.0f} {tot * n1 * 100.0:13.5e} "
                      f"{vis * n1 * 100.0:13.5e} {frac:13.5e}")

    print()
    print(f"# WORST visible fraction of kappa_P over domain: {worst_frac:.6e} at "
          f"T_gas={worst_at[0]:.0f} K, T_rad={worst_at[1]:.0f} K")
    print(f"# MAX absolute visible-band kappa_P: {max_vis_abs:.6e} 1/(m*atm)")


if __name__ == '__main__':
    main()
