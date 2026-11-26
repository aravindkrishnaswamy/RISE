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
import principled_math as pmath  # noqa: E402
import light_math as lmath  # noqa: E402
from types import SimpleNamespace  # noqa: E402

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


class AnisotropicRotationConversionTest(unittest.TestCase):
    """DL-208 (docs/DEBT_LEDGER.md) red-proof: Blender's Principled
    "Anisotropic Rotation" socket is a [0, 1] fraction of a full turn,
    not radians -- unlike every other angle-typed socket this add-on
    reads.  These pin the exact numbers the review round's own fix
    description named.
    """

    def test_quarter_turn_is_half_pi(self):
        # A Principled rotation of 0.25 (a quarter turn, 90 degrees in
        # Cycles) must export pi/2 radians, not 0.25 radian
        # (~14.3 degrees -- the pre-fix defect).
        radians = hmm.anisotropic_rotation_turns_to_radians(0.25)
        self.assertAlmostEqual(radians, math.pi / 2.0, places=9)

    def test_zero_rotation_is_zero(self):
        self.assertAlmostEqual(hmm.anisotropic_rotation_turns_to_radians(0.0), 0.0, places=9)

    def test_full_turn_is_two_pi(self):
        radians = hmm.anisotropic_rotation_turns_to_radians(1.0)
        self.assertAlmostEqual(radians, 2.0 * math.pi, places=9)

    def test_case_ii_composition_with_a_constant_vector_rotate_angle(self):
        # Case (ii)'s own composition: a Principled rotation of 0.25
        # (-> pi/2 radians) PLUS a ShaderNodeVectorRotate "Angle" of
        # pi/4 (already radians -- Blender angle sockets on non-
        # Principled nodes store radians directly) must sum to
        # 3*pi/4 -- exactly the review round's own named check.
        converted = hmm.anisotropic_rotation_turns_to_radians(0.25)
        composed = converted + (math.pi / 4.0)
        self.assertAlmostEqual(composed, 3.0 * math.pi / 4.0, places=9)


class PrincipledMathTest(unittest.TestCase):
    """ABI v18 conversions in `principled_math`: Blender 5.3 / OpenPBR
    dispersion folding and the dielectric thin-film tint.  The native half
    (Cauchy painter, per-wavelength metal substrate, GGX thinfilm base, AR
    layer) is tests/BlenderBridgeThinFilmDispersionTest.cpp."""

    def test_effective_abbe_is_abbe_over_scale(self):
        self.assertAlmostEqual(pmath.effective_abbe_number(20.0, 1.0), 20.0)
        self.assertAlmostEqual(pmath.effective_abbe_number(20.0, 0.5), 40.0)

    def test_zero_scale_means_no_dispersion(self):
        # OpenPBR: scale 0 -> V_d infinite -> no dispersion; the socket
        # default (scale 0, abbe 20) must export nothing.
        self.assertEqual(pmath.effective_abbe_number(20.0, 0.0), 0.0)
        self.assertEqual(pmath.effective_abbe_number(0.0, 1.0), 0.0)
        self.assertEqual(pmath.effective_abbe_number(float("nan"), 1.0), 0.0)

    def test_dielectric_film_substrate_default_is_the_ior(self):
        self.assertEqual(pmath.dielectric_film_substrate(1.5, 0.5, (1.0, 1.0, 1.0)), (1.5, (1.0, 1.0, 1.0)))

    def test_specular_level_adjusts_the_index_like_cycles(self):
        # kernel/svm/closure.h: f0 = F0(ior) * 2 * level; eta = ior_from_F0(f0).
        eta, _ = pmath.dielectric_film_substrate(1.5, 1.0, (1.0, 1.0, 1.0))
        self.assertAlmostEqual(((eta - 1.0) / (eta + 1.0)) ** 2, 2.0 * 0.04, places=12)
        eta0, _ = pmath.dielectric_film_substrate(1.5, 0.0, (1.0, 1.0, 1.0))
        self.assertAlmostEqual(eta0, 1.0, places=12)

    def test_specular_level_inverts_below_unit_ior_like_cycles(self):
        eta, _ = pmath.dielectric_film_substrate(0.8, 1.0, (1.0, 1.0, 1.0))
        inverse = 1.0 / eta
        r = (0.8 - 1.0) / (0.8 + 1.0)
        self.assertAlmostEqual(((inverse - 1.0) / (inverse + 1.0)) ** 2, 2.0 * r * r, places=12)
        self.assertLess(eta, 1.0)

    def test_film_tint_is_specular_tint_clamped_to_one(self):
        # A tint above 1 would push RISE's interface reflectance past 1
        # at grazing and make the diffuse term (1 - A_i)(1 - A_o) negative.
        _, tint = pmath.dielectric_film_substrate(1.5, 0.7, (1.4, 0.5, -0.2))
        self.assertEqual(tint, (1.0, 0.5, 0.0))

    def test_dispersion_guard_matches_the_band_ends(self):
        self.assertTrue(pmath.dispersion_keeps_positive_ior(1.5, 20.0))
        self.assertTrue(pmath.dispersion_keeps_positive_ior(1.5, 0.3))
        self.assertFalse(pmath.dispersion_keeps_positive_ior(1.5, 0.28))
        self.assertFalse(pmath.dispersion_keeps_positive_ior(1.5, 0.1))

    def test_thin_film_cutoff_matches_cycles(self):
        self.assertEqual(pmath.THIN_FILM_THICKNESS_CUTOFF_NM, 0.1)


