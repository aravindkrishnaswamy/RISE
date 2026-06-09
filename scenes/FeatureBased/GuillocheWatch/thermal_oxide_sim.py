#!/usr/bin/env python3
"""thermal_oxide_sim.py - torch heat-tint simulator for a titanium watch dial.

Produces an OXIDE-THICKNESS MAP (a TiO2 film thickness field) that the RISE
renderer turns into the heat-tint colour via the Phase-1/2 thin-film machinery.
This tool is the heat->thickness half of the strictly-decoupled pipeline
(docs/THIN_FILM_INTERFERENCE.md Sec.9 "Decoupling rule"): we emit ONLY a
thickness field; thickness->colour is the renderer's job (GGX
`fresnel_mode thinfilm`, `film_thickness` slot).  This separation means a
calibration mismatch here can never masquerade as an optics bug.

REFERENCE LOOK
--------------
A radial gradient on a titanium dial:

    warm GOLD/AMBER centre  ->  magenta/purple  ->  BLUE rim

Titanium temper colours are set by oxide-film thickness.  With INCREASING
thickness the tempering sequence runs roughly:

    silver -> straw/gold (thin) -> gold/amber -> purple/violet
           -> blue -> light-blue (thick)

so GOLD = THINNER oxide and BLUE = THICKER oxide.  The reference has a gold
centre and a blue rim, therefore THICKNESS INCREASES from centre to rim: the
rim ran hotter / was heated longer (a torch concentrating heat at the
perimeter), growing a thicker oxide.

PHYSICAL MODEL (heat -> thickness)
----------------------------------
1. Heat-dose field.  A monotone radial temperature/heat-dose profile T(r),
   lowest at the centre, highest at the rim.  Three falloffs are offered
   (--falloff): linear, quadratic (default; torch dwell concentrates at the
   rim), and smooth (smoothstep, soft shoulders).  We map a normalised radius
   rho = r/R in [0,1] through the falloff to a normalised heat dose
   h(rho) in [0,1], then to an absolute peak temperature
   T(rho) = T_min + h(rho) * (T_max - T_min).

2. Parabolic oxide growth law.  Thermal oxidation of a metal in the
   diffusion-limited regime follows a parabolic rate law

       d(T, t) = sqrt( k(T) * t )

   with an Arrhenius rate constant

       k(T) = A * exp( -Ea / (R * T) )      [ R = 8.314 J/(mol*K) ]

   A (pre-exponential) and Ea (activation energy) and the dwell time t are
   the model knobs.  Their ABSOLUTE values are NOT claimed to be metrology-
   grade for Ti/TiO2 - they are CALIBRATED so the resulting thickness band
   lands the COLOUR sequence gold->purple->blue against this repo's Phase-1
   thin-film swatch (see CALIBRATION).  What matters physically is the SHAPE:
   d is a smooth, monotone-increasing function of T (hence of radius), with
   the sqrt-of-Arrhenius curvature of real diffusion-limited oxide growth.

   To make the *colour endpoints* exactly controllable (the owner wants the
   gold centre and blue rim to be dialable), we additionally AFFINE-RESCALE
   the physical d(rho) profile so that d(rho=0) == d_center and
   d(rho=1) == d_rim.  The Arrhenius+parabolic law sets the *curve shape*
   between the endpoints; the endpoints themselves are the calibrated colour
   anchors.  (Set --raw-growth to skip the rescale and emit the unscaled
   physical thicknesses instead, for inspecting the bare growth law.)

CALIBRATION SOURCE (thickness -> colour)
----------------------------------------
The thickness->colour mapping is taken from THIS REPO's Phase-1 thin-film
oracle, not guessed.  We re-implement, in Python, the exact colorimetry the
C++ validation gate uses:

  * Air/TiO2(d)/Ti reflectance at NORMAL incidence via the single-film Airy
    closed form  R = |(r01 + r1s e^{+2i delta}) / (1 + r01 r1s e^{+2i delta})|^2
    (tests/thinfilm/AiryReference.h), reading the SAME tabulated optical
    constants the oracle reads:
        colors/thinfilm/substrates/Ti.{n,k}
        colors/thinfilm/oxides/TiO2.{n,k}
  * White-normalised D65 reflective colorimetry
        XYZ = sum_lambda R(l) S_D65(l) cmf(l) / sum_lambda S_D65(l) ybar(l)
    with the CIE 1931 2-deg CMFs and D65 SPD embedded verbatim from
    src/.../ColorUtils.cpp (CIE_DATA) and tests/.../OpticalConstants.h
    (D65_DATA) - so this script's swatch matches the renderer's observer.
  * Dominant wavelength / CIELAB hue from the unclamped XYZ, identical method
    to tests/thinfilm/AnodizeSwatch.h, used to classify each thickness into
    a hue family (gold / purple / blue / ...).

From that ladder we pick:
    d_center  -> the GOLD/AMBER band  (warm, dominant ~ 565-625 nm)
    d_rim     -> the BLUE band        (dominant ~ 450-485 nm), in the SAME
                 first interference order as the gold (so the on-dial path
                 gold -> purple -> blue is one monotone sweep, not a wrap-around)
The auto-picked values are printed and written to oxide_calibration.txt;
they can be overridden with --d-center-nm / --d-rim-nm.

OUTPUT CONVENTION (identical to the guilloche generator)
--------------------------------------------------------
A polar-UV image:
    column  <->  angle  theta in [0, 2*pi)   (x = column / width * 2pi)
    row     <->  radius r in [0, R]
    row 0        = dial CENTRE
    last row     = dial RIM
Default 2048 wide (angle, --resolution) x 1024 high (radius,
--radius-resolution, default resolution//2) - the SAME WxH/2 polar-UV layout
the guilloche generator (tools/guilloche_gen.py) emits, so a UV sample hits
the same (theta, r) cell in both the height field and the oxide field.  Set
--radius-resolution equal to the guilloche height map's row count to keep them
pixel-for-pixel aligned.  Because a future non-radial profile may be desired,
the FULL 2D field is baked even though today it is a pure function of the row
(radius).

Files written (into --out-dir, default scenes/FeatureBased/GuillocheWatch/):
  * oxide_thickness.png  - 16-bit grayscale, NORMALISED [0,1] thickness:
        0 -> d_min,  1 -> d_max  (d_min/d_max default to d_center/d_rim).
    The RISE scene recovers nanometres with a tunable affine map:
        scalar_painter { texture ... scale (d_max - d_min) bias d_min }
    so the heat-tint colour range is dialable FROM THE SCENE without
    re-baking (the owner's explicit requirement).
  * oxide_calibration.txt - the recommended scale/bias (nm), d_center/d_rim
    and the colour each produces (from the swatch), for the scene author.

OPTIONAL ridge coupling (--height-map)
--------------------------------------
If a guilloche height map is supplied (same polar-UV layout), raised ridges
run slightly hotter -> locally thicker oxide -> subtly bluer.  We add
    d += coupling * (d_max - d_min) * height_norm
with --coupling defaulting to 0 when no height map is given (the radial
gradient is the dominant, REQUIRED effect) and to a small value when a map
is supplied.  The full 2D bake makes this drop in cleanly.

DEPENDENCIES: numpy + Pillow.  (No scipy: the radial relaxation is analytic.)

Author: RISE thin-film Phase-3 worker (heat->thickness, P3-B)
"""

