#!/usr/bin/env python3
"""Generate the watch's `sdf_geometry` chunks (the sphere-traced implicit
primitive).  Prints ready-to-paste INLINE chunks -- one repeatable `part`
line per primitive -- for watch_dial.RISEscene.  Part-line grammar (shared
with external parts files; see SDFGeometry::ParsePartLines):

    <type> <op> <k>  <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <a b c>  <round>

type  : sphere | box | roundbox | cylinder | torus | capsule | roundcone
op    : union | smin | subtract | intersect   (smin/subtract/intersect use k as the blend radius)
a,b,c : per-primitive size (see SDFGeometry.h):
        sphere    a=radius
        box       a,b,c = half-extents       roundbox  + round = corner radius
        cylinder  a=radius b=half-height          (axis = local Y)
        torus     a=major  b=tube                 (ring in local XZ, around Y)
        capsule   a=radius b=half-height          (axis = local Y)
        roundcone a=base-radius b=tip-radius c=height  (axis = local Y, base at y=0)
euler : applied Rz*Ry*Rx (degrees).  scale : per-axis (non-uniform OK).

  python3 sdf_gen.py                 # print the seven watch chunks (paste into watch_dial.RISEscene)
  python3 sdf_gen.py --test-sphere   # print a lone r=5 sphere chunk, for validation
"""
import argparse
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def part(type, op, k, pos, euler=(0, 0, 0), scale=(1, 1, 1), a=0.0, b=0.0, c=0.0, round=0.0):
    g = lambda *v: " ".join("%g" % (0.0 if abs(x) < 1e-9 else x) for x in v)
    return "%s %s %s  %s  %s  %s  %s  %s" % (
        type, op, g(k), g(*pos), g(*euler), g(*scale), g(a, b, c), g(round))


def emit_chunk(name, parts, sampling_detail=None):
    """Print one inline `sdf_geometry` chunk (tab-indented, matching the
    watch_dial.RISEscene authoring style).  `sampling_detail`, when set,
    emits the optional tessellation-grid resolution line (needed for the
    emissive lume batons, whose thin features under-resolve at the default
    64-cell grid over their wide bbox -> the load-time 'PROVABLY missed
    surface' warning + a withdrawn CanBeAreaLight())."""
    print("sdf_geometry")
    print("{")
    print("\tname\t\t\t\t%s" % name)
    for p in parts:
        print("\tpart\t\t\t\t%s" % p)
    if sampling_detail is not None:
        print("\tsampling_detail\t\t%d" % sampling_detail)
    print("}")
    print("")


def case_body():
    """Case + caseback + bezel + 4 lugs as ONE melded ti_polished body, with the
    dial bowl carved out of the top.  All in the watch's world frame, so the
    sdf_geometry object sits at position 0 0 0."""
    P = []
    # base case: solid cylinder along Z (a local-Y cylinder rolled 90 about X),
    # radius 24, half-height 5.635, centred at z=-4.635 -> spans z in [-10.27, +1.0]
    P.append(part("cylinder", "union", 0, (0, 0, -4.635), (90, 0, 0), a=24.0, b=5.635))
    # 4 flying-blade lugs: a round-cone fat at the case root,
    # tapering to a blade tip, swept OUT + DOWN toward the wrist, smooth-melded
    # into the case (no seam).  ex tilts the cone's +Y axis out(+/-Y) and down(-Z).
    for (sx, sy) in [(-1, 1), (1, 1), (-1, -1), (1, -1)]:
        ex = -45.0 if sy > 0 else -135.0
        P.append(part("roundcone", "smin", 2.4, (sx*13.88, sy*20.5, -2.2),
                      euler=(ex, 0, 0), scale=(1.45, 1, 0.5), a=3.6, b=1.2, c=8.5))
    # bezel: thin ring (torus around Z) at the top rim, melded
    P.append(part("torus", "smin", 1.2, (0, 0, 1.0), (90, 0, 0), a=22.0, b=1.25))
    # carve the dial bowl from the top (subtract an inner cylinder, inside the bezel)
    P.append(part("cylinder", "subtract", 1.0, (0, 0, -1.0), (90, 0, 0), a=21.0, b=3.0))
    return P


