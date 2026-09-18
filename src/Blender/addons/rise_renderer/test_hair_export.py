"""Unit tests for the bpy-free half of hair export: the `.hair` binary
writer (`hair_file_writer.py`) and the melanin/offset conversion math
(`hair_material_math.py`).

No test harness / pytest exists elsewhere in this add-on (grepped for
`test`/`pytest` under `src/Blender` before writing this — there is
none; this is the first).  Plain `unittest` (stdlib only) so it runs
with a bare `python3`, matching the constraint that the exporter's
bpy-dependent glue (reading a Blender Curves datablock, walking a
Blender node graph) can't be exercised outside Blender at all — see
`exporter.py`'s hair functions for what stays manually-validated only.

Run directly:

    python3 src/Blender/addons/rise_renderer/test_hair_export.py

or as a module from the repo root:

    python3 -m unittest src.Blender.addons.rise_renderer.test_hair_export -v

(the second form needs `src/Blender/addons/rise_renderer/__init__.py`
to NOT try to import bpy at module-import time for submodule access to
work outside Blender — it does import bpy eagerly, which is why the
directory isn't on `sys.path` as a package for this test; the direct-
script form above avoids that entirely by loading this file standalone
and importing its bpy-free sibling modules by file path.  See the
`sys.path` shim below.)
"""

from __future__ import annotations

import ctypes
import math
import os
import re
import struct
import sys
import unittest
from unittest import mock

# This file lives inside `rise_renderer/`, whose `__init__.py` imports
# bpy at module scope (it registers Blender classes on add-on enable).
# Importing the package normally would therefore fail outside Blender.
# The two modules under test are themselves bpy-free, so reach them by
# adding this directory directly to `sys.path` rather than importing
# through the package `__init__.py`.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import hair_file_writer as hfw  # noqa: E402
import hair_material_math as hmm  # noqa: E402

# `bridge.py` is bpy-free too (ctypes / os / sys / dataclasses only) and
# does NOT load the native library at import time -- `_load_library` is
# only called from `render_scene` / `get_capabilities`.  So the ctypes
# struct declarations and the `_SceneHandle._marshal_*` functions can be
# exercised here with a plain `python3`, no Blender and no built dylib.
# What CANNOT be covered without a live Blender is unchanged: reading a
# Curves datablock and walking a Principled Hair BSDF node graph
# (`exporter.py`), and the actual ctypes call into the native library.
import bridge  # noqa: E402

_BRIDGE_HEADER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "native", "rise_blender_bridge.h"
)


# ---------------------------------------------------------------------------
# A minimal, independent .hair reader — deliberately NOT sharing any code
# with hair_file_writer.write_hair_file, so the roundtrip test actually
# exercises the writer's byte layout rather than checking it against itself.
# ---------------------------------------------------------------------------

def _read_hair_file(path):
    with open(path, "rb") as handle:
        data = handle.read()

    magic = data[0:4]
    if magic != b"HAIR":
        raise ValueError(f"bad magic {magic!r}")

    num_strands, num_points, flags, default_segments = struct.unpack_from("<IIII", data, 4)
    default_thickness, default_transparency = struct.unpack_from("<ff", data, 20)
    default_color = struct.unpack_from("<fff", data, 28)
    info_raw = data[40:128]
    info = info_raw.split(b"\x00", 1)[0].decode("ascii")

    offset = 128
    segments = None
    if flags & hfw.FLAG_SEGMENTS:
        segments = list(struct.unpack_from(f"<{num_strands}H", data, offset))
        offset += 2 * num_strands
    else:
        segments = [default_segments] * num_strands

    points_flat = struct.unpack_from(f"<{3 * num_points}f", data, offset)
    offset += 4 * 3 * num_points

    thickness_flat = None
    if flags & hfw.FLAG_THICKNESS:
        thickness_flat = struct.unpack_from(f"<{num_points}f", data, offset)
        offset += 4 * num_points

    if offset > len(data):
        raise ValueError("file shorter than its own header declares")

    # Reassemble per-strand records from the flat arrays, using the
    # segments array to know how many points each strand owns (points =
    # segments + 1), exactly as a real reader would.
    strands = []
    cursor = 0
    for seg_count in segments:
        n_points = seg_count + 1
        pts = []
        for _ in range(n_points):
            base = cursor * 3
            pts.append(points_flat[base:base + 3])
            cursor += 1
        strands.append(pts)

    thickness_by_strand = None
    if thickness_flat is not None:
        thickness_by_strand = []
        cursor = 0
        for seg_count in segments:
            n_points = seg_count + 1
            thickness_by_strand.append(list(thickness_flat[cursor:cursor + n_points]))
            cursor += n_points

    return {
        "num_strands": num_strands,
        "num_points": num_points,
        "flags": flags,
        "default_segments": default_segments,
        "default_thickness": default_thickness,
        "default_transparency": default_transparency,
        "default_color": default_color,
        "info": info,
        "segments": segments,
        "strands": strands,
        "thickness_by_strand": thickness_by_strand,
        "file_size": len(data),
    }


class HairFileWriterFlagBitValueTest(unittest.TestCase):
    """Pin the five flag-bit VALUES to literal integers.

    Every other test in this file references bits via the symbolic
    `hfw.FLAG_*` names (e.g. `hfw.FLAG_THICKNESS`), which is the right
    style for readability but means those tests would still pass even
    if the constants themselves got transposed (e.g. FLAG_POINTS and
    FLAG_THICKNESS swapped in `hair_file_writer.py`) -- every assertion
    would silently move in lockstep with the bug, since both the
    writer and the reader in this test module import the same symbol.
    This test is the one place that pins the actual on-disk bit
    layout, per `HairFileLoader.h` / `hair_file_writer.py`'s own
    module docstring, against hardcoded literals -- it must never
    change these values."""

    def test_flag_bit_values(self):
        self.assertEqual(hfw.FLAG_SEGMENTS, 1)
        self.assertEqual(hfw.FLAG_POINTS, 2)
        self.assertEqual(hfw.FLAG_THICKNESS, 4)
        self.assertEqual(hfw.FLAG_TRANSPARENCY, 8)
        self.assertEqual(hfw.FLAG_COLOR, 16)


class HairFileWriterByteLayoutTest(unittest.TestCase):
    """Hand-computed offset asserts against the 128-byte header, per
    HairFileLoader.h's documented layout."""

    def test_header_offsets(self):
        strands = [
            {"points": [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0)], "thickness": None},
        ]
        path = self._write_tmp(strands, info="hello")
        with open(path, "rb") as handle:
            data = handle.read()

        self.assertEqual(len(data), 128 + 2 * 1 + 4 * 3 * 2)  # header + segments(1) + points(2)
        self.assertEqual(data[0:4], b"HAIR")
        (num_strands,) = struct.unpack_from("<I", data, 4)
        self.assertEqual(num_strands, 1)
        (num_points,) = struct.unpack_from("<I", data, 8)
        self.assertEqual(num_points, 2)
        (flags,) = struct.unpack_from("<I", data, 12)
        self.assertEqual(flags, hfw.FLAG_SEGMENTS | hfw.FLAG_POINTS)
        (default_segments,) = struct.unpack_from("<I", data, 16)
        self.assertEqual(default_segments, 0)
        info_bytes = data[40:128]
        self.assertEqual(info_bytes[:5], b"hello")
        self.assertEqual(info_bytes[5:], b"\x00" * (88 - 5))

        # Body: one uint16 segment count (= 1, i.e. 2 points - 1), then
        # 2 points * 3 floats.
        (seg0,) = struct.unpack_from("<H", data, 128)
        self.assertEqual(seg0, 1)
        px, py, pz = struct.unpack_from("<fff", data, 130)
        self.assertEqual((px, py, pz), (0.0, 0.0, 0.0))
        qx, qy, qz = struct.unpack_from("<fff", data, 130 + 12)
        self.assertEqual((qx, qy, qz), (0.0, 1.0, 0.0))

        os.remove(path)

    def test_thickness_flag_only_set_when_present(self):
        no_thickness = [{"points": [(0, 0, 0), (1, 1, 1)], "thickness": None}]
        path = self._write_tmp(no_thickness)
        with open(path, "rb") as handle:
            (flags,) = struct.unpack_from("<I", handle.read(16), 12)
        self.assertFalse(flags & hfw.FLAG_THICKNESS)
        os.remove(path)

        with_thickness = [{"points": [(0, 0, 0), (1, 1, 1)], "thickness": [0.1, 0.05]}]
        path = self._write_tmp(with_thickness)
        with open(path, "rb") as handle:
            (flags,) = struct.unpack_from("<I", handle.read(16), 12)
        self.assertTrue(flags & hfw.FLAG_THICKNESS)
        os.remove(path)

    def test_info_truncated_to_88_bytes(self):
        strands = [{"points": [(0, 0, 0), (1, 1, 1)], "thickness": None}]
        long_info = "x" * 200
        path = self._write_tmp(strands, info=long_info)
        with open(path, "rb") as handle:
            data = handle.read()
        info_field = data[40:128]
        self.assertEqual(len(info_field), 88)
        self.assertEqual(info_field, b"x" * 88)
        os.remove(path)

    @staticmethod
    def _write_tmp(strands, **kwargs):
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".hair")
        os.close(fd)
        hfw.write_hair_file(path, strands, **kwargs)
        return path


