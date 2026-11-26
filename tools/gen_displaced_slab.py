#!/usr/bin/env python3
"""Generate a closed displaced-slab mesh in RISE raw format.

The slab is a 1×1 unit square (centered at origin in XZ) with:
  - Flat bottom face at y=0
  - Displaced top face with gentle bumps at y = thickness + displacement(x,z)
  - Four side walls connecting top and bottom edges

The mesh is fully closed so refraction is physically correct.
"""

import math
import sys

# --- Parameters ---
N = 32           # grid subdivisions per side (N+1 vertices per edge)
HALF = 0.5       # half-extent in X and Z
THICKNESS = 0.06 # base thickness of the slab
DISP_AMP = 0.04  # displacement amplitude on top surface
DISP_FREQ = 3.0  # spatial frequency of displacement bumps

def displacement_top(x, z):
    """Smooth displacement function for the top surface."""
    d = 0.0
    d += 0.5 * math.sin(DISP_FREQ * math.pi * x) * math.sin(DISP_FREQ * math.pi * z)
    d += 0.25 * math.sin(2.0 * DISP_FREQ * math.pi * x + 0.7)
    d += 0.25 * math.cos(2.0 * DISP_FREQ * math.pi * z + 1.3)
    return DISP_AMP * d

def displacement_bottom(x, z):
    """Gentler displacement for the bottom surface (different pattern to avoid symmetry)."""
    d = 0.0
    d += 0.4 * math.sin(DISP_FREQ * 0.7 * math.pi * x + 0.5) * math.cos(DISP_FREQ * 0.7 * math.pi * z)
    d += 0.3 * math.cos(1.5 * DISP_FREQ * math.pi * x + 1.0)
    d += 0.3 * math.sin(1.5 * DISP_FREQ * math.pi * z + 2.0)
    return DISP_AMP * 0.5 * d  # half amplitude of top

def surface_normal(disp_func, x, z, sign_y):
    """Approximate normal via central differences. sign_y: +1 for top, -1 for bottom."""
    eps = 1e-4
    dydx = (disp_func(x + eps, z) - disp_func(x - eps, z)) / (2.0 * eps)
    dydz = (disp_func(x, z + eps) - disp_func(x, z - eps)) / (2.0 * eps)
    # For top (sign_y=+1): normal = (-dydx, 1, -dydz)
    # For bottom (sign_y=-1): normal = (dydx, -1, dydz)
    nx, ny, nz = -sign_y * dydx, sign_y * 1.0, -sign_y * dydz
    mag = math.sqrt(nx*nx + ny*ny + nz*nz)
    return (nx/mag, ny/mag, nz/mag)

vertices = []   # list of (x, y, z, nx, ny, nz, u, v)
triangles = []  # list of (i0, i1, i2) — 0-based

# UVs follow the regular grid: (i/N, j/N) for top and bottom faces.
# Side walls use (i/N, t) where t=0 at bottom and t=1 at top, so every
# triangle has a non-degenerate 2×2 UV Jacobian — crucial for the
# UV-Jacobian-inverted surface-derivative path in SMS / Manifold Solver.

# ===== Bottom face (displaced, normal pointing down) =====
bottom_start = len(vertices)
for j in range(N + 1):
    for i in range(N + 1):
        x = -HALF + i * (2.0 * HALF / N)
        z = -HALF + j * (2.0 * HALF / N)
        y = displacement_bottom(x, z)
        nx, ny, nz = surface_normal(displacement_bottom, x, z, -1.0)
        u = i / N
        v = j / N
        vertices.append((x, y, z, nx, ny, nz, u, v))

for j in range(N):
    for i in range(N):
        v00 = bottom_start + j * (N + 1) + i
        v10 = v00 + 1
        v01 = v00 + (N + 1)
        v11 = v01 + 1
        # Wind so normal faces down (CW when viewed from below = CCW from above)
        triangles.append((v00, v10, v11))
        triangles.append((v00, v11, v01))

# ===== Top face (displaced, normal pointing up) =====
top_start = len(vertices)
for j in range(N + 1):
    for i in range(N + 1):
        x = -HALF + i * (2.0 * HALF / N)
        z = -HALF + j * (2.0 * HALF / N)
        y = THICKNESS + displacement_top(x, z)
        nx, ny, nz = surface_normal(displacement_top, x, z, 1.0)
        u = i / N
        v = j / N
        vertices.append((x, y, z, nx, ny, nz, u, v))