import argparse
import math
import os
import sys

import numpy as np

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment guard
    sys.stderr.write("ERROR: Pillow (PIL) is required: pip install Pillow\n")
    raise


# ---------------------------------------------------------------------------
#  Repo-relative data paths (this file lives in <repo>/scenes/FeatureBased/GuillocheWatch/).
# ---------------------------------------------------------------------------
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", "..", ".."))  # repo root (shared colors/ data)


def _data_path(*parts):
    return os.path.join(_REPO_ROOT, *parts)


# ===========================================================================
#  PART 1 - Phase-1 thin-film colorimetry (port of the C++ oracle).
#
#  This block reproduces, in Python, exactly what
#  tests/ThinFilmAnodizeSwatchTest.cpp computes for the Ti/TiO2 ladder, so the
#  thickness->colour calibration is read FROM the repo's validated machinery,
#  not invented.  Verified against the C++ conventions:
#    - Airy reflectance, normal incidence (tests/thinfilm/AiryReference.h)
#    - white-normalised D65 XYZ (tests/thinfilm/AnodizeSwatch.h)
#    - CIE 1931 2-deg CMFs + D65 SPD verbatim from the repo sources
# ===========================================================================

# --- CIE 1931 2-deg colour-matching functions, 380..780 nm @ 5 nm ----------
#   Verbatim from src/Library/Utilities/Color/ColorUtils.cpp (CIE_DATA).
#   z_2 is zero-filled past its last listed entry (the C++ array is declared
#   length 81 and C++ zero-initialises the tail) - we pad to match exactly.
_CIE_MIN_NM = 380
_CIE_STEP_NM = 5
_CIE_N = 81  # (780 - 380) / 5 + 1

_CIE_X = [
    0.0014, 0.0022, 0.0042, 0.0077, 0.0143, 0.0232, 0.0435, 0.0776, 0.1344, 0.2148,
    0.2839, 0.3285, 0.3483, 0.3481, 0.3362, 0.3187, 0.2908, 0.2511, 0.1954, 0.1421,
    0.0956, 0.0580, 0.0320, 0.0147, 0.0049, 0.0024, 0.0093, 0.0291, 0.0633, 0.1096,
    0.1655, 0.2257, 0.2904, 0.3597, 0.4334, 0.5121, 0.5945, 0.6784, 0.7621, 0.8425,
    0.9163, 0.9786, 1.0263, 1.0567, 1.0622, 1.0456, 1.0026, 0.9384, 0.8544, 0.7514,
    0.6424, 0.5419, 0.4479, 0.3608, 0.2835, 0.2187, 0.1649, 0.1212, 0.0874, 0.0636,
    0.0468, 0.0329, 0.0227, 0.0158, 0.0114, 0.0081, 0.0058, 0.0041, 0.0029, 0.0020,
    0.0014, 0.0010, 0.0007, 0.0005, 0.0003, 0.0002, 0.0002, 0.0001, 0.0001, 0.0001,
    0.0000,
]
_CIE_Y = [
    0, 0.0001, 0.0001, 0.0002, 0.0004, 0.0006, 0.0012, 0.0022, 0.004, 0.0073,
    0.0116, 0.0168, 0.023, 0.0298, 0.038, 0.048, 0.06, 0.0739, 0.091, 0.1126,
    0.139, 0.1693, 0.208, 0.2586, 0.323, 0.4073, 0.503, 0.6082, 0.71, 0.7932,
    0.862, 0.9149, 0.954, 0.9803, 0.995, 1, 0.995, 0.9786, 0.952, 0.9154,
    0.87, 0.8163, 0.757, 0.6949, 0.631, 0.5668, 0.503, 0.4412, 0.381, 0.321,
    0.265, 0.217, 0.175, 0.1382, 0.107, 0.0816, 0.061, 0.0446, 0.032, 0.0232,
    0.017, 0.0119, 0.0082, 0.0057, 0.0041, 0.0029, 0.0021, 0.0015, 0.001, 0.0007,
    0.0005, 0.0004, 0.0002, 0.0002, 0.0001, 0.0001, 0.0001, 0, 0, 0,
    0,
]
_CIE_Z = [
    0.0065, 0.0105, 0.0201, 0.0362, 0.0679, 0.1102, 0.2074, 0.3713, 0.6456, 1.0391,
    1.3856, 1.623, 1.7471, 1.7826, 1.7721, 1.7441, 1.6692, 1.5281, 1.2876, 1.0419,
    0.813, 0.6162, 0.4652, 0.3533, 0.272, 0.2123, 0.1582, 0.1117, 0.0782, 0.0573,
    0.0422, 0.0298, 0.0203, 0.0134, 0.0087, 0.0057, 0.0039, 0.0027, 0.0021, 0.0018,
    0.0017, 0.0014, 0.0011, 0.001, 0.0008, 0.0006, 0.0003, 0.0002, 0.0002, 0.0001,
    0,
]
# Pad z to length 81 (C++ tail is zero-initialised).
_CIE_Z = _CIE_Z + [0.0] * (_CIE_N - len(_CIE_Z))
assert len(_CIE_X) == _CIE_N and len(_CIE_Y) == _CIE_N and len(_CIE_Z) == _CIE_N

# --- CIE D65 relative SPD, 380..780 nm @ 5 nm, S(560)=100 ------------------
#   Verbatim from tests/thinfilm/OpticalConstants.h (D65_DATA).
_D65 = [
    49.9755, 52.3118, 54.6482, 68.7015, 82.7549, 87.1204, 91.4860,
    92.4589, 93.4318, 90.0570, 86.6823, 95.7736, 104.8650, 110.9360,
    117.0080, 117.4100, 117.8120, 116.3360, 114.8610, 115.3920, 115.9230,
    112.3670, 108.8110, 109.0820, 109.3540, 108.5780, 107.8020, 106.2960,
    104.7900, 106.2390, 107.6890, 106.0470, 104.4050, 104.2250, 104.0460,
    102.0230, 100.0000, 98.1671, 96.3342, 96.0611, 95.7880, 92.2368,
    88.6856, 89.3459, 90.0062, 89.8026, 89.5991, 88.6489, 87.6987,
    85.4936, 83.2886, 83.4939, 83.6992, 81.8630, 80.0268, 80.1207,
    80.2146, 81.2462, 82.2778, 80.2810, 78.2842, 74.0027, 69.7213,
    70.6652, 71.6091, 72.9790, 74.3490, 67.9765, 61.6040, 65.7448,
    69.8856, 72.4863, 75.0870, 69.3398, 63.5927, 55.0054, 46.4182,
    56.6118, 66.8054, 65.0941, 63.3828,
]
assert len(_D65) == _CIE_N

