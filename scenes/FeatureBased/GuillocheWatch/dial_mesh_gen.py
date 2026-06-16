#!/usr/bin/env python3
"""Bake a CARTESIAN-grid circular guilloché watch dial as a RAW2 triangle mesh
(positions + analytic normals + linear UV) for RISE — the Cartesian rebuild
that kills the polar singularity of the old `circulardisk` + `displaced_geometry`
dial.

WHY (read docs/THIN_FILM_INTERFERENCE.md Phase 3 + the session notes):
A `circulardisk` carries POLAR texture coords (u=angle, v=radius).  A guilloché
micro-cell spans a fixed slice of ANGLE, so its world width = radius·Δangle and
shrinks to ZERO at the centre (all angular columns collapse onto the pole).
Tessellated + displaced, the centre cells fall below triangle/pixel size and the
relief washes to a smooth blob (~inner 40% of the dial).  No resolution fixes it.

THE FIX — define the relief in CARTESIAN space and bake it straight into a mesh:
  * MACRO  : N near-radial "lightning" petals with jagged seams (low-frequency,
             swirl≈0 so it reads radial, NOT a spiral) — the dial's identity.
  * MICRO  : a WOVEN GRID whose cells are a fixed WORLD size everywhere (rim and
             dead-centre alike).  The grid is laid in a per-sector-rotated local
             frame so the cells run roughly radially and FLIP tilt at each petal
             seam (the woven "lightning" interlock) — but because the frame is a
             rotation (isometry) the cell size never collapses → blocks read all
             the way to the centre.
  * Z is baked per vertex (real engraving); per-vertex NORMALS come from the
    analytic height gradient (sharper than displaced-geometry finite differences);
    UV is linear ( (x,y) → [0,1]² ) so the radial oxide-thickness map samples
    uniformly.

OUTPUTS (into --out-dir, default scenes/FeatureBased/GuillocheWatch/):
  * dial.raw2            — the dial mesh (RAW2: `v x y z nx ny nz u v` + `t a b c`,
                           consumed by `rawmesh2_geometry { file ... }`).
  * oxide_cart.png       — 8-bit RGB radial oxide-thickness map in CARTESIAN
                           layout (centre gold → rim blue), normalized to the
                           scene's `scale`/`bias` (default 16 / 22 → 22..38 nm),
                           reusing thermal_oxide_sim's Arrhenius radial profile.
  * dial_preview.png     — height | oxide | hill-shade montage for eyeballing.

Only numpy + Pillow.  thermal_oxide_sim is imported for the radial nm profile.
"""

import argparse
import math
import os
import sys

import numpy as np

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("ERROR: Pillow (PIL) required: pip install Pillow\n")
    raise

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", "..", ".."))  # repo root
sys.path.insert(0, _THIS_DIR)
import thermal_oxide_sim as tox  # noqa: E402  (radial nm profile reuse)


# --------------------------------------------------------------------------
# Sharp engine-turned profile primitives (ported from guilloche_gen.py).
# --------------------------------------------------------------------------
def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lens(c, e0, e1):
    """Raised plateau of carrier c∈[-1,1]: 1 where |c| large, 0 past [e0,e1]."""
    return smoothstep(e0, e1, np.abs(c))


def stripe(arg, e0, e1):
    """Periodic groove/land stripe; |cos(2π·arg)| plateau, period 0.5 in arg
    (land-to-land spacing 0.5).  1 on the raised land, 0 in the groove."""
    return smoothstep(e0, e1, np.abs(np.cos(2.0 * math.pi * arg)))


def _tri(x):
    f = x - np.floor(x)
    return 2.0 * np.abs(2.0 * f - 1.0) - 1.0          # [-1,1] triangle wave