class LightMathTest(unittest.TestCase):
    """`light_math`: Blender -> RISE light power.  The factors were
    measured against Cycles 5.3 (tools/blender_light_parity.py); these pin
    the formulas so a refactor cannot drift from the measured ones."""

    def test_normalize_on_is_unscaled(self):
        for kind in ("POINT", "SPOT", "SUN"):
            light = SimpleNamespace(type=kind, normalize=True, shadow_soft_size=2.0, angle=0.5)
            self.assertEqual(lmath.unnormalized_power_scale(light), 1.0)

    def test_point_and_spot_scale_by_sphere_area(self):
        for kind in ("POINT", "SPOT"):
            light = SimpleNamespace(type=kind, normalize=False, shadow_soft_size=1.0)
            self.assertAlmostEqual(lmath.unnormalized_power_scale(light), 4.0 * math.pi, places=12)
            light.shadow_soft_size = 0.0
            self.assertEqual(lmath.unnormalized_power_scale(light), 4.0)

    def test_sun_scales_by_pi_sin_squared_half_angle(self):
        light = SimpleNamespace(type="SUN", normalize=False, angle=math.radians(30.0))
        self.assertAlmostEqual(lmath.unnormalized_power_scale(light), 0.2104468036, places=9)
        light.angle = 0.0
        self.assertEqual(lmath.unnormalized_power_scale(light), 1.0)

    def test_disk_polygon_area_approaches_the_ellipse(self):
        poly = lmath.area_light_polygon_area("DISK", 2.0, 2.0)
        self.assertAlmostEqual(poly, 0.5 * 64 * math.sin(2.0 * math.pi / 64), places=12)
        self.assertLess(poly, math.pi)
        self.assertEqual(lmath.area_light_polygon_area("RECTANGLE", 3.0, 1.0), 3.0)

    def test_area_exitance_normalized_divides_by_world_polygon_area(self):
        m = lmath.area_light_exitance(100.0, "SQUARE", 2.0, 2.0, plane_scale=4.0, normalize=True)
        self.assertAlmostEqual(m, 100.0 / 16.0, places=12)
        disk = lmath.area_light_exitance(100.0, "DISK", 2.0, 2.0, plane_scale=1.0, normalize=True)
        self.assertAlmostEqual(disk * lmath.area_light_polygon_area("DISK", 2.0, 2.0), 100.0, places=9)

    def test_area_exitance_unnormalized_is_power_per_true_area(self):
        # Flux = power * true world area, independent of the object scale.
        square = lmath.area_light_exitance(100.0, "SQUARE", 2.0, 2.0, plane_scale=4.0, normalize=False)
        self.assertAlmostEqual(square, 100.0, places=12)
        disk = lmath.area_light_exitance(100.0, "DISK", 2.0, 2.0, plane_scale=1.0, normalize=False)
        self.assertAlmostEqual(disk * lmath.area_light_polygon_area("DISK", 2.0, 2.0), 100.0 * math.pi, places=9)

    def test_degenerate_panel_emits_nothing(self):
        self.assertEqual(lmath.area_light_exitance(100.0, "SQUARE", 2.0, 2.0, plane_scale=0.0, normalize=True), 0.0)


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
    bridge._MixChild: "const rise_blender_mix_child*",
    bridge._MeshAttribute: "const rise_blender_mesh_attribute*",
    # The result image buffer -- the one non-const pointer in the ABI.
    ctypes.c_float: "float*",
    ctypes.c_uint32: "const uint32_t*",
    ctypes.c_char_p: "const char* const*",
}


