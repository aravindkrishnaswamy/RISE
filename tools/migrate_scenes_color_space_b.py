#!/usr/bin/env python3
"""Migrate `color_space ROMMRGB_Linear` declarations on image-painter
chunks (png_painter / jpg_painter / image_painter / etc.) that were
the canonical "verbatim store, no matrix conversion" idiom to
`color_space Rec709RGB_Linear`.

Context: pre-2026-05-24, RISE's internal working space was ROMM RGB,
so `color_space ROMMRGB_Linear` on an image-painter chunk was the
canonical "identity" decoder — bytes flowed verbatim into RISEPel.
Post Stage B of the colour-space migration, RISEPel = Rec709RGBPel,
and `ROMMRGB_Linear` now applies a real Rec.709 → ROMM Bradford+
matrix conversion that warps any verbatim data — most notably
normal-map vectors and packed scalar channels.

The new "identity" idiom (now that RISEPel is Rec.709) is
`color_space Rec709RGB_Linear`.

PRESERVED AS-IS (NOT migrated):
  * `file_rasterizeroutput` chunks: `color_space ROMMRGB_Linear` here
    means "encode the OUTPUT in ROMM primaries" — a deliberate
    archival choice.  Post Stage B this still works correctly (the
    EXR/HDR/PNG writers apply a real Rec.709 → ROMM conversion at
    encode time, producing actual ROMM-encoded files).
  * `uniformcolor_painter` chunks using `colorspace` (one word, no
    underscore): this is a USER COLOR-SPACE TAG meaning "interpret
    these RGB numbers as ROMM coordinates", which converts to RISEPel
    correctly post Stage B.

ONLY MIGRATED:
  * Image-painter chunks (png_painter, jpg_painter, image_painter,
    etc.) with `color_space ROMMRGB_Linear` on a parameter line that
    was the verbatim-store idiom — these would silently warp data
    post Stage B.

Usage:
    python3 tools/migrate_scenes_color_space_b.py            # walks scenes/
    python3 tools/migrate_scenes_color_space_b.py scenes/Tests/Materials
    python3 tools/migrate_scenes_color_space_b.py --dry-run  # report only
"""
import argparse
import os
import re
import sys


def find_scenes(roots):
    out = []
    for root in roots:
        if os.path.isfile(root) and root.endswith(".RISEscene"):
            out.append(root)
            continue
        for dirpath, _, filenames in os.walk(root):
            for fn in filenames:
                if fn.endswith(".RISEscene"):
                    out.append(os.path.join(dirpath, fn))
    return sorted(out)


# Chunk types that load image bytes and benefit from the verbatim-
# store idiom.  Anything not in this set is left alone.
IMAGE_PAINTER_CHUNKS = {
    "png_painter",
    "jpg_painter",
    "jpeg_painter",
    "tiff_painter",
    "tif_painter",
    "tga_painter",
    "image_painter",
    "exr_painter",
    "hdr_painter",
    "pfm_painter",
}


def migrate_text(text):
    """Walk chunk-by-chunk; rewrite `color_space ROMMRGB_Linear` only
    inside image-painter chunks.  Returns (new_text, count)."""
    out_lines = []
    current_chunk = None    # the most-recent chunk keyword we saw
    depth = 0               # brace depth (the file is a flat sequence
                            # of single-brace chunks, but defend anyway)
    count = 0
    for line in text.splitlines(keepends=True):
        stripped = line.strip()

        # Chunk-open detection: a bare identifier line followed by
        # a `{` on the next line is the canonical form.  We track the
        # keyword as soon as we see a non-comment, non-blank identifier
        # line OUTSIDE any chunk.
        if depth == 0:
            if stripped and not stripped.startswith("#"):
                # Treat the first identifier on the line as the chunk
                # keyword.  The keyword is followed by either nothing
                # (next line is `{`) or a `{` on the same line.
                tok = stripped.split(None, 1)[0]
                if tok != "{":
                    current_chunk = tok

        # Brace tracking.
        depth += line.count("{")
        depth -= line.count("}")

        # Inside a chunk, look for the `color_space ROMMRGB_Linear`
        # line and rewrite it ONLY for image-painter chunks.
        if depth > 0 and current_chunk in IMAGE_PAINTER_CHUNKS:
            m = re.match(
                r"^(?P<indent>\s*)color_space(?P<sep>\s+)ROMMRGB_Linear\s*$",
                line.rstrip("\n").rstrip("\r"),
            )
            if m:
                new_line = (
                    m.group("indent")
                    + "color_space"
                    + m.group("sep")
                    + "Rec709RGB_Linear"
                    + line[len(line.rstrip()):]   # preserve newline
                )
                out_lines.append(new_line)
                count += 1
                continue

        out_lines.append(line)

    return "".join(out_lines), count


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "paths",
        nargs="*",
        default=["scenes"],
        help="files or directories to walk (default: scenes/)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="report matches without modifying files",
    )
    args = parser.parse_args()

    scenes = find_scenes(args.paths)
    if not scenes:
        print(f"no .RISEscene files found under: {args.paths}", file=sys.stderr)
        sys.exit(1)

    total_files = 0
    total_changes = 0
    for path in scenes:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        new_text, count = migrate_text(text)
        if count == 0:
            continue
        total_files += 1
        total_changes += count
        rel = os.path.relpath(path)
        if args.dry_run:
            print(f"  [dry-run] {rel}: {count} replacement(s)")
        else:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new_text)
            print(f"  {rel}: {count} replacement(s)")

    print(
        f"\n{'Would migrate' if args.dry_run else 'Migrated'} "
        f"{total_files} file(s), {total_changes} `color_space ROMMRGB_Linear` "
        f"→ `color_space Rec709RGB_Linear` total (image-painter chunks only)."
    )
    if args.dry_run and total_changes > 0:
        print("Re-run without --dry-run to apply.")


if __name__ == "__main__":
    main()