# --- Standard XYZ->linear Rec.709 matrix (D65), for sanity sRGB swatches ----
#   Matches the canonical matrix in src/.../Color.cpp (the commented form).
_XYZ_TO_REC709 = np.array([
    [3.240479, -1.537150, -0.498535],
    [-0.969256, 1.875992, 0.041556],
    [0.055648, -0.204043, 1.057311],
], dtype=np.float64)


def _load_nk_table(base_path):
    """Load a 2-column 'nm value' file (same fscanf loop as the parser).

    Returns (nm_array, value_array) sorted ascending by nm.
    """
    nm_list, val_list = [], []
    with open(base_path, "r") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                nm = float(parts[0])
                v = float(parts[1])
            except ValueError:
                continue
            nm_list.append(nm)
            val_list.append(v)
    if not nm_list:
        raise RuntimeError("no numeric pairs in %s" % base_path)
    nm = np.asarray(nm_list, dtype=np.float64)
    val = np.asarray(val_list, dtype=np.float64)
    order = np.argsort(nm)
    return nm[order], val[order]


def _interp_flat_clamp(nm_query, nm_tab, val_tab):
    """Linear interp with flat-clamp extrapolation (PiecewiseLinearScalar)."""
    return np.interp(nm_query, nm_tab, val_tab,
                     left=val_tab[0], right=val_tab[-1])


class _MaterialNK:
    """Tabulated complex index N = n + i k (k >= 0 absorbing convention)."""

    def __init__(self, base):
        self._nm_n, self._n = _load_nk_table(base + ".n")
        self._nm_k, self._k = _load_nk_table(base + ".k")

    def index_at(self, nm):
        n = _interp_flat_clamp(nm, self._nm_n, self._n)
        k = _interp_flat_clamp(nm, self._nm_k, self._k)
        k = np.maximum(k, 0.0)  # absorbing convention (k >= 0)
        return n + 1j * k


def _airy_reflectance_normal(N_film, d_nm, N_sub, lambda_nm):
    """Unpolarized single-film Airy reflectance at NORMAL incidence.

    At theta=0 the s/p admittances coincide (cos=1 everywhere), so Rs==Rp and
    the unpolarized R is the single scalar below.  This is the theta=0 special
    case of AiryReference.h with N0 = 1 (air):

        r01 = (1 - N1) / (1 + N1)
        r1s = (N1 - Ns) / (N1 + Ns)
        delta = 2*pi*d*N1 / lambda
        r = (r01 + r1s e^{+2i delta}) / (1 + r01 r1s e^{+2i delta})
        R = |r|^2
    """
    N0 = 1.0 + 0j
    r01 = (N0 - N_film) / (N0 + N_film)
    r1s = (N_film - N_sub) / (N_film + N_sub)
    delta = (2.0 * math.pi * d_nm / lambda_nm) * N_film
    phase = np.exp(2j * delta)
    r = (r01 + r1s * phase) / (1.0 + r01 * r1s * phase)
    return np.abs(r) ** 2


class ThinFilmSwatchOracle:
    """Reproduces the Phase-1 Ti/TiO2 thickness->colour ladder in Python."""

    def __init__(self, substrate="Ti", oxide="TiO2"):
        # substrate/oxide name the colors/thinfilm/{substrates,oxides}/<name>.{n,k}
        # tables; defaults reproduce the Ti/TiO2 ladder.  self.ti/self.tio2 +
        # self._N_ti/_N_tio2 keep their names (internal) but hold the chosen pair.
        self.ti = _MaterialNK(_data_path("colors", "thinfilm", "substrates", substrate))
        self.tio2 = _MaterialNK(_data_path("colors", "thinfilm", "oxides", oxide))
        # Integration grid: 1 nm over [380,780] - matches ComputeSwatch defaults.
        self._grid = np.arange(380.0, 780.0 + 1e-9, 1.0)
        cie_nm = np.array(
            [_CIE_MIN_NM + i * _CIE_STEP_NM for i in range(_CIE_N)], dtype=np.float64)
        self._xbar = np.interp(self._grid, cie_nm, _CIE_X, left=0.0, right=0.0)
        self._ybar = np.interp(self._grid, cie_nm, _CIE_Y, left=0.0, right=0.0)
        self._zbar = np.interp(self._grid, cie_nm, _CIE_Z, left=0.0, right=0.0)
        # D65 with flat-clamp at the band edges (matches D65_DATA::SampleAtNM).
        d65_nm = cie_nm
        self._s = np.interp(self._grid, d65_nm, _D65, left=_D65[0], right=_D65[-1])
        # White normaliser denom = sum S * ybar (Delta-lambda = 1 cancels).
        self._denom = float(np.sum(self._s * self._ybar))
        # Pre-sample dispersive indices on the grid.
        self._N_ti = self.ti.index_at(self._grid)
        self._N_tio2 = self.tio2.index_at(self._grid)
        # White point chromaticity (R == 1) for dominant-wavelength reference.
        wX = float(np.sum(self._s * self._xbar)) / self._denom
        wY = float(np.sum(self._s * self._ybar)) / self._denom  # == 1
        wZ = float(np.sum(self._s * self._zbar)) / self._denom
        wsum = wX + wY + wZ
        self._white_xy = (wX / wsum, wY / wsum)
        # Spectral-locus chromaticity (fine grid) for dominant-wavelength scan.
        self._locus_nm = np.arange(360.0, 830.0 + 1e-9, 1.0)
        lx = np.interp(self._locus_nm, cie_nm, _CIE_X, left=0.0, right=0.0)
        ly = np.interp(self._locus_nm, cie_nm, _CIE_Y, left=0.0, right=0.0)
        lz = np.interp(self._locus_nm, cie_nm, _CIE_Z, left=0.0, right=0.0)
        locus_sum = lx + ly + lz
        ok = locus_sum > 0
        self._locus_xy = np.stack(
            [np.where(ok, lx / np.where(ok, locus_sum, 1), 0.0),
             np.where(ok, ly / np.where(ok, locus_sum, 1), 0.0)], axis=1)
        self._locus_ok = ok

    def swatch(self, d_nm):
        """Full colour signature for one TiO2 thickness (nm) on Ti.

        Returns a dict with xyz, chromaticity (x,y), Y, dominant wavelength
        (negative => purple line), CIELAB hue/chroma, linear+sRGB RGB, and a
        coarse hue-family string - mirroring AnodizeSwatch.h / the test ladder.
        """
        R = _airy_reflectance_normal(self._N_tio2, float(d_nm), self._N_ti, self._grid)
        X = float(np.sum(R * self._s * self._xbar)) / self._denom
        Y = float(np.sum(R * self._s * self._ybar)) / self._denom
        Z = float(np.sum(R * self._s * self._zbar)) / self._denom
        s = X + Y + Z
        x = X / s if s > 0 else 0.0
        y = Y / s if s > 0 else 0.0
        dom = self._dominant_wavelength(x, y)
        a_star, b_star, chroma, hue = self._lab_chroma(X, Y, Z)
        lin = _XYZ_TO_REC709 @ np.array([X, Y, Z])
        return {
            "d_nm": float(d_nm),
            "xyz": (X, Y, Z),
            "xy": (x, y),
            "Y": Y,
            "dominant_nm": dom,
            "aStar": a_star,
            "bStar": b_star,
            "chroma": chroma,
            "hue_deg": hue,
            "linRGB": tuple(lin.tolist()),
            "srgb8": _lin_to_srgb8(lin),
            "family": _hue_family(chroma, dom),
        }

    def _lab_chroma(self, X, Y, Z):
        # CIELAB a*, b*, C*, hue referenced to the matching D65 white.
        # White point in XYZ with Y==1 normalisation (R==1):
        wX = float(np.sum(self._s * self._xbar)) / self._denom
        wY = 1.0
        wZ = float(np.sum(self._s * self._zbar)) / self._denom

        def f(t):
            d = 6.0 / 29.0
            return t ** (1.0 / 3.0) if t > d * d * d else t / (3.0 * d * d) + 4.0 / 29.0

        fx = f(X / wX) if wX > 0 else 0.0
        fy = f(Y / wY) if wY > 0 else 0.0
        fz = f(Z / wZ) if wZ > 0 else 0.0
        a_star = 500.0 * (fx - fy)
        b_star = 200.0 * (fy - fz)
        chroma = math.sqrt(a_star * a_star + b_star * b_star)
        hue = math.degrees(math.atan2(b_star, a_star))
        if hue < 0:
            hue += 360.0
        return a_star, b_star, chroma, hue

    def _dominant_wavelength(self, cx, cy):
        # Ray from white through colour; find spectral-locus crossing.
        # Mirrors AnodizeSwatch.h detail::DominantWavelength.  Returns a
        # negative complementary wavelength when the colour is on the purple
        # line (non-spectral) - the test's "purple" flag.
        wx, wy = self._white_xy
        dx, dy = cx - wx, cy - wy
        length = math.hypot(dx, dy)
        if length < 1e-7:
            return -1.0
        ux, uy = dx / length, dy / length
        best_dom, best_proj = -1.0, -1e30
        best_comp, best_comp_proj = -1.0, -1e30
        xy = self._locus_xy
        ok = self._locus_ok
        for i in range(1, len(self._locus_nm)):
            if not (ok[i] and ok[i - 1]):
                continue
            px, py = xy[i - 1, 0], xy[i - 1, 1]
            sx, sy = xy[i, 0] - px, xy[i, 1] - py
            denom = ux * sy - uy * sx
            if abs(denom) <= 1e-12:
                continue
            rx, ry = px - wx, py - wy
            t = (rx * sy - ry * sx) / denom
            q = (rx * uy - ry * ux) / denom
            if -1e-6 <= q <= 1.0 + 1e-6:
                nm_hit = self._locus_nm[i - 1] + q * (self._locus_nm[i] - self._locus_nm[i - 1])
                if t > 0.0:
                    if t > best_proj:
                        best_proj, best_dom = t, nm_hit
                elif t < 0.0:
                    if -t > best_comp_proj:
                        best_comp_proj, best_comp = -t, nm_hit
        if best_dom > 0.0:
            return best_dom
        if best_comp > 0.0:
            return -best_comp
        return -1.0