def _canonical_ctype(ctype):
    if ctype in _CTYPE_TO_C:
        return _CTYPE_TO_C[ctype]
    element = getattr(ctype, "_type_", None)
    length = getattr(ctype, "_length_", None)
    if element is not None and length is not None:
        if element is ctypes.c_float:
            return f"float[{length}]"
        if element is ctypes.c_double:
            return f"double[{length}]"
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
        self.assertEqual(bridge._EXPECTED_API_VERSION, 23)

    def test_progress_info_struct_matches(self):
        self._assert_matches(bridge._ProgressInfo, "rise_blender_progress_info")

    def test_mesh_struct_matches(self):
        self._assert_matches(bridge._Mesh, "rise_blender_mesh")

    def test_tangent_payload_is_tail_appended(self):
        names = [name for name, _ in bridge._Mesh._fields_]
        self.assertEqual(names[-5:-2], ["tangent_attribute", "num_tangents", "tangent_is_shader_direction"])
        self.assertEqual(names[-2:], ["named_attributes", "num_named_attributes"])
        self.assertGreater(bridge._Mesh.tangent_attribute.offset, bridge._Mesh.use_face_normals.offset)
        self.assertEqual(bridge._Mesh.num_tangents.offset, bridge._Mesh.tangent_attribute.offset + ctypes.sizeof(ctypes.c_void_p))
        self.assertGreater(bridge._Mesh.named_attributes.offset, bridge._Mesh.tangent_is_shader_direction.offset)

    def test_mix_and_attribute_structs_match(self):
        self._assert_matches(bridge._MixChild, "rise_blender_mix_child")
        self._assert_matches(bridge._MeshAttribute, "rise_blender_mesh_attribute")
        self._assert_matches(bridge._Material, "rise_blender_material")
        self.assertEqual(
            [name for name, _ in bridge._Material._fields_[-3:]],
            ["mix_children", "num_mix_children", "mix_passthrough_painter_name"],
        )

    def test_abi_v21_marshalling_mix_and_mesh_attributes(self):
        # Exercise _SceneHandle._marshal_material and _marshal_mesh directly
        # for ABI v21 fields + non-multiple-of-4 rgba rejection (R1 A-P1-4 / B-P2-7).
        import types
        handle = bridge._SceneHandle.__new__(bridge._SceneHandle)
        handle.keepalive = []

        mix_child_a = types.SimpleNamespace(
            material_name="leaf0", weight_painter_name="w0", modifier_name="mod0"
        )
        mix_child_b = types.SimpleNamespace(
            material_name=None, weight_painter_name="0.25", modifier_name=None
        )
        mat_stub = types.SimpleNamespace(
            name="m_mix", model=5,
            diffuse_painter_name=None, specular_painter_name=None,
            alpha_x_painter_name=None, alpha_y_painter_name=None,
            ior_painter_name=None, extinction_painter_name=None,
            tau_painter_name=None, scatter_painter_name=None,
            emission_painter_name=None, double_sided=True,
            mix_children=[mix_child_a, mix_child_b],
            mix_passthrough_painter_name="w_pt",
        )
        c_mat = handle._marshal_material(mat_stub)
        self.assertEqual(c_mat.num_mix_children, 2)
        self.assertEqual(c_mat.mix_children[0].material_name, b"leaf0")
        self.assertEqual(c_mat.mix_children[0].weight_painter_name, b"w0")
        self.assertEqual(c_mat.mix_children[0].modifier_name, b"mod0")
        self.assertIsNone(c_mat.mix_children[1].material_name)
        self.assertEqual(c_mat.mix_passthrough_painter_name, b"w_pt")

        attr_ok = types.SimpleNamespace(name="distance", rgba=[0.1, 0.2, 0.3, 1.0, 0.4, 0.5, 0.6, 1.0, 0.7, 0.8, 0.9, 1.0])
        mesh_stub = types.SimpleNamespace(
            name="m_geo",
            vertices=[0.0] * 9, normals=[0.0] * 9, uvs=[],
            vertex_indices=[0, 1, 2], normal_indices=[0, 1, 2], uv_indices=[],
            num_vertices=3, num_normals=3, num_uvs=0, num_triangles=1,
            double_sided=True, use_face_normals=False,
            named_attributes=[attr_ok],
        )
        c_mesh = handle._marshal_mesh(mesh_stub)
        self.assertEqual(c_mesh.num_named_attributes, 1)
        self.assertEqual(c_mesh.named_attributes[0].name, b"distance")
        self.assertEqual(c_mesh.named_attributes[0].num_corners, 3)
        self.assertAlmostEqual(c_mesh.named_attributes[0].rgba[0], 0.1, places=5)

        # R1 A-P1-4: non-multiple-of-4 rgba must raise BridgeError
        attr_bad = types.SimpleNamespace(name="bad", rgba=[0.1, 0.2, 0.3, 1.0, 0.5])
        with self.assertRaises(bridge.BridgeError):
            handle._marshal_mesh_attribute(attr_bad)

    def test_stale_dylib_version_fails_loudly(self):
        # Simulate a current-minus-one ABI dylib beside the current addon. Mock both
        # path resolution and loading: the ABI guard must be independent
        # of whether a native build product exists in this checkout.
        bridge._LOADED_LIBRARY = None
        bridge._LOADED_PATH = None
        bridge._LOADED_CAPABILITIES = None
        fake_library = mock.MagicMock()
        fake_library.rise_blender_api_version.return_value = bridge._EXPECTED_API_VERSION - 1
        with mock.patch.object(bridge, "resolve_bridge_path", return_value="/tmp/stale-rise-bridge.dylib"), mock.patch.object(bridge.ctypes, "CDLL", return_value=fake_library):
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

    def test_render_settings_struct_matches(self):
        self._assert_matches(bridge._RenderSettings, "rise_blender_render_settings")

    def test_capabilities_struct_matches(self):
        self._assert_matches(bridge._Capabilities, "rise_blender_capabilities")

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

    def test_marshal_modifier_stack_and_blend(self):
        """Phase 3 R1 P1-1 regression test: _SceneHandle._marshal_modifier must
        append stack_member_names keepalive to self.keepalive (no AttributeError)
        and marshal MODIFIER_STACK / MODIFIER_BLEND payloads cleanly."""
        handle = object.__new__(bridge._SceneHandle)
        handle.keepalive = []

        stack_mod = type(
            "_Mod", (),
            {
                "name": "mod_stack",
                "kind": 3,
                "source_painter_name": None,
                "scale": 1.0,
                "window": 0.0,
                "normalize": False,
                "stack_member_names": ("mod_a", "mod_b"),
                "blend_a_name": None,
                "blend_b_name": None,
                "blend_weight_painter_name": None,
            },
        )()
        p_stack = handle._marshal_modifier(stack_mod)
        self.assertEqual(p_stack.num_stack_members, 2)
        self.assertEqual(p_stack.stack_member_names[0], b"mod_a")
        self.assertEqual(p_stack.stack_member_names[1], b"mod_b")
        self.assertTrue(len(handle.keepalive) >= 1)

        blend_mod = type(
            "_Mod", (),
            {
                "name": "mod_blend",
                "kind": 4,
                "source_painter_name": None,
                "scale": 1.0,
                "window": 0.0,
                "normalize": False,
                "stack_member_names": (),
                "blend_a_name": "mod_a",
                "blend_b_name": "mod_b",
                "blend_weight_painter_name": "0.5",
            },
        )()
        p_blend = handle._marshal_modifier(blend_mod)
        self.assertEqual(p_blend.num_stack_members, 0)
        self.assertEqual(p_blend.blend_a_name, b"mod_a")
        self.assertEqual(p_blend.blend_b_name, b"mod_b")
        self.assertEqual(p_blend.blend_weight_painter_name, b"0.5")


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
        start = self.source.index("def _export_principled_binding(")
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


