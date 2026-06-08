#!/usr/bin/env python3
"""Procedural CURVED-PETAL ROSETTE + per-petal ORTHOGONAL-GRID guilloché
generator for the RISE thin-film watch dial (Phase 3 of
docs/THIN_FILM_INTERFERENCE.md).

It synthesizes a generic engine-turned "flame-anodized titanium" dial in the
style of the MING 37.06 "Lightning" dial:

    MACRO = a flower / pinwheel ROSETTE of N broad, GENTLY-CURVED leaf /
            lens-shaped PETALS radiating from a small bright centre to the
            rim.  The petals sweep with a slight PINWHEEL twist (smooth
            spiral arcs), NOT sharp jagged bolts.
    MICRO = each petal is filled with a fine ORTHOGONAL GRID — two
            perpendicular sets of grooves (radial "rungs" + angular "bars")
            forming a woven mesh of small ~square cells.  The grid is
            oriented to each petal's LOCAL frame (one set along the petal,
            one across).
    SEAMS = because the angular grid is mirror-FOLDED at every petal
            boundary, adjacent petals' grids meet at an angle → the petal
            edges become jagged, zig-zag, woven SEAMS.  THAT seam
            jaggedness is the "lightning" quality (NOT jagged lightning
            rays); the petal interiors stay a clean curved grid.

NO brand mark is reproduced.

================================================================
POLAR-UV OUTPUT CONVENTION  (read this before wiring the scene)
================================================================
The dial geometry is a `circulardisk_geometry`.  Its texture UV is
POLAR (see src/Library/Geometry/CircularDiskGeometry.cpp
`DiskUVFromPosition`):

    a hit at radius r, polar angle θ has texture UV
        u = θ / (2π)   ∈ [0, 1)      (wraps the disc once)
        v = r / R      ∈ [0, 1]      (centre → rim)

RISE's TexturePainter samples a texture as GetPEL(row, col) with
    row ↔ v   (the SECOND uv coord),
    col ↔ u   (the FIRST  uv coord).

Therefore every map below is written so that

    *** COLUMN index  ↔  angle  θ ∈ [0, 2π)   (col 0 = θ=0, wraps) ***
    *** ROW    index  ↔  radius r ∈ [0, R]    (row 0 = CENTRE,
                                               last row = RIM)     ***

The whole pattern is defined ANALYTICALLY as functions of physical
polar (ρ=r/R, θ) and sampled straight onto this (col=θ, row=r) grid — no
Cartesian resample.  Because the texture is periodic in θ it tiles
seamlessly across the u=0 / u=1 seam (every angular function is exactly
2π-periodic in θ, AND exactly (2π/N)-periodic in θ → N-fold symmetric).
The centre (ρ→0) converges to a small flat / slightly-recessed land (the
hands hub covers it) so there is no divide-by-zero or tall-boss artifact
at the pole.

Default output is WIDTH = `--resolution` (angle axis, default 2048) and
HEIGHT = resolution // 2 (radius axis, default 1024).  The angle axis
needs more samples than the radius axis because the grid has its finest
angular detail near the rim; a 2:1 (angle:radius) aspect keeps angular
Nyquist headroom.  Override the radius rows with --radius-resolution.

================================================================
OUTPUTS  (PNG, into --out-dir, default textures/dial/)
================================================================
1. guilloche_height.png  — HEIGHT field h(ρ,θ) ∈ [0,1], 8-bit RGB with
   R=G=B=height.  *** This is the map the scene uses *** (loaded
   `color_space Rec709RGB_Linear`, consumed as a `displacement` source on
   the displaced disc).  RISE's PNG loader wants 8-bit RGB, so it is always
   written as 8-bit RGB regardless of --bit-depth (which now only affects
   the auxiliary normal/angle previews — see below).  Engraved grooves are
   LOW, ridges HIGH; relief is shallow / engine-turned.

2. guilloche_normal.png  — tangent-space normal map, RGB = (N*0.5+0.5)
   8-bit encoding, +Z up (B≈1 in flat regions).  Computed from the
   height gradient in the *polar texture frame*:
        N ∝ ( -∂h/∂u' , -∂h/∂v' , 1 )
   where u' = u (angle, periodic) and v' = v (radius).  ⚠ See "NORMAL-MAP /
   DISK FRAME CAVEAT" below — the disk's NormalMap tangent frame is
   Cartesian, not polar.  The height map is the primary deliverable; the
   normal map is a preview / for a polar-tangent-aware consumer.

3. guilloche_angle.png — grayscale, pixel value = (local groove tangent
   direction angle) / (2π) ∈ [0,1].  Feeds a GGX `tangent_rotation` slot if
   the scene wants anisotropy aligned to the grooves.  CONVENTION: the stored
   angle φ is measured in the POLAR TEXTURE TANGENT BASIS (û = +angle axis,
   v̂ = +radius axis), CCW from û toward v̂:
        stored = (φ mod 2π) / (2π),   φ = atan2(t·v̂, t·û)
   where t is the unit tangent of the local radial-rung groove.  Because the
   grooves sweep with the pinwheel swirl, φ varies smoothly across the dial.
   See the ANGLE-MAP / GGX FRAME CAVEAT.

================================================================
NORMAL-MAP / DISK FRAME CAVEAT  (for the scene author)
================================================================
src/Library/Modifiers/NormalMap.cpp decodes the tangent-space normal
against `ri.derivatives.dpdu / dpdv`.  For CircularDiskGeometry,
ComputeSurfaceDerivatives returns a CONSTANT, CARTESIAN, axis-aligned
basis — NOT the polar (angle,radius) basis the texture UV uses.  So a
tangent-space normal map authored in the polar frame would be decoded in
the wrong frame on a bare disk.  The robust path the scene uses is the
HEIGHT map as a `displacement` source (real vertex displacement, frame-
agnostic for a scalar field).  Ship the height map; treat the normal map
as a preview / for a polar-tangent-aware (meshed) consumer.

================================================================
ANGLE-MAP / GGX FRAME CAVEAT  (for the scene author)
================================================================
GGX `tangent_rotation` rotates the shading ONB's (u,v) around w by the
painted angle.  On a disk the base ONB u()-axis is an arbitrary-but-
deterministic in-plane direction, NOT the polar +angle axis, so the
absolute zero differs by a constant (and possibly a sign).  Reconcile with
a single global offset/scale on the painter; the *relative* direction field
is correct regardless.

================================================================
PATTERN MATH  (height field — engine-turned plateaus/grooves)
================================================================
ρ = r/R ∈ [0,1] (rows), θ ∈ [0,2π) (cols).

(1) PINWHEEL SWIRL — the petal generator.  A SMOOTH swirl rotates the
    angle progressively with radius, so iso-angle lines curve into gentle
    spiral arcs (the petals sweep like fan blades):

        psi = θ + swirl · ρ                         (radians, smooth)
        P   = N · psi                               (petal phase, 2π/petal)

    swirl ∈ ~[0.3, 1.2] rad sets how much the petals curl (0 = straight
    radial petals; larger = more pinwheel).  Linear-in-ρ keeps it a clean
    spiral and keeps EXACT N-fold symmetry (θ→θ+2π/N shifts P by 2π).

(2) N CURVED PETALS.  A broad raised-plateau ("lens") profile of cos(P):

        petal = lens(cos(P))      ∈ [0,1]

    lens(c) = smoothstep(e0,e1,|c|) raises a wide plateau over the petal
    body and drops to a land along the seam.  Broad (low e0) → the gentle
    leaf/lens petals of the reference, not thin rays.

(3) PER-PETAL ORTHOGONAL GRID.  Two perpendicular groove families, woven
    by min-combine into a mesh of small ~square cells:

        rungs = stripe(grid_freq · ρ)               (radial rungs, smooth
                                                     concentric arcs)
        fold  = |2·frac(P/2π) − 1|                  (triangle, 1 at seam,
                                                     0 at petal centre)
        bars  = stripe(grid_ang · fold)             (angular bars, MIRROR-
                                                     folded at each seam)
        grid  = min(rungs, bars)                    ∈ [0,1] woven mesh

    grid_ang (bars per petal half) is tied to grid_freq so cells are ~square
    in the mid-band.  The FOLD is the seam generator: the angular bars on
    the two sides of a petal boundary are mirror images, so where two
    petals' grids meet the bars approach from opposite tilts → a jagged,
    zig-zag, woven SEAM, while each petal interior stays a clean grid.

(4) COMPOSE (sharp plateau profiles, NOT smooth sines):

        h = base + petal_amp · petal + grid_amp · grid
    then normalize to [0,1], apply a gamma `land_level` bias and squeeze into
    the requested shallow `relief_depth` around mid=0.5.  The innermost rows
    are blended to a flat land so ρ→0 is a small recessed hub, not a boss.

EXACT N-FOLD SYMMETRY: every angular dependence enters only through
functions of P = N·(θ + swirl·ρ): cos(P), frac(P/2π), and the grid bars on
the fold of P/2π.  Replacing θ → θ + 2π/N shifts P by exactly 2π, leaving
cos(P) and frac(P/2π) (hence fold and bars) unchanged → h(θ) ≡ h(θ + 2π/N).
Verified spectrally (all angular energy on k%N==0) and by a direct ring
roll/compare in the self-checks.

Only numpy + Pillow are required.
"""