def _hue_family(chroma, dom):
    # Same coarse buckets as ThinFilmAnodizeSwatchTest.cpp HueFamily().
    if chroma < 6.0:
        return "neutral/grey-metal"
    if dom < 0.0:
        return "purple/magenta (non-spectral)"
    if dom < 480.0:
        return "blue"
    if dom < 490.0:
        return "blue/cyan"
    if dom < 510.0:
        return "cyan/teal"
    if dom < 560.0:
        return "green"
    if dom < 575.0:
        return "yellow-green"
    if dom < 585.0:
        return "yellow/gold"
    if dom < 600.0:
        return "gold/orange"
    if dom < 620.0:
        return "orange/bronze"
    return "red/bronze"


def _lin_to_srgb8(lin):
    out = []
    for c in lin:
        c = 0.0 if c < 0.0 else (1.0 if c > 1.0 else c)
        # sRGB OETF.
        s = 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055
        out.append(int(s * 255.0))
    return tuple(out)


# ===========================================================================
#  PART 2 - Auto-calibration: pick d_center (gold) and d_rim (blue).
# ===========================================================================

def auto_calibrate(oracle, verbose=True):
    """Scan the Ti/TiO2 ladder and pick gold (centre) and blue (rim) anchors.

    Returns (d_center, d_rim, ladder, gold_swatch, blue_swatch).
    Strategy (matches the test's FIRST-order progression gold->purple->blue):
      * Sweep d = 1..250 nm at 1 nm.
      * Segment the ladder into contiguous warm / purple / blue bands by
        dominant wavelength + chroma, and lock onto the FIRST of each in the
        canonical order: first warm band -> first purple after it -> first blue
        after that purple.  This deliberately ignores the globally most
        saturated band (the 2nd-order yellow-green at d~100 nm is more chromatic
        than the 1st-order gold at d~20 nm) so the anchors stay in ONE
        interference order and the on-dial sweep is a single monotone
        gold->purple->blue, not a wrap-around.
      * GOLD anchor (centre) = PEAK chroma within the first warm band.
      * BLUE anchor (rim)    = PEAK chroma within the first blue band after
        the first purple.
    """
    ladder = [oracle.swatch(d) for d in np.arange(1.0, 250.0 + 1e-9, 1.0)]

    # We must lock onto the FIRST interference order, not the globally most
    # saturated band.  (The 2nd-order yellow-green at d~100 nm is more chromatic
    # than the 1st-order gold at d~20 nm, so a global-peak search would wrongly
    # anchor there and skip the canonical straw->gold->purple->blue sequence.)
    # The robust structural fingerprint is the ORDER of first appearance:
    #   straw/gold (warm)  ->  purple (non-spectral)  ->  blue.
    # Walk the ladder once, segment into contiguous "warm" / "purple" / "blue"
    # bands, and take the FIRST of each, in that order.

    def is_warm(sw):
        # Warm = spectral dominant in the gold/orange/bronze range.
        dom = sw["dominant_nm"]
        return dom > 0.0 and 560.0 <= dom <= 640.0 and sw["chroma"] >= 8.0

    def is_purple(sw):
        return sw["dominant_nm"] < 0.0 and sw["chroma"] >= 10.0

    def is_blue(sw):
        dom = sw["dominant_nm"]
        return dom > 0.0 and 445.0 <= dom <= 492.0 and sw["chroma"] >= 10.0

    def first_band_peak(predicate, d_after, d_before=1e30):
        """Peak-chroma swatch within the FIRST contiguous run matching
        `predicate` strictly after d_after (and before d_before).  Returns
        (peak_swatch, run_end_d) or (None, d_after)."""
        in_run = False
        best = None
        run_end = d_after
        for sw in ladder:
            if sw["d_nm"] <= d_after or sw["d_nm"] >= d_before:
                if in_run:
                    break  # left the window after a run started
                continue
            if predicate(sw):
                in_run = True
                run_end = sw["d_nm"]
                if best is None or sw["chroma"] > best["chroma"]:
                    best = sw
            elif in_run:
                break  # first contiguous run ended
        return best, run_end

    # 1st-order gold: first warm band from the bare-metal edge.
    gold, gold_end = first_band_peak(is_warm, d_after=0.0)
    # 1st purple band after the gold (the gold->purple transition).
    purple, purple_end = first_band_peak(is_purple, d_after=gold_end if gold else 0.0)
    # 1st blue band after that purple (one monotone gold->purple->blue sweep).
    blue, _ = first_band_peak(is_blue, d_after=purple_end if purple else (gold_end if gold else 0.0))

    if gold is None or blue is None:
        raise RuntimeError("auto-calibration failed to locate gold/blue bands")

    if verbose:
        print("[calib] Ti/TiO2 anodize ladder (Phase-1 oracle, normal incidence, D65):")
        # Print a sparse ladder for human eyeballing.
        for sw in ladder:
            if int(round(sw["d_nm"])) % 5 == 0:
                dom = sw["dominant_nm"]
                domstr = ("%6.1f nm" % dom) if dom > 0 else ("purp(%.0f)" % -dom if dom < 0 else " achrom")
                print("   d=%6.1f | %s | hue=%6.1f | C*=%6.2f | sRGB%s | %s"
                      % (sw["d_nm"], domstr, sw["hue_deg"], sw["chroma"],
                         sw["srgb8"], sw["family"]))
    return gold["d_nm"], blue["d_nm"], ladder, gold, blue