class ExporterCoatNormalAlphaTangentGatingTest(unittest.TestCase):
    """DL-192 (Coat Normal, Tangent) / DL-193 (Alpha) source-level
    gating, docs/DEBT_LEDGER.md.

    Why source level and not behaviour: same reasoning as
    `ExporterSpecularTintGatingTest` above -- `_material_payload`
    cannot be called outside Blender at all.  What CAN be pinned
    without Blender is that the function's SOURCE reads each socket
    and forwards what it reads (or, for Tangent's unsupported cases,
    warns and drops rather than silently doing nothing)."""

    @classmethod
    def setUpClass(cls):
        with open(_EXPORTER_SOURCE, "r", encoding="utf-8") as handle:
            cls.source = handle.read()

    def _material_payload_body(self):
        start = self.source.index("def _export_principled_binding(")
        end = self.source.index("\ndef ", start + 1)
        return self.source[start:end]

    # --- Coat Normal (DL-192) -----------------------------------------

    def test_coat_normal_socket_is_read(self):
        body = self._material_payload_body()
        self.assertIn(
            '_node_input(principled_node, "Coat Normal")',
            body,
            "_material_payload no longer reads the Coat Normal socket",
        )

    def test_coat_normal_routes_through_the_dedicated_builder(self):
        body = self._material_payload_body()
        self.assertIn("_build_coat_normal_painter(", body)

    def test_the_pbr_payload_forwards_coat_normal_fields(self):
        body = self._material_payload_body()
        self.assertIn("coat_normal_painter_name=coat_normal_painter,", body)
        self.assertIn("coat_normal_scale=coat_normal_scale,", body)

    def test_a_direct_normal_map_node_is_the_only_supported_shape(self):
        # Anything else on Coat Normal (not a direct ShaderNodeNormalMap)
        # warns rather than silently doing nothing.
        body = self._material_payload_body()
        self.assertIn('coat_normal_source.bl_idname == "ShaderNodeNormalMap"', body)
        self.assertIn("only supports a direct Normal Map node on Coat Normal", body)

    def test_normal_map_painters_use_linear_not_romm_linear(self):
        # Post-Stage-B (`RISEPel = Rec709RGBPel`), ROMMRGB_Linear applies a
        # ProPhoto->Rec.709 primary matrix that warps tangent-space normal
        # vectors.  All three normal-map painter builders in exporter.py
        # must emit COLOR_SPACE_LINEAR (0) and never COLOR_SPACE_ROMM_LINEAR.
        coat_start = self.source.index("def _build_coat_normal_painter(")
        coat_end = self.source.index("\ndef ", coat_start + 1)
        coat_body = self.source[coat_start:coat_end]
        self.assertIn("COLOR_SPACE_LINEAR,", coat_body)
        self.assertNotIn("COLOR_SPACE_ROMM_LINEAR", coat_body)

        norm_start = self.source.index("def _build_normal_map_modifier(")
        norm_end = self.source.index("\ndef ", norm_start + 1)
        norm_body = self.source[norm_start:norm_end]
        self.assertIn("COLOR_SPACE_LINEAR,", norm_body)
        self.assertNotIn("COLOR_SPACE_ROMM_LINEAR", norm_body)

        mat_body = self._material_payload_body()
        self.assertNotIn("COLOR_SPACE_ROMM_LINEAR", mat_body)

    # --- Alpha (DL-193) -------------------------------------------------

    def test_alpha_socket_is_read(self):
        body = self._material_payload_body()
        self.assertIn('_node_input(principled_node, "Alpha")', body)

    def test_blend_method_selects_the_alpha_mode(self):
        body = self._material_payload_body()
        self.assertIn('getattr(material, "blend_method", None)', body)
        self.assertIn('"BLENDED": "BLEND"', body)
        self.assertIn('"DITHERED": "HASHED"', body)
        self.assertIn('blend_method == "CLIP"', body)
        self.assertIn("ALPHA_MODE_CLIP", body)
        self.assertIn("ALPHA_MODE_BLEND", body)

    def test_every_material_payload_forwards_alpha_fields(self):
        # All three MaterialData(...) constructions in the main path
        # (Dielectric, RandomWalk SSS, PBR) must forward alpha -- it is
        # not a PBR-only concept the way Coat Normal is.
        body = self._material_payload_body()
        self.assertEqual(body.count("alpha=alpha_value,"), 3)
        self.assertEqual(body.count("alpha_texture_painter_name=alpha_texture_painter,"), 3)
        self.assertEqual(body.count("alpha_mode=alpha_mode_value,"), 3)
        self.assertEqual(body.count("alpha_threshold=alpha_threshold_value,"), 3)

    def test_the_binding_computes_a_deterministic_shader_name(self):
        body = self._material_payload_body()
        self.assertIn('shader_name = f"{payload.name}.shader" if alpha_mode_value != ALPHA_MODE_OPAQUE else None', body)
        self.assertIn("shader_name=shader_name,", body)

    def test_alpha_texture_reads_the_raw_texture_not_a_flattened_view(self):
        # Mirrors GLTFSceneImporter.cpp's BuildAlphaPainter comment: the
        # native side extracts channel A itself (AddChannelPainter), so
        # the exporter must register the whole (typically RGBA) image
        # via _add_texture_painter, not a scalar-flattened wrapper.
        body = self._material_payload_body()
        self.assertIn(f'_add_texture_painter(\n                            state, f"{{material.name_full}}_alpha"', body)

    # DL-213 moves Tangent production to mesh export. Behaviour is covered by
    # test_tangent_export.py and the real Blender runtime gate.
    def test_tangent_rotation_not_double_composed(self):
        self.assertNotIn("anisotropy_rotation_value +=", self._material_payload_body())

    def test_mesh_producer_carries_corner_tangents(self):
        self.assertIn('tangent_bake.corner_tangents(mesh, material, matrix_world, eval_object, instance_info, bucket["normal_indices"], allow_parallel=bool(binding.modifier_name))', self.source)
        self.assertIn("tangent_attribute=", self.source)


