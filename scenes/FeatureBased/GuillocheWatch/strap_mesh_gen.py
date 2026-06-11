#!/usr/bin/env python3
"""Generate a curved leather watch-strap as a RAW2 swept-band mesh for the RISE
watch hero.  The strap leaves the lug (z ~ -4), curves down to the polished
tabletop (z=-10.30) and lays flat, extending outward (a hero-shot pose).

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
    ap.add_argument("--end-width", type=float, default=20.2, help="strap width at the free end (MING-style 20->16mm taper)")
    ap.add_argument("--thickness", type=float, default=3.0, help="strap thickness (scene units)")
    ap.add_argument("--crown", type=float, default=0.55, help="extra centre doming of the top surface (scene units)")
    ap.add_argument("--edge-pow", type=float, default=8.0, help="superellipse edge exponent (higher = flatter middle, tighter rounded edges)")
    ap.add_argument("--stitch-inset", type=float, default=0.085, help="stitch row inset from each edge (fraction of width)")
    ap.add_argument("--stitch-pitch", type=float, default=2.4, help="distance between stitches along the band (scene units)")
    ap.add_argument("--stitch-len", type=float, default=1.35, help="visible thread length per stitch")
    ap.add_argument("--stitch-r", type=float, default=0.14, help="thread radius")
    ap.add_argument("--stitch-angle", type=float, default=16.0, help="saddle-stitch slant (degrees, mirrored per row)")
    ap.add_argument("--groove", type=float, default=0.16, help="stitch channel depth pressed into the top surface")
    ap.add_argument("--stitch-out", default=os.path.join(_HERE, "strap_stitch.raw2"))
    ap.add_argument("--nlen", type=int, default=140, help="samples along the path")
    ap.add_argument("--nwid", type=int, default=14, help="samples across the width")
    args = ap.parse_args()

    # Centreline path (y, z) for the +Y half: lug -> curve down -> flat on table.
    # Table top is z=-10.30; centreline on the flat run = -10.30 + thickness/2.
    flatz = -10.30 + args.thickness / 2.0
    # MING-style drape: a gentler, longer descent off the lug, touch-down with a
    # SUBTLE residual arc (the band relaxes, never dead-flat), tip settling.
    ctrl = [(24.0, -3.4), (32.0, -4.9), (43.0, -7.3), (56.0, flatz + 0.55),
            (70.0, flatz + 0.12), (86.0, flatz + 0.30), (104.0, flatz + 0.02)]
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
        # FKM-rubber cross-section: superellipse half-thickness rounds the side
        # edges; the TOP additionally crowns toward the centre (subtle dome).
        def edge(u):
            return max(1e-3, (1.0 - abs(2.0 * u - 1.0) ** args.edge_pow)) ** 0.5
        def htop(u):
            h = (T / 2 + args.crown * (1.0 - (2.0 * u - 1.0) ** 2)) * edge(u)
            # recessed stitch CHANNEL along both rows -- the shadowed seam line
            # between stitches is half of what makes stitching read as real
            for u0 in (args.stitch_inset, 1.0 - args.stitch_inset):
                h -= args.groove * math.exp(-((u - u0) / 0.018) ** 2)
            return h
        du = 1.0 / (args.nwid - 1)
        for j in range(args.nwid):
            u = j / (args.nwid - 1)
            x = (u - 0.5) * W
            h = htop(u)
            # top normal: tilt across the width by the surface slope dh/dx,
            # expressed in the (X, frame-normal) basis
            dh_dx = (htop(min(1.0, u + du)) - htop(max(0.0, u - du))) / (2 * du) / max(W, 1e-6)
            nlen2 = math.sqrt(dh_dx * dh_dx + 1.0)
            nx_t = -dh_dx / nlen2
            verts.append((x, cy + h * ny, cz + h * nz, nx_t, ny / nlen2, nz / nlen2, u, frac))
            idx_top[i, j] = vc; vc += 1
        for j in range(args.nwid):
            u = j / (args.nwid - 1)
            x = (u - 0.5) * W
            hb = (T / 2) * edge(u)
            verts.append((x, cy - hb * ny, cz - hb * nz, 0.0, -ny, -nz, u, frac))
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
    # ---- stitch thread mesh (separate file -> its own material) ----
    # Walk the path by arc length; every stitch-pitch, drop one thread capsule
    # per edge row ON the top surface (poking out of the groove), slanted by
    # +/-stitch-angle about the surface normal (mirrored rows = saddle look).
    sv, st = [], []

    def capsule(center, d_hat, n_hat, half_len, r):
        """Tiny 8-sided capsule from `center` along d_hat; n_hat = outward."""
        base = len(sv)
        x_hat = np.cross(d_hat, n_hat); x_hat /= np.linalg.norm(x_hat)
        y_hat = np.cross(x_hat, d_hat)
        rings = [(-half_len + r * 0.4, 0.75), (-half_len + r, 1.0),
                 (half_len - r, 1.0), (half_len - r * 0.4, 0.75)]
        for (off, rs) in rings:
            for k in range(8):
                a = 2 * math.pi * k / 8
                rad = (math.cos(a) * x_hat + math.sin(a) * y_hat) * (r * rs)
                pp = center + d_hat * off + rad
                nn = rad / np.linalg.norm(rad)
                sv.append((pp[0], pp[1], pp[2], nn[0], nn[1], nn[2], k / 8.0, 0.5))
        for q in range(len(rings) - 1):
            for k in range(8):
                k2 = (k + 1) % 8
                a0, b0 = base + q * 8 + k, base + q * 8 + k2
                a1, b1 = base + (q + 1) * 8 + k, base + (q + 1) * 8 + k2
                st.append((a0, a1, b1)); st.append((a0, b1, b0))
        for (off, sgn) in ((-half_len, -1.0), (half_len, 1.0)):
            tipP = center + d_hat * off
            tipN = d_hat * sgn
            sv.append((tipP[0], tipP[1], tipP[2], tipN[0], tipN[1], tipN[2], 0.5, 0.5))
            tip = len(sv) - 1
            ring0 = base if sgn < 0 else base + (len(rings) - 1) * 8
            for k in range(8):
                k2 = (k + 1) % 8
                if sgn < 0: st.append((tip, ring0 + k2, ring0 + k))
                else:       st.append((tip, ring0 + k, ring0 + k2))

    arc = 0.0
    next_at = args.stitch_pitch * 0.5
    slant = math.radians(args.stitch_angle)
    for i in range(1, n):
        a, b = path[i - 1], path[i]
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        arc += seg
        if arc < next_at:
            continue
        next_at += args.stitch_pitch
        ty, tz = (b - a) / (seg or 1.0)
        ny, nz = -tz, ty
        if nz < 0: ny, nz = -ny, -nz
        frac = i / (n - 1)
        W = args.width * (1 - frac) + args.end_width * frac
        cy, cz = path[i]
        T_hat = np.array([0.0, ty, tz]); X_hat = np.array([1.0, 0.0, 0.0])
        for (u0, mirror) in ((args.stitch_inset, 1.0), (1.0 - args.stitch_inset, -1.0)):
            x = (u0 - 0.5) * W
            # surface height at the row (includes the groove); thread sits proud
            hh = (T / 2 + args.crown * (1.0 - (2.0 * u0 - 1.0) ** 2)) *                  max(1e-3, (1.0 - abs(2.0 * u0 - 1.0) ** args.edge_pow)) ** 0.5                  - args.groove + args.stitch_r * 0.9
            center = np.array([x, cy + hh * ny, cz + hh * nz])
            n_hat = np.array([0.0, ny, nz])
            d_hat = math.cos(slant) * T_hat + math.sin(slant) * mirror * X_hat
            d_hat /= np.linalg.norm(d_hat)
            capsule(center, d_hat, n_hat, args.stitch_len * 0.5, args.stitch_r)

    SV = np.array(sv, float); ST = np.array(st, int)
    with open(args.stitch_out, "w") as f:
        f.write("%d %d\n" % (SV.shape[0], ST.shape[0]))
        np.savetxt(f, SV, fmt="v %.5f %.5f %.5f %.5f %.5f %.5f %.6f %.6f")
        np.savetxt(f, ST, fmt="t %d %d %d")
    print("stitches: %d threads, %d verts, %d tris -> %s"
          % (SV.shape[0] // 33, SV.shape[0], ST.shape[0], args.stitch_out))

    zmin, zmax = V[:, 2].min(), V[:, 2].max()
    ymin, ymax = V[:, 1].min(), V[:, 1].max()
    print("strap: %d verts, %d tris  y[%.1f,%.1f] z[%.1f,%.1f]  -> %s"
          % (V.shape[0], Tr.shape[0], ymin, ymax, zmin, zmax, args.out))


if __name__ == "__main__":
    main()