class HairFileWriterRoundtripTest(unittest.TestCase):
    def _roundtrip(self, strands, **kwargs):
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".hair")
        os.close(fd)
        try:
            hfw.write_hair_file(path, strands, **kwargs)
            return _read_hair_file(path)
        finally:
            os.remove(path)

    def test_single_straight_strand_no_thickness(self):
        strands = [
            {"points": [(0.0, 0.0, 0.0), (0.0, 0.5, 0.0), (0.0, 1.0, 0.0)], "thickness": None},
        ]
        result = self._roundtrip(strands)
        self.assertEqual(result["num_strands"], 1)
        self.assertEqual(result["num_points"], 3)
        self.assertFalse(result["flags"] & hfw.FLAG_THICKNESS)
        self.assertEqual(result["strands"], [
            [(0.0, 0.0, 0.0), (0.0, 0.5, 0.0), (0.0, 1.0, 0.0)],
        ])
        self.assertIsNone(result["thickness_by_strand"])

    def test_ragged_strand_counts(self):
        # Three strands with different point counts (2, 5, 3) -- the
        # normal case for a real groom, and the case most likely to
        # expose an off-by-one in the flat-array offset bookkeeping.
        strands = [
            {"points": [(0, 0, 0), (0, 1, 0)], "thickness": None},
            {
                "points": [
                    (1, 0, 0), (1, 0.25, 0.1), (1, 0.5, 0.2), (1, 0.75, 0.1), (1, 1.0, 0.0),
                ],
                "thickness": None,
            },
            {"points": [(2, 0, 0), (2, 0.5, 0), (2, 1.0, 0)], "thickness": None},
        ]
        result = self._roundtrip(strands)
        self.assertEqual(result["num_strands"], 3)
        self.assertEqual(result["num_points"], 2 + 5 + 3)
        self.assertEqual(result["segments"], [1, 4, 2])
        self.assertEqual(len(result["strands"][0]), 2)
        self.assertEqual(len(result["strands"][1]), 5)
        self.assertEqual(len(result["strands"][2]), 3)
        # Spot-check a couple of coordinates land in the right strand
        # (catches a flat-array indexing bug that would shift points
        # between strands without changing counts).  float32 storage
        # means an exact equality would be fragile -- compare to
        # single-precision tolerance.
        for got, want in zip(result["strands"][1][2], (1.0, 0.5, 0.2)):
            self.assertAlmostEqual(got, want, places=6)
        for got, want in zip(result["strands"][2][1], (2.0, 0.5, 0.0)):
            self.assertAlmostEqual(got, want, places=6)

    def test_thickness_roundtrips_and_is_full_width(self):
        strands = [
            {"points": [(0, 0, 0), (0, 1, 0), (0, 2, 0)], "thickness": [0.002, 0.0015, 0.0005]},
            {"points": [(1, 0, 0), (1, 1, 0)], "thickness": [0.001, 0.0003]},
        ]
        result = self._roundtrip(strands)
        self.assertTrue(result["flags"] & hfw.FLAG_THICKNESS)
        self.assertAlmostEqual(result["thickness_by_strand"][0][0], 0.002, places=6)
        self.assertAlmostEqual(result["thickness_by_strand"][0][2], 0.0005, places=6)
        self.assertAlmostEqual(result["thickness_by_strand"][1][1], 0.0003, places=6)

    def test_header_scalar_fields_roundtrip(self):
        strands = [{"points": [(0, 0, 0), (1, 0, 0)], "thickness": None}]
        result = self._roundtrip(
            strands,
            default_segments=8,
            default_thickness=0.0001,
            default_transparency=0.5,
            default_color=(0.1, 0.2, 0.3),
            info="unit test",
        )
        self.assertEqual(result["default_segments"], 8)
        self.assertAlmostEqual(result["default_thickness"], 0.0001, places=8)
        self.assertAlmostEqual(result["default_transparency"], 0.5, places=6)
        for got, want in zip(result["default_color"], (0.1, 0.2, 0.3)):
            self.assertAlmostEqual(got, want, places=6)
        self.assertEqual(result["info"], "unit test")


class HairFileWriterErrorTest(unittest.TestCase):
    def test_empty_strand_list_rejected(self):
        with self.assertRaises(hfw.HairFileError):
            hfw.write_hair_file("/tmp/should_not_be_written.hair", [])

    def test_strand_with_one_point_rejected(self):
        with self.assertRaises(hfw.HairFileError):
            hfw.write_hair_file(
                "/tmp/should_not_be_written.hair",
                [{"points": [(0, 0, 0)], "thickness": None}],
            )

    def test_mismatched_thickness_length_rejected(self):
        with self.assertRaises(hfw.HairFileError):
            hfw.write_hair_file(
                "/tmp/should_not_be_written.hair",
                [{"points": [(0, 0, 0), (1, 1, 1)], "thickness": [0.1]}],
            )

    def test_inconsistent_thickness_presence_rejected(self):
        with self.assertRaises(hfw.HairFileError):
            hfw.write_hair_file(
                "/tmp/should_not_be_written.hair",
                [
                    {"points": [(0, 0, 0), (1, 1, 1)], "thickness": [0.1, 0.1]},
                    {"points": [(0, 0, 0), (1, 1, 1)], "thickness": None},
                ],
            )