class LegacyDefaultsTest(unittest.TestCase):
    """Legacy-deprecation Phase 2 slice C: the add-on's defaults are
    supported paths, and the frozen choices that remain selectable really
    select what they name.

    `properties.py` and `export_scene` import bpy, so the enum / default
    checks read `properties.py`'s source; the rasterizer selection itself is
    tested through behaviour: `bridge.resolve_rasterizer_selection` (what
    `export_scene` calls) and the ctypes payload `_marshal_settings` builds
    from its result.  The native side (rise_blender_bridge.cpp) maps kind 0
    to PT_PEL when use_path_tracing is set and to PIXELPEL otherwise; the
    native BlenderBridgeAlphaTest / BlenderBridgeTangentTest render kind 0
    with use_path_tracing 0 through the legacy pixel rasterizer."""

    @classmethod
    def setUpClass(cls):
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "properties.py"), "r", encoding="utf-8") as handle:
            cls.props = handle.read()
        with open(_EXPORTER_SOURCE, "r", encoding="utf-8") as handle:
            cls.exporter = handle.read()

    def _prop_block(self, name):
        start = self.props.index(f"    {name}: bpy.props.")
        end = self.props.index("\n    )\n", start)
        return self.props[start:end]

    def _items(self):
        return dict(re.findall(r'\("(\d+)",\s*"([^"]+)"', self._prop_block("rasterizer_type")))

    @staticmethod
    def _native_kind(kind, use_path_tracing):
        # Mirror of rise_blender_bridge.cpp's kind resolution.
        if kind == bridge.RASTERIZER_KIND_PIXELPEL:
            return bridge.RASTERIZER_KIND_PT_PEL if use_path_tracing else bridge.RASTERIZER_KIND_PIXELPEL
        return kind

    def _marshalled(self, rasterizer_type, use_path_tracing):
        kind, upt = bridge.resolve_rasterizer_selection(rasterizer_type, use_path_tracing)
        settings = mock.MagicMock()
        settings.use_path_tracing = upt
        settings.rasterizer_kind = kind
        settings.temporary_directory = ""
        handle = bridge._SceneHandle.__new__(bridge._SceneHandle)
        handle.keepalive = []
        payload = handle._marshal_settings(settings)
        return payload.rasterizer_kind, payload.use_path_tracing

    def test_default_rasterizer_is_supported(self):
        default = re.search(r'default="(\d+)"', self._prop_block("rasterizer_type")).group(1)
        items = self._items()
        self.assertNotIn("legacy", items[default].lower())
        kind, upt = self._marshalled(default, True)   # use_path_tracing defaults True
        self.assertNotIn(self._native_kind(kind, upt), (0, 7, 8))

    def test_frozen_rasterizers_remain_selectable_and_labelled_legacy(self):
        items = self._items()
        for key in ("0", "7", "8"):
            self.assertIn("[legacy]", items[key])

    def test_explicit_pixel_rasterizer_reaches_pixelpel(self):
        # The back-compat box defaults True and is not drawn; picking the
        # legacy pixel rasterizer must still select it natively.
        for upt in (True, False):
            kind, sent_upt = self._marshalled("0", upt)
            self.assertEqual((kind, sent_upt), (0, 0))
            self.assertEqual(self._native_kind(kind, sent_upt), bridge.RASTERIZER_KIND_PIXELPEL)

    def test_other_choices_pass_use_path_tracing_through(self):
        for key in self._items():
            if key == "0":
                continue
            for upt in (True, False):
                kind, sent_upt = self._marshalled(key, upt)
                self.assertEqual((kind, sent_upt), (int(key), int(upt)))
                self.assertEqual(self._native_kind(kind, sent_upt), int(key))

    def test_export_scene_uses_the_resolver(self):
        start = self.exporter.index("def _render_settings_from_scene(") if "def _render_settings_from_scene(" in self.exporter else 0
        self.assertIn("resolve_rasterizer_selection(", self.exporter[start:])
        self.assertIn("use_path_tracing=use_path_tracing,", self.exporter)
        self.assertIn("rasterizer_kind=rasterizer_kind,", self.exporter)

    def test_world_ambient_defaults_off(self):
        self.assertIn("default=False", self._prop_block("use_world_ambient"))

    def test_non_node_world_exports_as_uniform_environment(self):
        start = self.exporter.index("def export_scene(")
        end = self.exporter.find("\ndef ", start + 1)
        body = self.exporter[start:end if end >= 0 else len(self.exporter)]
        gate = body.index("world_radiance_painter is None and not render_settings.use_world_ambient")
        self.assertIn("not uses_nodes(scene.world)", body[gate:gate + 200])
        self.assertIn('_add_uniform_painter(state, "world_radiance_uniform", world_rgb)', body[gate:])
        # The explicitly-enabled ambient path is unchanged.
        self.assertIn("world_radiance_painter is None and render_settings.use_world_ambient", body)


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
        # ABI v14 -- DL-192/DL-193 (docs/DEBT_LEDGER.md).  None / 1.0 /
        # OPAQUE(0) means "no coat-normal perturbation" / "fully
        # opaque", matching every payload built before this debt closed.
        self.coat_normal_painter_name = None
        self.coat_normal_scale = 1.0
        self.alpha = 1.0
        self.alpha_texture_painter_name = None
        self.alpha_mode = 0
        self.alpha_threshold = 0.5
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