# --------------------------------------------------------------------------
# Cartesian height field h(x,y) ∈ [0,1].
# --------------------------------------------------------------------------
def height_field(X, Y, R, p):
    """Compose macro lightning petals + a per-sector-rotated Cartesian woven
    grid.  X,Y are world coords (arrays); R is the dial radius."""
    r = np.hypot(X, Y)
    rho = np.clip(r / R, 0.0, 1.0)
    theta = np.arctan2(Y, X)

    # ---- (1) warped angle ψ — small swirl + jagged radial "lightning" warp.
    jag = p["seam_jag"] * _tri(p["seam_jag_freq"] * rho)
    psi = theta + p["swirl"] * rho + jag
    N = p["num_arms"]
    P = N * psi

    # ---- (2) N near-radial petal envelope (broad organizing lobes).
    petal = lens(np.cos(P), p["petal_e0"], p["petal_e1"])

    # ---- (3) per-sector-rotated CARTESIAN woven grid (uniform world cells).
    # Choose the nearest of N sector directions IN THE WARPED angle so the
    # sector boundaries follow the same jagged petal seams; rotate (X,Y) into
    # that sector's local (radial,tangential) frame and lay an axis-aligned
    # woven grid there.  A rotation is an isometry → cells keep their world
    # size all the way to r→0 (no polar collapse).  The sector flip at each
    # seam mirrors the weave tilt = the woven "lightning" interlock.
    two_pi_over_N = 2.0 * math.pi / N
    sector = np.round(psi / two_pi_over_N)                 # nearest sector
    theta_c = sector * two_pi_over_N - p["swirl"] * rho - jag  # world dir of sector centre
    cc, ss = np.cos(theta_c), np.sin(theta_c)
    xr = cc * X + ss * Y                                   # along sector radial
    yr = -ss * X + cc * Y                                  # tangential

    freq = 0.5 / p["cell"]                                  # land-to-land = cell (world)
    ax = freq * xr
    ay = freq * yr
    rowpar = np.floor(2.0 * ax)                            # radial cell index
    ay = ay + 0.25 * np.mod(rowpar, 2.0)                   # half-cell brick offset
    rungs = stripe(ax, p["grid_e0"], p["grid_e1"])
    bars = stripe(ay, p["grid_e0"], p["grid_e1"])
    grid = rungs * bars                                    # woven pillows ∈[0,1]

    # ---- (4) compose, normalize, land-gamma, relief squeeze.
    h = p["base"] + p["petal_amp"] * petal + p["grid_amp"] * grid
    h = h - h.min()
    rng = h.max() - h.min()
    h = np.zeros_like(h) if rng < 1e-12 else h / rng
    ll = min(max(p["land_level"], 1e-3), 1.0 - 1e-3)
    h = np.power(h, math.log(ll) / math.log(0.5))
    h = 0.5 + (h - 0.5) * p["relief_depth"]

    # ---- (5) tiny flush hub: blend the inner center_radius·R down to the field
    # minimum (hands mount here; no raised boss).  Cartesian → only a small dot.
    hub_level = float(h.min())
    rin = p["center_radius"] * R
    w = np.clip(r / max(rin, 1e-6), 0.0, 1.0) ** 2.0       # 0 at centre → 1 at rin
    h = (1.0 - w) * hub_level + w * h

    return np.clip(h, 0.0, 1.0)