class BuildHairStrandsFromCurveArraysTest(unittest.TestCase):
    def test_basic_two_curves(self):
        # Two curves: first has 2 points, second has 3.
        positions = [
            0.0, 0.0, 0.0,   0.0, 1.0, 0.0,             # curve 0
            1.0, 0.0, 0.0,   1.0, 0.5, 0.0,   1.0, 1.0, 0.0,  # curve 1
        ]
        offsets = [0, 2, 5]
        strands = hfw.build_hair_strands_from_curve_arrays(positions, offsets)
        self.assertEqual(len(strands), 2)
        self.assertEqual(strands[0]["points"], [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0)])
        self.assertEqual(
            strands[1]["points"],
            [(1.0, 0.0, 0.0), (1.0, 0.5, 0.0), (1.0, 1.0, 0.0)],
        )
        self.assertIsNone(strands[0]["thickness"])
        self.assertIsNone(strands[1]["thickness"])

    def test_radius_becomes_full_width_thickness(self):
        positions = [0.0, 0.0, 0.0, 0.0, 1.0, 0.0]
        offsets = [0, 2]
        radii = [0.001, 0.0005]
        strands = hfw.build_hair_strands_from_curve_arrays(positions, offsets, radii)
        self.assertEqual(len(strands), 1)
        self.assertEqual(strands[0]["thickness"], [0.002, 0.001])

    def test_degenerate_single_point_curve_is_dropped(self):
        # A curve with exactly 1 point can't be a strand (needs >= 1
        # segment). Ragged input: curve 0 has 1 point (dropped), curve
        # 1 has 3 (kept).
        positions = [
            0.0, 0.0, 0.0,
            1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 2.0, 0.0,
        ]
        offsets = [0, 1, 4]
        strands = hfw.build_hair_strands_from_curve_arrays(positions, offsets)
        self.assertEqual(len(strands), 1)
        self.assertEqual(len(strands[0]["points"]), 3)

    def test_zero_curves_returns_empty(self):
        self.assertEqual(hfw.build_hair_strands_from_curve_arrays([], [0]), [])

    def test_writer_accepts_builder_output_end_to_end(self):
        # Full pipeline: raw arrays -> strand records -> .hair bytes ->
        # independent reader, including the radius-to-full-width step.
        positions = [
            0.0, 0.0, 0.0,  0.0, 1.0, 0.0,  0.0, 2.0, 0.0,
            1.0, 0.0, 0.0,  1.0, 1.0, 0.0,
        ]
        offsets = [0, 3, 5]
        radii = [0.001, 0.0009, 0.0003, 0.0012, 0.0004]
        strands = hfw.build_hair_strands_from_curve_arrays(positions, offsets, radii)

        import tempfile
        fd, path = tempfile.mkstemp(suffix=".hair")
        os.close(fd)
        try:
            hfw.write_hair_file(path, strands, info="pipeline test")
            result = _read_hair_file(path)
        finally:
            os.remove(path)

        self.assertEqual(result["num_strands"], 2)
        self.assertEqual(result["num_points"], 5)
        self.assertAlmostEqual(result["thickness_by_strand"][0][0], 0.002, places=6)
        self.assertAlmostEqual(result["thickness_by_strand"][1][1], 0.0008, places=6)


class MelaninConversionTest(unittest.TestCase):
    def test_zero_melanin_gives_zero_pigments(self):
        eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(0.0, 0.0)
        self.assertAlmostEqual(eu, 0.0, places=9)
        self.assertAlmostEqual(pheo, 0.0, places=9)

    def test_zero_melanin_zero_pigments_regardless_of_redness(self):
        eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(0.0, 1.0)
        self.assertAlmostEqual(eu, 0.0, places=9)
        self.assertAlmostEqual(pheo, 0.0, places=9)

    def test_pure_eumelanin_no_redness(self):
        eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(0.9, 0.0)
        expected_qty = -math.log(0.1)
        self.assertAlmostEqual(eu, expected_qty, places=6)
        self.assertAlmostEqual(pheo, 0.0, places=9)

    def test_pure_pheomelanin_full_redness(self):
        eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(0.9, 1.0)
        expected_qty = -math.log(0.1)
        self.assertAlmostEqual(pheo, expected_qty, places=6)
        self.assertAlmostEqual(eu, 0.0, places=9)

    def test_half_redness_splits_evenly(self):
        eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(0.9, 0.5)
        self.assertAlmostEqual(eu, pheo, places=9)

    def test_melanin_near_one_is_capped_not_infinite(self):
        eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(1.0, 0.0)
        expected_qty = -math.log(1e-4)
        self.assertAlmostEqual(eu, expected_qty, places=6)
        self.assertTrue(math.isfinite(eu))

    def test_out_of_range_inputs_are_clamped(self):
        eu_over, pheo_over = hmm.melanin_to_eumelanin_pheomelanin(1.5, 2.0)
        eu_clamped, pheo_clamped = hmm.melanin_to_eumelanin_pheomelanin(1.0, 1.0)
        self.assertAlmostEqual(eu_over, eu_clamped, places=9)
        self.assertAlmostEqual(pheo_over, pheo_clamped, places=9)

        eu_neg, pheo_neg = hmm.melanin_to_eumelanin_pheomelanin(-0.5, -1.0)
        eu_zero, pheo_zero = hmm.melanin_to_eumelanin_pheomelanin(0.0, 0.0)
        self.assertAlmostEqual(eu_neg, eu_zero, places=9)
        self.assertAlmostEqual(pheo_neg, pheo_zero, places=9)

    def test_outputs_always_nonnegative(self):
        for m in (0.0, 0.1, 0.3, 0.5, 0.7, 0.9, 0.99, 1.0):
            for r in (0.0, 0.25, 0.5, 0.75, 1.0):
                eu, pheo = hmm.melanin_to_eumelanin_pheomelanin(m, r)
                self.assertGreaterEqual(eu, 0.0)
                self.assertGreaterEqual(pheo, 0.0)


class OffsetConversionTest(unittest.TestCase):
    def test_blender_default_offset_is_two_degrees(self):
        # Blender's Principled Hair BSDF default Offset is 2 degrees,
        # stored as ~0.0349066 radians (the ANGLE-subtype socket
        # default) -- confirms the round-trip matches the documented
        # default in docs/BLENDER_MATERIAL_TRANSLATION.md.
        degrees = hmm.offset_radians_to_alpha_degrees(0.0349065850398866)
        self.assertAlmostEqual(degrees, 2.0, places=4)

    def test_zero_offset(self):
        self.assertAlmostEqual(hmm.offset_radians_to_alpha_degrees(0.0), 0.0, places=9)

    def test_negative_offset(self):
        degrees = hmm.offset_radians_to_alpha_degrees(-math.radians(3.0))
        self.assertAlmostEqual(degrees, -3.0, places=6)


# ---------------------------------------------------------------------------
# Bridge (ABI v9) — the ctypes mirror of the native hair structs, and the
# marshalling that fills them.
#
# The drift risk these guard is specific and has no compiler behind it:
# `bridge.py` re-declares C structs by hand, and a field added to the .h
# without the matching ctypes entry does not fail to build — it silently
# misaligns every field after it, and the native side reads garbage.  So
# the layout tests below parse the actual header and compare it, field by
# field and type by type, with the Python declaration.
# ---------------------------------------------------------------------------


def _parse_c_struct_fields(source, struct_name):
    """Field (name, canonical-type) pairs of one `typedef struct` in the
    bridge header, in declaration order.  Deliberately a small, strict
    parser rather than a general C one: it understands exactly the shapes
    the bridge ABI uses (scalar, pointer, fixed array) and raises on
    anything else, so a future field in an unfamiliar shape fails loudly
    here instead of being skipped silently."""

    match = re.search(
        r"typedef struct " + struct_name + r"\s*\{(.*?)\}\s*" + struct_name + r"\s*;",
        source,
        re.S,
    )
    if match is None:
        raise AssertionError(f"struct {struct_name} not found in {_BRIDGE_HEADER}")

    body = match.group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)          # block comments
    body = re.sub(r"//[^\n]*", "", body)                        # line comments

    fields = []
    for statement in body.split(";"):
        statement = " ".join(statement.split())
        if not statement:
            continue
        array_match = re.match(r"^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*(\d+)\s*\]$", statement)
        if array_match:
            type_text = " ".join(array_match.group(1).split())
            fields.append((array_match.group(2), f"{type_text}[{array_match.group(3)}]"))
            continue
        scalar_match = re.match(r"^(.*?[\s\*])([A-Za-z_][A-Za-z0-9_]*)$", statement)
        if scalar_match is None:
            raise AssertionError(f"unparsed field in {struct_name}: {statement!r}")
        type_text = " ".join(scalar_match.group(1).split())
        fields.append((scalar_match.group(2), type_text))
    return fields


_CTYPE_TO_C = {
    ctypes.c_char_p: "const char*",
    ctypes.c_int: "int",
    ctypes.c_float: "float",
    ctypes.c_uint32: "uint32_t",
    ctypes.c_double: "double",
}