# ===========================================================================
#  PART 3 - Heat -> thickness radial model.
# ===========================================================================

# Arrhenius / parabolic-growth constants.  DOCUMENTED, calibration-grade (not
# metrology): they set the CURVE SHAPE; the endpoints are pinned to the
# colour anchors by the affine rescale in build_thickness_profile.
_GAS_CONSTANT = 8.314          # J / (mol K)
_ARRHENIUS_A = 5.0e4           # nm^2 / s  (pre-exponential, parabolic rate)
_ACTIVATION_EA = 9.0e4         # J / mol   (~90 kJ/mol, typical oxide regime)
_DWELL_TIME_S = 30.0           # s         (torch dwell)
_T_MIN_K = 700.0               # K  centre temperature (~427 C, light straw)
_T_MAX_K = 900.0               # K  rim temperature   (~627 C, blue regime)

# Per-metal parabolic-oxidation activation energy Ea (J/mol).  Ea sets the radial
# dose->thickness SHAPE (curvature): higher Ea => growth concentrates at the hot
# rim (larger thin/gold centre, sharper rim transition); lower Ea => more gradual.
# REPRESENTATIVE literature values (parabolic regime; real oxidation kinetics are
# temperature/regime dependent, so these are calibration-grade, not metrology):
#   Ti  ~155-171 kJ/mol (TiO2 parabolic growth; ScienceDirect / Dergipark)
#   Fe  ~164 kJ/mol     (Young, High-Temp Oxidation, 700-1000 C; via npj Mat.Deg.)
#   Nb  ~134-174 kJ/mol (Nb redox-bracketed; UCR J.Appl.Phys.)
#   Ta  ~65 kJ/mol      (thin-oxide Deal-Grove; UCR J.Appl.Phys. -- lowest, so the
#                        most gradual radial profile of the four)
METAL_KINETICS = {             # J/mol
    "Ti":    160.0e3,
    "Nb":    135.0e3,
    "Ta":     80.0e3,
    "Steel": 165.0e3,
}


def _falloff(rho, mode):
    """Map normalised radius rho in [0,1] -> normalised heat dose in [0,1].

    Monotone increasing (centre cool -> rim hot) for every mode.
    """
    rho = np.clip(rho, 0.0, 1.0)
    if mode == "linear":
        return rho
    if mode == "quadratic":
        return rho * rho
    if mode == "smooth":
        # Smoothstep 3rho^2 - 2rho^3 (C1, soft shoulders at both ends).
        return rho * rho * (3.0 - 2.0 * rho)
    raise ValueError("unknown falloff %r" % mode)


def _parabolic_oxide_nm(T_K, Ea=_ACTIVATION_EA):
    """Parabolic-law oxide thickness d = sqrt(k(T) t), Arrhenius k(T).

    Ea (J/mol) is the parabolic-oxidation activation energy and sets the radial
    curve SHAPE (see METAL_KINETICS).  The pre-exponential cancels in the
    normalised profile, so Ea alone drives the per-metal shape difference.
    """
    k = _ARRHENIUS_A * np.exp(-Ea / (_GAS_CONSTANT * T_K))
    return np.sqrt(np.maximum(k * _DWELL_TIME_S, 0.0))


def build_thickness_profile(n_rows, falloff, d_center, d_rim, raw_growth=False, Ea=_ACTIVATION_EA):
    """Per-row (radius) absolute thickness profile in nm.

    The physical Arrhenius+parabolic law sets the curve SHAPE across radius;
    the endpoints are then affine-rescaled to (d_center, d_rim) so the colour
    anchors are exactly hit (unless raw_growth=True).
    """
    rho = np.linspace(0.0, 1.0, n_rows)            # row0=centre .. last=rim
    heat = _falloff(rho, falloff)                  # [0,1] dose
    T = _T_MIN_K + heat * (_T_MAX_K - _T_MIN_K)    # absolute K
    d_phys = _parabolic_oxide_nm(T, Ea)            # physical nm (curve shape; Ea-driven)

    if raw_growth:
        return d_phys, T, d_phys.copy()

    # Affine rescale the physical curve so endpoints hit the colour anchors.
    lo, hi = d_phys[0], d_phys[-1]
    span = hi - lo
    if abs(span) < 1e-12:
        # Degenerate (flat T): fall back to a direct falloff interpolation.
        shaped = d_center + heat * (d_rim - d_center)
    else:
        t = (d_phys - lo) / span                   # [0,1] preserving curvature
        shaped = d_center + t * (d_rim - d_center)
    return shaped, T, d_phys


def apply_torch_pattern(base01, pattern01, amount):
    """Non-uniform torch: simulate the artist holding the torch LONGER (amount > 0,
    thicker oxide) or SHORTER (amount < 0) along a spatial PATTERN, on top of a
    base normalized thickness field.

      base01, pattern01 : float arrays in [0,1], same shape.  pattern01 marks
                          where the extra/reduced dwell lands (1 = full effect).
      amount            : signed dwell delta in NORMALIZED thickness units.  The
                          scene's oxide_thk scale/bias still maps the result to
                          nm, so e.g. amount=+0.40 with scale=16 shifts the
                          patterned region ~6.4 nm thicker -> a distinct
                          interference colour.

    Returns the clipped [0,1] field.
    """
    return np.clip(base01 + amount * pattern01, 0.0, 1.0)


# ===========================================================================
#  PART 4 - Polar-UV bake + outputs.
# ===========================================================================