# --------------------------------------------------------------------------
# Cartesian radial oxide-thickness map (centre gold → rim blue).
# --------------------------------------------------------------------------
def build_oxide_cart(size, R, falloff="quadratic", Ea=None):
    """Normalized radial heat SHAPE t(ρ) ∈ [0,1] (0 = centre, 1 = rim): the
    Arrhenius/parabolic oxide-growth curvature under the chosen radial torch
    falloff, INDEPENDENT of absolute nanometres.  The dial's `oxide_thk`
    scalar_painter `scale`/`bias` in the SCENE set the torch start/end thickness
    (bias = centre nm, bias+scale = rim nm), so the whole heat-tint colour sweep
    is tunable per render WITHOUT re-baking this map.  The radial FALLOFF and the
    per-metal activation energy `Ea` (J/mol; see tox.METAL_KINETICS) shape the
    curvature -- higher Ea concentrates growth at the hot rim.  Pure radial →
    orientation/flip-invariant."""
    # d_center=0, d_rim=1 makes build_thickness_profile return the normalized
    # curve t directly (shaped = 0 + t·(1-0) = t).
    t_prof, _, _ = tox.build_thickness_profile(
        2048, falloff, 0.0, 1.0, Ea=(Ea if Ea is not None else tox._ACTIVATION_EA))
    rho_tab = np.linspace(0.0, 1.0, t_prof.size)
    ii = (np.arange(size) + 0.5) / size                   # pixel centres in [0,1]
    U, V = np.meshgrid(ii, ii)
    rr = np.hypot(U - 0.5, V - 0.5) * 2.0                  # 0 at centre, 1 at edge
    rho = np.clip(rr, 0.0, 1.0)
    return np.clip(np.interp(rho, rho_tab, t_prof), 0.0, 1.0)


# --------------------------------------------------------------------------
# Lightning-zigzag torch mask (the N-arm petal pattern, Cartesian UV).
# --------------------------------------------------------------------------
def lightning_mask(size, p):
    """Bake the dial's N-arm lightning-petal pattern as a [0,1] mask in the SAME
    Cartesian UV as build_oxide_cart / the dial mesh (u=(X+R)/2R, so pixel
    (U,V) -> (X/R, Y/R) = (2U-1, 2V-1)).  This is exactly the `petal` term of
    height_field, so the colour zigzag lines up with the relief zigzag.  Feed it
    to thermal_oxide_sim.apply_torch_pattern to torch the lightning more (or
    less) than the surrounding field."""
    ii = (np.arange(size) + 0.5) / size
    U, V = np.meshgrid(ii, ii)
    x = 2.0 * U - 1.0                                      # X/R
    y = 2.0 * V - 1.0                                      # Y/R
    rho = np.clip(np.hypot(x, y), 0.0, 1.0)
    theta = np.arctan2(y, x)
    jag = p["seam_jag"] * _tri(p["seam_jag_freq"] * rho)
    psi = theta + p["swirl"] * rho + jag
    petal = lens(np.cos(p["num_arms"] * psi), p["petal_e0"], p["petal_e1"])
    return np.clip(np.where(rho <= 1.0, petal, 0.0), 0.0, 1.0)


# --------------------------------------------------------------------------
# Mesh build + RAW2 writer.
# --------------------------------------------------------------------------
def build_mesh(p, height_fn=None):
    # height_fn(X, Y, R, p) -> [0,1] field; defaults to the stock height_field.
    # Variant generators (dial_variants_gen.py) pass their own to reuse this
    # mesh/UV/triangulation machinery unchanged.
    R = p["radius"]
    N = p["mesh_n"]
    xs = np.linspace(-R, R, N)
    ys = np.linspace(-R, R, N)
    X, Y = np.meshgrid(xs, ys)                             # X[j,i]=xs[i]

    h = (height_fn or height_field)(X, Y, R, p)
    z = (h - float(h.mean())) * p["disp"]

    # Analytic normals from the baked height gradient (np.gradient: axis0=y).
    dzdy, dzdx = np.gradient(z, ys, xs)
    nx, ny = -dzdx, -dzdy
    nz = np.ones_like(z)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx * inv, ny * inv, nz * inv

    u = (X + R) / (2.0 * R)
    v = (Y + R) / (2.0 * R)
    r = np.hypot(X, Y)
    keep = r <= R

    idx = np.full((N, N), -1, dtype=np.int64)
    idx[keep] = np.arange(int(keep.sum()))

    verts = np.column_stack([X[keep], Y[keep], z[keep],
                             nx[keep], ny[keep], nz[keep],
                             u[keep], v[keep]])

    c00 = idx[:-1, :-1]
    c01 = idx[:-1, 1:]
    c10 = idx[1:, :-1]
    c11 = idx[1:, 1:]
    cell = (c00 >= 0) & (c01 >= 0) & (c10 >= 0) & (c11 >= 0)
    # CCW from +Z (front face up); per-vertex normals carry the shading.
    triA = np.column_stack([c00[cell], c01[cell], c11[cell]])
    triB = np.column_stack([c00[cell], c11[cell], c10[cell]])
    tris = np.concatenate([triA, triB], axis=0)

    return verts, tris, h, z


