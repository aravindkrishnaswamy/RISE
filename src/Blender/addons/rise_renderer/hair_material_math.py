"""Pure-python conversion math for translating a Blender
``ShaderNodeBsdfHairPrincipled`` (Principled Hair BSDF) into RISE's
``hair_material`` (Chiang et al. 2016) parameter tiers.

Bpy-free by design — see ``hair_file_writer.py``'s module docstring
for why (unit-testable with plain ``python3``; the bpy-dependent node
-graph walking that reads socket values lives in ``exporter.py`` and
calls into this module with plain floats).
"""

from __future__ import annotations

import math

__all__ = [
    "melanin_to_eumelanin_pheomelanin",
    "offset_radians_to_alpha_degrees",
]

# Cycles clamps the artist-facing melanin fraction away from exactly 1.0
# before taking its log (a literal -log(0) would be +inf, and a groom's
# absorption has to stay finite).  1e-4 matches Cycles'
# `bsdf_hair_principled.h` convention — see this module's own
# `melanin_to_eumelanin_pheomelanin` docstring for the derivation this
# constant participates in.
_MELANIN_FLOOR = 1e-4


def melanin_to_eumelanin_pheomelanin(melanin: float, redness: float) -> tuple[float, float]:
    """Convert Blender Principled Hair BSDF's 'Melanin concentration'
    parametrization (melanin fraction ``m`` in [0, 1] + redness ``r``
    in [0, 1]) into the eumelanin / pheomelanin concentration pair
    RISE's `hair_material` Tier 1 expects.

    Formula (matches Cycles' internal conversion in
    `bsdf_hair_principled.h`, `bsdf_principled_hair_parametrization_x`
    territory — the artist-facing [0, 1] melanin slider is not itself
    a physical concentration; Cycles first log-remaps it into an
    unbounded absorption-scale quantity, matching Chiang et al. 2016's
    own parametrization where "melanin" has no upper bound, then
    splits that quantity between the two pigments by redness):

        melanin_qty = -log(max(1 - m, 1e-4))
        eumelanin   = melanin_qty * (1 - r)
        pheomelanin = melanin_qty * r

    At ``m = 0`` (no pigment) this gives ``melanin_qty = 0`` and both
    outputs are 0.  As ``m -> 1`` the ``1e-4`` floor caps
    ``melanin_qty`` at ``-log(1e-4) ~= 9.21`` rather than diverging to
    infinity, so a slider dragged all the way to 1.0 still produces a
    finite, renderable (very dark / saturated) fibre rather than a
    NaN-propagating absorption coefficient.

    Both inputs are clamped to [0, 1] before use (Blender's own UI
    range for both sockets), so a value fed in slightly out of range
    by an upstream node graph edit doesn't produce a negative or
    superlinear result.

    Returns:
        ``(eumelanin, pheomelanin)``, both >= 0 — directly usable as
        RISE `hair_material` `eumelanin` / `pheomelanin` values (both
        "the SINGLE melanin tier" per the chunk's own description; a
        redness of 0 sets pheomelanin to exactly 0.0, which is fine —
        `hair_material`'s Finalize only requires that at least one of
        the pair be bound, not both).

    UNITS DISCLOSURE — the *remap* (this function) matches Cycles
    exactly, but the *concentration -> sigma_a* coefficient sets that
    consume these outputs downstream do NOT match between renderers,
    so the same ``(eumelanin, pheomelanin)`` pair does not produce the
    same absorption in RISE as in Cycles:

        pigment      Cycles (bsdf_hair_principled.h)   RISE (OMLC, G-anchored)
        -----------  --------------------------------  -----------------------
        eumelanin    (0.506, 0.841, 1.653)              (0.518, 0.697, 1.293)
        pheomelanin  (0.343, 0.733, 1.924)               (0.232, 0.400, 1.067)

    Both are per-unit-concentration sigma_a triples (R, G, B); RISE's
    are implemented in `src/Library/Materials/HairBSDF.cpp` from the
    in-tree OMLC extinction tables, anchored so the green channel at
    550 nm matches the OMLC curve shape.  At EQUAL concentrations this
    makes RISE absorb roughly 17% less green light for eumelanin
    (0.697 / 0.841) and roughly 45% less for pheomelanin (0.400 /
    0.733) than Cycles would for the same slider values — i.e. a groom
    that matches a Cycles reference by eye will read slightly lighter
    / less saturated in RISE at the same Melanin / Melanin Redness
    values.

    The per-pigment rescale that restores Blender parity (multiply
    ``eumelanin`` by 0.841/0.697 and ``pheomelanin`` by 0.733/0.400)
    IS applied, and is on by default — but in the NATIVE BRIDGE, not
    here.  ``add_hair_material`` in
    ``src/Blender/native/rise_blender_bridge.cpp`` owns it
    (``kEumelaninBlenderParityScale`` / ``kPheomelaninBlenderParityScale``),
    switchable per material via the ABI's
    ``apply_melanin_parity_rescale``.

    It lives there rather than here because the rescale is a
    LIVE-BLENDER-SESSION concern: its whole purpose is to make a groom
    look the way the artist dialled it in against Cycles' viewport.  A
    ``.RISEscene`` written out of this add-on would be authored in
    RISE's own units and should NOT carry it.  Keeping this function
    the pure Cycles remap keeps that distinction available to both
    consumers.  See docs/BLENDER_MATERIAL_TRANSLATION.md's hair section
    for the decision and its limits (it is a green-channel anchor, not
    a per-channel match).
    """

    m = min(max(float(melanin), 0.0), 1.0)
    r = min(max(float(redness), 0.0), 1.0)

    melanin_qty = -math.log(max(1.0 - m, _MELANIN_FLOOR))
    eumelanin = melanin_qty * (1.0 - r)
    pheomelanin = melanin_qty * r
    return eumelanin, pheomelanin


def offset_radians_to_alpha_degrees(offset_radians: float) -> float:
    """Convert Blender Principled Hair BSDF's 'Offset' socket (cuticle
    scale tilt, stored in RADIANS — an ANGLE-subtype socket, Blender's
    default of 2 degrees reads back as ~0.0349066 rad) into RISE
    `hair_material`'s `alpha` parameter (the same physical quantity,
    in DEGREES).
    """

    return math.degrees(float(offset_radians))