_POINTER_TO_C = {
    bridge._Camera: "const rise_blender_camera*",
    bridge._Painter: "const rise_blender_painter*",
    bridge._Modifier: "const rise_blender_modifier*",
    bridge._Material: "const rise_blender_material*",
    bridge._Mesh: "const rise_blender_mesh*",
    bridge._Object: "const rise_blender_object*",
    bridge._Light: "const rise_blender_light*",
    bridge._Medium: "const rise_blender_medium*",
    bridge._HairMaterial: "const rise_blender_hair_material*",
    bridge._HairObject: "const rise_blender_hair_object*",
    # The result image buffer -- the one non-const pointer in the ABI.
    ctypes.c_float: "float*",
    ctypes.c_uint32: "const uint32_t*",
}


def _canonical_ctype(ctype):
    if ctype in _CTYPE_TO_C:
        return _CTYPE_TO_C[ctype]
    element = getattr(ctype, "_type_", None)
    length = getattr(ctype, "_length_", None)
    if element is not None and length is not None:
        if element is ctypes.c_float:
            return f"float[{length}]"
        if element is ctypes.c_char:
            return f"char[{length}]"
        raise AssertionError(f"unmapped array element type {element}")
    if element is not None and length is None:
        # A POINTER(...) type.
        if element in _POINTER_TO_C:
            return _POINTER_TO_C[element]
        raise AssertionError(f"unmapped pointer target {element}")
    raise AssertionError(f"unmapped ctype {ctype}")


class BridgeAbiLayoutTest(unittest.TestCase):
    """`bridge.py`'s ctypes declarations against the C header they mirror."""

    @classmethod
    def setUpClass(cls):
        with open(_BRIDGE_HEADER, "r", encoding="utf-8") as handle:
            cls.source = handle.read()

    @staticmethod
    def _drop_pointer_const(fields):
        # ctypes has no notion of const -- POINTER(c_float) is the same
        # Python type whether the C field is `float*` or `const float*`
        # -- and `_POINTER_TO_C` maps each pointee ctype to a single
        # canonical string (picked to match whichever struct needed it
        # first; see its "one non-const pointer in the ABI" comment).
        # `rise_blender_mesh.vertices/normals/uvs` are `const float*`
        # while `rise_blender_render_result.rgba` is a plain `float*`,
        # so comparing the literal header text against that one fixed
        # string would fail one of them no matter which way it's
        # mapped, on a qualifier that carries no ABI/layout meaning.
        # Strip it from both sides so the comparison is exact on what
        # actually determines memory layout (field order, scalar width,
        # pointer-vs-array-vs-scalar) without going stale over an
        # unrelated const annotation.
        return [(name, type_text.replace("const ", "")) for name, type_text in fields]

    def _assert_matches(self, python_struct, c_name):
        expected = _parse_c_struct_fields(self.source, c_name)
        actual = [(name, _canonical_ctype(ctype)) for name, ctype in python_struct._fields_]
        self.assertEqual(
            self._drop_pointer_const(actual),
            self._drop_pointer_const(expected),
            f"{python_struct.__name__} drifted from {c_name}",
        )

    def test_expected_api_version_matches_the_header(self):
        match = re.search(r"#define RISE_BLENDER_API_VERSION\s+(\d+)", self.source)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), bridge._EXPECTED_API_VERSION)
        self.assertEqual(bridge._EXPECTED_API_VERSION, 13)

    def test_stale_dylib_version_fails_loudly(self):
        # Simulate a v12 dylib (built before this ABI bump) sitting
        # next to a v13 add-on: `_load_library`'s version check must
        # refuse it with a clear message, not silently marshal v13
        # fields (coat_*/subsurface_*) into a v12 struct layout the
        # native side never declared.  `ctypes.CDLL` is mocked rather
        # than shipping a stale .dylib fixture -- the real bridge in
        # this worktree is already v13, so a genuine stale binary isn't
        # available to load.
        bridge._LOADED_LIBRARY = None
        bridge._LOADED_PATH = None
        bridge._LOADED_CAPABILITIES = None
        fake_library = mock.MagicMock()
        fake_library.rise_blender_api_version.return_value = bridge._EXPECTED_API_VERSION - 1
        with mock.patch.object(bridge.ctypes, "CDLL", return_value=fake_library):
            with self.assertRaises(bridge.BridgeError) as ctx:
                bridge._load_library()
        message = str(ctx.exception)
        self.assertIn(str(bridge._EXPECTED_API_VERSION - 1), message)
        self.assertIn(str(bridge._EXPECTED_API_VERSION), message)
        self.assertIn("Rebuild src/Blender/native", message)

    def test_hair_material_struct_matches(self):
        self._assert_matches(bridge._HairMaterial, "rise_blender_hair_material")

    def test_hair_object_struct_matches(self):
        self._assert_matches(bridge._HairObject, "rise_blender_hair_object")

    def test_scene_struct_matches(self):
        # The whole scene struct, not just the v9 tail: an insertion
        # anywhere above the hair fields would shift them.
        self._assert_matches(bridge._Scene, "rise_blender_scene")

    def test_render_result_struct_matches(self):
        self._assert_matches(bridge._RenderResult, "rise_blender_render_result")

    def test_all_pointer_target_structs_match(self):
        """Every ctypes.Structure named as a POINTER(...) target in
        `_POINTER_TO_C`, not just the four hand-picked structs checked
        by name above.  Without this, a struct newly added to
        `_POINTER_TO_C` (e.g. `bridge._Medium` for
        `rise_blender_medium`) passes `_canonical_ctype` resolution --
        that dict only needs to exist for pointer-field decoding -- with
        no test ever comparing its `_fields_` against the header, so a
        drift in that struct's layout would go undetected."""
        for python_struct, c_type in _POINTER_TO_C.items():
            if not (isinstance(python_struct, type) and issubclass(python_struct, ctypes.Structure)):
                continue  # ctypes.c_float / ctypes.c_uint32: the result-buffer pointer, not a struct.
            match = re.match(r"^const (\w+)\s*\*$", c_type)
            self.assertIsNotNone(match, f"unparsed pointer C type {c_type!r} for {python_struct.__name__}")
            with self.subTest(struct=python_struct.__name__):
                self._assert_matches(python_struct, match.group(1))

    def test_tier_tags_match_the_header_enum(self):
        for name, value in (
            ("RISE_BLENDER_HAIR_TIER_MELANIN", bridge.HAIR_TIER_MELANIN),
            ("RISE_BLENDER_HAIR_TIER_SIGMA_A", bridge.HAIR_TIER_SIGMA_A),
            ("RISE_BLENDER_HAIR_TIER_COLOR", bridge.HAIR_TIER_COLOR),
        ):
            match = re.search(name + r"\s*=\s*(\d+)", self.source)
            self.assertIsNotNone(match, f"{name} missing from the header")
            self.assertEqual(int(match.group(1)), value, f"{name} tag drifted")


_EXPORTER_SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "exporter.py")


