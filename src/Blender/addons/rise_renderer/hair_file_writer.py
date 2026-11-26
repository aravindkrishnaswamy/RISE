"""Pure-python writer for the Cem Yuksel `.hair` binary strand format
(cemyuksel.com/research/hairmodels).

Deliberately **bpy-free** so it is importable and unit-testable with a
plain ``python3`` interpreter (see ``test_hair_file_writer.py`` in this
directory, run with ``python3 -m unittest
src.Blender.addons.rise_renderer.test_hair_file_writer`` or directly as
a script — see that file's own header for the exact invocation).  The
bpy-dependent glue that pulls point/radius arrays out of a Blender
``Curves`` datablock lives in ``exporter.py`` and calls into
``build_hair_strands_from_curve_arrays`` below with plain lists, so the
translation logic itself stays covered by tests that don't need a
running Blender.

Format reference: the authoritative byte layout is documented in
``src/Library/Importers/HairFileLoader.h``'s header comment (the RISE
reader this writer's output is meant to feed).  Summary, little-endian
throughout:

    offset  size  field
    ------  ----  ----------------------------------------------------
      0      4    magic 'H','A','I','R'
      4      4    uint32  strand count
      8      4    uint32  total point count (ALL strands)
     12      4    uint32  array-presence bit flags
     16      4    uint32  default segments per strand
     20      4    float   default thickness
     24      4    float   default transparency
     28     12    float3  default colour
     40     88    char    free-form ASCII file info
    ------  ----  ----------------------------------------------------
    then, in this order, each present only if its flag bit is set:
      bit 0 (0x01)  segments      uint16 per STRAND
      bit 1 (0x02)  points        float3 per POINT   (always written)
      bit 2 (0x04)  thickness     float  per POINT
      bit 3 (0x08)  transparency  float  per POINT    (never written)
      bit 4 (0x10)  colours       float3 per POINT    (never written)

This writer always emits the segments array (bit 0) and the points
array (bit 1); it emits the thickness array (bit 2) iff every strand
carries thickness data.  It never emits transparency or colour arrays
— ``HairFileLoader.h`` reads and discards both (RISE has no per-vertex
strand opacity or albedo: colour comes from the bound ``hair_material``
and hair has no per-point transparency concept), so writing them would
just be dead weight in the file.

THICKNESS IS A FULL WIDTH, NOT A RADIUS — matching
``HairFileLoader.h``'s documented reading of the ambiguous
specification and ``hair_geometry``'s own ``width_root``/``width_tip``
convention.  A caller converting from a per-point RADIUS (Blender's
curve ``radius`` attribute, Cem Yuksel's usual convention when curves
come from a modelling tool) must multiply by 2 before calling
``write_hair_file`` — ``build_hair_strands_from_curve_arrays`` below
does this for the Blender export path.
"""

from __future__ import annotations

import struct

__all__ = [
    "HairFileError",
    "write_hair_file",
    "build_hair_strands_from_curve_arrays",
]

_MAGIC = b"HAIR"
_HEADER_SIZE = 128
_INFO_SIZE = 88

FLAG_SEGMENTS = 1 << 0
FLAG_POINTS = 1 << 1
FLAG_THICKNESS = 1 << 2
FLAG_TRANSPARENCY = 1 << 3
FLAG_COLOR = 1 << 4

# uint16 per-strand segment count cap (the field's own width) — a
# strand with more than this many segments (65536 points) can't be
# expressed in this format at all.  Vastly beyond anything a real
# groom (or even a research hero-strand asset) uses; this guard exists
# so a pathological input fails loudly instead of silently wrapping.
_MAX_SEGMENTS_PER_STRAND = 0xFFFF


class HairFileError(ValueError):
    """Raised by `write_hair_file` on a malformed strand list."""


def build_hair_strands_from_curve_arrays(
    positions_flat,
    curve_offsets,
    radii_flat=None,
):
    """Translate flat per-point arrays (as pulled from a Blender
    ``Curves`` datablock via ``foreach_get``) into the strand-record
    list `write_hair_file` accepts.

    Pure logic, no bpy — the caller hands in plain sequences so this
    stays independently testable.

    Args:
        positions_flat: flat sequence of floats, length
            ``3 * total_points``, POINT-major (x, y, z per point, every
            strand's points concatenated in curve order).
        curve_offsets: sequence of ints, length ``num_curves + 1``.
            Curve ``i`` owns points
            ``[curve_offsets[i], curve_offsets[i + 1])`` into
            `positions_flat` / `radii_flat`.  This is exactly
            Blender's ``Curves.curves[i].first_point_index`` shape
            with one extra trailing sentinel equal to the total point
            count.
        radii_flat: optional flat sequence of floats, length
            ``total_points`` — Blender's per-point curve RADIUS (half
            the fibre width).  When ``None``, strands carry no
            thickness data (the caller's `.hair` file omits the
            thickness array entirely and RISE falls back to its own
            human-hair default at import — see
            ``HairFileLoader.h``'s width-fallback note).  When
            present, every strand gets a thickness array of
            ``2 * radius`` per point (full width, per this module's
            docstring).

    Returns:
        A list of strand dicts: ``{"points": [(x, y, z), ...],
        "thickness": [w0, ..., wn] | None}``, ready for
        `write_hair_file`.  Strands with fewer than 2 points are
        DROPPED (a `.hair` strand needs >= 1 segment, i.e. >= 2
        points) — this is a per-strand skip, not a whole-groom
        failure, mirroring `HairFileLoader.h`'s own per-strand
        rejection policy.  Ragged strand point-counts (a different
        number of points per strand) are fully supported; that's the
        normal case for hair.
    """

    num_curves = len(curve_offsets) - 1
    if num_curves < 1:
        return []

    strands = []
    for i in range(num_curves):
        start = int(curve_offsets[i])
        end = int(curve_offsets[i + 1])
        if end < start:
            raise HairFileError(
                f"build_hair_strands_from_curve_arrays: curve {i} has a negative point "
                f"range (offsets[{i}]={start} > offsets[{i + 1}]={end})"
            )
        count = end - start
        if count < 2:
            continue

        points = []
        for p in range(start, end):
            base = p * 3
            points.append(
                (
                    float(positions_flat[base]),
                    float(positions_flat[base + 1]),
                    float(positions_flat[base + 2]),
                )
            )

        thickness = None
        if radii_flat is not None:
            thickness = [2.0 * float(radii_flat[p]) for p in range(start, end)]

        strands.append({"points": points, "thickness": thickness})

    return strands


