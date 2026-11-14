"""bpy-free numeric helpers for Principled BSDF features the RISE bridge
translates (ABI v18).

Kept free of `bpy` (like `hair_material_math.py`) so the conversions are
unit-tested by `test_hair_export.py` with a plain Python interpreter.  The
wavelength-dependent constructions (the Cauchy dispersion curve, the
per-wavelength metal substrate under a thin film) live in the native
bridge, `src/Blender/native/rise_blender_bridge.cpp`, and are tested by
`tests/BlenderBridgeThinFilmDispersionTest.cpp`.
"""

from __future__ import annotations

import math

# Cycles ignores films at or below this thickness (nm):
# THINFILM_THICKNESS_CUTOFF in intern/cycles/kernel/svm/types.h.
THIN_FILM_THICKNESS_CUTOFF_NM = 0.1
# Cycles' ior_from_F0 (kernel/closure/bsdf_util.h) clamps F0 to 0.99.
_IOR_FROM_F0_MAX = 0.99


def effective_abbe_number(abbe_number: float, dispersion_scale: float) -> float:
    """Blender 5.3 / OpenPBR 1.1.1 transmission dispersion as the effective
    Abbe number ``V_d = abbe / scale``; 0.0 means "no dispersion".

    Scale 0 (the socket default) is V_d = infinity, i.e. no dispersion; a
    non-positive or non-finite input also disables it.  The native bridge's
    ``dispersion_abbe`` field takes this value (<= 0 means none)."""

    abbe = float(abbe_number)
    scale = float(dispersion_scale)
    if not (math.isfinite(abbe) and math.isfinite(scale)):
        return 0.0
    if scale <= 1.0e-6 or abbe <= 0.0:
        return 0.0
    return abbe / scale


def dielectric_film_substrate(ior: float, specular_ior_level: float, specular_tint):
    """``(eta, tint)`` of a dielectric under a thin film, as Cycles builds it
    (``intern/cycles/kernel/svm/closure.h``, Principled specular):

    - Specular IOR Level ADJUSTS THE INDEX the film sits on:
      ``F0 = F0(IOR) * 2 * level``, ``eta = ior_from_F0(F0)``, inverted
      when IOR < 1 (level 0.5 leaves IOR unchanged);
    - the film reflectance is then scaled by ``F0 * tint / F0(eta) = tint``.

    The tint is clamped to [0, 1]: Cycles fades its scale out as the
    reflectance approaches 1, but RISE's ``thinfilm`` mode multiplies the
    whole interface reflectance by the tint, so a tint above 1 would push
    it past 1 at grazing angles and make the diffuse transmission term
    ``(1 - A_i)(1 - A_o)`` negative.  A tint below 1 is therefore matched
    at normal incidence and over-applied toward grazing."""

    ior = float(ior)
    level = max(0.0, float(specular_ior_level))
    eta = max(1.0e-3, ior)
    if abs(level - 0.5) > 1.0e-9:
        r = (eta - 1.0) / (eta + 1.0)
        f0 = min(_IOR_FROM_F0_MAX, r * r * 2.0 * level)
        root = math.sqrt(max(0.0, f0))
        eta = (1.0 + root) / (1.0 - root)
        if ior < 1.0:
            eta = 1.0 / eta
    tint = tuple(min(1.0, max(0.0, float(specular_tint[c]))) for c in range(3))
    return eta, tint


def dispersion_keeps_positive_ior(n_d: float, abbe: float) -> bool:
    """True when the Cauchy curve the native bridge builds for
    ``(n_d, abbe)`` stays positive over its 300-900 nm sampling band (it
    is monotone in wavelength, so the band ends decide).  An extreme Abbe
    number (V_d below ~0.29 at IOR 1.5) would otherwise make the bridge
    refuse the material and abort the render."""

    if not (abbe > 0.0) or not (n_d > 0.0):
        return False
    inv_f, inv_c, inv_d = 1.0 / 486.1 ** 2, 1.0 / 656.3 ** 2, 1.0 / 587.6 ** 2
    b = (n_d - 1.0) / (abbe * (inv_f - inv_c))
    a = n_d - b * inv_d
    return min(a + b / 300.0 ** 2, a + b / 900.0 ** 2) > 0.0