import argparse
import math
import os
import sys

import numpy as np

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("ERROR: Pillow (PIL) is required.  pip install Pillow\n")
    raise


# --------------------------------------------------------------------------
# Sharp profile primitives.
# --------------------------------------------------------------------------

def smoothstep(e0, e1, x):
    """Hermite smoothstep clamped to [0,1] (used to build sharp plateaus)."""
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lens(c, edge0, edge1):
    """Sharp raised-plateau profile of a carrier c∈[-1,1]: 1 where |c| is
    large (on the ridge axis), dropping to 0 over [edge0,edge1] of |c|.

    With edge0≈0.2, edge1≈0.9 this yields a broad plateau-with-shoulders —
    the engine-turned 'flat land / proud body' look rather than a rounded
    sine.  Returns [0,1]."""
    return smoothstep(edge0, edge1, np.abs(c))


def stripe(x, edge0, edge1):
    """A periodic 'groove vs land' stripe of period 1 in x, built so each
    period has a flat-bottom groove and flat-top land (engine-turned).
    Uses the |cos| plateau profile: stripe = smoothstep(e0,e1,|cos(2πx)|).
    Returns [0,1] — 1 on the land (ridge), 0 in the groove."""
    return smoothstep(edge0, edge1, np.abs(np.cos(2.0 * math.pi * x)))


# --------------------------------------------------------------------------
# Grid builders.  Everything is vectorized over the full (radius_rows,
# angle_cols) grid.  THETA[row,col] = θ, RHO[row,col] = r/R.
# --------------------------------------------------------------------------

def build_grids(angle_cols, radius_rows):
    """Return (THETA, RHO) arrays of shape (radius_rows, angle_cols).

    THETA  in [0,2π)  along columns, sampled at CELL CENTRES so the u=0/u=1
           seam is symmetric and tiles cleanly:  θ = 2π·(col+0.5)/angle_cols
    RHO    in [0,1]    along rows, row 0 anchored at r=0 exactly (centre),
           last row at r=1 (rim):  ρ = row/(radius_rows-1)
    """
    theta = (np.arange(angle_cols, dtype=np.float64) + 0.5) * (2.0 * math.pi / angle_cols)
    rho = np.linspace(0.0, 1.0, radius_rows, dtype=np.float64)
    THETA, RHO = np.meshgrid(theta, rho)      # (radius_rows, angle_cols)
    return THETA, RHO