def write_raw2(path, verts, tris):
    with open(path, "w") as f:
        f.write("%d %d\n" % (verts.shape[0], tris.shape[0]))
        np.savetxt(f, verts, fmt="v %.5f %.5f %.5f %.5f %.5f %.5f %.6f %.6f")
        np.savetxt(f, tris, fmt="t %d %d %d")


# --------------------------------------------------------------------------
# Preview + self-checks.
# --------------------------------------------------------------------------
def save_preview(path, h, oxide, R):
    """height | oxide | hill-shade montage (all masked to the dial circle)."""
    N = h.shape[0]
    yy, xx = np.mgrid[0:N, 0:N]
    cx = (N - 1) / 2.0
    rr = np.hypot(xx - cx, yy - cx) / (N / 2.0)
    mask = rr <= (R / R)  # h already spans the [-R,R] grid → unit disc in grid frac
    mask = np.hypot(xx - cx, yy - cx) <= (N / 2.0)

    def g3(a):
        a = np.clip(a, 0, 1)
        return np.stack([np.where(mask, a, 0.0)] * 3, -1)

    # hill-shade: lambert of analytic normal vs a raking light.
    dzy, dzx = np.gradient(h)
    nx, ny, nz = -dzx * 8.0, -dzy * 8.0, np.ones_like(h)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    L = np.array([0.5, 0.5, 0.7]); L = L / np.linalg.norm(L)
    shade = np.clip(nx * inv * L[0] + ny * inv * L[1] + nz * inv * L[2], 0, 1)

    ox = oxide
    if ox.shape != h.shape:
        ox = np.asarray(Image.fromarray((np.clip(oxide, 0, 1) * 255).astype(np.uint8))
                        .resize((N, N), Image.BILINEAR)) / 255.0
    gap = np.full((N, 8, 3), 0.12)
    row = np.concatenate([g3(h), gap, g3(ox), gap, g3(shade)], axis=1)
    Image.fromarray((np.clip(row, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(path)


def self_checks(verts, tris, h, z, p):
    print("\n=== SELF-CHECKS ===")
    ok = True

    def chk(name, cond, detail):
        nonlocal ok
        ok = ok and cond
        print("  [%s] %s\n        %s" % ("PASS" if cond else "FAIL", name, detail))

    chk("mesh finite", bool(np.isfinite(verts).all()) and bool(np.isfinite(tris).all()),
        "verts=%d tris=%d" % (verts.shape[0], tris.shape[0]))
    n = verts[:, 3:6]
    ln = np.sqrt((n * n).sum(1))
    chk("normals unit-length, +Z dominant", float(np.abs(ln - 1).max()) < 1e-4 and bool((n[:, 2] > 0).all()),
        "max|‖n‖-1|=%.2e  +Z frac=%.4f" % (float(np.abs(ln - 1).max()), float((n[:, 2] > 0).mean())))
    imax = int(tris.max())
    chk("triangle indices in range", imax < verts.shape[0],
        "max idx=%d < nverts=%d" % (imax, verts.shape[0]))

    # The whole point: CENTRE relief is NOT washed.  Compare the std of the
    # height in the inner 12% radius disc against the mid-band — Cartesian cells
    # mean the centre carries comparable relief (old polar dial → ~0 here).
    R = p["radius"]
    N = p["mesh_n"]
    xs = np.linspace(-R, R, N)
    X, Y = np.meshgrid(xs, xs)
    rr = np.hypot(X, Y)
    inner = (rr <= 0.12 * R)
    midb = (rr >= 0.45 * R) & (rr <= 0.6 * R)
    s_in = float(h[inner].std())
    s_mid = float(h[midb].std())
    ratio = s_in / s_mid if s_mid > 1e-9 else 0.0
    chk("centre relief survives (Cartesian, no polar wash)", ratio > 0.5,
        "inner-disc height std=%.4f vs mid-band std=%.4f → ratio=%.2f (want >0.5; "
        "old polar dial collapses toward 0)" % (s_in, s_mid, ratio))

    print("=== %s ===\n" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED"))
    return ok


# --------------------------------------------------------------------------
# Main.
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radius", type=float, default=20.6, help="dial radius (world). Default 20.6 (tucks under the bezel inner edge ~20.8).")
    ap.add_argument("--mesh-n", type=int, default=560, help="grid samples across the diameter. Default 560.")
    ap.add_argument("--disp", type=float, default=0.42, help="relief amplitude (world units, peak-to-peak ≈ disp·relief_depth). Default 0.42.")
    ap.add_argument("--num-arms", type=int, default=12, help="N near-radial lightning petals / grid sectors. Default 12.")
    ap.add_argument("--swirl", type=float, default=0.0, help="angular lean over centre→rim (radians). 0 = pure radial (no spiral). Default 0.")
    ap.add_argument("--seam-jag", type=float, default=0.16, help="lightning seam zig-zag amplitude (rad). Default 0.16.")
    ap.add_argument("--seam-jag-freq", type=float, default=3.0, help="zig-zag teeth centre→rim. Default 3.")
    ap.add_argument("--cell", type=float, default=0.9, help="woven cell world size (land-to-land). Default 0.9.")
    ap.add_argument("--grid-amp", type=float, default=0.85, help="woven-grid weight (dominant relief). Default 0.85.")
    ap.add_argument("--petal-amp", type=float, default=0.30, help="petal envelope weight (organizing lobes). Default 0.30.")
    ap.add_argument("--grid-e0", type=float, default=0.12, help="grid groove inner shoulder (low → wide flat cells, deep grooves). Default 0.12.")
    ap.add_argument("--grid-e1", type=float, default=0.5, help="grid groove outer shoulder. Default 0.5.")
    ap.add_argument("--petal-e0", type=float, default=0.0, help="petal plateau inner shoulder. Default 0.0.")
    ap.add_argument("--petal-e1", type=float, default=0.82, help="petal plateau outer shoulder. Default 0.82.")
    ap.add_argument("--base", type=float, default=0.15)
    ap.add_argument("--land-level", type=float, default=0.45)
    ap.add_argument("--relief-depth", type=float, default=0.85)
    ap.add_argument("--center-radius", type=float, default=0.03, help="inner fraction flattened to a flush hub. Default 0.03.")
    ap.add_argument("--oxide-size", type=int, default=1024)
    ap.add_argument("--oxide-falloff", choices=["linear", "quadratic", "smooth"], default="quadratic",
                    help="radial torch heat falloff baked into the oxide SHAPE map "
                         "(quadratic = dwell concentrates at the rim). The absolute nm "
                         "(torch start/end) is set in the SCENE via the oxide_thk "
                         "scale/bias, NOT here. Default quadratic.")
    ap.add_argument("--out-dir", default=_THIS_DIR)  # write mesh/maps next to the scene
    ap.add_argument("--oxide-only", action="store_true",
                    help="bake only the oxide SHAPE maps (radial + lightning variants); skip the 28 MB mesh")
    ap.add_argument("--lightning-hot", type=float, default=0.40,
                    help="extra normalized oxide along the lightning zigzag (torch held LONGER -> bluer). Default 0.40.")
    ap.add_argument("--lightning-cool", type=float, default=0.40,
                    help="reduced normalized oxide along the lightning zigzag (torch held LESS -> golder). Default 0.40.")
    args = ap.parse_args(argv)

    p = dict(radius=args.radius, mesh_n=args.mesh_n, disp=args.disp,
             num_arms=args.num_arms, swirl=args.swirl, seam_jag=args.seam_jag,
             seam_jag_freq=args.seam_jag_freq, cell=args.cell,
             grid_amp=args.grid_amp, petal_amp=args.petal_amp,
             grid_e0=args.grid_e0, grid_e1=args.grid_e1,
             petal_e0=args.petal_e0, petal_e1=args.petal_e1,
             base=args.base, land_level=args.land_level,
             relief_depth=args.relief_depth, center_radius=args.center_radius)

    out_dir = args.out_dir if os.path.isabs(args.out_dir) else os.path.join(_THIS_DIR, args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    print("Cartesian dial mesh — radius=%.2f mesh_n=%d disp=%.3f arms=%d swirl=%.2f cell=%.2f"
          % (args.radius, args.mesh_n, args.disp, args.num_arms, args.swirl, args.cell))

    written = []

    if not args.oxide_only:
        verts, tris, h, z = build_mesh(p)
        raw2 = os.path.join(out_dir, "dial.raw2")
        write_raw2(raw2, verts, tris)
        written.append(raw2)
        print("  mesh: %d verts, %d tris  z∈[%.4f, %.4f] (p2p %.4f world)"
              % (verts.shape[0], tris.shape[0], z.min(), z.max(), z.max() - z.min()))

    # --- oxide SHAPE maps: uniform radial base + lightning-zigzag torch variants.
    # All share the dial's Cartesian UV and the scene's oxide_thk scale/bias -> nm,
    # so they are drop-in swappable for the tf_dial film_thickness painter.
    oxide = build_oxide_cart(args.oxide_size, args.radius, args.oxide_falloff,
                             Ea=tox.METAL_KINETICS["Ti"])  # Ti shape (Ea-driven curvature)
    petal = lightning_mask(args.oxide_size, p)

    def _save_oxide(field, name, note):
        path = os.path.join(out_dir, name)
        u8 = (np.clip(field, 0, 1) * 255 + 0.5).astype(np.uint8)
        Image.fromarray(np.stack([u8] * 3, -1), "RGB").save(path)
        written.append(path)
        print("  oxide: %s  (%s)" % (os.path.basename(path), note))

    _save_oxide(oxide, "oxide_cart.png",
                "UNIFORM radial heat SHAPE 0=centre..1=rim, falloff=%s" % args.oxide_falloff)
    _save_oxide(tox.apply_torch_pattern(oxide, petal, +args.lightning_hot),
                "oxide_lightning_hot.png",
                "torch held LONGER on the lightning zigzag (+%.2f -> bluer zigzag)" % args.lightning_hot)
    _save_oxide(tox.apply_torch_pattern(oxide, petal, -args.lightning_cool),
                "oxide_lightning_cool.png",
                "torch held LESS on the lightning zigzag (-%.2f -> golder zigzag)" % args.lightning_cool)

    # Per-base-metal oxide SHAPE maps: same torch dose, but each metal's
    # parabolic-oxidation activation energy (tox.METAL_KINETICS) bends the radial
    # curvature differently -- the dose SHAPE itself is now per-metal-physical.
    for metal in ("Nb", "Ta", "Steel"):
        ea = tox.METAL_KINETICS[metal]
        fld = build_oxide_cart(args.oxide_size, args.radius, args.oxide_falloff, Ea=ea)
        _save_oxide(fld, "oxide_%s.png" % metal.lower(),
                    "%s base-metal SHAPE (Ea=%.0f kJ/mol)" % (metal, ea / 1e3))

    if not args.oxide_only:
        pv = os.path.join(out_dir, "dial_preview.png")
        save_preview(pv, h, oxide, args.radius)
        written.append(pv)

    print("\n=== FILES WRITTEN ===")
    for f in written:
        print("  %s" % os.path.abspath(f))

    if args.oxide_only:
        return 0
    return 0 if self_checks(verts, tris, h, z, p) else 1


if __name__ == "__main__":
    sys.exit(main())