class ExporterHairTextureGatingTest(unittest.TestCase):
    """The v10 gating rule inside `_hair_material_payload`, checked at
    the SOURCE level.

    Why source level and not behaviour: `exporter.py` imports `bpy` at
    module scope and `_hair_material_payload` walks a live Blender node
    graph, so it cannot be called outside Blender at all (the module
    docstring above says the same about every other bpy-dependent
    exporter function).  What CAN be pinned without Blender is the rule
    the rest of the pipe depends on -- that each `*_texture_painter_name`
    is populated ONLY under its `*_texture` guard, so an unbound socket
    sends `None` and the native side uses the number.  A behavioural
    version of this test needs a Blender-in-the-loop harness that does
    not exist here."""

    @classmethod
    def setUpClass(cls):
        with open(_EXPORTER_SOURCE, "r", encoding="utf-8") as handle:
            cls.source = handle.read()

    def _hair_material_payload_body(self):
        start = self.source.index("def _hair_material_payload(")
        end = self.source.index("\ndef ", start + 1)
        return self.source[start:end]

    def test_each_texture_field_is_gated_on_a_resolved_texture(self):
        body = self._hair_material_payload_body()
        for slot in ("beta_m", "beta_n", "ior"):
            with self.subTest(slot=slot):
                # `<slot> = _scalar_or_texture_painter(...) if <slot>_texture else None`
                self.assertRegex(
                    body,
                    rf"{slot}\s*=\s*\(\s*\n\s*_scalar_or_texture_painter\(.*?\n\s*if {slot}_texture\s*\n\s*else None",
                    f"{slot} is no longer gated on a resolved texture",
                )

    def test_every_tier_branch_forwards_the_texture_field(self):
        body = self._hair_material_payload_body()
        for slot in ("beta_m", "beta_n", "ior"):
            with self.subTest(slot=slot):
                # One per colour tier: color, sigma_a, melanin.
                self.assertEqual(
                    body.count(f"{slot}_texture_painter_name={slot},"),
                    3,
                    f"{slot}_texture_painter_name is not forwarded by all three tier branches",
                )

    def test_the_flattening_warning_is_gone(self):
        # The pre-v10 warning claimed roughness / IOR textures "cannot be
        # sent to the renderer".  They can now; the warning would be a
        # false statement in the artist's Blender info log.
        self.assertNotIn("at their constant values", self.source)


class ExporterSpecularTintGatingTest(unittest.TestCase):
    """DL-151 (docs/DEBT_LEDGER.md; no source-doc heading, opened by the
    debt-dl18 slice's sibling audit): `_material_payload` never read
    Blender's Principled "Specular Tint" socket at all, so
    `specular_color_painter_name` -- an ABI field that has existed
    since Landing 7 and that `rise_blender_bridge.cpp`'s
    `add_pbr_metallic_roughness_material` already forwards correctly
    -- stayed `None` (the ABI's own "no tint" default) on EVERY
    exported material, tinted or not.

    RED-PROOF HISTORY: against the pre-fix source, `test_specular_tint_socket_is_read`
    and `test_the_payload_forwards_specular_color_painter_name` both
    failed -- `_material_payload`'s body contained no reference to
    "Specular Tint" at all, and its `MaterialData(...)` call had no
    `specular_color_painter_name=` keyword.  See the fix commit
    message for the captured failure text.

    Why source level and not behaviour: same reasoning as
    `ExporterHairTextureGatingTest` above -- `exporter.py` imports
    `bpy` at module scope and `_material_payload` walks a live
    Blender node graph, so it cannot be called outside Blender at
    all.  What CAN be pinned without Blender is that the function's
    SOURCE reads the socket and forwards what it reads."""

    @classmethod
    def setUpClass(cls):
        with open(_EXPORTER_SOURCE, "r", encoding="utf-8") as handle:
            cls.source = handle.read()

    def _material_payload_body(self):
        start = self.source.index("def _material_payload(")
        end = self.source.index("\ndef ", start + 1)
        return self.source[start:end]

    def test_specular_tint_socket_is_read(self):
        body = self._material_payload_body()
        self.assertIn(
            '_node_input(principled_node, "Specular Tint")',
            body,
            "_material_payload no longer reads the Specular Tint socket",
        )

    def test_the_payload_forwards_specular_color_painter_name(self):
        body = self._material_payload_body()
        self.assertIn(
            "specular_color_painter_name=specular_color_painter,",
            body,
            "the PBR MaterialData(...) payload no longer forwards a computed "
            "specular_color_painter_name -- specular tint would silently stop "
            "reaching the bridge again",
        )

    def test_both_blender_versions_are_dispatched_by_socket_type(self):
        # Blender 4.x's "Specular Tint" is an RGB colour socket; pre-4.0
        # Blender used the SAME socket name for a float 0..1 slider.
        # This add-on's bl_info declares Blender 4.0 as its minimum
        # supported version, so only the colour branch is reachable in
        # practice, but the source should still dispatch on the
        # socket's own `.type` (this file's existing `inp.type ==
        # "RGBA"` convention), not silently assume one shape.
        body = self._material_payload_body()
        self.assertIn('specular_tint_socket.type == "VALUE"', body)

    def test_a_default_white_tint_produces_no_painter(self):
        # Bit-identical-to-pre-fix contract: a material that never
        # touches Specular Tint (or leaves it at Blender's default
        # white) must still send specular_color_painter_name=None, the
        # ABI's pre-existing "no tint" sentinel -- not a redundant
        # "white" uniform painter on every single PBR material ever
        # exported.
        body = self._material_payload_body()
        self.assertIn("specular_color_painter = None", body)
        self.assertIn("is_default_white", body)


class _StubHairMaterial:
    """The subset of `exporter.HairMaterialData` the bridge marshals.
    Deliberately a stand-in rather than the real dataclass: importing
    `exporter` pulls in bpy."""

    def __init__(self, **kwargs):
        self.name = "hairmat"
        self.tier = "melanin"
        self.color_painter_name = None
        self.sigma_a = (0.0, 0.0, 0.0)
        self.eumelanin = 0.0
        self.pheomelanin = 0.0
        self.beta_m = 0.3
        self.beta_n = 0.3
        self.alpha_degrees = 2.0
        self.ior = 1.55
        self.apply_melanin_parity_rescale = True
        # ABI v10 -- None unless the Blender socket was texture-driven.
        self.beta_m_texture_painter_name = None
        self.beta_n_texture_painter_name = None
        self.ior_texture_painter_name = None
        for key, value in kwargs.items():
            setattr(self, key, value)


class _StubHairObject:
    def __init__(self, **kwargs):
        self.name = "hairobj"
        self.file_path = "/tmp/groom.hair"
        self.material_name = "hairmat"
        self.transform = [1.0 if i % 5 == 0 else 0.0 for i in range(16)]
        self.width_root_scale = 1.0
        self.width_tip_scale = 1.0
        for key, value in kwargs.items():
            setattr(self, key, value)


def _handle():
    """A `_SceneHandle` with only its keepalive list initialised.  The
    real `__init__` marshals an entire scene (camera, settings, meshes);
    the hair marshallers need nothing but the string keepalive, so this
    exercises them without inventing a whole stub scene."""

    handle = bridge._SceneHandle.__new__(bridge._SceneHandle)
    handle.keepalive = []
    return handle