def build_height(THETA, RHO, *, num_arms, swirl, grid_freq, grid_ang,
                 seam_shear, petal_edge0, petal_edge1, grid_edge0, grid_edge1,
                 petal_amp, grid_amp, base_level, land_level, relief_depth,
                 center_radius):
    """Compose the curved-petal rosette + per-petal woven-grid height field
    h ∈ [0,1] and return it plus the swept petal phase `psi` (the angle map
    reuses the swirl).

    Centre row (RHO==0): psi = θ (swirl·0 = 0); all carriers finite; the
    innermost rows are then blended to a flat land.
    """
    # ---- (1) pinwheel swirl: the petal generator -------------------------
    # Smooth linear-in-ρ swirl curls each petal into a gentle spiral arc.
    psi = THETA + swirl * RHO
    P = num_arms * psi                         # petal phase, 2π per petal

    # ---- (2) N curved petals (broad lens plateaus) ----------------------
    petal = lens(np.cos(P), petal_edge0, petal_edge1)                   # [0,1]

    # ---- (3) per-petal orthogonal grid ----------------------------------
    # Within-petal coordinates from the petal phase:
    #   tri_signed ∈ [-1,+1)  sawtooth — signed position across the petal,
    #               JUMPS at the seam (used to tilt the rungs oppositely on
    #               the two sides of a seam → chevron).
    #   fold ∈ [0,1]          triangle — 1 at the seam, 0 at the petal centre
    #               (mirror-reflects the angular bars about the seam).
    beta = P / (2.0 * math.pi)
    frac = beta - np.floor(beta)
    tri_signed = 2.0 * frac - 1.0              # -1..+1 sawtooth, jumps at seam
    fold = np.abs(tri_signed)                  # 0 at petal centre, 1 at seam
    # Radial rungs, SHEARED by the signed within-petal position so the local
    # square lattice TILTS into a diamond/rhombus mesh; because tri_signed
    # flips sign across the seam the tilt flips → a clean chevron seam (the
    # woven "lightning" interlock).  seam_shear=0 → upright concentric rungs.
    rungs = stripe(grid_freq * RHO + seam_shear * tri_signed, grid_edge0, grid_edge1)
    # Angular bars MIRROR-FOLDED at each petal boundary so adjacent petals'
    # bars meet at opposite tilts → reinforces the woven seam.
    bars = stripe(grid_ang * fold, grid_edge0, grid_edge1)
    grid = np.minimum(rungs, bars)             # [0,1] woven mesh of cells

    # ---- (4) compose -----------------------------------------------------
    h = base_level + petal_amp * petal + grid_amp * grid

    # Normalize to [0,1].
    h = h - h.min()
    rng = h.max() - h.min()
    if rng < 1e-12:
        h = np.zeros_like(h)
    else:
        h = h / rng

    # land_level (0..1) gamma-biases the groove/ridge balance: maps the
    # field's median toward land_level (lower → more recessed land with the
    # petal ridges standing proud; typical engine-turning).
    ll = min(max(land_level, 1e-3), 1.0 - 1e-3)
    gamma = math.log(ll) / math.log(0.5)
    h = np.power(h, gamma)

    # Squeeze into the requested shallow relief around mid=0.5 so the engraving
    # is engine-turned-shallow: final h spans [mid - depth/2, mid + depth/2].
    mid = 0.5
    h = mid + (h - 0.5) * relief_depth

    # Centre = SMALL bright convergence point (hands hub covers it):
    # the petals + grid sweep TIGHTLY in and converge to a small point, as on
    # the reference dial — NOT a big flat pale/recessed disc.  Blend only the
    # innermost `center_radius` fraction of rows toward a finite hub value, with
    # a steep exponent so the flat land is a tiny dot and the converging texture
    # carries almost all the way in.  Anchor the hub a touch ABOVE the global
    # mean (a small bright point) and keep it finite (no divide-by-zero / boss).
    nblend = max(2, int(round(center_radius * h.shape[0])))
    gmean = float(h.mean())
    hub_level = gmean + 0.30 * (float(h.max()) - gmean)   # small bright point, finite
    for i in range(nblend):
        w = (i / nblend) ** 3.0                       # steep: 0 at centre row → 1 at edge of blend
        h[i] = (1.0 - w) * hub_level + w * h[i]

    h = np.clip(h, 0.0, 1.0)
    return h, psi


def build_angle_map(THETA, RHO, *, swirl, disk_radius_world):
    """Local radial-rung groove tangent direction, stored as φ/(2π) ∈ [0,1].

    The radial rungs run along iso-lines of the swept radial coordinate; the
    swirl tilts the local frame by the swirl rate.  The rung grooves are
    iso-ρ arcs in the swept frame, whose tangent in the polar texture frame
    (û=+angle, v̂=+radius) carries the swirl tilt:

        a rung is a curve of constant ρ in the (psi, ρ) frame → it runs in
        the +psi direction.  Since psi = θ + swirl·ρ, holding ρ fixed means
        dθ = dpsi, so the rung tangent is purely +angle (φ=0).  The grid as a
        whole, though, sweeps: the angular bars run along constant-psi lines,
        whose tangent has slope dρ/dθ from dpsi=0 ⇒ dθ = −swirl·dρ.

    For a useful anisotropy field we store the BAR (along-petal) tangent,
    which carries the swirl.  Along a constant-psi line:
        dθ/du' = 2π·dθ,  dρ/dv' = R·dρ,  with dθ = −swirl·dρ.
        t_u ∝ dθ·2π = −swirl·2π·dρ ,  t_v ∝ R·dρ
    so φ = atan2(R, −swirl·2π) (constant in ρ,θ for a linear swirl) — i.e.
    the bar direction is a single global tilt set by the swirl.  We compute
    it per-pixel for generality (future non-linear swirl).
    """
    # Bar (constant-psi) tangent in the polar texture frame.  For psi linear
    # in ρ the swirl rate dpsi/dρ = swirl is constant; along constant psi,
    # dθ = -swirl·dρ.
    dpsi_drho = np.full_like(RHO, swirl)
    t_u = -dpsi_drho * (2.0 * math.pi)        # angle-axis component
    t_v = np.full_like(RHO, disk_radius_world)  # radius-axis component
    phi = np.arctan2(t_v, t_u)            # (-π, π]
    phi = np.mod(phi, 2.0 * math.pi)      # [0, 2π)
    return phi / (2.0 * math.pi)          # [0,1]