class BridgeMaterialCoatNormalAlphaMarshallingTest(unittest.TestCase):
    """ABI v14's `coat_normal_*` / `alpha*` fields on `_marshal_material`
    (DL-192/DL-193, docs/DEBT_LEDGER.md).

    RED-PROOF HISTORY: before this debt closed, `bridge._Material` had
    no `coat_normal_*`/`alpha*` fields at all, so the assignments in
    `_marshal_material` this test exercises raised an `AttributeError`
    -- mirrors `BridgeMaterialCoatSubsurfaceMarshallingTest`'s own
    red-proof history for its v13 fields."""

    def test_no_coat_normal_no_alpha_sends_defaults(self):
        payload = _handle()._marshal_material(_StubMaterial())
        self.assertIsNone(payload.coat_normal_painter_name)
        self.assertAlmostEqual(payload.coat_normal_scale, 1.0, places=6)
        self.assertAlmostEqual(payload.alpha, 1.0, places=6)
        self.assertIsNone(payload.alpha_texture_painter_name)
        self.assertEqual(payload.alpha_mode, 0)
        self.assertAlmostEqual(payload.alpha_threshold, 0.5, places=6)

    def test_coat_normal_fields_travel_through(self):
        payload = _handle()._marshal_material(
            _StubMaterial(coat_normal_painter_name="mat_coat_normal", coat_normal_scale=2.0)
        )
        self.assertEqual(payload.coat_normal_painter_name, b"mat_coat_normal")
        self.assertAlmostEqual(payload.coat_normal_scale, 2.0, places=6)

    def test_alpha_fields_travel_through(self):
        payload = _handle()._marshal_material(
            _StubMaterial(
                alpha=0.4,
                alpha_texture_painter_name="mat_alpha_tex",
                alpha_mode=2,
                alpha_threshold=0.75,
            )
        )
        self.assertAlmostEqual(payload.alpha, 0.4, places=6)
        self.assertEqual(payload.alpha_texture_painter_name, b"mat_alpha_tex")
        self.assertEqual(payload.alpha_mode, 2)
        self.assertAlmostEqual(payload.alpha_threshold, 0.75, places=6)

    def test_a_pre_v14_exporter_payload_still_marshals(self):
        # `_marshal_material` reads every v14 field with a getattr
        # default, so an older exporter object that has never heard of
        # them marshals as "no coat normal / fully opaque" rather than
        # raising -- the same back-compat contract v10-v13 established.
        stub = _StubMaterial()
        for attr in (
            "coat_normal_painter_name", "coat_normal_scale",
            "alpha", "alpha_texture_painter_name", "alpha_mode", "alpha_threshold",
        ):
            delattr(stub, attr)
        payload = _handle()._marshal_material(stub)
        self.assertIsNone(payload.coat_normal_painter_name)
        self.assertAlmostEqual(payload.coat_normal_scale, 1.0, places=6)
        self.assertAlmostEqual(payload.alpha, 1.0, places=6)
        self.assertEqual(payload.alpha_mode, 0)


class _StubObject:
    """The subset of `exporter.ObjectData` `_marshal_object` reads.
    Deliberately a stand-in rather than the real dataclass -- see
    `_StubMaterial`'s own docstring for why."""

    def __init__(self, **kwargs):
        self.name = "obj"
        self.geometry_name = "geom"
        self.material_name = "mat"
        self.transform = [float(i) for i in range(16)]
        self.casts_shadows = True
        self.receives_shadows = True
        self.visible = True
        self.modifier_name = None
        self.interior_medium_name = None
        # ABI v14 -- DL-193 (docs/DEBT_LEDGER.md).  None means "no
        # shader override", matching every payload built before this
        # debt closed.
        self.shader_name = None
        for key, value in kwargs.items():
            setattr(self, key, value)


class BridgeObjectShaderMarshallingTest(unittest.TestCase):
    """ABI v14's `shader_name` field on `_marshal_object` (DL-193,
    docs/DEBT_LEDGER.md).

    RED-PROOF HISTORY: before this debt closed, `bridge._Object` had no
    `shader_name` field, and `add_object` (rise_blender_bridge.cpp)
    always passed a hard-coded NULL shader to `IJob::AddObject` --
    there was no ABI path from the exporter to an object's shader at
    all."""

    def test_no_shader_sends_null(self):
        payload = _handle()._marshal_object(_StubObject())
        self.assertIsNone(payload.shader_name)

    def test_shader_name_travels_through(self):
        payload = _handle()._marshal_object(_StubObject(shader_name="mat.shader"))
        self.assertEqual(payload.shader_name, b"mat.shader")

    def test_a_pre_v14_exporter_payload_still_marshals(self):
        stub = _StubObject()
        delattr(stub, "shader_name")
        payload = _handle()._marshal_object(stub)
        self.assertIsNone(payload.shader_name)


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