def hand(length, r_base, r_tip, thickness):
    """A fat-at-the-hub, tapering-to-the-tip hand: a round cone along +Y (base at
    y=0, tip at y=length), flattened in Z to the hand thickness.  The scene Object
    pivots it at the hub (y=0) and rotates it to the time angle."""
    flat = (thickness * 0.5) / max(r_base, 1e-3)
    return [part("roundcone", "union", 0, (0, 0, 0), (0, 0, 0),
                 scale=(1, 1, flat), a=r_base, b=r_tip, c=length)]


def crown(center=(26.0, 0.0, -4.635), r=3.2, halfL=2.0, n_flutes=16, depth=0.72, width=0.42):
    """Gear-fluted crown: a cylinder along X with N deep flutes cut
    around the circumference, on a narrower stem that melds toward the case."""
    P = []
    # crown body: a local-Y cylinder rolled so its axis points +X
    P.append(part("cylinder", "union", 0, center, (0, 0, 90), a=r, b=halfL))
    # stem / shoulder bridging to the case, smooth-melded
    P.append(part("cylinder", "smin", 0.5,
                  (center[0] - halfL - 1.1, center[1], center[2]), (0, 0, 90), a=1.4, b=1.3))
    # N gear-tooth flutes: subtract long boxes around the X axis
    for k in range(n_flutes):
        phi = k * 360.0 / n_flutes
        rad = math.radians(phi)
        gy = center[1] + r * math.cos(rad)
        gz = center[2] + r * math.sin(rad)
        P.append(part("box", "subtract", 0.06, (center[0], gy, gz), (phi, 0, 0),
                      a=halfL * 1.1, b=depth, c=width))
    return P


# Crystal geometry constants (double-domed sapphire).  See watch_dial.RISEscene
# crystal section + docs: a half-chord a=21.5 cap on two CONCENTRIC spheres
# (1.5 thick), seated on a flat underside plane at z=1.30, with the hour markers
# ETCHED into the flat flange and lume-filled.
CR_A = 21.5            # half-chord (the cap's xy half-width)
CR_ZFLAT = 1.30        # flat underside (seat) plane z
CR_H = 3.5             # dome rise above the seat -> apex at z = 1.30 + 3.5 = 4.80
CR_T = 1.5             # shell thickness
CR_RO = round((CR_A * CR_A + CR_H * CR_H) / (2.0 * CR_H), 4)   # 67.7857
CR_ZC = round(CR_ZFLAT + CR_H - CR_RO, 4)                      # -62.9857 (sphere centre)
CR_RI = round(CR_RO - CR_T, 4)                                 # 66.2857 (inner sphere)
MK_R = 18.0            # hour-marker ring radius


def _marker_angles():
    """The 12 hour positions on the marker ring, as (gx, gy, theta_deg) tuples --
    12 o'clock at +Y, going clockwise.  theta = 90 - k*30; reproduces the legacy
    15.5885 = 18*cos30 coordinates byte-for-byte."""
    out = []
    for k in range(12):
        theta = 90.0 - k * 30.0
        rad = math.radians(theta)
        out.append((MK_R * math.cos(rad), MK_R * math.sin(rad), theta))
    return out