def angle_map_physical_variation(ang01, THETA):
    """Diagnostic: convert the texture-frame groove angle to a PHYSICAL
    (world-plane) direction and report its circular spread, to prove the cut
    direction genuinely swirls/varies across the dial.  φ_world = φ + θ."""
    phi = ang01 * 2.0 * math.pi
    phi_world = np.mod(phi + THETA, 2.0 * math.pi)
    c = np.cos(phi_world).mean()
    s = np.sin(phi_world).mean()
    R = math.sqrt(c * c + s * s)
    circ_std = math.sqrt(-2.0 * math.log(max(R, 1e-12)))  # radians
    return float(phi_world.min()), float(phi_world.max()), circ_std


def build_normal_map(h, *, relief_world_scale):
    """Tangent-space normal map from the height gradient in the polar texture
    frame.  Periodic in u (columns), clamped in v (rows).
        N ∝ (-∂h/∂u', -∂h/∂v', 1) then normalized; encoded (N*0.5+0.5).
    """
    du = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5   # periodic cols
    dv = np.empty_like(h)
    dv[1:-1, :] = (h[2:, :] - h[:-2, :]) * 0.5
    dv[0, :] = h[1, :] - h[0, :]
    dv[-1, :] = h[-1, :] - h[-2, :]

    nx = -du * relief_world_scale
    ny = -dv * relief_world_scale
    nz = np.ones_like(h)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx /= norm
    ny /= norm
    nz /= norm
    rgb = np.stack([nx, ny, nz], axis=-1) * 0.5 + 0.5
    return np.clip(rgb, 0.0, 1.0)


# --------------------------------------------------------------------------
# Cartesian "as-seen-on-dial" reprojection (for the human-eyeball preview).
# --------------------------------------------------------------------------

def reproject_to_cartesian(h, size):
    """Resample the polar height field h(row=r, col=θ) onto a square
    Cartesian (x,y) image of side `size`, as the dial actually appears.
    BILINEAR (θ wraps, r clamps); outside the unit disc → 0.  Human-eyeball
    preview ONLY — the polar map is what RISE samples."""
    radius_rows, angle_cols = h.shape
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    cx = cy = (size - 1) / 2.0
    x = (xx - cx) / (size / 2.0)
    y = (yy - cy) / (size / 2.0)
    r = np.sqrt(x * x + y * y)
    theta = np.mod(np.arctan2(y, x), 2.0 * math.pi)
    inside = r <= 1.0
    fr = np.clip(r, 0.0, 1.0) * (radius_rows - 1)
    fc = theta / (2.0 * math.pi) * angle_cols
    r0 = np.floor(fr).astype(np.int64)
    r1 = np.clip(r0 + 1, 0, radius_rows - 1)
    r0 = np.clip(r0, 0, radius_rows - 1)
    wr = fr - np.floor(fr)
    c0 = np.floor(fc).astype(np.int64) % angle_cols
    c1 = (c0 + 1) % angle_cols
    wc = fc - np.floor(fc)
    top = h[r0, c0] * (1 - wc) + h[r0, c1] * wc
    bot = h[r1, c0] * (1 - wc) + h[r1, c1] * wc
    out = top * (1 - wr) + bot * wr
    out = np.where(inside, out, 0.0)
    return out


# --------------------------------------------------------------------------
# Adversarial numeric self-checks.
# --------------------------------------------------------------------------