def _load_height_map(path, width, height):
    """Load a guilloche height map and return a [0,1] float field (H,W).

    Resized to (height,width) if needed.  Multi-channel -> luminance.
    """
    img = Image.open(path)
    if img.mode not in ("L", "I", "I;16"):
        img = img.convert("L")
    if img.size != (width, height):
        img = img.resize((width, height), Image.BILINEAR)
    arr = np.asarray(img, dtype=np.float64)
    mx = arr.max()
    if mx > 255.0:        # 16-bit
        arr = arr / 65535.0
    else:
        arr = arr / 255.0
    return arr


def bake_polar_uv(profile_nm, width, height, d_min, d_max,
                  height_field=None, coupling=0.0):
    """Build the full 2D thickness field (nm) and its normalised [0,1] map.

    Layout: column<->angle, row<->radius, row0=centre, last row=rim.  The
    field is a pure function of row today (radial), broadcast across columns,
    plus the optional ridge coupling term.
    """
    # Broadcast the per-row radial profile across all columns.
    field_nm = np.repeat(profile_nm[:, None], width, axis=1)  # (H, W)

    if height_field is not None and coupling != 0.0:
        # Ridges run hotter -> thicker -> bluer.  Add coupling * span * height.
        field_nm = field_nm + coupling * (d_max - d_min) * height_field

    # Normalise to [0,1] against [d_min, d_max] so the scene's scale/bias
    # recovers nm.  Clip so the optional coupling can't push outside [0,1].
    span = d_max - d_min
    if abs(span) < 1e-12:
        norm = np.zeros_like(field_nm)
    else:
        norm = (field_nm - d_min) / span
    norm = np.clip(norm, 0.0, 1.0)
    return field_nm, norm


def write_thickness_png(norm_field, out_path):
    """Write the normalised [0,1] field as a 16-bit grayscale PNG.

    16-bit (I;16) gives ~span/65535 nm precision over the [d_min,d_max] range
    - far finer than the renderer needs and immune to 8-bit banding across the
    smooth gradient.  We build the image via frombytes (little-endian uint16)
    rather than fromarray(mode=...) to avoid Pillow's deprecated mode kwarg.
    """
    u16 = np.clip(np.rint(norm_field * 65535.0), 0, 65535).astype("<u2")
    h, w = u16.shape
    img = Image.frombytes("I;16", (w, h), u16.tobytes())
    img.save(out_path)


def write_calibration_txt(out_path, d_center, d_rim, d_min, d_max, scale, bias,
                          gold_sw, blue_sw, falloff, width, height, coupling,
                          height_map):
    lines = []
    lines.append("# RISE thin-film heat-tint calibration (tools/thermal_oxide_sim.py)")
    lines.append("# Titanium dial, oxide thickness -> colour via Phase-1 thin-film oracle")
    lines.append("# (Ti/TiO2, normal incidence, D65 white-normalised colorimetry).")
    lines.append("#")
    lines.append("# COLOUR ANCHORS (read from the Phase-1 Ti/TiO2 swatch ladder):")
    lines.append("#   d_center = %.1f nm  ->  %s  (dominant %s, C*=%.1f)  [dial CENTRE]"
                 % (d_center, gold_sw["family"], _domstr(gold_sw["dominant_nm"]),
                    gold_sw["chroma"]))
    lines.append("#   d_rim    = %.1f nm  ->  %s  (dominant %s, C*=%.1f)  [dial RIM]"
                 % (d_rim, blue_sw["family"], _domstr(blue_sw["dominant_nm"]),
                    blue_sw["chroma"]))
    lines.append("#")
    lines.append("# On-dial sweep centre->rim: gold/amber -> magenta/purple -> blue")
    lines.append("# (thickness INCREASES outward; rim ran hotter -> thicker oxide).")
    lines.append("#")
    lines.append("# NORMALISED MAP RANGE (oxide_thickness.png 0..1 maps to):")
    lines.append("#   d_min (pixel 0)   = %.1f nm" % d_min)
    lines.append("#   d_max (pixel 1)   = %.1f nm" % d_max)
    lines.append("#")
    lines.append("# SCENE RECIPE - recover nanometres from the normalised texture with a")
    lines.append("# tunable affine map (colour range dialable WITHOUT re-baking):")
    lines.append("#")
    lines.append("#   scalar_painter { texture \"scenes/FeatureBased/GuillocheWatch/oxide_thickness.png\" scale %.4f bias %.4f }"
                 % (scale, bias))
    lines.append("#")
    lines.append("# i.e.  thickness_nm = pixel01 * scale + bias")
    lines.append("#   scale = d_max - d_min = %.4f" % scale)
    lines.append("#   bias  = d_min         = %.4f" % bias)
    lines.append("#")
    lines.append("# To shift the whole heat-tint warmer/cooler from the scene: lower 'bias'")
    lines.append("# for a thinner (warmer/goldier) overall film, raise it for thicker (bluer);")
    lines.append("# widen 'scale' for a longer gold->blue sweep, narrow it to compress.")
    lines.append("#")
    lines.append("# NOTE on the thickness RANGE (20-33 nm): these are the FIRST-order")
    lines.append("# titanium temper colours (straw/gold ~20 nm, purple ~30 nm, blue ~33 nm).")
    lines.append("# We deliberately use first order so the centre->rim sweep is a SINGLE")
    lines.append("# clean gold->purple->blue (the reference look).  A 'couple hundred nm'")
    lines.append("# blue exists too (2nd-order blue ~140 nm, 3rd ~250 nm) but reaching it")
    lines.append("# from a 20 nm gold centre would wrap the dial through the full spectrum")
    lines.append("# twice (gold->blue->green->gold->purple->blue), not the reference's")
    lines.append("# monotone gold->magenta->blue.  To use a wider/2nd-order range instead,")
    lines.append("# re-bake with e.g. --d-center-nm 20 --d-rim-nm 140 (purple~125, blue~140),")
    lines.append("# or override scale/bias here in the scene (no re-bake needed).")
    lines.append("#")
    lines.append("# Bake parameters:")
    lines.append("#   falloff    = %s" % falloff)
    lines.append("#   resolution = %d x %d  (W=angle theta in [0,2pi), H=radius, row0=centre)"
                 % (width, height))
    lines.append("#   coupling   = %.4f%s"
                 % (coupling, ("  (height map: %s)" % height_map) if height_map else "  (no height map)"))
    lines.append("")
    # Machine-readable key=value block (easy for the controller to parse).
    lines.append("d_center_nm = %.4f" % d_center)
    lines.append("d_rim_nm = %.4f" % d_rim)
    lines.append("d_min_nm = %.4f" % d_min)
    lines.append("d_max_nm = %.4f" % d_max)
    lines.append("scale = %.6f" % scale)
    lines.append("bias = %.6f" % bias)
    lines.append("")
    with open(out_path, "w") as fh:
        fh.write("\n".join(lines))


def _domstr(dom):
    if dom > 0:
        return "%.1f nm" % dom
    if dom < 0:
        return "purple line (complement %.0f nm)" % -dom
    return "achromatic"


# ===========================================================================
#  PART 5 - Adversarial self-review (numeric invariants).
# ===========================================================================