class RnaIdentityKeyTest(unittest.TestCase):
    """Graph walks must key visited nodes on the RNA pointer, never on
    Python ``id()``: Blender builds a fresh wrapper per node access, so
    ``id()`` is reused across different nodes and a walk skips branches
    (test_graph_walk_blender_runtime.py reproduces it)."""

    def test_rna_key_prefers_the_stable_pointer(self):
        import importlib.util
        here = os.path.dirname(os.path.abspath(__file__))
        spec = importlib.util.spec_from_file_location("_compat_under_test", os.path.join(here, "blender_compat.py"))
        compat = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(compat)

        class Wrapper:
            """A fresh object per access that names the same Blender data."""
            def __init__(self, pointer):
                self._pointer = pointer
            def as_pointer(self):
                return self._pointer

        self.assertEqual(compat.rna_key(Wrapper(0x1000)), compat.rna_key(Wrapper(0x1000)))
        self.assertNotEqual(compat.rna_key(Wrapper(0x1000)), compat.rna_key(Wrapper(0x2000)))
        plain = object()
        self.assertEqual(compat.rna_key(plain), id(plain))

    def test_no_python_identity_on_blender_data(self):
        """No ``id(...)`` call and no ``is`` / ``is not`` between two
        non-constant values anywhere in the add-on modules that touch
        Blender data: both compare wrapper objects, not the data."""

        import ast
        here = os.path.dirname(os.path.abspath(__file__))
        offenders = []
        for name in ("exporter.py", "material_bake.py", "procedural_expr.py", "tangent_bake.py", "engine.py", "ui.py"):
            with open(os.path.join(here, name), encoding="utf-8") as stream:
                tree = ast.parse(stream.read(), filename=name)
            for node in ast.walk(tree):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "id":
                    offenders.append(f"{name}:{node.lineno} id(...)")
                if isinstance(node, ast.Compare):
                    operands = [node.left] + list(node.comparators)
                    for op, left, right in zip(node.ops, operands, operands[1:]):
                        if not isinstance(op, (ast.Is, ast.IsNot)):
                            continue
                        constant = lambda e: isinstance(e, ast.Constant) and e.value in (None, True, False)
                        if not (constant(left) or constant(right)):
                            offenders.append(f"{name}:{node.lineno} identity comparison")
        self.assertEqual(offenders, [], "use blender_compat.rna_key / == instead")