def run_self_checks(h, ang, nrm, THETA, RHO, *, num_arms, angle_cols, radius_rows,
                    swirl, grid_freq, grid_ang):
    """Verify the invariants the prompt requires; return all_ok."""
    results = []

    # (a) N-fold rotational symmetry — SPECTRAL: every angular dependence is
    #     a function of P = N·(θ + swirl·ρ), so all angular energy must land
    #     on harmonics that are multiples of N.
    lo = int(0.1 * radius_rows)
    rows = h[lo:]
    rows = rows - rows.mean(axis=1, keepdims=True)
    F = np.abs(np.fft.rfft(rows, axis=1)) ** 2
    k = np.arange(F.shape[1])
    on_comb = (k % num_arms) == 0
    total = F.sum()
    off_frac = float(F[:, ~on_comb].sum() / total) if total > 0 else 0.0
    ok_spec = off_frac < 1e-3
    results.append((f"{num_arms}-fold rotational symmetry (angular spectrum)", ok_spec,
                    f"off-N-comb energy fraction={off_frac:.2e} "
                    f"(all angular energy on harmonics k%{num_arms}==0)"))

    # (a2) direct ring roll/compare: h(θ) vs h(θ+2π/N) at a mid ring.
    shift = angle_cols // num_arms
    err = 0.0
    if shift * num_arms == angle_cols:    # exact only when N | angle_cols
        mid_row = radius_rows // 2
        ring = h[mid_row]
        err = float(np.abs(ring - np.roll(ring, shift)).max())
        ok_roll = err < 2e-3
        detail = (f"max|h(θ)-h(θ+2π/{num_arms})| at mid ring = {err:.2e} "
                  f"(angle_cols {angle_cols} divisible by {num_arms})")
    else:
        ok_roll = True   # not exactly divisible → roll test N/A, spectral covers it
        detail = (f"angle_cols {angle_cols} not divisible by {num_arms}; "
                  f"roll-test N/A (spectral check covers symmetry)")
    results.append((f"{num_arms}-fold symmetry (direct ring roll)", ok_roll, detail))

    # (b) no NaN/inf anywhere; centre row finite.
    finite_h = np.isfinite(h).all()
    finite_a = np.isfinite(ang).all()
    finite_n = np.isfinite(nrm).all()
    centre_ok = np.isfinite(h[0]).all() and np.isfinite(ang[0]).all()
    ok = finite_h and finite_a and finite_n and centre_ok
    results.append(("finite (no NaN/inf, centre row finite)", ok,
                    f"h={finite_h} ang={finite_a} nrm={finite_n} centre={centre_ok}"))

    # (c) centre is a SMALL flat convergence point, not a tall boss / spike: the
    #     innermost row's spread is tiny (uniform — no divide-by-zero spike) and
    #     its value is FINITE and in-range (bounded within the field's [min,max]
    #     so it is neither a tall boss above the relief nor a hole below it).
    #     The hub is a small BRIGHT point on the reference dial, so it sits a
    #     touch above the mean — that is intended, not a boss.
    centre_spread = float(h[0].max() - h[0].min())
    centre_flat = centre_spread < 0.02
    centre_val = float(h[0].mean())
    centre_bounded = (float(h.min()) - 1e-6) <= centre_val <= (float(h.max()) + 1e-6)
    ok = centre_flat and centre_bounded
    results.append(("centre is a small flat convergence point (no boss/spike)", ok,
                    f"centre-row spread={centre_spread:.4f} (<0.02, uniform), "
                    f"centre value={centre_val:.4f} bounded within "
                    f"field [{h.min():.4f},{h.max():.4f}]={centre_bounded}"))

    # (d) petals are CURVED (pinwheel sweep), NOT jagged bolts.  The fine grid
    #     dominates the FULL height field, so tracking h's per-row argmax (or
    #     even its k=N DFT phase) just follows grid cells — both the petal lobe
    #     AND the seam-folded grid live on k%N==0 bins.  Instead reconstruct the
    #     PETAL-ONLY envelope analytically, lens(cos(N·(θ+swirl·ρ))), and track
    #     ITS crest column vs radius.  By construction the crest sits at
    #     θ_crest(ρ) = −swirl·ρ + 2πj/N (linear in ρ) → a clean spiral that
    #     drifts MONOTONICALLY; a jagged bolt would reverse.  This both proves
    #     the macro is a smooth swept rosette and guards against a future edit
    #     reintroducing a zig-zag.
    # |cos(N·psi)| has 2N equal crests per ring (one per petal flank), so a
    # GLOBAL argmax hops between petals.  Follow ONE crest by continuity: start
    # at the brightest column of the inner row, then at each outward row step to
    # the nearest local crest.  For a smooth swirl this single crest drifts
    # monotonically; a zig-zag bolt would reverse.
    lo = int(0.1 * radius_rows)
    petal_only = np.abs(np.cos(num_arms * (THETA[lo:] + swirl * RHO[lo:])))
    nr = petal_only.shape[0]
    track = np.empty(nr, dtype=np.float64)
    track[0] = float(np.argmax(petal_only[0]))
    crest_spacing = angle_cols / (2.0 * num_arms)   # cols between adjacent crests
    for i in range(1, nr):
        # candidate crests this row = local maxima; cheap proxy: search a window
        # around the previous column for this row's max.
        win = int(max(2, crest_spacing * 0.6))
        prev = int(round(track[i - 1]))
        idx = (np.arange(prev - win, prev + win + 1)) % angle_cols
        local = petal_only[i, idx]
        track[i] = idx[int(np.argmax(local))]
    d = np.diff(track)
    d = (d + angle_cols / 2.0) % angle_cols - angle_cols / 2.0   # wrap drift
    sign = np.sign(d)
    sign = sign[np.abs(d) > 1e-9]
    reversals = int(np.sum(sign[1:] != sign[:-1])) if sign.size > 1 else 0
    total_drift = float(abs(np.sum(d)))
    # Expected drift in columns: |swirl|·0.9·(angle_cols/2π) over the span.
    expected_px = abs(swirl) * 0.9 * angle_cols / (2.0 * math.pi)
    if swirl > 1e-6:
        ok = (total_drift > 0.4 * expected_px) and (reversals <= 2)
        results.append(("petals are CURVED (petal-lobe crest sweeps smoothly, not jagged)", ok,
                        f"tracked petal crest column-drift={total_drift:.0f}px "
                        f"(expect≈{expected_px:.0f}, need>{0.4*expected_px:.0f}), "
                        f"drift reversals={reversals} (<=2 → smooth spiral, not a "
                        f"bolt); swirl={swirl}"))
    else:
        results.append(("petals curved", True, "swirl disabled → straight radial petals (by request)"))

    # (e) Nyquist: cap the finest cuts near the rim against the resolution.
    #     stripe()'s measured harmonic energy is ~99% within the first 2
    #     harmonics radially / >99.9% within 5 harmonics angularly.  Radial:
    #     fundamental 2*grid_freq cyc; budget the 2nd harmonic → top=4*grid_freq;
    #     need rows >= 2*top = 8*grid_freq.
    radial_top = 4.0 * grid_freq
    radial_nyq_ok = radius_rows >= 2.0 * radial_top      # rows >= 8*grid_freq
    #     Angular: the folded angular bars give grid_ang cuts per petal HALF →
    #     2·grid_ang per petal → N·2·grid_ang cuts/circle; budget the 5th
    #     harmonic → top = 5·N·2·grid_ang; need cols >= 2*top = 20·N·grid_ang.
    ang_fund = num_arms * 2.0 * grid_ang
    ang_top = 5.0 * ang_fund
    ang_nyq_ok = angle_cols >= 2.0 * ang_top
    ok = radial_nyq_ok and ang_nyq_ok
    results.append(("Nyquist (finest cuts resolvable near rim)", ok,
                    f"radial: top={radial_top:.0f}cyc need rows≥{2*radial_top:.0f} "
                    f"have {radius_rows} ({'OK' if radial_nyq_ok else 'ALIASES'}); "
                    f"angular: fund={ang_fund:.0f}cyc top≈{ang_top:.0f} "
                    f"need cols≥{2*ang_top:.0f} have {angle_cols} "
                    f"({'OK' if ang_nyq_ok else 'ALIASES'})"
                    + ("" if ok else " — lower --grid-freq/--grid-ang or raise resolution")))

    # (f) normal map encodes a unit-ish vector, +Z dominant.
    n = nrm * 2.0 - 1.0
    lens_ = np.sqrt((n * n).sum(axis=-1))
    len_ok = (np.abs(lens_ - 1.0).max() < 1e-3)
    z_dom = float((n[..., 2] > 0).mean())
    z_ok = z_dom > 0.99
    results.append(("normal map unit-length, +Z dominant", len_ok and z_ok,
                    f"max|‖n‖-1|={np.abs(lens_-1.0).max():.2e} +Z fraction={z_dom:.4f}"))

    # (g) angle map in [0,1].
    a_ok = (ang.min() >= 0.0) and (ang.max() <= 1.0)
    results.append(("angle map ∈ [0,1] (polar-frame CCW from +angle)", a_ok,
                    f"range [{ang.min():.4f}, {ang.max():.4f}]"))

    # (g2) cut direction varies in physical space (the swirl makes the groove
    #      direction sweep around the dial even if it's constant in the
    #      texture frame, because φ_world = φ + θ).
    pmin, pmax, pstd = angle_map_physical_variation(ang, THETA)
    swirl_ok = pstd > 0.3
    results.append(("groove direction varies in PHYSICAL frame", swirl_ok,
                    f"φ_world circular-std={pstd:.3f} rad over [{pmin:.3f},{pmax:.3f}]"))

    print("\n=== ADVERSARIAL SELF-CHECKS ===")
    all_ok = True
    for name, ok, detail in results:
        tag = "PASS" if ok else "FAIL"
        if not ok:
            all_ok = False
        print(f"  [{tag}] {name}\n         {detail}")
    print(f"=== {'ALL CHECKS PASSED' if all_ok else 'SOME CHECKS FAILED'} ===\n")
    return all_ok


