#!/usr/bin/env python3
"""
migrate_scenes_light_colorspace.py — preserve the look of scenes authored
before lights took LINEAR colour (2026-09-02).

WHAT CHANGED.  `omni_light`, `spot_light`, `directional_light` and
`ambient_light` used to gamma-DECODE their authored `color` triple as sRGB:
`Job::Add*Light` wrapped it in `sRGBPel(...)`, so `color 1.0 0.2 0.2` lit the
scene with (1.0, 0.033, 0.033).  Nothing in the scene language said so, and
the SAME triple on a `uniformcolor_painter` (or any emissive material's
`exitance`) meant what it said — linear.  Lights now read `color` as LINEAR
Rec.709 and take an optional `colorspace` parameter, exactly like the
painters.

WHAT THIS SCRIPT DOES.  For every `omni_light` / `spot_light` /
`directional_light` / `ambient_light` chunk that has a `color` line and no
`colorspace` line, insert `colorspace<tabs>sRGB` right after the `color`
line, so the value keeps decoding exactly as it did before.  The rendered
image is unchanged bit-for-bit.

WHAT IT DELIBERATELY SKIPS.  A chunk whose `color` components are ALL in
{0, 1} — `1 1 1`, `1 0 0`, `0 0 0`.  Those are the fixed points of the sRGB
transfer function, so decoding and not decoding give the identical RISEPel
and adding the line would be noise in the diff.  This is also what makes the
script idempotent-in-spirit: re-running it on a migrated corpus finds
nothing left to do.

Idempotent: a chunk that already carries a `colorspace` line is never
touched, whatever its value.

Line-by-line so indentation, tab style, comments and brace placement are all
preserved.

Usage:
    python3 tools/migrate_scenes_light_colorspace.py [--dry-run] [--root scenes]
    python3 tools/migrate_scenes_light_colorspace.py --root scenes/Tests -v
"""

import argparse
import pathlib
import re
import sys

LIGHT_KINDS = ('omni_light', 'spot_light', 'directional_light', 'ambient_light')

# A chunk keyword sitting alone on its line (optionally with trailing
# whitespace / a comment).  The scene language puts the opening brace on its
# own line, so the keyword line carries nothing else.
KIND_RE = re.compile(r'^\s*(' + '|'.join(LIGHT_KINDS) + r')\s*(?:#.*)?$')

# `color` followed by its three components; capture the whitespace run
# between the keyword and the first component so the inserted line can
# reproduce the file's own tab style.
COLOR_RE = re.compile(
    r'^(\s*)color(\s+)('
    r'[-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s*(?:#.*)?$')

COLORSPACE_RE = re.compile(r'^\s*colorspace\b')


def _is_transfer_fixed_point(components):
    """True when every component is 0 or 1 -- sRGB-decoding is a no-op."""
    for c in components:
        try:
            v = float(c)
        except ValueError:
            return False        # unparseable -> be conservative, migrate it
        if v not in (0.0, 1.0):
            return False
    return True


def migrate_text(text, stats, path_label=''):
    """Return (new_text, changed).  Mutates `stats` in place."""
    lines = text.split('\n')
    out = []
    i = 0
    n = len(lines)
    changed = False

    while i < n:
        line = lines[i]
        m = KIND_RE.match(line)
        if not m:
            out.append(line)
            i += 1
            continue

        kind = m.group(1)

        # Collect the chunk: keyword line, `{` line, body, `}` line.  If the
        # next non-blank line is not an opening brace this is not a chunk
        # header (e.g. prose in a comment block that the regex let through);
        # emit it untouched.
        j = i + 1
        while j < n and lines[j].strip() == '':
            j += 1
        if j >= n or lines[j].strip() != '{':
            out.append(line)
            i += 1
            continue

        depth = 0
        end = None
        for k in range(j, n):
            s = lines[k].strip()
            if s == '{':
                depth += 1
            elif s == '}':
                depth -= 1
                if depth == 0:
                    end = k
                    break
        if end is None:
            # Unterminated chunk -- leave the rest of the file alone.
            out.extend(lines[i:])
            i = n
            break

        body = lines[i:end + 1]
        stats['chunks_seen'] += 1

        if any(COLORSPACE_RE.match(b) for b in body):
            stats['skipped_has_colorspace'] += 1
            out.extend(body)
            i = end + 1
            continue

        color_idx = None
        color_match = None
        for idx, b in enumerate(body):
            cm = COLOR_RE.match(b)
            if cm:
                color_idx = idx
                color_match = cm
                break

        if color_idx is None:
            stats['skipped_no_color'] += 1
            out.extend(body)
            i = end + 1
            continue

        comps = color_match.group(3, 4, 5)
        if _is_transfer_fixed_point(comps):
            stats['skipped_neutral'] += 1
            out.extend(body)
            i = end + 1
            continue

        indent = color_match.group(1)
        gap = color_match.group(2)
        # `colorspace` is 5 characters longer than `color`; if the file uses
        # tabs, drop one tab-stop's worth so the values stay aligned when the
        # tab width is 4 or 8.  If it uses spaces, just reuse the run.
        if '\t' in gap and len(gap) > 1:
            new_gap = gap[:-1]
        else:
            new_gap = gap
        body.insert(color_idx + 1, '%scolorspace%ssRGB' % (indent, new_gap))
        stats['colors_encoded'] += 1
        if path_label:
            stats['files_touched_set'].add(path_label)
        changed = True
        out.extend(body)
        i = end + 1

    return '\n'.join(out), changed


def migrate_file(path, stats, dry_run=False, verbose=False):
    text = path.read_text()
    new_text, changed = migrate_text(text, stats, path_label=str(path))
    if changed and not dry_run:
        path.write_text(new_text)
    if changed and verbose:
        print('  %s: %s' % ('would migrate' if dry_run else 'migrated', path))
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='scenes')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    stats = {
        'chunks_seen': 0,
        'colors_encoded': 0,
        'skipped_neutral': 0,
        'skipped_no_color': 0,
        'skipped_has_colorspace': 0,
        'files_touched_set': set(),
    }

    root = pathlib.Path(args.root)
    paths = sorted(root.rglob('*.RISEscene')) if root.is_dir() else [root]
    changed_count = 0
    for path in paths:
        try:
            if migrate_file(path, stats, dry_run=args.dry_run,
                            verbose=args.verbose):
                changed_count += 1
                if not args.verbose:
                    print('  %s: %s' % (
                        'would migrate' if args.dry_run else 'migrated', path))
        except Exception as e:                       # noqa: BLE001
            print('  ERROR %s: %s' % (path, e), file=sys.stderr)

    print('', file=sys.stderr)
    print('%d files scanned, %d light chunks seen' % (len(paths), stats['chunks_seen']),
          file=sys.stderr)
    print('  %d colour lines %sgiven `colorspace sRGB` in %d files'
          % (stats['colors_encoded'],
             'would be ' if args.dry_run else '',
             changed_count),
          file=sys.stderr)
    print('  %d chunks skipped: colour components all in {0,1} (sRGB is identity there)'
          % stats['skipped_neutral'], file=sys.stderr)
    print('  %d chunks skipped: no `color` line' % stats['skipped_no_color'],
          file=sys.stderr)
    print('  %d chunks skipped: already carry a `colorspace` line'
          % stats['skipped_has_colorspace'], file=sys.stderr)


if __name__ == '__main__':
    main()