def crystal():
    """Double-domed sapphire crystal as a sphere-traced shell, authored in the
    watch's ABSOLUTE world frame (object sits at 0 0 0).  Two CONCENTRIC spheres
    1.5 apart form the dome shell; a keep-slab box clips the giant sphere to the
    cap above the flat seat plane (z=1.30); a central inner-sphere subtract carves
    the concave recess (leaving a solid flat-bottomed flange for r>~16.16); and 12
    shallow boxes etch the hour-marker cavities into the flange underside.

    Box half-extent slots at the hour positions (euler Rz(theta)): a=RADIAL,
    b=TANGENTIAL (along the circumference), c=VERTICAL -- confirmed against
    SDFGeometry::MakePart's column convention (Rz maps local +X onto the radial
    direction at each on-ring position)."""
    P = [
        # 1. outer sphere (the giant additive sphere; only its cap survives)
        part("sphere", "union", 0, (0, 0, CR_ZC), a=CR_RO),
        # 2. keep-slab: clip to z in [1.30, 6.00], xy within +-22 (above the seat)
        part("box", "intersect", 0, (0, 0, 3.65), a=22.0, b=22.0, c=2.35),
        # 3. inner sphere subtract: carve the concave dome (central recess)
        part("sphere", "subtract", 0, (0, 0, CR_ZC), a=CR_RI),
    ]
    # 4. twelve etched marker CAVITIES in the flat flange underside (ring r=18).
    # Box centred AT the underside plane (z=1.30) so the cut opens at the surface;
    # radial 0.475, tangential 1.05 (2.1 at the wider 12 o'clock signature marker),
    # vertical 0.45 -> etch depth 0.45 into the glass (ceiling at z=1.75).
    for i, (gx, gy, theta) in enumerate(_marker_angles()):
        tang = 2.1 if i == 0 else 1.05
        P.append(part("box", "subtract", 0, (gx, gy, CR_ZFLAT), (0, 0, theta),
                      a=0.475, b=tang, c=0.45))
    return P


def marker_lume():
    """Lume FILL for the 12 etched marker cavities: the same 12 segments, INSET
    from every cavity wall (no coincident surfaces -> no z-fighting; physically
    the fill meniscus sits just below flush).  Pure union of small boxes ->
    a tight bbox.  Centre z = 1.5225 -> spans z 1.31..1.735: bottom face 0.01
    below-flush (the visible face), sides inset 0.015, top 0.015 short of the
    cavity ceiling (z=1.75)."""
    P = []
    for i, (gx, gy, theta) in enumerate(_marker_angles()):
        tang = 2.085 if i == 0 else 1.035
        P.append(part("box", "union", 0, (gx, gy, 1.5225), (0, 0, theta),
                      a=0.46, b=tang, c=0.2125))
    return P


def pin(r=0.85, halfh=0.16, z=1.02):
    """Small FLAT central pin cap (a low z-axis cylinder) sitting ON TOP of the
    hand stack -- hour at z~0.2, minute above at z~0.7, pin covers the pivot.
    Rendered in lume white (see the LUME PLAN note in watch_dial.RISEscene)."""
    return [part("cylinder", "union", 0, (0, 0, z), (90, 0, 0), a=r, b=halfh)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test-sphere", action="store_true", help="emit test_sphere.sdf (a lone r=5 sphere)")
    args = ap.parse_args(argv)

    if args.test_sphere:
        emit_chunk("testspheresdf", [part("sphere", "union", 0, (0, 0, 0), a=5.0)])
        return 0

    emit_chunk("casebodysdf", case_body())
    emit_chunk("crownsdf", crown())
    # Fatter at the hub, gentle linear taper, ROUNDED tip (the round-cone tip
    # radius is the rounding).
    emit_chunk("handhoursdf",
               hand(length=10.0, r_base=1.45, r_tip=0.48, thickness=0.42))
    emit_chunk("handminutesdf",
               hand(length=16.0, r_base=1.22, r_tip=0.40, thickness=0.42))
    emit_chunk("pinsdf", pin())
    # Double-domed sapphire crystal with the hour markers ETCHED into its flat
    # flange underside, and the lume that fills those cavities.
    emit_chunk("crystalsdf", crystal())
    emit_chunk("markerlumesdf", marker_lume(), sampling_detail=384)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
