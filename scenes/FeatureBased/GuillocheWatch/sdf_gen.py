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

  python3 sdf_gen.py                 # print the six watch chunks (paste into watch_dial.RISEscene)
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


def emit_chunk(name, parts):
    """Print one inline `sdf_geometry` chunk (tab-indented, matching the
    watch_dial.RISEscene authoring style)."""
    print("sdf_geometry")
    print("{")
    print("\tname\t\t\t\t%s" % name)
    for p in parts:
        print("\tpart\t\t\t\t%s" % p)
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
    # 4 flying-blade lugs (MING-style): a round-cone fat at the case root,
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
    """Gear-fluted crown (MING-style): a cylinder along X with N deep flutes cut
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


def marker_ring(r=18.0, width=0.95, halfh=0.11, z=0.72, gap_half=1.05, gap12_half=2.1):
    """MING-style floating hour ring: a thin FLAT band (washer, not wire)
    suspended just below the sapphire dome, BROKEN at the 12 hour positions --
    the gaps ARE the markers, wider at 12 o'clock.  Flat top catches the
    softbox pair as a broad annular highlight.  Outer cylinder minus inner
    cylinder minus 12 radial gap boxes."""
    P = [part("cylinder", "union", 0, (0, 0, z), (90, 0, 0), a=r + width * 0.5, b=halfh),
         part("cylinder", "subtract", 0, (0, 0, z), (90, 0, 0), a=r - width * 0.5, b=halfh * 4)]
    for k in range(12):
        theta = 90.0 - k * 30.0          # 12 o'clock at +Y, clockwise
        rad = math.radians(theta)
        gx, gy = r * math.cos(rad), r * math.sin(rad)
        half = gap12_half if k == 0 else gap_half
        # local x = radial (euler Rz(theta)); a cuts through the band radially,
        # b is the half GAP along the circumference, c cuts it vertically
        P.append(part("box", "subtract", 0, (gx, gy, z), (0, 0, theta),
                      a=width * 1.5, b=half, c=halfh * 4))
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
    # MING-style hands: fatter at the hub, gentle linear taper, ROUNDED tip
    # (the round-cone tip radius is the rounding).
    emit_chunk("handhoursdf",
               hand(length=10.0, r_base=1.45, r_tip=0.48, thickness=0.42))
    emit_chunk("handminutesdf",
               hand(length=16.0, r_base=1.22, r_tip=0.40, thickness=0.42))
    emit_chunk("pinsdf", pin())
    emit_chunk("markerringsdf", marker_ring())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
