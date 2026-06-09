#!/usr/bin/env python3
"""Flexible ALTERNATE dial-geometry generator — an experimentation platform for
the guilloché watch.  The stock dial (dial_mesh_gen.py -> dial.raw2) has a
UNIFORM woven-cell size everywhere; this script lets the cell size (and other
relief knobs) VARY by region so features stand out in the relief.

First experiment: make the lightning-zigzag carry a DIFFERENT guilloché cell
width than the surrounding field (a finer or coarser weave on the bolts), the
way an engraver cuts the lightning at a different pitch so it reads as its own
band.

It reuses dial_mesh_gen's primitives (lens/stripe/_tri/smoothstep), its
mesh/UV/triangulation (build_mesh, called with our height function), and its
RAW2 writer + preview.  The UV is identical to the stock dial, so every oxide
map (oxide_cart.png, the palettes, the per-metal shapes) applies unchanged — a
variant only changes the RELIEF.

  ## Add an experiment
  Write a `field_<name>(X, Y, R, p)` returning a [0,1] height field and register
  it in FIELDS.  Knobs come in through `p` (add an argparse entry + a default).
  `--field <name>` selects it; everything else (mesh, UV, oxide) is unchanged.

Usage:
  # lightning at a finer weave than the field (default experiment):
  python3 scenes/FeatureBased/GuillocheWatch/dial_variants_gen.py \
      --field lightning_cell --lightning-cell-scale 0.6 --out dial_lightning.raw2
  # fast iteration (coarse mesh), then a clean preview:
  python3 ... --mesh-n 280 --preview
  # the stock uniform field, for an A/B baseline:
  python3 ... --field uniform --out dial_uniform.raw2
Outputs: <out>.raw2 (+ <out>_preview.png with --preview).
"""
import argparse
import math
import os
import sys

import numpy as np

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _THIS_DIR)
import dial_mesh_gen as dm          # noqa: E402  (primitives + build_mesh + writer)


# --------------------------------------------------------------------------
# Shared front matter for every field: polar warp + petal (lightning) mask +
# per-sector rotated (radial, tangential) frame.  Returns everything a field
# needs so experiments don't re-derive it.
# --------------------------------------------------------------------------
def _frame(X, Y, R, p):
    r = np.hypot(X, Y)
    rho = np.clip(r / R, 0.0, 1.0)
    theta = np.arctan2(Y, X)
    jag = p["seam_jag"] * dm._tri(p["seam_jag_freq"] * rho)
    psi = theta + p["swirl"] * rho + jag
    N = p["num_arms"]
    petal = dm.lens(np.cos(N * psi), p["petal_e0"], p["petal_e1"])   # lightning mask [0,1]
    two_pi_over_N = 2.0 * math.pi / N
    sector = np.round(psi / two_pi_over_N)
    theta_c = sector * two_pi_over_N - p["swirl"] * rho - jag
    cc, ss = np.cos(theta_c), np.sin(theta_c)
    xr = cc * X + ss * Y           # along sector radial
    yr = -ss * X + cc * Y          # tangential
    return r, rho, petal, xr, yr


def _woven(xr, yr, freq, p):
    """One woven-pillow grid at a given cell frequency (freq = 0.5/cell)."""
    ax = freq * xr
    ay = freq * yr
    rowpar = np.floor(2.0 * ax)
    ay = ay + 0.25 * np.mod(rowpar, 2.0)                  # half-cell brick offset
    return dm.stripe(ax, p["grid_e0"], p["grid_e1"]) * dm.stripe(ay, p["grid_e0"], p["grid_e1"])


def _finish(h, r, R, p):
    """Normalise -> land-gamma -> relief squeeze -> flush hub (same as stock)."""
    h = h - h.min()
    rng = h.max() - h.min()
    h = np.zeros_like(h) if rng < 1e-12 else h / rng
    ll = min(max(p["land_level"], 1e-3), 1.0 - 1e-3)
    h = np.power(h, math.log(ll) / math.log(0.5))
    h = 0.5 + (h - 0.5) * p["relief_depth"]
    hub = float(h.min())
    rin = p["center_radius"] * R
    w = np.clip(r / max(rin, 1e-6), 0.0, 1.0) ** 2.0
    h = (1.0 - w) * hub + w * h
    return np.clip(h, 0.0, 1.0)


# --------------------------------------------------------------------------
# FIELDS — each returns h(X,Y) in [0,1].  Register new experiments here.
# --------------------------------------------------------------------------
def field_uniform(X, Y, R, p):
    """The stock dial: one cell size everywhere (A/B baseline)."""
    r, rho, petal, xr, yr = _frame(X, Y, R, p)
    grid = _woven(xr, yr, 0.5 / p["cell"], p)
    return _finish(p["base"] + p["petal_amp"] * petal + p["grid_amp"] * grid, r, R, p)