class TestProgressAndETAFormatting(unittest.TestCase):
    """ABI v22 progress + live ETA formatting and bridge dispatch tests."""

    @classmethod
    def _load_engine_module(cls):
        import importlib.util
        import types

        here = os.path.dirname(os.path.abspath(__file__))
        fake_bpy = types.ModuleType("bpy")
        fake_bpy.types = types.SimpleNamespace(RenderEngine=object)
        fake_bpy.app = types.SimpleNamespace(background=False)
        fake_pkg = types.ModuleType("_rise_engine_test_pkg")
        fake_pkg.__path__ = [here]
        fake_props = types.ModuleType("_rise_engine_test_pkg.properties")
        fake_props.addon_preferences = lambda: None
        fake_exporter = types.ModuleType("_rise_engine_test_pkg.exporter")
        fake_bake = types.ModuleType("_rise_engine_test_pkg.material_bake")

        stubs = {
            "bpy": fake_bpy,
            "_rise_engine_test_pkg": fake_pkg,
            "_rise_engine_test_pkg.bridge": bridge,
            "_rise_engine_test_pkg.exporter": fake_exporter,
            "_rise_engine_test_pkg.material_bake": fake_bake,
            "_rise_engine_test_pkg.properties": fake_props,
        }
        spec = importlib.util.spec_from_file_location(
            "_rise_engine_test_pkg.engine",
            os.path.join(here, "engine.py"),
        )
        mod = importlib.util.module_from_spec(spec)
        with mock.patch.dict(sys.modules, stubs):
            spec.loader.exec_module(mod)
        return mod

    def test_format_duration_boundaries_and_non_finite(self):
        eng = self._load_engine_module()
        self.assertEqual(eng._format_duration(0.0), "00:00.00")
        self.assertEqual(eng._format_duration(-5.0), "00:00.00")
        self.assertEqual(eng._format_duration(float("nan")), "00:00.00")
        self.assertEqual(eng._format_duration(float("inf")), "00:00.00")
        self.assertEqual(eng._format_duration(12.34), "00:12.34")
        # Centisecond carry must roll over to 01:00.00, never 00:60.00
        self.assertEqual(eng._format_duration(59.996), "01:00.00")
        self.assertEqual(eng._format_duration(3599.996), "01:00:00.00")
        self.assertEqual(eng._format_duration(3661.5), "01:01:01.50")

    def test_live_remaining_seconds_counts_down_and_clamps(self):
        eng = self._load_engine_module()
        self.assertEqual(eng._live_remaining_seconds(10.0, False, 100.0, 102.0), -1.0)
        self.assertAlmostEqual(eng._live_remaining_seconds(10.0, True, 100.0, 102.5), 7.5)
        self.assertEqual(eng._live_remaining_seconds(10.0, True, 100.0, 115.0), 0.0)
        # Negative clock skew does not inflate remaining time
        self.assertAlmostEqual(eng._live_remaining_seconds(10.0, True, 100.0, 99.0), 10.0)

    def test_format_render_status_stages_and_eta(self):
        eng = self._load_engine_module()
        # Indeterminate stage (Building scene / Denoising): non-empty info, no % or Remaining
        self.assertEqual(
            eng._format_render_status("Building scene", -1.0, -1.0, False),
            "Building scene",
        )
        self.assertEqual(
            eng._format_render_status("Denoising: ", -1.0, -1.0, False),
            "Denoising",
        )
        # Warmup (has_remaining=False): stage + percentage, no Remaining
        self.assertEqual(
            eng._format_render_status("Rasterizing Scene: ", 0.02, -1.0, False),
            "Rasterizing Scene 2%",
        )
        # Active ETA: Cycles-style "Remaining: MM:SS.CC | Stage Pct%"
        self.assertEqual(
            eng._format_render_status("Rasterizing Scene: ", 0.25, 42.15, True),
            "Remaining: 00:42.15 | Rasterizing Scene 25%",
        )
        # At 100%: omit Remaining
        self.assertEqual(
            eng._format_render_status("Rasterizing Scene", 1.0, 0.0, True),
            "Rasterizing Scene 100%",
        )
        # Empty title falls back to non-empty "Rendering" so Blender keeps ticking Time:
        self.assertEqual(
            eng._format_render_status("", 0.5, 5.0, True),
            "Remaining: 00:05.00 | Rendering 50%",
        )

    def test_should_refresh_stats_gui_and_background_modes(self):
        eng = self._load_engine_module()
        # GUI mode: refreshes on interval, stage change, ETA warmup, or fast pct change
        self.assertFalse(eng._should_refresh_stats(
            is_background=False, stage_changed=False, pct_changed=False,
            eta_became_ready=False, elapsed_since_update=0.10, status_interval=0.25,
        ))
        self.assertTrue(eng._should_refresh_stats(
            is_background=False, stage_changed=False, pct_changed=False,
            eta_became_ready=False, elapsed_since_update=0.25, status_interval=0.25,
        ))
        self.assertTrue(eng._should_refresh_stats(
            is_background=False, stage_changed=False, pct_changed=True,
            eta_became_ready=False, elapsed_since_update=0.06, status_interval=0.25,
        ))
        # Background mode: skips timer-only ticks (avoids stdout spam), fires on stage/pct/ETA-ready
        self.assertFalse(eng._should_refresh_stats(
            is_background=True, stage_changed=False, pct_changed=False,
            eta_became_ready=False, elapsed_since_update=10.0, status_interval=0.25,
        ))
        self.assertTrue(eng._should_refresh_stats(
            is_background=True, stage_changed=True, pct_changed=False,
            eta_became_ready=False, elapsed_since_update=0.0, status_interval=0.25,
        ))
        self.assertTrue(eng._should_refresh_stats(
            is_background=True, stage_changed=False, pct_changed=True,
            eta_became_ready=False, elapsed_since_update=0.0, status_interval=0.25,
        ))
        self.assertTrue(eng._should_refresh_stats(
            is_background=True, stage_changed=False, pct_changed=False,
            eta_became_ready=True, elapsed_since_update=0.0, status_interval=0.25,
        ))

    def test_render_scene_uses_ex_entry_point_for_progress_info(self):
        # Verify bridge.render_scene dispatches to rise_blender_render_scene_ex
        # when progress_info is passed and decodes _ProgressInfo fields cleanly.
        captured = []

        class FakeLib:
            def __init__(self):
                self.called_ex = False
                self.called_legacy = False

            def rise_blender_render_scene_ex(
                self, _scene, _settings, _prog_cb, info_cb, _img_cb, _ud, res_ptr, _err, _err_sz
            ):
                self.called_ex = True
                fn = ctypes.cast(info_cb, bridge._PROGRESS_INFO_CALLBACK)
                info = bridge._ProgressInfo(
                    progress=0.5,
                    elapsed_seconds=4.0,
                    remaining_seconds=4.0,
                    has_remaining=1,
                    title=b"Rasterizing Scene",
                )
                self.cb_ret = fn(None, ctypes.byref(info))
                res = res_ptr._obj
                res.width = 0
                res.height = 0
                return 1

            def rise_blender_render_scene(self, *args):
                self.called_legacy = True
                return 1

            def rise_blender_free_render_result(self, _res_ptr):
                pass

        fake_lib = FakeLib()
        fake_handle = SimpleNamespace(scene=bridge._Scene(), settings=bridge._RenderSettings())
        with mock.patch.object(bridge, "_load_library", return_value=fake_lib), \
             mock.patch.object(bridge, "_SceneHandle", return_value=fake_handle):
            bridge.render_scene(
                scene=None,
                settings=None,
                progress_info=lambda p, e, r, hr, t: (captured.append((p, e, r, hr, t)) or True),
            )
        self.assertTrue(fake_lib.called_ex)
        self.assertFalse(fake_lib.called_legacy)
        self.assertEqual(fake_lib.cb_ret, 1)
        self.assertEqual(captured, [(0.5, 4.0, 4.0, True, "Rasterizing Scene")])


if __name__ == "__main__":
    unittest.main(verbosity=2)