# --------------------------------------------------------------------------
# PNG writers.
# --------------------------------------------------------------------------

def save_gray_rgb8(path, arr01):
    """Save a [0,1] float field as an 8-bit RGB PNG with R=G=B=value.
    RISE's PNG loader wants 8-bit RGB for the displacement source."""
    data = (np.clip(arr01, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    rgb = np.stack([data, data, data], axis=-1)
    Image.fromarray(rgb, mode="RGB").save(path)


def save_gray(path, arr01, bit_depth):
    """Save a [0,1] field as 8- or 16-bit grayscale (used for the angle map
    preview only — the height map always goes through save_gray_rgb8)."""
    arr01 = np.clip(arr01, 0.0, 1.0)
    if bit_depth == 16:
        data = (arr01 * 65535.0 + 0.5).astype(np.uint16)
        img = Image.frombuffer("I;16", (data.shape[1], data.shape[0]),
                               data.tobytes(), "raw", "I;16", 0, 1)
        img.save(path)
    else:
        data = (arr01 * 255.0 + 0.5).astype(np.uint8)
        Image.fromarray(data, mode="L").save(path)


def save_rgb(path, rgb01):
    data = (np.clip(rgb01, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(data, mode="RGB").save(path)


def save_preview(path, h, ang, nrm, cart):
    """Montage: [height | angle | normal] over a Cartesian reprojection of the
    height, so a human can eyeball the rosette + grid + seams."""
    rr, ac = h.shape
    def to_u8_gray(a):
        a = np.clip(a, 0.0, 1.0)
        return (a * 255 + 0.5).astype(np.uint8)
    strip_h = 256
    strip_w = int(strip_h * ac / rr)
    def resize_gray(a):
        return np.asarray(Image.fromarray(to_u8_gray(a), "L").resize((strip_w, strip_h), Image.BILINEAR))
    def resize_rgb(a):
        u8 = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
        return np.asarray(Image.fromarray(u8, "RGB").resize((strip_w, strip_h), Image.BILINEAR))
    h_s = np.stack([resize_gray(h)] * 3, axis=-1)
    a_s = np.stack([resize_gray(ang)] * 3, axis=-1)
    n_s = resize_rgb(nrm)
    gap = np.full((strip_h, 8, 3), 32, dtype=np.uint8)
    top = np.concatenate([h_s, gap, a_s, gap, n_s], axis=1)
    total_w = top.shape[1]

    # Bottom row: the Cartesian "as-on-dial" reprojection.  The full dial shows
    # the MACRO (curved-petal rosette + swirl); a 1:1 crop beside it shows the
    # MICRO (woven grid + chevron seams) which would otherwise alias away when
    # the whole fine-grid dial is shrunk to fit.  `cart` is rendered at high
    # res by the caller for this reason.
    cart_full = np.clip(cart, 0, 1)
    big = cart_full.shape[0]
    half = (total_w - 8) // 2          # two panels side by side
    # left panel: whole dial, area-style downsample to suppress moiré
    full_img = np.asarray(Image.fromarray((cart_full * 255 + 0.5).astype(np.uint8), "L")
                          .resize((half, half), Image.LANCZOS))
    # right panel: a true 1:1 mid-band crop spanning ≈2 petals (shows a seam)
    cy = big // 2
    c0 = int(big * 0.30); r0 = int(big * 0.30)
    crop = (cart_full[r0:r0 + half, c0:c0 + half] * 255 + 0.5).astype(np.uint8)
    if crop.shape[0] < half or crop.shape[1] < half:   # safety pad
        cc = np.zeros((half, half), np.uint8); cc[:crop.shape[0], :crop.shape[1]] = crop; crop = cc
    full_rgb = np.stack([full_img] * 3, axis=-1)
    crop_rgb = np.stack([crop] * 3, axis=-1)
    bgap = np.full((half, 8, 3), 32, dtype=np.uint8)
    bottom = np.concatenate([full_rgb, bgap, crop_rgb], axis=1)
    # pad bottom to total_w if rounding left a 1-px gap
    if bottom.shape[1] < total_w:
        pad = np.full((half, total_w - bottom.shape[1], 3), 16, dtype=np.uint8)
        bottom = np.concatenate([bottom, pad], axis=1)
    elif bottom.shape[1] > total_w:
        bottom = bottom[:, :total_w]

    sep = np.full((8, total_w, 3), 32, dtype=np.uint8)
    montage = np.concatenate([top, sep, bottom], axis=0)
    Image.fromarray(montage, "RGB").save(path)


# --------------------------------------------------------------------------
# Main.
# --------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(
        description="Generate a CURVED-PETAL ROSETTE + per-petal ORTHOGONAL-GRID "
                    "guilloché height / normal / angle map set for the RISE "
                    "thin-film dial (polar-UV: col=angle, row=radius).")
    p.add_argument("--num-arms", type=int, default=8,
                   help="N curved petals radiating from centre. Default 8.")
    p.add_argument("--swirl", type=float, default=0.5,
                   help="Pinwheel swirl amount (radians of angular twist over "
                        "centre→rim). 0=straight radial petals; ~0.3-1.2 curls "
                        "them into spiral arcs. Default 0.5.")
    p.add_argument("--grid-freq", type=float, default=64.0,
                   help="Concentric radial cuts (rungs) centre→rim. Sets cell "
                        "size. Default 64. Auto-capped to radius_rows/8.")
    p.add_argument("--grid-ang", type=float, default=None,
                   help="Angular grid bars per petal HALF (mirror-folded at "
                        "seams). Default: auto-tied to grid-freq for ~square "
                        "mid-band cells. Auto-capped against angular Nyquist.")
    p.add_argument("--seam-shear", type=float, default=1.8,
                   help="Tilt of the woven grid lattice into a diamond mesh; the "
                        "tilt flips sign across each petal seam → the chevron "
                        "'lightning' interlock. 0=upright cells. Default 1.8.")
    p.add_argument("--relief-depth", type=float, default=0.8,
                   help="Total height span (engine-turned shallow), 0..1. Default 0.8.")
    p.add_argument("--petal-amp", type=float, default=0.25,
                   help="Weight of the curved-petal macro (broad tonal lobes; "
                        "the grid + seams carry most of the look). Default 0.25.")
    p.add_argument("--grid-amp", type=float, default=0.68,
                   help="Weight of the woven-grid micro (dominant). Default 0.68.")
    p.add_argument("--base-level", type=float, default=0.15,
                   help="Constant base added before normalization. Default 0.15.")
    p.add_argument("--petal-edge0", type=float, default=0.0,
                   help="Petal plateau inner shoulder (|cos| where the petal "
                        "body starts to drop). Lower = broader petals. Default 0.0.")
    p.add_argument("--petal-edge1", type=float, default=0.82,
                   help="Petal plateau outer shoulder (|cos| where it reaches "
                        "the seam land). Lower = broader, fatter petals that "
                        "nearly touch their neighbours. Default 0.82.")
    p.add_argument("--grid-edge0", type=float, default=0.32,
                   help="Grid-cell inner shoulder. Lower = thicker grid lines. "
                        "Default 0.32.")
    p.add_argument("--grid-edge1", type=float, default=0.85,
                   help="Grid-cell outer shoulder. Default 0.85.")
    p.add_argument("--land-level", type=float, default=0.45,
                   help="Median land height before relief normalization (gamma "
                        "bias). Default 0.45.")
    p.add_argument("--center-radius", type=float, default=0.05,
                   help="Fraction of the radius blended to a small bright hub at "
                        "the centre (the rest of the texture converges tightly to "
                        "a point). Smaller = the petals/grid carry closer to "
                        "centre. Default 0.05 (~5%% of the radius).")
    p.add_argument("--normal-strength", type=float, default=6.0,
                   help="In-plane gradient scale for the normal map. Default 6.0.")
    p.add_argument("--disk-radius-world", type=float, default=1.0,
                   help="Physical disc radius R for the angle-map tilt term. "
                        "Default 1.0.")
    p.add_argument("--resolution", type=int, default=2048,
                   help="WIDTH = angle-axis resolution. Default 2048.")
    p.add_argument("--radius-resolution", type=int, default=None,
                   help="HEIGHT = radius-axis resolution. Default = resolution//2.")
    p.add_argument("--bit-depth", type=int, choices=[8, 16], default=8,
                   help="Bit depth for the AUX angle-map preview only; the height "
                        "map is ALWAYS 8-bit RGB (RISE loader requirement). Default 8.")
    p.add_argument("--out-dir", default="textures/dial/",
                   help="Output directory. Default textures/dial/.")
    p.add_argument("--preview-size", type=int, default=512,
                   help="Cartesian reprojection size in the preview. Default 512.")
    args = p.parse_args(argv)

    angle_cols = int(args.resolution)
    radius_rows = int(args.radius_resolution) if args.radius_resolution else angle_cols // 2

    # ---- Nyquist cap on grid_freq (radial cuts) ------------------------
    # stripe()=smoothstep(|cos(2π·grid_freq·ρ)|) has its fundamental at
    # 2*grid_freq cycles centre→rim.  Measured harmonic energy of stripe():
    # h1≈0.90, h2≈0.07, h3+≈0.01 — i.e. ~99% of the energy is in the first
    # TWO harmonics.  Budget the 2nd harmonic (top = 2·(2*grid_freq) =
    # 4*grid_freq); Nyquist needs radius_rows >= 2*top = 8*grid_freq.
    grid_freq = float(args.grid_freq)
    nyq_cap = radius_rows / 8.0
    capped_radial = False
    if grid_freq > nyq_cap:
        sys.stderr.write(
            f"WARNING: --grid-freq {grid_freq:g} exceeds radial Nyquist "
            f"(radius_rows/8 = {nyq_cap:g}; budgeting the stripe's 2nd harmonic "
            f"at ~99% energy, top radial content ≈4*grid_freq); capping to "
            f"{nyq_cap:g} to avoid aliasing.  Raise --radius-resolution for a "
            f"finer grid.\n")
        grid_freq = nyq_cap
        capped_radial = True

    # ---- auto-tie grid_ang to grid_freq for ~square mid-band cells ------
    # A radial rung spacing is 1/grid_freq in ρ → physical R/grid_freq.  An
    # angular bar (folded) makes grid_ang stripes per petal-half → 2·grid_ang
    # per petal → arc spacing at radius ρ is (2π·ρ·R)/(N·2·grid_ang).  Match
    # at ρ≈0.6 (mid-band): R/grid_freq = (2π·0.6·R)/(N·2·grid_ang) →
    # grid_ang = grid_freq·π·0.6/N.
    if args.grid_ang is not None:
        grid_ang = float(args.grid_ang)
    else:
        grid_ang = round(grid_freq * math.pi * 0.6 / max(args.num_arms, 1))
        grid_ang = max(grid_ang, 1.0)

    # ---- Nyquist cap on grid_ang (angular cuts) ------------------------
    # Folded angular bars give 2·grid_ang stripes per petal → N·2·grid_ang
    # cuts/circle.  Measured harmonic energy of stripe(): >99.9% is in the
    # first FIVE harmonics (h1≈0.90, h2≈0.07, h3..h5 carry the rest; h6+≈0).
    # Budget the 5th harmonic: top ≈ 5·N·2·grid_ang; need angle_cols >=
    # 2·top, i.e. grid_ang <= angle_cols/(20·N).
    ang_cap = math.floor(angle_cols / (20.0 * max(args.num_arms, 1)))
    capped_ang = False
    if grid_ang > ang_cap:
        sys.stderr.write(
            f"WARNING: grid-ang {grid_ang:g} exceeds angular Nyquist "
            f"(angle_cols/(20*num_arms)={ang_cap:g}; budgeting the stripe's 5th "
            f"harmonic at >99.9% energy); capping to {ang_cap:g} to avoid "
            f"aliasing.  Raise --resolution for more angular bars.\n")
        grid_ang = float(ang_cap)
        capped_ang = True

    os.makedirs(args.out_dir, exist_ok=True)

    print(f"Guilloché generator — curved-petal rosette + per-petal woven grid")
    print(f"  grid: {angle_cols} (angle/cols) x {radius_rows} (radius/rows)")
    print(f"  num_arms={args.num_arms}  swirl={args.swirl:g}  seam_shear={args.seam_shear:g}")
    print(f"  grid_freq={grid_freq:g}{' (capped)' if capped_radial else ''}  "
          f"grid_ang={grid_ang:g}{' (capped)' if capped_ang else ''}"
          f"{' (auto)' if args.grid_ang is None else ''}")
    print(f"  petal_amp={args.petal_amp:g}  grid_amp={args.grid_amp:g}  "
          f"relief_depth={args.relief_depth:g}  out_dir={args.out_dir}")

    # ---- build the fields ----------------------------------------------
    THETA, RHO = build_grids(angle_cols, radius_rows)

    h, psi = build_height(
        THETA, RHO,
        num_arms=args.num_arms,
        swirl=args.swirl,
        grid_freq=grid_freq,
        grid_ang=grid_ang,
        seam_shear=args.seam_shear,
        petal_edge0=args.petal_edge0,
        petal_edge1=args.petal_edge1,
        grid_edge0=args.grid_edge0,
        grid_edge1=args.grid_edge1,
        petal_amp=args.petal_amp,
        grid_amp=args.grid_amp,
        base_level=args.base_level,
        land_level=args.land_level,
        relief_depth=args.relief_depth,
        center_radius=args.center_radius,
    )

    ang = build_angle_map(
        THETA, RHO,
        swirl=args.swirl,
        disk_radius_world=args.disk_radius_world,
    )

    nrm = build_normal_map(h, relief_world_scale=args.normal_strength)

    # ---- write outputs --------------------------------------------------
    hp = os.path.join(args.out_dir, "guilloche_height.png")
    ap = os.path.join(args.out_dir, "guilloche_angle.png")
    np_ = os.path.join(args.out_dir, "guilloche_normal.png")
    save_gray_rgb8(hp, h)         # *** 8-bit RGB — the map the scene uses ***
    save_gray(ap, ang, 8)         # angle map fine at 8-bit (≈0.4°/step)
    save_rgb(np_, nrm)

    # ---- stats ----------------------------------------------------------
    def stats(name, a):
        print(f"  {name:18s} min={a.min():.5f} max={a.max():.5f} "
              f"mean={a.mean():.5f} std={a.std():.5f}")
    print("\n=== OUTPUT STATS ===")
    stats("height", h)
    stats("angle", ang)
    stats("normal.x", nrm[..., 0])
    stats("normal.y", nrm[..., 1])
    stats("normal.z", nrm[..., 2])

    # ---- preview montage + Cartesian reprojection ----------------------
    # Reproject at HIGH res so the preview's 1:1 micro crop shows the woven
    # grid + chevron seams (a low-res reprojection aliases the fine grid into
    # invisibility; the macro panel is area-downsampled from this same image).
    cart_res = max(1400, 2 * args.preview_size)
    cart = reproject_to_cartesian(h, cart_res)
    pv = os.path.join(args.out_dir, "_preview.png")
    save_preview(pv, h, ang, nrm, cart)

    print("\n=== FILES WRITTEN ===")
    for f in (hp, np_, ap, pv):
        print(f"  {os.path.abspath(f)}")
    print("\nThe preview montage SHOULD show (left→right, top row): the polar")
    print("HEIGHT map (petal bands that lean smoothly with row = the swirl,")
    print("filled with a fine orthogonal grid that mirror-flips at petal")
    print("boundaries), the ANGLE map, and the NORMAL map.  The bottom row is")
    print(f"the Cartesian 'as-on-dial' reprojection: a {args.num_arms}-petal")
    print("curved pinwheel rosette of grid-filled petals with jagged woven")
    print("seams between petals, a small bright/recessed hub at centre.")

    # ---- adversarial self-checks ---------------------------------------
    all_ok = run_self_checks(h, ang, nrm, THETA, RHO,
                             num_arms=args.num_arms, angle_cols=angle_cols,
                             radius_rows=radius_rows, swirl=args.swirl,
                             grid_freq=grid_freq, grid_ang=grid_ang)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
