#!/usr/bin/env python3
"""Regenerate the tiny binary assets used by the ChunkCoverage equivalence scenes.

These are the smallest valid files that make the four ASSET-DEPENDENT chunk
parsers (exr_painter, tiff_painter, 3dsmesh_geometry, datadriven_material) reach
a successful Finalize, so the CST-vs-legacy gate (tests/CstCorpusEquivalenceTest)
exercises them.  They are deliberately hand-rolled (RISE has no in-tree authoring
tool for .3ds or its .bdf BSDF format), so this script is the source of truth.

Run from the repo root:  python3 scenes/Tests/ChunkCoverage/assets/gen_chunkcoverage_assets.py
Requires: numpy, OpenEXR + Imath, Pillow (all already used elsewhere in the repo).

Endianness note: RISE's DiskFileReadBuffer reads int/uint/float/double as NATIVE
little-endian on LE hosts (see DiskFileReadBuffer.cpp), so all packs below use '<'.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def gen_exr():
    """Tiny 2x2 half-float RGB OpenEXR -> exr_painter."""
    import numpy as np
    import OpenEXR
    import Imath
    w, h = 2, 2
    hdr = OpenEXR.Header(w, h)
    half = Imath.PixelType(Imath.PixelType.HALF)
    hdr['channels'] = {c: Imath.Channel(half) for c in ('R', 'G', 'B')}
    out = OpenEXR.OutputFile(os.path.join(HERE, 'tiny.exr'), hdr)
    data = np.array([0.5, 0.6, 0.4, 0.5], dtype=np.float16).tobytes()
    out.writePixels({'R': data, 'G': data, 'B': data})
    out.close()
    print("wrote tiny.exr")


def gen_tiff():
    """Tiny 2x2 uncompressed baseline RGB TIFF -> tiff_painter (libtiff)."""
    from PIL import Image
    img = Image.new('RGB', (2, 2), (128, 64, 200))
    img.putpixel((0, 0), (255, 0, 0))
    img.putpixel((1, 1), (0, 255, 0))
    img.save(os.path.join(HERE, 'tiny.tiff'), compression='raw')
    print("wrote tiny.tiff")


def gen_3ds():
    """Minimal one-triangle .3ds -> 3dsmesh_geometry.

    RISE's TriangleMeshLoader3DS only reads the MAIN3DS/EDIT3DS/EDIT_OBJECT/
    OBJ_TRIMESH/TRI_VERTEXL/TRI_FACEL1 chunk chain (it ignores everything else)
    and requires byte 28 of the file to be >= 3 (its crude version gate).  The
    layout below satisfies both: the M3D_VERSION chunk right after the 6-byte
    MAIN header makes the file long enough that byte 28 is a non-zero length byte.
    """
    def chunk(cid, payload):
        return struct.pack('<HI', cid, 6 + len(payload)) + payload
    MAIN3DS, M3D_VERSION, EDIT3DS, EDIT_OBJECT = 0x4D4D, 0x0002, 0x3D3D, 0x4000
    OBJ_TRIMESH, TRI_VERTEXL, TRI_FACEL1 = 0x4100, 0x4110, 0x4120
    ver = chunk(M3D_VERSION, struct.pack('<I', 3))
    verts = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
    vbuf = struct.pack('<H', len(verts)) + b''.join(struct.pack('<fff', *v) for v in verts)
    faces = [(0, 1, 2)]
    fbuf = struct.pack('<H', len(faces)) + b''.join(struct.pack('<HHHH', a, b, c, 0) for (a, b, c) in faces)
    trimesh = chunk(OBJ_TRIMESH, chunk(TRI_VERTEXL, vbuf) + chunk(TRI_FACEL1, fbuf))
    obj = chunk(EDIT_OBJECT, b'cube\x00' + trimesh)
    main = chunk(MAIN3DS, ver + chunk(EDIT3DS, obj))
    assert main[28] >= 3, "byte 28 must satisfy the loader's version gate"
    with open(os.path.join(HERE, 'tiny.3ds'), 'wb') as f:
        f.write(main)
    print("wrote tiny.3ds")


def gen_bdf():
    """Minimal RISE data-driven BSDF (.bdf) -> datadriven_material.

    Format (DataDrivenBSDF.cpp): int signature(0xBDF), uint version(1),
    int numEmitterPositions, int numPatches, then per emitter a double theta
    followed by, per patch, 5 doubles (brdf) + 5 doubles (btdf).
    """
    out = bytearray()
    out += struct.pack('<i', 0x0BDF)
    out += struct.pack('<I', 1)
    num_emitter, num_patches = 1, 2
    out += struct.pack('<i', num_emitter)
    out += struct.pack('<i', num_patches)
    for _ in range(num_emitter):
        out += struct.pack('<d', 0.0)
        for p in range(num_patches):
            out += struct.pack('<ddddd', 0.1 * p, 0.1 * p + 0.05, 0.5, 0.5, 0.5)  # brdf
            out += struct.pack('<ddddd', 0.1 * p, 0.1 * p + 0.05, 0.0, 0.0, 0.0)  # btdf
    with open(os.path.join(HERE, 'tiny.bdf'), 'wb') as f:
        f.write(out)
    print("wrote tiny.bdf")


if __name__ == '__main__':
    gen_exr()
    gen_tiff()
    gen_3ds()
    gen_bdf()
    print("done")
