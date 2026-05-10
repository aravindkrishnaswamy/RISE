#!/usr/bin/env python3
"""
Phase B2 scene-format migration: RISE ASCII SCENE 5 → 6.

Moves width/height/pixelAR lines OUT of <camera_type>_camera chunks
INTO a new `film` chunk inserted before the first camera chunk.

Multi-camera handling: if multiple cameras author the dim params,
the LAST camera's values win in the film chunk (matches the v5
parser's last-wins SetFilm semantics so renders are bit-equivalent
post-migration).

Usage:
    python tools/migrate_scenes_v5_to_v6.py [path-or-dir]

If no argument, defaults to `scenes/`.  Recurses; modifies in place.
Files without `RISE ASCII SCENE 5` header are left alone.

Round-trip:
- `git diff --shortstat` after running gives you the blast radius.
- Each migrated file: header bumped 5→6; if any camera authored
  width/height/pixelAR, a `film` chunk appears before the first
  camera chunk and the dim lines are removed from camera chunks.
- Files with no camera-authored dims (the rare default-defaults
  case) are version-bumped only.

Pre-existing `film` chunks (e.g. smoke-test scenes) are left
untouched apart from the version bump — the script never removes
or modifies an existing `film` chunk.
"""

import os
import re
import sys

# Every camera chunk keyword that AddCameraCommonParams populates.
CAMERA_KEYWORDS = {
    "pinhole_camera",
    "onb_pinhole_camera",
    "thinlens_camera",
    "fisheye_camera",
    "orthographic_camera",
}

# The three params being relocated.  Order matters for the emitted
# film chunk (matches the smoke-test scene's authoring order).
DIM_KEYS = ("width", "height", "pixelAR")


def detect_line_ending(text):
    if "\r\n" in text:
        return "\r\n"
    if "\r" in text and "\n" not in text:
        return "\r"
    return "\n"


def parse_dim_line(line):
    """If `line` declares one of width/height/pixelAR, return (key, value_str).
    Otherwise return None.  Preserves whatever the value side looks like
    (literal, math expr `$(...)`, macro ref `@FOO`, etc.) — we just
    extract the raw token string."""
    stripped = line.strip()
    for key in DIM_KEYS:
        # Anchor the key at the start, require whitespace after, capture
        # the value side up to a trailing comment or end of line.
        m = re.match(rf"^{key}\s+(.+?)\s*(?:#.*)?$", stripped)
        if m:
            return key, m.group(1).strip()
    return None


def migrate_text(content):
    """Returns (new_content, migrated_bool, dropped_lines)."""
    if "RISE ASCII SCENE 5" not in content:
        return content, False, 0

    le = detect_line_ending(content)
    lines = content.split(le)

    # Bump the version header.  There can be only one such line; the
    # parser fails on anything else.
    for i, line in enumerate(lines):
        if line.strip().startswith("RISE ASCII SCENE"):
            lines[i] = line.replace("RISE ASCII SCENE 5", "RISE ASCII SCENE 6", 1)
            break

    out_lines = []
    extracted = {}  # key -> raw value string (last-wins across cameras)
    first_camera_block_idx = None  # in out_lines coords
    dropped = 0

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped in CAMERA_KEYWORDS:
            # Record where the first camera chunk lands so we can
            # insert the film chunk above it.
            if first_camera_block_idx is None:
                first_camera_block_idx = len(out_lines)

            # Keep the keyword line.
            out_lines.append(line)
            i += 1
            # Expect `{` on its own line per the chunk-syntax invariant
            # (CLAUDE.md: "chunk braces must be on their own lines").
            if i < len(lines) and lines[i].strip() == "{":
                out_lines.append(lines[i])
                i += 1
                # Walk to matching `}` (no nested braces in chunk
                # bodies — RISE chunks are flat).
                while i < len(lines) and lines[i].strip() != "}":
                    inner = lines[i]
                    parsed = parse_dim_line(inner)
                    if parsed is not None:
                        key, value_str = parsed
                        extracted[key] = value_str
                        dropped += 1
                        # Drop the line — the film chunk above will
                        # carry the value.
                    else:
                        out_lines.append(inner)
                    i += 1
                # Append the closing `}`
                if i < len(lines):
                    out_lines.append(lines[i])
                    i += 1
            # else: malformed chunk — keep as-is (we already kept the
            # keyword line; just continue).
        else:
            out_lines.append(line)
            i += 1

    # Insert the film chunk above the first camera chunk if we extracted
    # anything.  We don't insert when nothing was authored — the
    # rendered scene then falls through to the qHD default installed
    # by Job::InitializeContainers, which is the same default that
    # would have applied if the camera chunk's GetUInt fallback ran.
    if extracted and first_camera_block_idx is not None:
        film_chunk = []
        film_chunk.append("film")
        film_chunk.append("{")
        for key in DIM_KEYS:
            if key in extracted:
                film_chunk.append(f"\t{key}\t\t\t{extracted[key]}")
        film_chunk.append("}")
        film_chunk.append("")  # blank line after for readability
        out_lines = (
            out_lines[:first_camera_block_idx] + film_chunk + out_lines[first_camera_block_idx:]
        )

    new_content = le.join(out_lines)
    return new_content, new_content != content, dropped


def migrate_file(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        content = f.read()
    new_content, changed, dropped = migrate_text(content)
    if changed:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(new_content)
    return changed, dropped


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "scenes"
    if os.path.isfile(target):
        files = [target]
    else:
        files = []
        for dp, _, fns in os.walk(target):
            for fn in fns:
                if fn.endswith(".RISEscene"):
                    files.append(os.path.join(dp, fn))

    migrated = 0
    unchanged = 0
    total_dropped = 0
    for fp in sorted(files):
        changed, dropped = migrate_file(fp)
        if changed:
            migrated += 1
            total_dropped += dropped
        else:
            unchanged += 1

    print(f"Scanned: {len(files)} files")
    print(f"Migrated: {migrated} files")
    print(f"Unchanged: {unchanged} files")
    print(f"Dim lines relocated: {total_dropped}")


if __name__ == "__main__":
    main()