for j in range(N):
    for i in range(N):
        v00 = top_start + j * (N + 1) + i
        v10 = v00 + 1
        v01 = v00 + (N + 1)
        v11 = v01 + 1
        # Wind so normal faces up (CCW when viewed from above)
        triangles.append((v00, v01, v11))
        triangles.append((v00, v11, v10))

# ===== Side walls (four edges) =====
# Each side wall connects the bottom edge to the top edge.
# We need to stitch corresponding vertices along each boundary.

def add_side_wall(bottom_indices, top_indices, outward_normal):
    """Stitch a strip between bottom and top edge vertex lists.

    Each emitted side-wall vertex carries UV = (k/(N), 0) at the bottom
    and (k/N, 1) at the top.  This gives a non-degenerate 2×2 UV
    Jacobian on every side-wall triangle so the SMS/Manifold-Solver
    surface-derivative path can invert it instead of falling back to
    a per-triangle barycentric edge frame."""
    nx, ny, nz = outward_normal
    nK = len(bottom_indices) - 1  # number of wall quads along this edge
    for k in range(nK):
        b0 = bottom_indices[k]
        b1 = bottom_indices[k + 1]
        t0 = top_indices[k]
        t1 = top_indices[k + 1]
        u0 = k / nK
        u1 = (k + 1) / nK
        # Two triangles per quad, wound so outward_normal faces out
        # We duplicate vertices with side-wall normals for sharp edges
        vb0 = len(vertices); vertices.append((*vertices[b0][:3], nx, ny, nz, u0, 0.0))
        vb1 = len(vertices); vertices.append((*vertices[b1][:3], nx, ny, nz, u1, 0.0))
        vt0 = len(vertices); vertices.append((*vertices[t0][:3], nx, ny, nz, u0, 1.0))
        vt1 = len(vertices); vertices.append((*vertices[t1][:3], nx, ny, nz, u1, 1.0))
        triangles.append((vb0, vb1, vt1))
        triangles.append((vb0, vt1, vt0))

# Front edge: z = -HALF, i goes 0..N  (outward normal = (0, 0, -1))
front_bottom = [bottom_start + i for i in range(N + 1)]
front_top    = [top_start + i for i in range(N + 1)]
add_side_wall(front_bottom, front_top, (0.0, 0.0, -1.0))

# Back edge: z = +HALF, i goes 0..N  (outward normal = (0, 0, +1))
back_bottom = [bottom_start + N * (N + 1) + i for i in range(N + 1)]
back_top    = [top_start + N * (N + 1) + i for i in range(N + 1)]
# Reverse order so winding is correct for outward normal
add_side_wall(list(reversed(back_bottom)), list(reversed(back_top)), (0.0, 0.0, 1.0))

# Left edge: x = -HALF, j goes 0..N  (outward normal = (-1, 0, 0))
left_bottom = [bottom_start + j * (N + 1) for j in range(N + 1)]
left_top    = [top_start + j * (N + 1) for j in range(N + 1)]
add_side_wall(list(reversed(left_bottom)), list(reversed(left_top)), (-1.0, 0.0, 0.0))

# Right edge: x = +HALF, j goes 0..N  (outward normal = (+1, 0, 0))
right_bottom = [bottom_start + j * (N + 1) + N for j in range(N + 1)]
right_top    = [top_start + j * (N + 1) + N for j in range(N + 1)]
add_side_wall(right_bottom, right_top, (1.0, 0.0, 0.0))

# ===== Write raw mesh =====
num_verts = len(vertices)
num_tris = len(triangles)

with open("models/raw/displaced_slab.raw", "w") as f:
    f.write(f"{num_verts} {num_tris}\n")
    for (x, y, z, nx, ny, nz, u, v) in vertices:
        f.write(f"v {x:.6f} {y:.6f} {z:.6f}    {nx:.6f} {ny:.6f} {nz:.6f}    {u:.6f} {v:.6f}\n")
    for (i0, i1, i2) in triangles:
        f.write(f"t {i0} {i1} {i2}\n")

print(f"Generated closed displaced slab: {num_verts} vertices, {num_tris} triangles")
print(f"  Thickness: {THICKNESS}, displacement amplitude: {DISP_AMP}")
print(f"  Y range: ~{-DISP_AMP*0.5:.3f} to ~{THICKNESS + DISP_AMP:.3f}")