def self_review(profile_nm, norm_field, field_nm, d_min, d_max, scale, bias,
                gold_sw, blue_sw, d_center, d_rim, oracle):
    print("\n[self-review] adversarial invariant checks:")
    ok = True

    # (a) Monotone increasing centre->rim, smooth, finite.
    diffs = np.diff(profile_nm)
    mono = bool(np.all(diffs >= -1e-9))
    finite = bool(np.all(np.isfinite(profile_nm)))
    print("   (a) thickness monotone increasing centre->rim: %s "
          "(min step %.4f nm, max step %.4f nm)"
          % ("PASS" if mono else "FAIL", float(diffs.min()), float(diffs.max())))
    print("       thickness finite (no NaN/Inf): %s" % ("PASS" if finite else "FAIL"))
    ok = ok and mono and finite

    # (b) The chosen d_center/d_rim map to gold/blue per the oracle.
    g_fam, b_fam = gold_sw["family"], blue_sw["family"]
    gold_ok = ("gold" in g_fam or "yellow" in g_fam or "orange" in g_fam or "bronze" in g_fam)
    blue_ok = ("blue" in b_fam)
    print("   (b) d_center=%.1f nm -> %s : %s" % (d_center, g_fam,
          "PASS (warm)" if gold_ok else "FAIL"))
    print("       d_rim   =%.1f nm -> %s : %s" % (d_rim, b_fam,
          "PASS (blue)" if blue_ok else "FAIL"))
    ok = ok and gold_ok and blue_ok

    # (c) Normalised map spans [0,1] and scale/bias recovers nm.
    nmin, nmax = float(norm_field.min()), float(norm_field.max())
    spans01 = (nmin >= -1e-6) and (nmax <= 1.0 + 1e-6) and (nmin < 0.02) and (nmax > 0.98)
    recovered = norm_field * scale + bias
    # Compare against the (clipped-to-range) field the PNG represents.
    field_clipped = np.clip(field_nm, d_min, d_max)
    max_err = float(np.max(np.abs(recovered - field_clipped)))
    recover_ok = max_err < 1e-2  # nm; 16-bit quantisation is ~span/65535
    print("   (c) normalised map range [%.4f, %.4f] spans [0,1]: %s"
          % (nmin, nmax, "PASS" if spans01 else "FAIL"))
    print("       scale*pixel+bias recovers nm field (max err %.4e nm): %s"
          % (max_err, "PASS" if recover_ok else "FAIL"))
    ok = ok and spans01 and recover_ok

    # (d) Layout: row0=centre(thin/gold), last row=rim(thick/blue).
    row0_thin = profile_nm[0] <= profile_nm[-1]
    print("   (d) layout row0=centre(d=%.1f nm) < last row=rim(d=%.1f nm): %s"
          % (profile_nm[0], profile_nm[-1], "PASS" if row0_thin else "FAIL"))
    # Confirm the endpoints' colours directly from the oracle.
    c0 = oracle.swatch(profile_nm[0])
    c1 = oracle.swatch(profile_nm[-1])
    print("       row0 colour: %s ; last-row colour: %s" % (c0["family"], c1["family"]))
    ok = ok and row0_thin

    print("   [self-review] %s" % ("ALL PASS" if ok else "FAILURES PRESENT"))
    return ok


# ===========================================================================
#  PART 6 - Colour-band reporting.
# ===========================================================================

def report_color_bands(profile_nm, oracle, n_rows):
    """Print the per-colour radial bands (which rows show which hue family)."""
    print("\n[bands] per-colour radial bands (row -> radius rho -> family):")
    prev_fam = None
    band_start = 0
    bands = []
    for row in range(n_rows):
        sw = oracle.swatch(profile_nm[row])
        fam = sw["family"]
        if fam != prev_fam:
            if prev_fam is not None:
                bands.append((band_start, row - 1, prev_fam))
            band_start = row
            prev_fam = fam
    bands.append((band_start, n_rows - 1, prev_fam))
    for (r0, r1, fam) in bands:
        rho0, rho1 = r0 / (n_rows - 1), r1 / (n_rows - 1)
        d0, d1 = profile_nm[r0], profile_nm[r1]
        print("   rows %4d..%4d  rho %.3f..%.3f  d %.1f..%.1f nm  -> %s"
              % (r0, r1, rho0, rho1, d0, d1, fam))


# ===========================================================================
#  Per-base-metal temper data (the data behind the tf_dial_<metal> materials).
# ===========================================================================

# Each metal's oxide reaches a DIFFERENT nm window for a comparable temper sweep,
# because the oxide n,k differ.  These windows are the scale/bias on the scene's
# oxide_thk_<metal> painters; the colours are computed rigorously in-renderer
# from each metal's substrate + oxide n,k.  Run with --metal-ladders to inspect.
METAL_LADDERS = [
    ("Ti",    "TiO2",  (22, 38), "gold -> blue"),
    ("Nb",    "Nb2O5", (30, 55), "gold/orange -> blue (Nb2O5 lower index => thicker)"),
    ("Ta",    "Ta2O5", (26, 52), "bronze -> gold -> purple (Ta2O5 has no clean blue)"),
    ("Steel", "Fe3O4", (28, 56), "gold -> purple -> blue (classic steel temper)"),
]


def print_metal_ladders():
    """Print each base metal's thickness->colour ladder + its temper window.

    This is the rigorous physics behind the scene's per-metal materials: the
    Airy single-film reflectance (each metal's substrate + oxide n,k) integrated
    against CIE/D65.  The recommended window is the scale/bias on oxide_thk_<m>.
    """
    for sub, ox, (lo, hi), note in METAL_LADDERS:
        orc = ThinFilmSwatchOracle(sub, ox)
        print("\n=== %-5s / %-5s   window %d..%d nm   (%s) ===" % (sub, ox, lo, hi, note))
        for d in range(10, 131, 6):
            sw = orc.swatch(float(d))
            tag = "  <-- centre" if abs(d - lo) <= 3 else ("  <-- rim" if abs(d - hi) <= 3 else "")
            print("    d=%3d nm   %-28s dom=%6.1f%s" % (d, sw["family"], sw["dominant_nm"], tag))


# ===========================================================================
#  Main.
# ===========================================================================