class BridgeHairMarshallingTest(unittest.TestCase):
    def test_melanin_tier(self):
        payload = _handle()._marshal_hair_material(
            _StubHairMaterial(name="brown", tier="melanin", eumelanin=1.3, pheomelanin=0.25)
        )
        self.assertEqual(payload.name, b"brown")
        self.assertEqual(payload.tier, bridge.HAIR_TIER_MELANIN)
        self.assertAlmostEqual(payload.eumelanin, 1.3, places=6)
        self.assertAlmostEqual(payload.pheomelanin, 0.25, places=6)
        # Parity is the shipped default; the native side does the actual
        # rescale (see rise_blender_bridge.cpp's constants).
        self.assertEqual(payload.apply_melanin_parity_rescale, 1)

    def test_parity_rescale_can_be_switched_off(self):
        payload = _handle()._marshal_hair_material(
            _StubHairMaterial(apply_melanin_parity_rescale=False)
        )
        self.assertEqual(payload.apply_melanin_parity_rescale, 0)

    def test_sigma_a_tier(self):
        payload = _handle()._marshal_hair_material(
            _StubHairMaterial(tier="sigma_a", sigma_a=(0.245, 0.46, 1.6))
        )
        self.assertEqual(payload.tier, bridge.HAIR_TIER_SIGMA_A)
        self.assertAlmostEqual(payload.sigma_a[0], 0.245, places=6)
        self.assertAlmostEqual(payload.sigma_a[1], 0.46, places=6)
        self.assertAlmostEqual(payload.sigma_a[2], 1.6, places=6)

    def test_color_tier_carries_a_painter_name(self):
        payload = _handle()._marshal_hair_material(
            _StubHairMaterial(tier="color", color_painter_name="tint")
        )
        self.assertEqual(payload.tier, bridge.HAIR_TIER_COLOR)
        self.assertEqual(payload.color_painter_name, b"tint")

    def test_unknown_tier_is_sent_as_an_invalid_tag(self):
        # Version skew between the add-on and a stale/newer exporter.
        # -1 is not a valid tier, so the native side skips that material
        # with a named warning instead of guessing one.
        payload = _handle()._marshal_hair_material(_StubHairMaterial(tier="teal"))
        self.assertEqual(payload.tier, -1)

    def test_shared_parameters(self):
        payload = _handle()._marshal_hair_material(
            _StubHairMaterial(beta_m=0.2, beta_n=0.4, alpha_degrees=3.5, ior=1.6)
        )
        self.assertAlmostEqual(payload.beta_m, 0.2, places=6)
        self.assertAlmostEqual(payload.beta_n, 0.4, places=6)
        self.assertAlmostEqual(payload.alpha_degrees, 3.5, places=6)
        self.assertAlmostEqual(payload.ior, 1.6, places=6)

    def test_texture_driven_scalar_slots_travel_as_painter_names(self):
        # ABI v10.  A texture-driven Roughness / Radial Roughness / IOR
        # reaches the native side as the NAME of a colour painter, which
        # the bridge wraps into an IScalarPainter.  The numeric fields
        # ride along unchanged as the per-slot fallback.
        payload = _handle()._marshal_hair_material(
            _StubHairMaterial(
                beta_m_texture_painter_name="mat_beta_m",
                beta_n_texture_painter_name="mat_beta_n",
                ior_texture_painter_name="mat_ior",
            )
        )
        self.assertEqual(payload.beta_m_texture_painter_name, b"mat_beta_m")
        self.assertEqual(payload.beta_n_texture_painter_name, b"mat_beta_n")
        self.assertEqual(payload.ior_texture_painter_name, b"mat_ior")

    def test_untextured_scalar_slots_send_no_painter_name(self):
        # The common case, and the one the "empty => use the number"
        # contract rests on: no texture bound means a NULL pointer, not
        # the name of a synthesized uniform painter.
        payload = _handle()._marshal_hair_material(_StubHairMaterial())
        self.assertIsNone(payload.beta_m_texture_painter_name)
        self.assertIsNone(payload.beta_n_texture_painter_name)
        self.assertIsNone(payload.ior_texture_painter_name)

    def test_a_pre_v10_exporter_payload_still_marshals(self):
        # `_marshal_hair_material` reads the three v10 fields with
        # getattr defaults, so an older exporter object that has never
        # heard of them marshals as "no texture" rather than raising.
        stub = _StubHairMaterial()
        del stub.beta_m_texture_painter_name
        del stub.beta_n_texture_painter_name
        del stub.ior_texture_painter_name
        payload = _handle()._marshal_hair_material(stub)
        self.assertIsNone(payload.beta_m_texture_painter_name)
        self.assertAlmostEqual(payload.beta_m, 0.3, places=6)

    def test_hair_object(self):
        transform = [float(i) for i in range(16)]
        payload = _handle()._marshal_hair_object(
            _StubHairObject(
                name="fur",
                file_path="/tmp/fur.hair",
                material_name="brown",
                transform=transform,
                width_root_scale=2.0,
                width_tip_scale=0.5,
            )
        )
        self.assertEqual(payload.name, b"fur")
        self.assertEqual(payload.file_path, b"/tmp/fur.hair")
        self.assertEqual(payload.material_name, b"brown")
        self.assertEqual(list(payload.transform), transform)
        self.assertAlmostEqual(payload.width_root_scale, 2.0, places=6)
        self.assertAlmostEqual(payload.width_tip_scale, 0.5, places=6)
        # The exporter's HairObjectData carries no visibility/shadow
        # fields yet; the marshaller must default them to "on" rather
        # than to zero, or every groom would render invisible.
        self.assertEqual(payload.visible, 1)
        self.assertEqual(payload.casts_shadows, 1)
        self.assertEqual(payload.receives_shadows, 1)

    def test_marshalled_strings_stay_alive(self):
        # The ctypes payloads hold raw `char*` into buffers the handle
        # must keep referenced; a dropped keepalive is a use-after-free
        # the native side reads as a corrupted name.
        handle = _handle()
        handle._marshal_hair_object(_StubHairObject())
        self.assertGreaterEqual(len(handle.keepalive), 3)


class _StubMaterial:
    """The subset of `exporter.MaterialData` the bridge marshals.
    Deliberately a stand-in rather than the real dataclass: importing
    `exporter` pulls in bpy (see `_StubHairMaterial` above, same
    reasoning)."""

    def __init__(self, **kwargs):
        self.name = "mat"
        self.model = 3  # MATERIAL_PBR_METALLIC_ROUGHNESS
        self.diffuse_painter_name = None
        self.specular_painter_name = None
        self.alpha_x_painter_name = None
        self.alpha_y_painter_name = None
        self.ior_painter_name = None
        self.extinction_painter_name = None
        self.tau_painter_name = None
        self.scatter_painter_name = None
        self.emission_painter_name = None
        self.double_sided = True
        self.base_color_painter_name = "base"
        self.metallic_painter_name = "metallic"
        self.roughness_painter_name = "roughness"
        self.specular_factor_painter_name = None
        self.specular_color_painter_name = None
        self.anisotropy_factor_painter_name = None
        self.anisotropy_rotation_painter_name = None
        self.emissive_scale = 1.0
        # ABI v12 -- DL-18 (docs/DEBT_LEDGER.md; source heading
        # CLOTH_FABRIC_DESIGN.md §15 item 13).  None / 0.0 means "no
        # sheen", matching every payload built before this debt closed.
        self.sheen_color_painter_name = None
        self.sheen_roughness = 0.0
        self.sheen_roughness_texture_painter_name = None
        # ABI v13 -- DL-186 (docs/DEBT_LEDGER.md).  None / defaults mean
        # "no coat" / "no subsurface", matching every payload built
        # before this debt closed.
        self.coat_weight_texture_painter_name = None
        self.coat_weight = 0.0
        self.coat_tint_painter_name = None
        self.coat_roughness_texture_painter_name = None
        self.coat_roughness = 0.03
        self.coat_ior = 1.5
        self.subsurface_absorption = None
        self.subsurface_scattering = None
        self.subsurface_ior = 1.4
        self.subsurface_g = 0.0
        self.subsurface_roughness = 0.0
        for key, value in kwargs.items():
            setattr(self, key, value)