def write_hair_file(
    path,
    strands,
    *,
    default_segments: int = 0,
    default_thickness: float = 0.0,
    default_transparency: float = 0.0,
    default_color=(0.0, 0.0, 0.0),
    info: str = "",
):
    """Write `strands` as a Cem Yuksel `.hair` file at `path`.

    Args:
        path: output filepath (opened for binary write; any existing
            file is overwritten).
        strands: a list of ``{"points": [(x, y, z), ...], "thickness":
            [w0, ...] | None}`` dicts — see
            `build_hair_strands_from_curve_arrays`.  ``thickness``
            must be present on EVERY strand or on NONE (the format's
            thickness array, if present at all, covers the whole
            file) — a mix raises `HairFileError`.
        default_segments / default_thickness / default_transparency /
            default_color / info: verbatim header fields.  `info` is
            truncated to 88 bytes (UTF-8 unsafe — ASCII only, per the
            format's "free-form ASCII" field) and NUL-padded.

    Raises:
        HairFileError: empty strand list, a strand with < 2 points, a
            strand whose thickness array length doesn't match its
            point count, inconsistent thickness presence across
            strands, or a strand with more than 65535 segments (the
            format's uint16 per-strand segment field can't hold more).
    """

    if not strands:
        raise HairFileError("write_hair_file: no strands to write (empty groom)")

    num_strands = len(strands)
    segments = []
    total_points = 0
    has_thickness = strands[0].get("thickness") is not None

    for index, strand in enumerate(strands):
        points = strand.get("points")
        if points is None or len(points) < 2:
            raise HairFileError(
                f"write_hair_file: strand {index} has fewer than 2 points"
            )
        n_segments = len(points) - 1
        if n_segments > _MAX_SEGMENTS_PER_STRAND:
            raise HairFileError(
                f"write_hair_file: strand {index} has {n_segments} segments, over the "
                f"format's uint16 per-strand cap of {_MAX_SEGMENTS_PER_STRAND}"
            )
        segments.append(n_segments)
        total_points += len(points)

        strand_has_thickness = strand.get("thickness") is not None
        if strand_has_thickness != has_thickness:
            raise HairFileError(
                "write_hair_file: thickness must be present on every strand or on none "
                f"(strand 0 {'has' if has_thickness else 'lacks'} it, strand {index} "
                f"{'has' if strand_has_thickness else 'lacks'} it)"
            )
        if strand_has_thickness and len(strand["thickness"]) != len(points):
            raise HairFileError(
                f"write_hair_file: strand {index} thickness array length "
                f"({len(strand['thickness'])}) does not match its point count ({len(points)})"
            )

    flags = FLAG_SEGMENTS | FLAG_POINTS
    if has_thickness:
        flags |= FLAG_THICKNESS

    info_bytes = info.encode("ascii", errors="replace")[:_INFO_SIZE]
    info_bytes = info_bytes + b"\x00" * (_INFO_SIZE - len(info_bytes))

    header = bytearray()
    header += _MAGIC
    header += struct.pack("<I", num_strands)
    header += struct.pack("<I", total_points)
    header += struct.pack("<I", flags)
    header += struct.pack("<I", int(default_segments))
    header += struct.pack("<f", float(default_thickness))
    header += struct.pack("<f", float(default_transparency))
    header += struct.pack("<fff", *[float(c) for c in default_color])
    header += info_bytes
    if len(header) != _HEADER_SIZE:
        # Defensive — a bug in the packing above, not a caller error.
        raise HairFileError(
            f"write_hair_file: internal error, header packed to {len(header)} bytes, expected {_HEADER_SIZE}"
        )

    body = bytearray()
    body += struct.pack(f"<{num_strands}H", *segments)
    for strand in strands:
        for (x, y, z) in strand["points"]:
            body += struct.pack("<fff", x, y, z)
    if has_thickness:
        for strand in strands:
            for t in strand["thickness"]:
                body += struct.pack("<f", t)

    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(body)
