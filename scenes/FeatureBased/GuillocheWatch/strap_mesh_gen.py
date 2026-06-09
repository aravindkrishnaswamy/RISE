#!/usr/bin/env python3
"""Generate a curved leather watch-strap as a RAW2 swept-band mesh for the RISE
watch hero.  The strap leaves the lug (z ~ -4), curves down to the polished
tabletop (z=-10.30) and lays flat, extending outward (the MING b_hero pose).

It models ONE half (the +Y / 12-o'clock side); the scene places it twice (the
-Y / 6-o'clock side is the same mesh rotated 180 deg about Z).  Output is a
solid band (top + bottom + two side edges + far end cap) with per-vertex normals
and linear UV (u across width, v along length) so a stitching texture can be
added later.  Units are SCENE UNITS (1 unit = 0.79167 mm, scene_unit).

Only numpy.
"""
import argparse
import math
import os
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))  # the GuillocheWatch scene folder


def catmull_rom(ctrl, n):
    """Sample a Catmull-Rom spline through ctrl (list of 2-vectors) at ~n points."""
    P = np.array(ctrl, float)
    pad = np.vstack([P[0] + (P[0] - P[1]), P, P[-1] + (P[-1] - P[-2])])
    segs = len(P) - 1
    per = max(2, n // segs)
    out = []
    for s in range(segs):
        p0, p1, p2, p3 = pad[s], pad[s + 1], pad[s + 2], pad[s + 3]
        for t in np.linspace(0.0, 1.0, per, endpoint=False):
            t2, t3 = t * t, t * t * t
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t
                              + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
    out.append(P[-1])
    return np.array(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(_HERE, "strap.raw2"))
    ap.add_argument("--width", type=float, default=25.26, help="strap width at the lug (scene units; 20mm)")
    ap.add_argument("--end-width", type=float, default=22.0, help="strap width at the free end (slight taper)")
    ap.add_argument("--thickness", type=float, default=3.0, help="leather thickness (scene units)")
    ap.add_argument("--nlen", type=int, default=140, help="samples along the path")
    ap.add_argument("--nwid", type=int, default=14, help="samples across the width")
    args = ap.parse_args()

    # Centreline path (y, z) for the +Y half: lug -> curve down -> flat on table.
    # Table top is z=-10.30; centreline on the flat run = -10.30 + thickness/2.
    flatz = -10.30 + args.thickness / 2.0
    ctrl = [(24.0, -3.6), (31.0, -5.4), (41.0, -8.0), (54.0, flatz),
            (78.0, flatz), (104.0, flatz)]
    path = catmull_rom(ctrl, args.nlen)
    n = len(path)
    T = args.thickness

    verts = []        # (x,y,z, nx,ny,nz, u,v)
    idx_top = np.zeros((n, args.nwid), int)
    idx_bot = np.zeros((n, args.nwid), int)
    vc = 0
    for i in range(n):
        a = path[max(0, i - 1)]
        b = path[min(n - 1, i + 1)]
        ty, tz = (b - a)
        tl = math.hypot(ty, tz) or 1.0
        ty, tz = ty / tl, tz / tl
        ny, nz = (-tz, ty)            # top-facing normal in YZ (perp to tangent)
        if nz < 0:
            ny, nz = -ny, -nz          # keep it pointing up (+Z)
        frac = i / (n - 1)
        W = args.width * (1 - frac) + args.end_width * frac
        cy, cz = path[i]
        for j in range(args.nwid):
            u = j / (args.nwid - 1)
            x = (u - 0.5) * W
            verts.append((x, cy + (T / 2) * ny, cz + (T / 2) * nz, 0.0, ny, nz, u, frac))
            idx_top[i, j] = vc; vc += 1
        for j in range(args.nwid):
            u = j / (args.nwid - 1)
            x = (u - 0.5) * W
            verts.append((x, cy - (T / 2) * ny, cz - (T / 2) * nz, 0.0, -ny, -nz, u, frac))
            idx_bot[i, j] = vc; vc += 1

    tris = []
    def quad(a, b, c, d):
        tris.append((a, b, c)); tris.append((a, c, d))
    for i in range(n - 1):
        for j in range(args.nwid - 1):
            quad(idx_top[i, j], idx_top[i + 1, j], idx_top[i + 1, j + 1], idx_top[i, j + 1])      # top
            quad(idx_bot[i, j], idx_bot[i, j + 1], idx_bot[i + 1, j + 1], idx_bot[i + 1, j])      # bottom (reversed)
        quad(idx_top[i, 0],  idx_bot[i, 0],  idx_bot[i + 1, 0],  idx_top[i + 1, 0])               # left edge
        quad(idx_top[i, -1], idx_top[i + 1, -1], idx_bot[i + 1, -1], idx_bot[i, -1])              # right edge
    for j in range(args.nwid - 1):                                                                # far end cap
        quad(idx_top[-1, j], idx_top[-1, j + 1], idx_bot[-1, j + 1], idx_bot[-1, j])

    V = np.array(verts, float)
    Tr = np.array(tris, int)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("%d %d\n" % (V.shape[0], Tr.shape[0]))
        np.savetxt(f, V, fmt="v %.5f %.5f %.5f %.5f %.5f %.5f %.6f %.6f")
        np.savetxt(f, Tr, fmt="t %d %d %d")
    zmin, zmax = V[:, 2].min(), V[:, 2].max()
    ymin, ymax = V[:, 1].min(), V[:, 1].max()
    print("strap: %d verts, %d tris  y[%.1f,%.1f] z[%.1f,%.1f]  -> %s"
          % (V.shape[0], Tr.shape[0], ymin, ymax, zmin, zmax, args.out))


if __name__ == "__main__":
    main()
