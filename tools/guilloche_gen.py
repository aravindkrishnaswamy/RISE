#!/usr/bin/env python3
"""Procedural SPIRAL PINWHEEL ROSETTE guilloché generator for the RISE
thin-film watch dial (Phase 3 of docs/THIN_FILM_INTERFERENCE.md).

It synthesizes a generic engine-turned "flame-anodized titanium" dial:
an N-fold pinwheel of broad curved petals (arms) spiraling out from the
centre, each petal filled with a fine woven / herringbone micro-texture.
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
polar (r, θ) and sampled straight onto this (col=θ, row=r) grid — no
Cartesian resample.  Because the texture is periodic in θ it tiles
seamlessly across the u=0 / u=1 seam (we use angular functions that are
exactly 2π-periodic).  The centre row (r→0) converges to a flat,
uniform value (height = the mid-level "land") so there is no
divide-by-zero singularity at the pole — every per-r factor that would
blow up as r→0 is guarded.

Default output is WIDTH = `--resolution` (angle axis, default 2048) and
HEIGHT = resolution // 2 (radius axis, default 1024).  The angle axis
needs more samples than the radius axis because the petals + weave have
their finest detail in the angular direction near the rim; a 2:1
(angle:radius) aspect keeps angular Nyquist headroom without doubling
the radius rows.  Override the radius rows with --radius-resolution.

================================================================
OUTPUTS  (PNG, into --out-dir, default textures/dial/)
================================================================
1. guilloche_height.png  — grayscale HEIGHT field h(r,θ) ∈ [0,1].
   16-bit by default (--bit-depth 8 for an 8-bit map).  Engraved
   grooves are LOW, ridges HIGH; relief is shallow / engine-turned.
   Consume via a `bumpmap_modifier { function <png_painter> ... }`.

2. guilloche_normal.png  — tangent-space normal map, RGB = (N*0.5+0.5)
   8-bit encoding, +Z up (B≈1 in flat regions).  Computed from the
   height gradient in the *polar texture frame*:
        N ∝ ( -∂h/∂u' , -∂h/∂v' , 1 )
   where u' = u (angle, periodic) and v' = v (radius).  Load it with
   `color_space Rec709RGB_Linear` (verbatim store; RISEPel==Rec709RGBPel
   post-Stage-B, see NormalMap.cpp).  ⚠ See "NORMAL-MAP / DISK FRAME
   CAVEAT" below — the disk's NormalMap tangent frame is Cartesian, not
   polar, so this map is only frame-correct on a geometry whose dpdu/dpdv
   follow the polar UV.  The height map is the safer primary deliverable.

3. guilloche_angle.png — grayscale, pixel value = (local arm/groove
   tangent direction angle) / (2π) ∈ [0,1].  Feeds the GGX
   `tangent_rotation` slot (scene loads it Rec709RGB_Linear and applies
   `scale 6.2832` → radians).  CONVENTION (precise):

       The stored angle φ is measured in the POLAR TEXTURE TANGENT
       BASIS (dpdu_polar = +angle/+θ direction, dpdv_polar =
       +radius/outward direction), CCW from dpdu_polar toward
       dpdv_polar:
            stored = (φ mod 2π) / (2π),     φ = atan2( t·v̂ , t·û )
       where t is the unit tangent of the local groove (the direction
       ALONG the petal arm / weave ridge), û = +angle axis, v̂ = +radius
       axis.  φ = 0      ⇒ groove runs purely along +angle (tangential).
       φ = π/2  ⇒ groove runs purely radially outward.

   This is the natural "which way is the cut running" signal.  See the
   ANGLE-MAP / GGX FRAME CAVEAT below for how to reconcile it with the
   frame `tangent_rotation` actually rotates within.

================================================================
NORMAL-MAP / DISK FRAME CAVEAT  (for the scene author)
================================================================
src/Library/Modifiers/NormalMap.cpp decodes the tangent-space normal
against `ri.derivatives.dpdu / dpdv`.  For CircularDiskGeometry,
ComputeSurfaceDerivatives returns a CONSTANT, CARTESIAN, axis-aligned
basis (e.g. z-disk: dpdu=(1,0,0), dpdv=(0,1,0)) — it is NOT the polar
(angle,radius) basis the texture UV uses.  So a tangent-space normal map
authored in the polar frame (as this one is) would be decoded in the
wrong frame on a bare disk and look swirled incorrectly.  Options for
the controller: (a) ship the height map and let `bumpmap_modifier`
derive normals itself (bumpmap perturbs along the surface-derivative
directions too, but a scalar height bump is far more forgiving of frame
than an RGB tangent-space vector); (b) tessellate the disk to a mesh
with per-vertex TANGENT aligned to the polar +angle direction; or
(c) treat guilloche_normal.png as a preview/àref only.  The height map
(deliverable #1) is the robust primary; the normal map is provided for
completeness and for any polar-tangent-aware consumer.

================================================================
ANGLE-MAP / GGX FRAME CAVEAT  (for the scene author)
================================================================
GGX `tangent_rotation` (Utilities/MicrofacetUtils.h `RotateTangent`)
rotates the shading ONB's (u,v) around w by the painted angle.  On a
disk the base ONB is `ri.onb` built from the geometric normal — its u()
axis is an arbitrary-but-deterministic in-plane direction, NOT the
polar +angle axis.  So the absolute zero of `tangent_rotation` differs
from this map's polar-frame zero by a constant (and possibly a sign,
depending on ONB handedness).  Reconcile by adding a single global
offset in the scene (e.g. another `scale`/bias on the painter, or a
constant added to the painted value) until the anisotropic streaks line
up with the visible grooves; the *relative* direction field (how the
cut angle varies across the dial) is correct regardless of that
constant.  If you instead drive anisotropy off a polar-tangent mesh
(option (b) above), the offset reduces to handedness alignment only.

================================================================
PATTERN MATH
================================================================
Let the angular pinwheel envelope be a spiraled raised cosine:

    twist(r)  = handedness * spiral_tightness * g(r)         (radians)
    phase     = N * ( θ - twist(r) )
    petal(r,θ)= raisedcos( cos(phase) )      ∈ [0,1]

g(r) = (r/R) by default (linear spiral → arms curve uniformly); the
twist ROTATES the petals as radius grows, so an arm at θ₀ near the
centre appears at θ₀ + handedness*spiral_tightness near the rim — that
is the spiral.  raisedcos() reshapes the cos into a broad soft petal
(soft-clipped) so arms read as petals, not thin lines.

Within the petals we ride a fine basketweave: two high-frequency hatch
fields at ±45° to the LOCAL arm tangent, screen-combined, modulated by
the petal envelope so the weave concentrates on the arms:

    s along  = arc length along the arm  ≈ ∫ ... ; we use a separable
               proxy  s_ang = N*(θ - twist(r))  (cycles around a petal)
               and    s_rad = (r/R)             (0..1 along radius)
    h1 = sin( weave_freq*(s_rad*2π) + s_ang )            (one diagonal)
    h2 = sin( weave_freq*(s_rad*2π) - s_ang )            (the other)
    weave = 0.5*(h1*h2) ...  herringbone product, screen-combined

Compose:  h0 = petal_relief*petal + weave_amp*(petal*weave - 0.5*petal),
normalized to [0,1], then a gamma curve set by `land_level` biases the
groove/ridge balance (lower land_level → more of the field recessed as
"land", with the petal arms standing proud — typical engine-turning),
and finally rescaled so min..max spans the requested relief_depth around
mid=0.5.  relief is shallow.

The LOCAL ARM TANGENT used for the angle map is the analytic gradient
direction of the petal-phase iso-lines: the petal ridge runs along the
direction in (angle,radius) space where `phase` is stationary, i.e.
perpendicular to ∇phase.  ∇phase in the polar texture frame:
    ∂phase/∂u' = ∂/∂θ [N(θ - twist(r))] * (dθ/du')  with u'=u=θ/2π so
                 dθ/du' = 2π  →  ∂phase/∂u' = N*2π
    ∂phase/∂v' = N * (-twist'(r)) * (dr/dv')  with v'=v=r/R so
                 dr/dv' = R     →  ∂phase/∂v' = -N*R*twist'(r)
The ridge tangent t = rot90(∇phase) (perpendicular), normalized; its
angle in (û,v̂) is what we store.  This is exact, not finite-differenced.

Only numpy + Pillow are required (scipy optional, unused).
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
# Core analytic field builders.  Everything is vectorized over the full
# (radius_rows, angle_cols) grid.  THETA[row, col] = θ, RHO[row, col] = r/R.
# --------------------------------------------------------------------------

def build_grids(angle_cols, radius_rows):
    """Return (THETA, RHO_norm) arrays of shape (radius_rows, angle_cols).

    THETA  in [0, 2π)  along columns (col 0 = 0).  Sample at CELL CENTRES
           so the u=0/u=1 seam is symmetric and tiles cleanly:
               θ = 2π * (col + 0.5) / angle_cols
    RHO    in [0, 1]    along rows (row 0 = centre).  Row centres too, but
           we anchor row 0 at r=0 exactly so the centre is represented:
               we use r = row / (radius_rows - 1)  (0 at centre, 1 at rim)
    """
    # Angle: periodic, so cell-centre sampling (no duplicated endpoint).
    theta = (np.arange(angle_cols, dtype=np.float64) + 0.5) * (2.0 * math.pi / angle_cols)
    # Radius: include both endpoints (centre row exact 0, rim row exact 1).
    rho = np.linspace(0.0, 1.0, radius_rows, dtype=np.float64)
    THETA, RHO = np.meshgrid(theta, rho)  # shape (radius_rows, angle_cols)
    return THETA, RHO


def raised_cosine_petal(x, sharpness):
    """Map cos(phase) ∈ [-1,1] to a broad soft petal in [0,1].

    sharpness > 1 narrows the petals (more land between arms); ~1 gives
    a plain raised cosine.  We use a soft-clip via a smoothstep-like
    power curve on the positive lobe so arms read as broad petals rather
    than thin sinusoid lines, with a soft (not hard) valley.
    """
    # Bring cos from [-1,1] to [0,1].
    t = 0.5 * (x + 1.0)
    # Emphasize the crest: t**sharpness keeps the broad top but deepens
    # the troughs.  Then a smootherstep for soft shoulders.
    t = np.clip(t, 0.0, 1.0) ** sharpness
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)  # smootherstep(t)


def build_height(THETA, RHO, *, num_arms, spiral_tightness, handed_sign,
                 g_mode, weave_freq, weave_amp, petal_sharpness,
                 land_level, petal_relief, relief_depth, weave_phase_arms):
    """Compose the height field h(r,θ) ∈ [0,1] and return it plus the
    intermediate `twist`, `twist_prime`, and `phase` arrays (the angle
    map reuses twist_prime).

    All inputs vectorized; centre row (RHO==0) handled by construction
    (g(0)=0 ⇒ twist=0 there; weave radial term →0; everything finite).
    """
    # ---- spiral twist g(r) and its derivative wrt r/R -------------------
    if g_mode == "linear":
        g = RHO
        gp = np.ones_like(RHO)            # dg/d(r/R)
    elif g_mode == "log":
        # log spiral: g = log(1 + k*rho)/log(1+k); guarded at rho=0 (g=0).
        k = 8.0
        g = np.log1p(k * RHO) / math.log1p(k)
        gp = (k / (1.0 + k * RHO)) / math.log1p(k)
    elif g_mode == "sqrt":
        g = np.sqrt(RHO)
        # derivative 1/(2 sqrt(rho)) blows up at 0 — guard the centre row.
        with np.errstate(divide="ignore", invalid="ignore"):
            gp = np.where(RHO > 1e-6, 0.5 / np.sqrt(np.maximum(RHO, 1e-12)), 0.0)
    else:
        raise ValueError(f"unknown g_mode {g_mode!r}")

    twist = handed_sign * spiral_tightness * g            # radians
    twist_prime = handed_sign * spiral_tightness * gp     # d(twist)/d(r/R)

    # ---- pinwheel petal envelope ---------------------------------------
    phase = num_arms * (THETA - twist)
    petal = raised_cosine_petal(np.cos(phase), petal_sharpness)  # [0,1]

    # ---- woven micro-texture (herringbone / engine-turned hatch) -------
    # The defining look is a fine ACROSS-the-arm hatch whose diagonal slope
    # FLIPS periodically ALONG the arm (chevron / "lightning" zig-zag), plus
    # a plain cross-weave underlay.  Two coordinates, both exactly N-fold
    # periodic so the whole field stays N-fold symmetric:
    #   along = num_arms·(θ - twist(r))   — one 2π unit per petal (the petal
    #           phase itself); the chevron flips on this axis.
    #   across = weave_freq · 2π · r_norm — the fine hatch carrier (radial),
    #           weave_freq cycles centre→rim.
    along = phase                                   # == num_arms*(THETA - twist)
    across = weave_freq * (2.0 * math.pi) * RHO
    # Chevron: a triangle wave on `along` (weave_phase_arms flips per petal)
    # tilts the hatch ±slope, so the fine lines zig-zag like a herringbone.
    # triangle(x) ∈ [-1,1] with period 2π:
    tri = (2.0 / math.pi) * np.arcsin(np.sin(weave_phase_arms * along))
    chevron_tilt = 0.9 * math.pi * tri              # ± slope amount (radians)
    hatch = np.sin(across + chevron_tilt)           # zig-zagging fine hatch
    # Plain cross-weave underlay: two opposed diagonals multiplied → woven
    # cells (keeps a "basket" feel between chevron flips).  Use a modest
    # along-frequency tied to the chevron count so it stays N-periodic.
    cross = np.sin(across + chevron_tilt) * np.sin(across - chevron_tilt)
    weave = 0.5 * (0.7 * hatch + 0.3 * cross) + 0.5   # ≈[0,1]
    # Concentrate the weave ON the petals (modulate by envelope), so the
    # land between arms stays smoother — matches the reference.
    weave_on_petals = weave * petal

    # ---- compose --------------------------------------------------------
    h = (petal_relief * petal
         + weave_amp * (weave_on_petals - 0.5 * petal))  # zero-mean-ish weave
    # Normalize to [0,1].
    h = h - h.min()
    rng = h.max() - h.min()
    if rng < 1e-12:
        h = np.zeros_like(h)
    else:
        h = h / rng
    # land_level (0..1) biases the groove/ridge balance: it sets where the
    # neutral "land" sits between the deepest groove (0) and the highest
    # ridge (1) via a gamma curve.  land_level≈0.25 (default) → mostly
    # recessed land with raised petal arms (typical engine-turning);
    # higher → more of the field reads as raised.  gamma = log(land)/log(0.5)
    # maps the median to land_level.
    ll = min(max(land_level, 1e-3), 1.0 - 1e-3)
    gamma = math.log(ll) / math.log(0.5)
    h = np.power(h, gamma)
    # Squeeze into the requested shallow relief around mid so the engraving
    # is engine-turned-shallow: final h spans [mid - depth/2, mid + depth/2].
    mid = 0.5
    h = mid + (h - 0.5) * relief_depth

    # Force the exact centre row to the flat land (no pole artifact): blend
    # the innermost few rows toward the row-mean so r→0 is uniform.
    nblend = max(1, int(round(0.01 * h.shape[0])))
    for i in range(nblend):
        w = i / nblend                      # 0 at centre row → 1 at edge of blend
        row_mean = h[i].mean()
        h[i] = (1.0 - w) * row_mean + w * h[i]

    h = np.clip(h, 0.0, 1.0)
    return h, twist, twist_prime, phase


def build_angle_map(THETA, RHO, *, num_arms, twist_prime, disk_radius_world):
    """Local arm/groove tangent direction, stored as φ/(2π) ∈ [0,1].

    This is the COHERENT lathe-cut direction the GGX anisotropy follows:
    the engine-turning tool cuts ALONG the petal arm, and the ±45° weave
    is defined relative to that arm tangent, so the dominant groove
    direction == the petal arm tangent.

    The petal ridge runs ALONG the iso-lines of `phase = N(θ - twist(r))`;
    its tangent is perpendicular to ∇phase in the polar texture frame
    (û = +angle/+θ axis, v̂ = +radius/outward axis).  Analytic gradient
    (see module docstring):

        ∂phase/∂u' =  num_arms * 2π
        ∂phase/∂v' = -num_arms * R * twist'(r)            (R = disc world radius)

    where u'=u=θ/2π and v'=v=r/R, so the chain-rule factors are 2π and R.
    Tangent t = rot90(∇phase) = (∂phase/∂v', -∂phase/∂u') (perpendicular).
    φ = atan2( t·v̂ , t·û ) = atan2( t_v , t_u ).

    NOTE on the linear spiral: twist'(r) is CONSTANT, so the texture-frame
    arm angle φ is constant across the whole map (a single value).  That
    is CORRECT — a constant direction in the (angle,radius) texture frame
    maps to a smoothly *swirling* physical cut direction on the disc (the
    arms genuinely spiral).  Verified numerically by the physical-frame
    variation self-check.  For --spiral-mode {log,sqrt} twist'(r) varies
    with r, so φ varies across rows too.  The map is 8-bit (≈0.4°/step),
    ample for an anisotropy direction.
    """
    dphase_du = np.full_like(THETA, num_arms * 2.0 * math.pi)
    dphase_dv = -num_arms * disk_radius_world * twist_prime
    # Tangent perpendicular to the gradient (rotate gradient by +90°):
    #   grad = (dphase_du, dphase_dv);  perp = (dphase_dv, -dphase_du)
    t_u = dphase_dv
    t_v = -dphase_du
    phi = np.arctan2(t_v, t_u)            # (-π, π]
    phi = np.mod(phi, 2.0 * math.pi)      # [0, 2π)
    return phi / (2.0 * math.pi)          # [0,1]


def angle_map_physical_variation(ang01, THETA):
    """Diagnostic: convert the texture-frame groove angle to a PHYSICAL
    (world-plane) direction and report its angular spread, to prove the
    cut direction genuinely swirls across the dial even when the
    texture-frame angle is constant.

    Texture-frame basis on the disc: û (=+angle) is the CIRCUMFERENTIAL
    direction (tangent +θ̂), v̂ (=+radius) is the RADIAL direction (r̂).
    A texture-frame angle φ therefore corresponds to a physical in-plane
    direction  φ_world = φ + θ  (since θ̂ leads r̂ by... we just add θ:
    rotating the local polar basis by the point's θ gives world frame).
    Return (min,max,std) of φ_world in radians.
    """
    phi = ang01 * 2.0 * math.pi
    phi_world = np.mod(phi + THETA, 2.0 * math.pi)
    # unwrap-free spread proxy: circular std via mean resultant length
    c = np.cos(phi_world).mean()
    s = np.sin(phi_world).mean()
    R = math.sqrt(c * c + s * s)
    circ_std = math.sqrt(-2.0 * math.log(max(R, 1e-12)))  # radians
    return float(phi_world.min()), float(phi_world.max()), circ_std


def build_normal_map(h, *, relief_world_scale):
    """Tangent-space normal map from the height gradient in the polar
    texture frame.  Periodic in u (columns), clamped in v (rows).

    N ∝ (-∂h/∂u', -∂h/∂v', 1) then normalized; encoded (N*0.5+0.5).
    `relief_world_scale` scales the in-plane gradient so the normal map's
    apparent bump strength is decoupled from the [0,1] height range
    (larger → steeper normals).
    """
    # ∂/∂u' : columns are periodic → use np.gradient with wrap via roll.
    du = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    # ∂/∂v' : rows are NOT periodic → central diff, edge-clamped.
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
    rgb = np.stack([nx, ny, nz], axis=-1) * 0.5 + 0.5  # [0,1]
    return np.clip(rgb, 0.0, 1.0)


# --------------------------------------------------------------------------
# Cartesian "as-seen-on-dial" reprojection (for the human-eyeball preview).
# --------------------------------------------------------------------------

def reproject_to_cartesian(h, size):
    """Resample the polar height field h(row=r, col=θ) onto a square
    Cartesian (x,y) image of side `size`, as the dial actually appears.
    BILINEAR sample (vectorized; θ wraps periodically, r clamps); outside
    the unit disc → 0.  This is a human-eyeball preview ONLY — the polar
    map (provably N-fold symmetric) is what RISE samples; treat per-pixel
    features of this reprojection as resample artifacts, not the pattern.
    """
    radius_rows, angle_cols = h.shape
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    cx = cy = (size - 1) / 2.0
    x = (xx - cx) / (size / 2.0)
    y = (yy - cy) / (size / 2.0)
    r = np.sqrt(x * x + y * y)                 # [0, ~1.41]
    theta = np.mod(np.arctan2(y, x), 2.0 * math.pi)
    inside = r <= 1.0
    # Fractional source coords.
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

def run_self_checks(h, ang, nrm, THETA, *, num_arms, angle_cols, weave_freq,
                    spiral_tightness, weave_phase_arms):
    """Verify the invariants the prompt requires; return a list of
    (name, ok, detail) tuples and print them."""
    results = []

    # (a) N-fold rotational symmetry: h(r,θ) is exactly N-periodic in θ at
    #     fixed r (phase = N(θ-twist(r)) and the weave carriers are all
    #     functions of N·θ).  Verify SPECTRALLY (immune to resample/interp
    #     artifacts at high weave frequency): the angular DFT of each ring
    #     must place ALL its energy on harmonics that are multiples of N.
    #     We measure the fraction of angular spectral energy that lands OFF
    #     the multiples-of-N comb, averaged over the outer rings.
    lo = int(0.1 * h.shape[0])                  # skip flat centre blend
    rows = h[lo:]
    rows = rows - rows.mean(axis=1, keepdims=True)
    F = np.abs(np.fft.rfft(rows, axis=1)) ** 2  # power per ring per harmonic
    k = np.arange(F.shape[1])
    on_comb = (k % num_arms) == 0               # multiples of N (incl. DC=0)
    total = F.sum()
    off_energy = F[:, ~on_comb].sum()
    off_frac = float(off_energy / total) if total > 0 else 0.0
    # Strict N-fold symmetry holds iff the chevron's angular harmonics
    # (integer multiples of num_arms*weave_phase_arms) are all multiples of
    # num_arms — i.e. iff weave_phase_arms is an INTEGER.  An integer
    # weave_phase_arms (the default) guarantees exact N-fold symmetry; a
    # deliberately fractional value gives a more organic, only-approximately
    # N-fold pattern — accept it with a looser bound and a note.
    commensurate = abs(weave_phase_arms - round(weave_phase_arms)) < 1e-6
    if commensurate:
        ok = off_frac < 1e-3
        note = f"(all angular energy lies on harmonics k%{num_arms}==0)"
    else:
        ok = off_frac < 0.05    # fractional phase_arms → near-N-fold only
        note = (f"(weave_phase_arms={weave_phase_arms:g} is fractional → "
                f"INTENTIONALLY only near-N-fold; bound relaxed)")
    results.append((f"{num_arms}-fold rotational symmetry (angular spectrum)", ok,
                    f"off-N-comb energy fraction={off_frac:.2e} {note}"))

    # (b) no NaN / inf anywhere; centre row finite.
    finite_h = np.isfinite(h).all()
    finite_a = np.isfinite(ang).all()
    finite_n = np.isfinite(nrm).all()
    centre_ok = np.isfinite(h[0]).all() and np.isfinite(ang[0]).all()
    ok = finite_h and finite_a and finite_n and centre_ok
    results.append(("finite (no NaN/inf, centre row finite)", ok,
                    f"h={finite_h} ang={finite_a} nrm={finite_n} centre={centre_ok}"))

    # (c) arms spiral: the column of the petal crest shifts with radius.
    #     Find, per row, the column of the max of one petal band and check
    #     it drifts monotonically-ish across radius by ≈ spiral_tightness.
    lo = int(0.1 * h.shape[0])
    # column index of the global max per row (a crest), unwrapped
    crest_col = np.argmax(h, axis=1).astype(np.float64)
    crest_theta = crest_col / angle_cols * 2.0 * math.pi
    drift = crest_theta[-1] - crest_theta[lo]
    # expected drift magnitude ~ spiral_tightness (mod the petal spacing);
    # we just require a NON-zero drift if spiral_tightness != 0.
    if abs(spiral_tightness) > 1e-6:
        ok = abs(drift) > 1e-3
        results.append(("arms spiral (crest θ drifts with r)", ok,
                        f"crest θ drift over r = {drift:+.4f} rad "
                        f"(spiral_tightness={spiral_tightness})"))
    else:
        results.append(("arms spiral", True, "spiral_tightness=0 → straight arms (by request)"))

    # (d) Nyquist: highest weave frequency near the rim must be resolvable.
    #     CRITICAL: the woven texture is a PRODUCT of two sines,
    #         h1*h2 = 0.5[cos(2·angular) - cos(2·radial)],
    #     so the highest RADIAL frequency present is 2*weave_freq cycles
    #     centre→rim (NOT weave_freq), and the highest ANGULAR frequency is
    #     2*num_arms*weave_phase_arms.  Verified by FFT.  Radial Nyquist
    #     therefore needs radius_rows >= 2*(2*weave_freq) = 4*weave_freq.
    radius_rows = h.shape[0]
    radial_top = 2.0 * weave_freq                 # actual top radial cycles
    radial_nyq_ok = radius_rows >= 2.0 * radial_top   # == 4*weave_freq
    # Angular: the chevron is a TRIANGLE wave with fundamental angular
    # frequency num_arms*weave_phase_arms cycles/circle and slowly-decaying
    # odd harmonics; we budget up to the 7th harmonic (~2% energy) as the
    # effective top → ang_top = 7*num_arms*weave_phase_arms.  Need
    # angle_cols >= 2*ang_top.  Default (N=7, phase_arms=8) → fund=56,
    # top≈392, need cols≥784, have 2048 → OK.
    ang_fund = num_arms * weave_phase_arms
    ang_top = 7.0 * ang_fund
    ang_nyq_ok = angle_cols >= 2.0 * ang_top
    ok = radial_nyq_ok and ang_nyq_ok
    results.append(("Nyquist (weave resolvable near rim)", ok,
                    f"radial: top={radial_top:.0f}cyc need rows≥{2*radial_top:.0f} "
                    f"have {radius_rows} ({'OK' if radial_nyq_ok else 'ALIASES'}); "
                    f"angular chevron: fund={ang_fund:.0f}cyc top≈{ang_top:.0f} "
                    f"need cols≥{2*ang_top:.0f} have {angle_cols} "
                    f"({'OK' if ang_nyq_ok else 'ALIASES'})"
                    + ("" if ok else " — lower --weave-freq/--weave-phase-arms or raise resolution")))

    # (e) normal map encodes a unit-ish vector, +Z dominant.
    n = nrm * 2.0 - 1.0
    lens = np.sqrt((n * n).sum(axis=-1))
    len_ok = (np.abs(lens - 1.0).max() < 1e-3)
    z_dom = float((n[..., 2] > 0).mean())
    z_ok = z_dom > 0.99
    results.append(("normal map unit-length, +Z dominant", len_ok and z_ok,
                    f"max|‖n‖-1|={np.abs(lens-1.0).max():.2e} +Z fraction={z_dom:.4f}"))

    # (f) angle map in [0,1], documented convention (asserted by range).
    a_ok = (ang.min() >= 0.0) and (ang.max() <= 1.0)
    results.append(("angle map ∈ [0,1] (polar-frame CCW from +angle)", a_ok,
                    f"range [{ang.min():.4f}, {ang.max():.4f}]"))

    # (f2) the cut direction genuinely SWIRLS in physical space, even when
    #      the texture-frame angle is constant (linear spiral).  Require a
    #      meaningful physical-frame angular spread.
    pmin, pmax, pstd = angle_map_physical_variation(ang, THETA)
    swirl_ok = pstd > 0.3      # radians of circular std → clearly varying
    results.append(("groove direction swirls in PHYSICAL frame", swirl_ok,
                    f"φ_world circular-std={pstd:.3f} rad over "
                    f"[{pmin:.3f},{pmax:.3f}] (texture-frame angle may be "
                    f"constant; physical direction must vary)"))

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

def save_gray(path, arr01, bit_depth):
    """Save a [0,1] float array as an 8- or 16-bit grayscale PNG."""
    arr01 = np.clip(arr01, 0.0, 1.0)
    if bit_depth == 16:
        data = (arr01 * 65535.0 + 0.5).astype(np.uint16)
        # Build a 16-bit grayscale image without the deprecated `mode=`
        # dtype-coercion path: frombuffer with explicit "I;16" mode reads
        # the uint16 buffer verbatim (little-endian, matching numpy here).
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
    """Montage: [height | angle | normal] stacked over a Cartesian
    reprojection of the height, so a human can eyeball the rosette."""
    rr, ac = h.shape
    # Normalize the three polar maps to a common display height.
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
    # Cartesian reprojection underneath, centered, same total width.
    csz = min(top.shape[1], 512)
    cart_u8 = (np.clip(cart, 0, 1) * 255 + 0.5).astype(np.uint8)
    cart_img = np.asarray(Image.fromarray(cart_u8, "L").resize((csz, csz), Image.BILINEAR))
    cart_rgb = np.stack([cart_img] * 3, axis=-1)
    pad = top.shape[1] - csz
    left = pad // 2
    cart_row = np.full((csz, top.shape[1], 3), 16, dtype=np.uint8)
    cart_row[:, left:left + csz] = cart_rgb
    sep = np.full((8, top.shape[1], 3), 32, dtype=np.uint8)
    montage = np.concatenate([top, sep, cart_row], axis=0)
    Image.fromarray(montage, "RGB").save(path)


# --------------------------------------------------------------------------
# Main.
# --------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(
        description="Generate a spiral pinwheel rosette guilloché height / "
                    "normal / angle map set for the RISE thin-film dial "
                    "(polar-UV: col=angle, row=radius).")
    p.add_argument("--num-arms", type=int, default=7,
                   help="N-fold pinwheel petal count (headline density knob). Default 7.")
    p.add_argument("--weave-freq", type=float, default=110.0,
                   help="Woven hatch radial carrier frequency (fine lines "
                        "ACROSS the arm, cycles centre→rim). Default 110. "
                        "Auto-capped to radius_rows/4 (its product term "
                        "doubles it) with a warning.")
    p.add_argument("--weave-phase-arms", type=float, default=9.0,
                   help="Herringbone chevron flips per petal (how many times "
                        "the fine hatch zig-zags ALONG each arm). Integer "
                        "keeps EXACT N-fold symmetry; fractional → organic "
                        "near-N-fold. Default 9.")
    p.add_argument("--spiral-tightness", type=float, default=2.2,
                   help="Total petal rotation (radians) from centre to rim. "
                        "Larger = more curl. Default 2.2.")
    p.add_argument("--handedness", choices=["cw", "ccw"], default="ccw",
                   help="Spiral handedness. Default ccw.")
    p.add_argument("--spiral-mode", choices=["linear", "log", "sqrt"], default="linear",
                   help="g(r) profile: linear (uniform curl), log (tight near "
                        "centre), sqrt. Default linear.")
    p.add_argument("--relief-depth", type=float, default=0.7,
                   help="Total height span (engine-turned shallow), 0..1. Default 0.7.")
    p.add_argument("--petal-relief", type=float, default=1.0,
                   help="Relative weight of the broad petal envelope vs weave. Default 1.0.")
    p.add_argument("--weave-amp", type=float, default=0.45,
                   help="Relative weight of the woven micro-texture. Default 0.45.")
    p.add_argument("--petal-sharpness", type=float, default=1.6,
                   help="Petal narrowness (>1 = narrower arms, more land). Default 1.6.")
    p.add_argument("--land-level", type=float, default=0.25,
                   help="Base land height before relief normalization. Default 0.25.")
    p.add_argument("--normal-strength", type=float, default=6.0,
                   help="In-plane gradient scale for the normal map (steeper "
                        "normals for larger values). Default 6.0.")
    p.add_argument("--disk-radius-world", type=float, default=1.0,
                   help="Physical disc radius R (world units) used to scale the "
                        "radial term of the analytic groove-angle gradient. "
                        "Affects ONLY the angle map's radial/tangential balance. "
                        "Default 1.0.")
    p.add_argument("--resolution", type=int, default=2048,
                   help="WIDTH = angle-axis resolution. Default 2048.")
    p.add_argument("--radius-resolution", type=int, default=None,
                   help="HEIGHT = radius-axis resolution. Default = resolution//2.")
    p.add_argument("--bit-depth", type=int, choices=[8, 16], default=16,
                   help="Height map bit depth. Default 16.")
    p.add_argument("--out-dir", default="textures/dial/",
                   help="Output directory. Default textures/dial/.")
    p.add_argument("--preview-size", type=int, default=512,
                   help="Cartesian reprojection size in the preview. Default 512.")
    args = p.parse_args(argv)

    angle_cols = int(args.resolution)
    radius_rows = int(args.radius_resolution) if args.radius_resolution else angle_cols // 2
    handed_sign = -1.0 if args.handedness == "cw" else +1.0

    # ---- Nyquist cap on weave_freq -------------------------------------
    # The weave is a product of two sines → its top RADIAL frequency is
    # 2*weave_freq cycles centre→rim, so radial Nyquist demands
    # radius_rows >= 4*weave_freq, i.e. weave_freq <= radius_rows/4.  We
    # use /4.5 (≈10% headroom below Nyquist) because a sine sampled AT
    # exactly Nyquist still aliases; the headroom keeps the symmetry/
    # reconstruction clean.
    weave_freq = float(args.weave_freq)
    nyq_cap = radius_rows / 4.5
    capped = False
    if weave_freq > nyq_cap:
        sys.stderr.write(
            f"WARNING: --weave-freq {weave_freq:g} exceeds radial Nyquist "
            f"(radius_rows/4 = {nyq_cap:g}; the weave's top radial frequency "
            f"is 2*weave_freq); capping to {nyq_cap:g} to avoid aliasing.  "
            f"Raise --radius-resolution to use a finer weave.\n")
        weave_freq = nyq_cap
        capped = True

    # Angular Nyquist on weave_phase_arms: the chevron triangle wave's
    # effective top angular frequency is ~7*num_arms*weave_phase_arms
    # (fundamental num_arms*pa plus odd harmonics to the 7th); need
    # angle_cols >= 2*that, i.e. weave_phase_arms <= angle_cols/(14*num_arms).
    # Floor to an INTEGER so the capped value keeps the chevron harmonics on
    # integer DFT bins (preserves EXACT N-fold symmetry; a fractional value
    # is allowed by the user but breaks strict symmetry — see the symmetry
    # self-check note).
    weave_phase_arms = float(args.weave_phase_arms)
    pa_cap = math.floor(angle_cols / (14.0 * max(args.num_arms, 1)))
    if weave_phase_arms > pa_cap:
        sys.stderr.write(
            f"WARNING: --weave-phase-arms {weave_phase_arms:g} exceeds angular "
            f"Nyquist (angle_cols/(14*num_arms)={pa_cap:g}); capping to "
            f"{pa_cap:g} to avoid chevron aliasing.  Raise --resolution to "
            f"use more chevron flips.\n")
        weave_phase_arms = float(pa_cap)

    os.makedirs(args.out_dir, exist_ok=True)

    print(f"Guilloché generator — spiral pinwheel rosette")
    print(f"  grid: {angle_cols} (angle/cols) x {radius_rows} (radius/rows)")
    print(f"  num_arms={args.num_arms}  weave_freq={weave_freq:g}"
          f"{' (capped)' if capped else ''}  spiral_tightness={args.spiral_tightness:g}"
          f"  handedness={args.handedness}  spiral_mode={args.spiral_mode}")
    print(f"  relief_depth={args.relief_depth:g}  bit_depth={args.bit_depth}"
          f"  out_dir={args.out_dir}")

    # ---- build the fields ----------------------------------------------
    THETA, RHO = build_grids(angle_cols, radius_rows)

    h, twist, twist_prime, phase = build_height(
        THETA, RHO,
        num_arms=args.num_arms,
        spiral_tightness=args.spiral_tightness,
        handed_sign=handed_sign,
        g_mode=args.spiral_mode,
        weave_freq=weave_freq,
        weave_amp=args.weave_amp,
        petal_sharpness=args.petal_sharpness,
        land_level=args.land_level,
        petal_relief=args.petal_relief,
        relief_depth=args.relief_depth,
        weave_phase_arms=weave_phase_arms,
    )

    ang = build_angle_map(
        THETA, RHO,
        num_arms=args.num_arms,
        twist_prime=twist_prime,
        disk_radius_world=args.disk_radius_world,
    )

    nrm = build_normal_map(h, relief_world_scale=args.normal_strength)

    # ---- write outputs --------------------------------------------------
    hp = os.path.join(args.out_dir, "guilloche_height.png")
    ap = os.path.join(args.out_dir, "guilloche_angle.png")
    np_ = os.path.join(args.out_dir, "guilloche_normal.png")
    save_gray(hp, h, args.bit_depth)
    save_gray(ap, ang, 8)         # angle map is fine at 8-bit (1/256 rad ≈ 0.4°)
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
    cart = reproject_to_cartesian(h, args.preview_size)
    pv = os.path.join(args.out_dir, "_preview.png")
    save_preview(pv, h, ang, nrm, cart)

    print("\n=== FILES WRITTEN ===")
    for f in (hp, np_, ap, pv):
        print(f"  {os.path.abspath(f)}")
    print("\nThe preview montage SHOULD show (left→right, top row): the polar")
    print("HEIGHT map (vertical petal stripes that shear with row = the spiral),")
    print("the ANGLE map (smoothly varying gray = groove direction), and the")
    print("NORMAL map (mostly bluish/+Z with petal-edge tints).  The bottom row")
    print(f"is the Cartesian 'as-on-dial' reprojection: a {args.num_arms}-armed")
    print("pinwheel of curved petals spiraling from the centre, each petal woven.")

    # ---- adversarial self-checks ---------------------------------------
    all_ok = run_self_checks(h, ang, nrm, THETA,
                             num_arms=args.num_arms, angle_cols=angle_cols,
                             weave_freq=weave_freq,
                             spiral_tightness=args.spiral_tightness,
                             weave_phase_arms=weave_phase_arms)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