class BridgeMaterialSheenMarshallingTest(unittest.TestCase):
    """ABI v12's three `sheen_*` fields on `_marshal_material` (DL-18:
    Principled BSDF Sheen -> `fabric_material`, docs/DEBT_LEDGER.md /
    CLOTH_FABRIC_DESIGN.md §15 item 13).

    RED-PROOF HISTORY: before this debt closed, `bridge._Material` had
    no `sheen_*` fields at all, so the assignments in
    `_marshal_material` this test exercises raised
    `AttributeError: 'c_char_p' object has no attribute ...` /
    `ValueError` from ctypes the moment a stub carrying these
    attributes was marshalled -- there was no sheen slot to set.  See
    the fix commit message for the captured traceback."""

    def test_no_sheen_sends_null_and_zero(self):
        payload = _handle()._marshal_material(_StubMaterial())
        self.assertIsNone(payload.sheen_color_painter_name)
        self.assertAlmostEqual(payload.sheen_roughness, 0.0, places=6)
        self.assertIsNone(payload.sheen_roughness_texture_painter_name)

    def test_sheen_color_and_numeric_roughness_travel_through(self):
        payload = _handle()._marshal_material(
            _StubMaterial(sheen_color_painter_name="mat_sheen_color", sheen_roughness=0.3)
        )
        self.assertEqual(payload.sheen_color_painter_name, b"mat_sheen_color")
        self.assertAlmostEqual(payload.sheen_roughness, 0.3, places=6)
        self.assertIsNone(payload.sheen_roughness_texture_painter_name)

    def test_textured_sheen_roughness_travels_as_a_painter_name(self):
        # The one v12 texture exception, mirroring hair's v10
        # beta_m/beta_n/ior pattern: `sheen_roughness` is a
        # `fabric_material` IScalarPainter slot, so a texture-driven
        # value travels as the NAME of a registered colour painter,
        # which the native side wraps -- not as a raw number.
        payload = _handle()._marshal_material(
            _StubMaterial(
                sheen_color_painter_name="mat_sheen_color",
                sheen_roughness=0.5,
                sheen_roughness_texture_painter_name="mat_sheen_rough_tex",
            )
        )
        self.assertEqual(payload.sheen_roughness_texture_painter_name, b"mat_sheen_rough_tex")
        self.assertAlmostEqual(payload.sheen_roughness, 0.5, places=6)

    def test_a_pre_v12_exporter_payload_still_marshals(self):
        # `_marshal_material` reads the three v12 fields with getattr
        # defaults, so an older exporter object that has never heard of
        # them marshals as "no sheen" rather than raising -- the same
        # back-compat contract hair's v10 fields established.
        stub = _StubMaterial()
        del stub.sheen_color_painter_name
        del stub.sheen_roughness
        del stub.sheen_roughness_texture_painter_name
        payload = _handle()._marshal_material(stub)
        self.assertIsNone(payload.sheen_color_painter_name)
        self.assertAlmostEqual(payload.sheen_roughness, 0.0, places=6)
        self.assertIsNone(payload.sheen_roughness_texture_painter_name)

    def test_sheen_and_emission_travel_through_together(self):
        # P1 fix (post-DL-18-review, 2026-09-17): the native side used
        # to fail `add_material` outright for a material combining
        # Emission Strength > 0 with Sheen Weight > 0 on the SAME
        # Principled node (see rise_blender_bridge.cpp's
        # `add_pbr_metallic_roughness_material` fix commit). That bug
        # lived entirely on the native side -- `sheen_color_painter_name`
        # and `emission_painter_name` are independent ctypes fields with
        # no interaction at the marshalling layer -- but there was no
        # test proving the two travel through TOGETHER, only each in
        # isolation (this class's other tests, and the base
        # emission-only coverage elsewhere in this file). This closes
        # that gap: both fields must reach the payload unmodified when
        # BOTH are set on one material, so a future marshalling change
        # can't silently start dropping one when the other is present.
        payload = _handle()._marshal_material(
            _StubMaterial(
                sheen_color_painter_name="mat_sheen_color",
                sheen_roughness=0.3,
                emission_painter_name="mat_emission",
                emissive_scale=2.0,
            )
        )
        self.assertEqual(payload.sheen_color_painter_name, b"mat_sheen_color")
        self.assertAlmostEqual(payload.sheen_roughness, 0.3, places=6)
        self.assertEqual(payload.emission_painter_name, b"mat_emission")
        self.assertAlmostEqual(payload.emissive_scale, 2.0, places=6)


class BridgeMaterialSpecularColorMarshallingTest(unittest.TestCase):
    """`_marshal_material`'s `specular_color_painter_name` field (DL-151,
    docs/DEBT_LEDGER.md).  This field has existed in the ABI since
    Landing 7 and `_marshal_material` already forwarded it correctly
    the whole time -- DL-151's defect was entirely upstream, in
    `exporter.py`'s `_material_payload` never COMPUTING a non-None
    value to hand it (see `ExporterSpecularTintGatingTest` above for
    that red-proof).  This class is therefore a behavioural
    CONFIRMATION of the marshalling layer, mirroring
    `BridgeMaterialSheenMarshallingTest`'s pattern for its own (also
    already-correct) v12 fields, not a second red-proof."""

    def test_no_tint_sends_null(self):
        payload = _handle()._marshal_material(_StubMaterial())
        self.assertIsNone(payload.specular_color_painter_name)

    def test_a_tint_painter_name_travels_through(self):
        payload = _handle()._marshal_material(
            _StubMaterial(specular_color_painter_name="mat_specular_tint")
        )
        self.assertEqual(payload.specular_color_painter_name, b"mat_specular_tint")

    def test_a_pre_dl151_exporter_payload_still_marshals(self):
        # `_marshal_material` reads this field via `getattr(material,
        # "specular_color_painter_name", None)`, so a `MaterialData`
        # built by an exporter module that predates DL-151 (the field
        # existed on the dataclass since Landing 7, but no code path
        # ever set it to anything but its `None` default) marshals as
        # "no tint" rather than raising -- the same back-compat
        # contract the v12 sheen fields and the v10 hair texture
        # fields established.
        stub = _StubMaterial()
        del stub.specular_color_painter_name
        payload = _handle()._marshal_material(stub)
        self.assertIsNone(payload.specular_color_painter_name)


class BridgeMaterialCoatSubsurfaceMarshallingTest(unittest.TestCase):
    """ABI v13's `coat_*` / `subsurface_*` fields on `_marshal_material`
    (DL-186, docs/DEBT_LEDGER.md).

    RED-PROOF HISTORY: before this debt closed, `bridge._Material` had
    no `coat_*`/`subsurface_*` fields at all, so the assignments in
    `_marshal_material` this test exercises raised an `AttributeError`
    (no such ctypes field to set) -- mirrors
    `BridgeMaterialSheenMarshallingTest`'s own red-proof history for
    its v12 fields."""

    def test_no_coat_no_subsurface_sends_defaults(self):
        payload = _handle()._marshal_material(_StubMaterial())
        self.assertIsNone(payload.coat_weight_texture_painter_name)
        self.assertAlmostEqual(payload.coat_weight, 0.0, places=6)
        self.assertIsNone(payload.coat_tint_painter_name)
        self.assertIsNone(payload.coat_roughness_texture_painter_name)
        self.assertAlmostEqual(payload.coat_roughness, 0.03, places=6)
        self.assertAlmostEqual(payload.coat_ior, 1.5, places=6)
        self.assertIsNone(payload.subsurface_absorption)
        self.assertIsNone(payload.subsurface_scattering)
        self.assertAlmostEqual(payload.subsurface_ior, 1.4, places=6)
        self.assertAlmostEqual(payload.subsurface_g, 0.0, places=6)
        self.assertAlmostEqual(payload.subsurface_roughness, 0.0, places=6)

    def test_coat_fields_travel_through(self):
        payload = _handle()._marshal_material(
            _StubMaterial(
                coat_weight=0.8,
                coat_tint_painter_name="mat_coat_tint",
                coat_roughness=0.15,
                coat_roughness_texture_painter_name="mat_coat_rough_tex",
                coat_ior=1.6,
            )
        )
        self.assertEqual(payload.coat_tint_painter_name, b"mat_coat_tint")
        self.assertAlmostEqual(payload.coat_weight, 0.8, places=6)
        self.assertEqual(payload.coat_roughness_texture_painter_name, b"mat_coat_rough_tex")
        self.assertAlmostEqual(payload.coat_roughness, 0.15, places=6)
        self.assertAlmostEqual(payload.coat_ior, 1.6, places=6)

    def test_subsurface_fields_travel_through(self):
        payload = _handle()._marshal_material(
            _StubMaterial(
                subsurface_absorption="0.02 0.05 0.10",
                subsurface_scattering="1.5 2.0 2.5",
                subsurface_ior=1.35,
                subsurface_g=0.2,
                subsurface_roughness=0.07,
            )
        )
        self.assertEqual(payload.subsurface_absorption, b"0.02 0.05 0.10")
        self.assertEqual(payload.subsurface_scattering, b"1.5 2.0 2.5")
        self.assertAlmostEqual(payload.subsurface_ior, 1.35, places=6)
        self.assertAlmostEqual(payload.subsurface_g, 0.2, places=6)
        self.assertAlmostEqual(payload.subsurface_roughness, 0.07, places=6)

    def test_a_pre_v13_exporter_payload_still_marshals(self):
        # `_marshal_material` reads every v13 field with a getattr
        # default, so an older exporter object that has never heard of
        # them marshals as "no coat / no subsurface" rather than
        # raising -- the same back-compat contract v10/v11/v12
        # established.
        stub = _StubMaterial()
        for attr in (
            "coat_weight_texture_painter_name", "coat_weight", "coat_tint_painter_name",
            "coat_roughness_texture_painter_name", "coat_roughness", "coat_ior",
            "subsurface_absorption", "subsurface_scattering", "subsurface_ior",
            "subsurface_g", "subsurface_roughness",
        ):
            delattr(stub, attr)
        payload = _handle()._marshal_material(stub)
        self.assertIsNone(payload.coat_weight_texture_painter_name)
        self.assertAlmostEqual(payload.coat_weight, 0.0, places=6)
        self.assertIsNone(payload.subsurface_absorption)
        self.assertIsNone(payload.subsurface_scattering)