def main():
    ap = argparse.ArgumentParser(
        description="Torch heat-tint -> TiO2 oxide-thickness map for a Ti watch dial "
                    "(polar-UV; gold centre -> blue rim).")
    ap.add_argument("--d-center-nm", type=float, default=None,
                    help="oxide thickness (nm) at the dial CENTRE (gold/amber). "
                         "Default: auto-calibrated from the Phase-1 Ti/TiO2 swatch.")
    ap.add_argument("--d-rim-nm", type=float, default=None,
                    help="oxide thickness (nm) at the dial RIM (blue). "
                         "Default: auto-calibrated from the Phase-1 Ti/TiO2 swatch.")
    ap.add_argument("--falloff", choices=["linear", "quadratic", "smooth"],
                    default="quadratic",
                    help="radial heat-dose falloff (default: quadratic - torch "
                         "dwell concentrates at the rim).")
    ap.add_argument("--resolution", type=int, default=2048,
                    help="output WIDTH in px (angle axis, col=theta). Default 2048 "
                         "- matches the guilloche generator's --resolution.")
    ap.add_argument("--radius-resolution", type=int, default=None,
                    help="output HEIGHT in px (radius axis, row=r, row0=centre). "
                         "Default resolution//2 (1024 at the 2048 default) - matches "
                         "the guilloche generator's WxH/2 polar-UV layout. Set equal "
                         "to the guilloche height map's row count so the two maps "
                         "sample pixel-for-pixel.")
    ap.add_argument("--out-dir", default=_THIS_DIR,
                    help="output directory (default: scenes/FeatureBased/GuillocheWatch/).")
    ap.add_argument("--height-map", default=None,
                    help="optional guilloche height map (same polar-UV layout); "
                         "enables ridge->thickness coupling.")
    ap.add_argument("--coupling", type=float, default=None,
                    help="ridge->thickness coupling, as a fraction of the d span "
                         "(default 0 with no height map, 0.08 with one).")
    ap.add_argument("--d-min-nm", type=float, default=None,
                    help="nm mapped to normalised 0 (default: d_center).")
    ap.add_argument("--d-max-nm", type=float, default=None,
                    help="nm mapped to normalised 1 (default: d_rim).")
    ap.add_argument("--raw-growth", action="store_true",
                    help="emit the unscaled physical Arrhenius+parabolic thickness "
                         "(skip the endpoint affine rescale to the colour anchors).")
    ap.add_argument("--metal-ladders", action="store_true",
                    help="print each base metal's thickness->colour ladder + temper window, then "
                         "exit (the physics behind the scene's tf_dial_<metal> materials).")
    args = ap.parse_args()

    if args.metal_ladders:
        print_metal_ladders()
        return 0

    if args.resolution < 8:
        ap.error("--resolution must be >= 8")

    # Polar-UV layout matching the guilloche generator: WIDTH = angle axis
    # (--resolution), HEIGHT = radius axis (--radius-resolution, default
    # resolution//2).  The two maps MUST share these dims so a UV sample hits
    # the same (theta, r) cell in both the height field and the oxide field.
    width = int(args.resolution)
    height = int(args.radius_resolution) if args.radius_resolution is not None \
        else max(8, width // 2)
    if height < 8:
        ap.error("--radius-resolution must be >= 8")

    # --- Calibrate thickness->colour from the Phase-1 oracle ---------------
    oracle = ThinFilmSwatchOracle()
    auto_center, auto_rim, _ladder, gold_sw, blue_sw = auto_calibrate(oracle)

    d_center = args.d_center_nm if args.d_center_nm is not None else auto_center
    d_rim = args.d_rim_nm if args.d_rim_nm is not None else auto_rim
    # If the user overrode an anchor, re-evaluate its swatch for the report.
    if args.d_center_nm is not None:
        gold_sw = oracle.swatch(d_center)
    if args.d_rim_nm is not None:
        blue_sw = oracle.swatch(d_rim)

    print("\n[calib] CHOSEN colour anchors:")
    print("   d_center = %.1f nm -> %s (dominant %s, C*=%.1f)  [CENTRE]"
          % (d_center, gold_sw["family"], _domstr(gold_sw["dominant_nm"]), gold_sw["chroma"]))
    print("   d_rim    = %.1f nm -> %s (dominant %s, C*=%.1f)  [RIM]"
          % (d_rim, blue_sw["family"], _domstr(blue_sw["dominant_nm"]), blue_sw["chroma"]))

    # --- Heat -> thickness radial profile ----------------------------------
    profile_nm, T_profile, d_phys = build_thickness_profile(
        height, args.falloff, d_center, d_rim, raw_growth=args.raw_growth)

    # --- Normalised map range (scene-tunable) ------------------------------
    d_min = args.d_min_nm if args.d_min_nm is not None else min(d_center, d_rim)
    d_max = args.d_max_nm if args.d_max_nm is not None else max(d_center, d_rim)
    if d_max <= d_min:
        ap.error("d_max (%.3f) must exceed d_min (%.3f)" % (d_max, d_min))

    # --- Optional ridge coupling -------------------------------------------
    coupling = args.coupling
    if coupling is None:
        coupling = 0.08 if args.height_map else 0.0
    height_field = None
    if args.height_map:
        hm_path = args.height_map
        if not os.path.isabs(hm_path):
            hm_path = os.path.join(_THIS_DIR, hm_path)
        if not os.path.exists(hm_path):
            ap.error("height map not found: %s" % hm_path)
        height_field = _load_height_map(hm_path, width, height)
        print("[height] loaded ridge coupling map %s (coupling %.3f of span)"
              % (args.height_map, coupling))

    # --- Bake the polar-UV field -------------------------------------------
    field_nm, norm_field = bake_polar_uv(
        profile_nm, width, height, d_min, d_max,
        height_field=height_field, coupling=coupling)

    # --- Write outputs -----------------------------------------------------
    out_dir = args.out_dir
    if not os.path.isabs(out_dir):
        out_dir = os.path.join(_THIS_DIR, out_dir)
    os.makedirs(out_dir, exist_ok=True)
    png_path = os.path.join(out_dir, "oxide_thickness.png")
    txt_path = os.path.join(out_dir, "oxide_calibration.txt")

    write_thickness_png(norm_field, png_path)

    scale = d_max - d_min
    bias = d_min
    write_calibration_txt(txt_path, d_center, d_rim, d_min, d_max, scale, bias,
                          gold_sw, blue_sw, args.falloff, width, height,
                          coupling, args.height_map)

    # --- Stats -------------------------------------------------------------
    print("\n[stats] thickness field (nm):")
    print("   min  = %.2f nm" % float(field_nm.min()))
    print("   max  = %.2f nm" % float(field_nm.max()))
    print("   mean = %.2f nm" % float(field_nm.mean()))
    print("   temperature profile: centre %.0f K (%.0f C) -> rim %.0f K (%.0f C)"
          % (T_profile[0], T_profile[0] - 273.15, T_profile[-1], T_profile[-1] - 273.15))

    report_color_bands(profile_nm, oracle, height)

    # --- Self-review -------------------------------------------------------
    self_review(profile_nm, norm_field, field_nm, d_min, d_max, scale, bias,
                gold_sw, blue_sw, d_center, d_rim, oracle)

    # --- Recipe echo -------------------------------------------------------
    print("\n[scene] RISE material recipe (paste into the dial's ggx_material):")
    print("   scalar_painter { texture \"scenes/FeatureBased/GuillocheWatch/oxide_thickness.png\" "
          "scale %.4f bias %.4f }" % (scale, bias))
    print("   -> thickness_nm = pixel01 * %.4f + %.4f  (d_min=%.1f, d_max=%.1f nm)"
          % (scale, bias, d_min, d_max))
    print("\n[out] wrote:")
    print("   %s" % png_path)
    print("   %s" % txt_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