def field_lightning_cell(X, Y, R, p):
    """Lightning zigzag at a DIFFERENT cell width than the field.

    `lightning_cell_scale` < 1 => finer weave on the bolts, > 1 => coarser.
    The bolt region is the petal mask, soft-bounded by [lightning_lo, lightning_hi]
    (wider band = lower lo).  `cell_mode`:
      * freqblend : one grid whose local cell frequency lerps field<->bolt by the
                    mask (smooth, organic; some bend far from centre).
      * select    : two full grids blended by the mask (crisper two-pitch look;
                    a thin transition band where the mask crosses ~0.5).
    `lightning_relief` raises (+) / recesses (-) the bolt region so it also reads
    as a relief step (0 = pure cell-size contrast)."""
    r, rho, petal, xr, yr = _frame(X, Y, R, p)
    mask = dm.smoothstep(p["lightning_lo"], p["lightning_hi"], petal)
    f_field = 0.5 / p["cell"]
    f_bolt = 0.5 / (p["cell"] * p["lightning_cell_scale"])
    if p["cell_mode"] == "freqblend":
        freq = f_field * (1.0 - mask) + f_bolt * mask
        grid = _woven(xr, yr, freq, p)
    elif p["cell_mode"] == "select":
        grid = _woven(xr, yr, f_field, p) * (1.0 - mask) + _woven(xr, yr, f_bolt, p) * mask
    else:
        raise ValueError("unknown cell_mode %r" % p["cell_mode"])
    h = p["base"] + p["petal_amp"] * petal + p["grid_amp"] * grid + p["lightning_relief"] * mask
    return _finish(h, r, R, p)


FIELDS = {"uniform": field_uniform, "lightning_cell": field_lightning_cell}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--field", choices=sorted(FIELDS), default="lightning_cell")
    ap.add_argument("--out", default="dial_lightning.raw2", help="output RAW2 (in this folder unless absolute)")
    ap.add_argument("--preview", action="store_true", help="also write <out>_preview.png (height|hillshade)")
    # base dial knobs (mirror dial_mesh_gen defaults)
    ap.add_argument("--radius", type=float, default=20.6)
    ap.add_argument("--mesh-n", type=int, default=560, help="grid samples across the diameter (lower = faster iteration)")
    ap.add_argument("--disp", type=float, default=0.42)
    ap.add_argument("--num-arms", type=int, default=12)
    ap.add_argument("--swirl", type=float, default=0.0)
    ap.add_argument("--seam-jag", type=float, default=0.16)
    ap.add_argument("--seam-jag-freq", type=float, default=3.0)
    ap.add_argument("--cell", type=float, default=0.9, help="FIELD woven cell size (land-to-land, world)")
    ap.add_argument("--grid-amp", type=float, default=0.85)
    ap.add_argument("--petal-amp", type=float, default=0.30)
    ap.add_argument("--grid-e0", type=float, default=0.12)
    ap.add_argument("--grid-e1", type=float, default=0.5)
    ap.add_argument("--petal-e0", type=float, default=0.0)
    ap.add_argument("--petal-e1", type=float, default=0.82)
    ap.add_argument("--base", type=float, default=0.15)
    ap.add_argument("--land-level", type=float, default=0.45)
    ap.add_argument("--relief-depth", type=float, default=0.85)
    ap.add_argument("--center-radius", type=float, default=0.03)
    # lightning-cell experiment knobs
    ap.add_argument("--cell-mode", choices=["freqblend", "select"], default="freqblend")
    ap.add_argument("--lightning-cell-scale", type=float, default=0.6,
                    help="bolt cell size / field cell size; <1 finer on the bolt, >1 coarser. Default 0.6.")
    ap.add_argument("--lightning-lo", type=float, default=0.30, help="petal-mask lower shoulder (lower = wider bolt). Default 0.30.")
    ap.add_argument("--lightning-hi", type=float, default=0.72, help="petal-mask upper shoulder. Default 0.72.")
    ap.add_argument("--lightning-relief", type=float, default=0.0,
                    help="raise(+)/recess(-) the bolt region as a relief step (0 = pure cell-size contrast). Default 0.")
    args = ap.parse_args(argv)

    p = vars(args).copy()
    p["mesh_n"] = args.mesh_n            # argparse dest is mesh_n already; keep explicit
    out = args.out if os.path.isabs(args.out) else os.path.join(_THIS_DIR, args.out)

    print("variant '%s' — mesh_n=%d cell=%.2f mode=%s lightning_cell_scale=%.2f band=[%.2f,%.2f] relief=%.2f"
          % (args.field, args.mesh_n, args.cell, args.cell_mode, args.lightning_cell_scale,
             args.lightning_lo, args.lightning_hi, args.lightning_relief))
    verts, tris, h, z = dm.build_mesh(p, height_fn=FIELDS[args.field])
    dm.write_raw2(out, verts, tris)
    print("  mesh: %d verts, %d tris  z∈[%.4f,%.4f] (p2p %.4f)  -> %s"
          % (verts.shape[0], tris.shape[0], z.min(), z.max(), z.max() - z.min(), os.path.abspath(out)))
    if args.preview:
        pv = os.path.splitext(out)[0] + "_preview.png"
        # reuse stock preview (height | oxide | hillshade); pass a flat oxide so it's height|hillshade-ish
        dm.save_preview(pv, h, dm.build_oxide_cart(1024, args.radius), args.radius)
        print("  preview -> %s" % os.path.abspath(pv))
    return 0


if __name__ == "__main__":
    sys.exit(main())