class BridgeWarningDecodeTest(unittest.TestCase):
    """The ABI v9 non-fatal warning channel, Python side."""

    def _result(self, raw):
        result = bridge._RenderResult()
        result.warnings = raw
        return result

    def test_empty_buffer_is_no_warnings(self):
        self.assertEqual(bridge._decode_bridge_warnings(self._result(b"")), [])

    def test_single_warning(self):
        decoded = bridge._decode_bridge_warnings(self._result(b"RISE skipped hair object 'fur'."))
        self.assertEqual(decoded, ["RISE skipped hair object 'fur'."])

    def test_newline_separated_warnings_split(self):
        decoded = bridge._decode_bridge_warnings(self._result(b"first\nsecond\nthird"))
        self.assertEqual(decoded, ["first", "second", "third"])

    def test_blank_lines_are_dropped(self):
        decoded = bridge._decode_bridge_warnings(self._result(b"first\n\n  \nsecond\n"))
        self.assertEqual(decoded, ["first", "second"])

    def test_undecodable_bytes_do_not_raise(self):
        decoded = bridge._decode_bridge_warnings(self._result(b"bad \xff byte"))
        self.assertEqual(len(decoded), 1)


# ---------------------------------------------------------------------------
# `exporter._bump_modifier_scale` -- the SIGN a Blender Bump node exports.
#
# `exporter.py` imports bpy at module scope, so every other exporter test
# in this file is source-level (see ExporterHairTextureGatingTest's
# docstring).  This one is BEHAVIOURAL, because a sign is exactly the kind
# of thing a regex pins badly: the import is made to succeed by installing
# four stub modules and a synthetic `rise_renderer` package whose
# `__path__` points at this directory, which sidesteps the real
# `__init__.py` (that one DOES need a live Blender).  Nothing in the
# function under test touches bpy -- it is pure arithmetic on three
# numbers, deliberately factored out of `_build_bump_modifier` so it could
# be reached from here.
# ---------------------------------------------------------------------------

def _import_exporter_with_stub_bpy():
    """Import `exporter` outside Blender, or return None with a reason."""

    import importlib
    import types

    stubs = {}
    if "bpy" not in sys.modules:
        bpy_stub = types.ModuleType("bpy")
        bpy_stub.path = types.SimpleNamespace(abspath=lambda p, library=None: p)
        bpy_stub.types = types.SimpleNamespace()
        stubs["bpy"] = bpy_stub

        extras = types.ModuleType("bpy_extras")
        node_shader_utils = types.ModuleType("bpy_extras.node_shader_utils")

        class PrincipledBSDFWrapper:  # noqa: D401 - annotation target only
            pass

        node_shader_utils.PrincipledBSDFWrapper = PrincipledBSDFWrapper
        extras.node_shader_utils = node_shader_utils
        stubs["bpy_extras"] = extras
        stubs["bpy_extras.node_shader_utils"] = node_shader_utils

        mathutils = types.ModuleType("mathutils")
        mathutils.Vector = tuple
        stubs["mathutils"] = mathutils

    here = os.path.dirname(os.path.abspath(__file__))
    if "rise_renderer" not in sys.modules:
        pkg = types.ModuleType("rise_renderer")
        pkg.__path__ = [here]
        stubs["rise_renderer"] = pkg

    sys.modules.update(stubs)
    try:
        return importlib.import_module("rise_renderer.exporter")
    except Exception as exc:  # pragma: no cover - reported, not silently skipped
        for name in stubs:
            sys.modules.pop(name, None)
        raise unittest.SkipTest(f"exporter is not importable outside Blender: {exc!r}")


class ExporterBumpSignTest(unittest.TestCase):
    """`_bump_modifier_scale` composes with the bridge shim's negation to
    give Blender's / Blinn's / PBRT's `N - amp*grad(h)`.

    The chain, spelled out because no single file holds all of it:

        exporter  scale  = -Strength*Distance          (Invert off)
        shim      scale' = -scale                      (normalize=True,
                                                        RISE_API.cpp)
        relief    N'     = N - (T*h_T + B*h_B)*scale'  (ReliefModifier.cpp)
        =>        N'     = N - Strength*Distance*grad(h)

    `tests/BlenderBridgeHairTest.cpp` case 6a/6c pins the second and third
    links against the real C++; this pins the first."""

    @classmethod
    def setUpClass(cls):
        cls.exporter = _import_exporter_with_stub_bpy()

    def test_invert_off_is_negative(self):
        # Negative here == "tilt away from the up-slope" after the shim's
        # own negation.  Positive would render every dent as a bump.
        self.assertLess(self.exporter._bump_modifier_scale(0.7, 1.5, False), 0.0)
        self.assertAlmostEqual(
            self.exporter._bump_modifier_scale(0.7, 1.5, False), -1.05, places=12)

    def test_invert_on_flips_the_sign(self):
        self.assertAlmostEqual(
            self.exporter._bump_modifier_scale(0.7, 1.5, True), 1.05, places=12)

    def test_invert_is_exactly_a_negation(self):
        for strength, distance in ((1.0, 1.0), (0.25, 4.0), (2.0, 0.1)):
            with self.subTest(strength=strength, distance=distance):
                off = self.exporter._bump_modifier_scale(strength, distance, False)
                on = self.exporter._bump_modifier_scale(strength, distance, True)
                self.assertEqual(off, -on)

    def test_a_negative_strength_composes_rather_than_clamping(self):
        # Cycles lets Strength go negative; two negations cancel there
        # too, so the exported sign must follow the product's sign, not
        # be forced.
        self.assertGreater(self.exporter._bump_modifier_scale(-0.7, 1.5, False), 0.0)
        self.assertLess(self.exporter._bump_modifier_scale(-0.7, 1.5, True), 0.0)

    def test_zero_amplitude_stays_zero(self):
        # `ReliefModifier::Modify` is inert at scale 0; -0.0 and 0.0 both
        # satisfy its `!(x != 0)` test, so either is fine -- what must NOT
        # happen is a nonzero amplitude appearing out of a zero one.
        self.assertEqual(abs(self.exporter._bump_modifier_scale(0.0, 1.5, False)), 0.0)
        self.assertEqual(abs(self.exporter._bump_modifier_scale(0.7, 0.0, True)), 0.0)

    def test_the_builder_reads_the_nodes_invert_flag(self):
        # Source-level companion: the behavioural test above cannot reach
        # `_build_bump_modifier` (it walks a live node graph), so pin that
        # the builder actually consults `invert` and feeds the helper,
        # rather than passing a hard-coded False.
        with open(_EXPORTER_SOURCE, "r", encoding="utf-8") as handle:
            source = handle.read()
        start = source.index("def _build_bump_modifier(")
        end = source.index("\ndef ", start + 1)
        body = source[start:end]
        self.assertIn('_bump_modifier_scale(strength, distance, bool(getattr(normal_node, "invert", False)))', body)
        self.assertNotIn("scale=float(strength * distance)", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
