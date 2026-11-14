"""bpy-free light-power conversions (Blender light -> RISE light), so the
maths is unit-tested by `test_hair_export.py` without Blender.

Every factor below was MEASURED against Cycles in Blender 5.3 with
`tools/blender_light_parity.py` (white Lambertian floor, Cycles vs RISE);
see docs/BLENDER_MATERIAL_TRANSLATION.md "Lights".
"""

from __future__ import annotations

import math

AREA_LIGHT_DISK_SEGMENTS = 64


def unnormalized_power_scale(light) -> float:
    """Power multiplier for a point / spot / sun light with Blender's
    "Normalize" OFF (1.0 when it is on, the default).  Cycles then stops
    dividing the power by the light's size:

    - point / spot, radius r > 0: x 4 pi r^2 (measured x12.566 at r = 1);
    - point / spot, radius 0: x 4 (normalize ON at radius 0 is x1);
    - sun, angle a > 0: x pi sin^2(a/2) (measured x0.21045 at 30 deg);
      angle 0: x1.

    RISE's point / spot / directional lights are deltas, so only this
    power scaling carries over -- not the soft shadows a size would cast.
    Area lights use `area_light_exitance`.  `light` is a bpy Light (or any
    object with ``type`` / ``normalize`` / ``shadow_soft_size`` / ``angle``)."""

    if getattr(light, "normalize", True):
        return 1.0
    kind = getattr(light, "type", "")
    if kind in ("POINT", "SPOT"):
        radius = max(0.0, float(getattr(light, "shadow_soft_size", 0.0)))
        return 4.0 * math.pi * radius * radius if radius > 0.0 else 4.0
    if kind == "SUN":
        angle = max(0.0, float(getattr(light, "angle", 0.0)))
        return math.pi * math.sin(0.5 * angle) ** 2 if angle > 0.0 else 1.0
    return 1.0


def area_light_polygon_area(shape: str, size_x: float, size_y: float,
                            segments: int = AREA_LIGHT_DISK_SEGMENTS) -> float:
    """Local area of the mesh the exporter emits from: the rectangle
    exactly, or the inscribed ``segments``-gon of the ellipse (an affine
    image of the regular polygon, so 1/2 N a b sin(2 pi / N))."""

    if shape in ("SQUARE", "RECTANGLE"):
        return size_x * size_y
    return 0.5 * segments * (0.5 * size_x) * (0.5 * size_y) * math.sin(2.0 * math.pi / segments)


def area_light_true_area(shape: str, size_x: float, size_y: float) -> float:
    """Local area of Blender's light shape (the exact ellipse for disks)."""

    if shape in ("SQUARE", "RECTANGLE"):
        return size_x * size_y
    return math.pi * (0.5 * size_x) * (0.5 * size_y)


def area_light_exitance(power: float, shape: str, size_x: float, size_y: float,
                        plane_scale: float, normalize: bool) -> float:
    """Radiant exitance (W/m^2) for the emitted area-light mesh.

    ``plane_scale`` is the in-plane area scale of the object matrix,
    ``|X x Y|`` -- the factor that turns the mesh's local area into its
    world area.  Normalize ON: Cycles' flux equals ``power``, so the
    exitance is power over the emitted WORLD area.  Normalize OFF: Cycles
    treats power as per unit area of the true shape, so the flux is
    ``power * true world area`` and the exitance ``power * true / polygon``
    (independent of scale).  Measured within 0.1% of Cycles for unscaled,
    x2 scaled and non-uniformly scaled panels.  A genuinely sheared panel
    (rotated child of a non-uniformly scaled parent) keeps exactly the stated
    power here but not in Cycles 5.3 (tools/blender_light_parity.py
    reports it ungated)."""

    polygon = area_light_polygon_area(shape, size_x, size_y)
    if polygon <= 0.0 or plane_scale <= 0.0:
        return 0.0
    if normalize:
        return power / (polygon * plane_scale)
    return power * area_light_true_area(shape, size_x, size_y) / polygon
