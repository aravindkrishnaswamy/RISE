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

Structural decisions are made on a COMMENT-STRIPPED view of the file that
mirrors RISE's own lexer (see `strip_comments`), so a comment can never hide
a brace or a `color` line; the bytes written back are the originals.  An
unterminated chunk is DIAGNOSED and makes the run exit non-zero, rather than
silently abandoning the rest of the file.

Usage:
    python3 tools/migrate_scenes_light_colorspace.py [--dry-run] [--root scenes]
    python3 tools/migrate_scenes_light_colorspace.py --root scenes/Tests -v
    python3 tools/migrate_scenes_light_colorspace.py --selftest
"""

import argparse
import pathlib
import re
import sys

LIGHT_KINDS = ('omni_light', 'spot_light', 'directional_light', 'ambient_light')

# A chunk keyword on its own line, optionally with the opening brace on the
# SAME line.  Both spellings are legal: RISE's lexer (Cst.cpp `Tokenize`)
# treats `{` / `}` as single-character punctuation and whitespace as trivia,
# so `omni_light {` tokenises identically to `omni_light` + newline + `{`.
# Matched against the COMMENT-STRIPPED view of the line (see strip_comments),
# so a trailing `# note` is already gone by the time this runs.
KIND_RE = re.compile(r'^\s*(' + '|'.join(LIGHT_KINDS) + r')\s*(\{)?\s*$')

# `color` followed by its three components; capture the whitespace run
# between the keyword and the first component so the inserted line can
# reproduce the file's own tab style.  Also matched against the stripped view.
COLOR_RE = re.compile(
    r'^(\s*)color(\s+)('
    r'[-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s*$')

COLORSPACE_RE = re.compile(r'^\s*colorspace\b')


def strip_comments(lines):
    """Return a same-length list of lines with COMMENT TEXT removed.

    Mirrors RISE's own lexer (`Tokenize` in src/Library/Cst/Cst.cpp), which
    absorbs `#`-to-end-of-line comments and `/* ... */` block comments as
    trivia ANYWHERE -- including immediately after a brace.  The scene
    language has no quoted strings at all (a word token stops at whitespace,
    `#`, `{`, `}` or `/*`), so a `#` is unconditionally the start of a
    comment and stripping it can never eat data.

    Why this matters here: the brace-depth walk below used to compare
    `line.strip()` against exactly `'{'` / `'}'`, so a perfectly legal
    `}\t# end of the key light` was invisible to it -- the walk never found
    the chunk's close, gave up, and silently abandoned the REST OF THE FILE
    (a two-light file with one such brace reported 0 chunks and exited 0).

    Block-comment state carries ACROSS lines, so a `/* ... */` spanning a
    `}` hides that brace exactly as the lexer does.
    """
    out = []
    in_block = False
    for line in lines:
        buf = []
        i = 0
        n = len(line)
        while i < n:
            if in_block:
                j = line.find('*/', i)
                if j < 0:
                    i = n
                else:
                    in_block = False
                    i = j + 2
                continue
            if line.startswith('/*', i):
                in_block = True
                i += 2
                continue
            if line[i] == '#':
                i = n                       # `#` to end of line, per the lexer
                continue
            buf.append(line[i])
            i += 1
        out.append(''.join(buf))
    return out


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
    """Return (new_text, changed).  Mutates `stats` in place.

    Two parallel views of the file: `lines` (verbatim -- what gets written
    back, so indentation, tab style, comments and brace placement all
    survive) and `code` (comment-stripped -- what every structural decision
    is made on, so a comment can never hide a brace or a `color` line).
    """
    lines = text.split('\n')
    code = strip_comments(lines)
    out = []
    i = 0
    n = len(lines)
    changed = False

    while i < n:
        line = lines[i]
        m = KIND_RE.match(code[i])
        if not m:
            out.append(line)
            i += 1
            continue

        kind = m.group(1)
        brace_on_keyword_line = m.group(2) is not None

        # Locate the chunk's opening brace.  Either it is on the keyword line
        # (`omni_light {`) or it is the next non-blank CODE line.  If that
        # line is something else, this is not a chunk header (e.g. the
        # keyword named in running prose); emit it untouched.
        if brace_on_keyword_line:
            start = i
            head_offset = m.start(2)          # count braces from the `{` itself
        else:
            j = i + 1
            while j < n and code[j].strip() == '':
                j += 1
            if j >= n or not code[j].lstrip().startswith('{'):
                out.append(line)
                i += 1
                continue
            start = j
            head_offset = 0

        # Brace-depth walk over the COMMENT-STRIPPED text, character by
        # character rather than whole-line equality -- that is what the lexer
        # does, and it is what makes `}\t# end` and `{ # note` (and a brace
        # sharing a line with anything else) count correctly.
        depth = 0
        end = None
        for k in range(start, n):
            seg = code[k][head_offset:] if k == start else code[k]
            for ch in seg:
                if ch == '{':
                    depth += 1
                elif ch == '}':
                    depth -= 1
                    if depth == 0:
                        end = k
                        break
            if end is not None:
                break
        if end is None:
            # Unterminated chunk.  This is a real defect in the input (or in
            # this scanner), not something to swallow: say so and make the
            # process fail, rather than silently abandoning the rest of the
            # file the way the pre-2026-09-02 version did.
            stats['unterminated'] += 1
            print('  ERROR %s: unterminated `%s` chunk starting at line %d '
                  '(no matching `}`); the rest of the file was NOT scanned'
                  % (path_label or '<text>', kind, i + 1), file=sys.stderr)
            out.extend(lines[i:])
            i = n
            break

        body = lines[i:end + 1]
        body_code = code[i:end + 1]
        stats['chunks_seen'] += 1

        if any(COLORSPACE_RE.match(b) for b in body_code):
            stats['skipped_has_colorspace'] += 1
            out.extend(body)
            i = end + 1
            continue

        # LAST `color` line, not the first.  The derive is LAST-WINS for a
        # non-repeatable param (`ParseStateBag::SetSingle` is an unconditional
        # overwrite, and Cst::ParamValueAsParsed reads the same way), so on a
        # chunk that spells `color` twice the EFFECTIVE line is the last one --
        # and `colorspace` has to sit where it governs that line's reading, not
        # a dead one above it.  Inserting after the first would also mis-skip:
        # the neutrality test below has to be applied to the components that
        # actually derive.
        color_idx = None
        color_match = None
        color_count = 0
        for idx, b in enumerate(body_code):
            cm = COLOR_RE.match(b)
            if cm:
                color_idx = idx
                color_match = cm
                color_count += 1

        if color_count > 1:
            # Not an error -- the scene still derives (the last line wins) --
            # but it IS a defect worth naming: every earlier `color` line is
            # dead text, and RISE's own editor REFUSES to write a param it
            # finds duplicated (Job::ApplyCstParamEdit's duplicate-occurrence
            # guard), so a light like this cannot be colour-edited in the GUI
            # until the dead lines are deleted.
            stats['duplicate_color_chunks'] += 1
            print('  NOTE %s: `%s` chunk at line %d spells `color` %d times; '
                  'the scene derives from the LAST one, so that is the line any '
                  '`colorspace` goes after (delete the dead `color` lines -- the '
                  'editor refuses to write a duplicated param)'
                  % (path_label or '<text>', kind, i + 1, color_count))

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


def new_stats():
    return {
        'chunks_seen': 0,
        'colors_encoded': 0,
        'skipped_neutral': 0,
        'skipped_no_color': 0,
        'skipped_has_colorspace': 0,
        'duplicate_color_chunks': 0,
        'unterminated': 0,
        'files_touched_set': set(),
    }


#######################################################################
# Self-test.  `python3 tools/migrate_scenes_light_colorspace.py --selftest`
#
# Every case here is a shape the pre-2026-09-02 scanner got WRONG (or a
# shape it got right that the rewrite must not break).  The comment-on-a-
# brace cases are the ones that motivated the rewrite: they made the walk
# abandon the rest of the file with a 0 exit status, so a migration could
# report success while leaving most of a scene un-migrated.
#######################################################################

_SELFTESTS = [
    (
        'trailing comment on the closing brace',
        'omni_light\n{\nname a\ncolor 0.5 0.4 0.3\n}\t# end of key\n'
        'omni_light\n{\nname b\ncolor 0.2 0.2 0.2\n}\n',
        2, 2,
    ),
    (
        'trailing comment on the opening brace',
        'omni_light\n{ # the key light\nname a\ncolor 0.5 0.4 0.3\n}\n',
        1, 1,
    ),
    (
        'comment on the keyword line',
        'omni_light  # key\n{\nname a\ncolor 0.5 0.4 0.3\n}\n',
        1, 1,
    ),
    (
        'opening brace on the keyword line',
        'spot_light {\nname a\ncolor 0.5 0.4 0.3\ntarget 0 0 0\n}\n',
        1, 1,
    ),
    (
        'trailing comment on the colour line',
        'omni_light\n{\nname a\ncolor 0.5 0.4 0.3   # picked off a swatch\n}\n',
        1, 1,
    ),
    (
        'block comment hiding a brace',
        'omni_light\n{\nname a\ncolor 0.5 0.4 0.3\n/* a note\n   } not a real brace\n*/\n}\n',
        1, 1,
    ),
    (
        'a `#` inside a block comment does not end it',
        'omni_light\n{\nname a\ncolor 0.5 0.4 0.3\n/* # still a comment\n*/\n}\n',
        1, 1,
    ),
    (
        'already migrated -- idempotent',
        'omni_light\n{\nname a\ncolor 0.5 0.4 0.3\ncolorspace sRGB\n}\n',
        1, 0,
    ),
    (
        'transfer fixed point -- skipped as diff noise',
        'omni_light\n{\nname a\ncolor 1 1 1\n}\n',
        1, 0,
    ),
    (
        'keyword inside a comment is not a chunk header',
        '# an omni_light\nsphere_geometry\n{\nname g\nradius 1\n}\n',
        0, 0,      # nothing here is a light chunk
    ),
    (
        'bare keyword with no brace is not a chunk header',
        'omni_light\nsphere_geometry\n{\nname g\nradius 1\n}\n',
        0, 0,
    ),
    (
        'nested braces do not close the chunk early',
        'omni_light\n{\nname a\n{\n}\ncolor 0.5 0.4 0.3\n}\n',
        1, 1,
    ),
    (
        # Round-2 review: the derive is LAST-WINS, so the line has to govern
        # the LAST `color`.  The positional half of this is asserted
        # separately below (the counters alone cannot see WHERE it landed).
        'duplicate `color` lines -- migrate after the LAST one',
        'omni_light\n{\nname a\ncolor 0.9 0.8 0.7\ncolor 0.5 0.4 0.3\n}\n',
        1, 1,
    ),
    (
        # ... and the neutrality skip must be judged on the components that
        # actually derive.  First line is non-neutral, LAST is a transfer
        # fixed point -> nothing to do.  Keying off the FIRST would insert a
        # line here and change the render.
        'duplicate `color` lines -- neutrality judged on the LAST one',
        'omni_light\n{\nname a\ncolor 0.9 0.8 0.7\ncolor 1 1 1\n}\n',
        1, 0,
    ),
]


def selftest():
    failures = 0

    for label, text, want_chunks, want_encoded in _SELFTESTS:
        stats = new_stats()
        new_text, changed = migrate_text(text, stats, path_label='<selftest>')
        ok = stats['chunks_seen'] == want_chunks \
            and stats['colors_encoded'] == want_encoded \
            and stats['unterminated'] == 0
        if not ok:
            failures += 1
            print('  FAIL: %s -- chunks_seen=%d (want %d), colors_encoded=%d (want %d)'
                  % (label, stats['chunks_seen'], want_chunks,
                     stats['colors_encoded'], want_encoded), file=sys.stderr)
            continue
        # An encoding case must actually have inserted the line, and must be a
        # FIXED POINT: re-running finds nothing left to do.
        if want_encoded:
            if 'colorspace' not in new_text or not changed:
                failures += 1
                print('  FAIL: %s -- no `colorspace` line landed' % label, file=sys.stderr)
                continue
            again = new_stats()
            text2, changed2 = migrate_text(new_text, again, path_label='<selftest>')
            if changed2 or text2 != new_text:
                failures += 1
                print('  FAIL: %s -- not idempotent on a second pass' % label, file=sys.stderr)
                continue
        elif changed:
            failures += 1
            print('  FAIL: %s -- expected no change but the text was rewritten' % label, file=sys.stderr)
            continue
        print('  ok: %s' % label)

    # WHERE the line landed, for the duplicate-`color` case -- the counters
    # above prove one was inserted, not that it governs the effective line.
    # `colorspace` must sit AFTER the last `color`, or the derive reads the
    # live colour through the language default (linear) and the look changes.
    stats = new_stats()
    dup_in = 'omni_light\n{\nname a\ncolor 0.9 0.8 0.7\ncolor 0.5 0.4 0.3\n}\n'
    dup_out, _ = migrate_text(dup_in, stats, path_label='<selftest>')
    dup_want = ('omni_light\n{\nname a\ncolor 0.9 0.8 0.7\ncolor 0.5 0.4 0.3\n'
                'colorspace sRGB\n}\n')
    if dup_out != dup_want:
        failures += 1
        print('  FAIL: duplicate `color` -- `colorspace` did not land after the LAST '
              'colour line; got:\n%r\nwant:\n%r' % (dup_out, dup_want), file=sys.stderr)
    elif stats['duplicate_color_chunks'] != 1:
        failures += 1
        print('  FAIL: duplicate `color` -- the chunk was not COUNTED as duplicated '
              '(got %d)' % stats['duplicate_color_chunks'], file=sys.stderr)
    else:
        print('  ok: duplicate `color` -- inserted after the LAST line, and reported')

    # An UNTERMINATED chunk must be diagnosed and counted, not swallowed.
    stats = new_stats()
    migrate_text('omni_light\n{\nname a\ncolor 0.5 0.4 0.3\n', stats, path_label='<selftest>')
    if stats['unterminated'] != 1:
        failures += 1
        print('  FAIL: unterminated chunk was not diagnosed', file=sys.stderr)
    else:
        print('  ok: unterminated chunk is diagnosed')

    print('selftest: %d failure(s)' % failures, file=sys.stderr)
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='scenes')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--selftest', action='store_true',
                    help='run the built-in scanner tests and exit')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    stats = new_stats()

    root = pathlib.Path(args.root)
    paths = sorted(root.rglob('*.RISEscene')) if root.is_dir() else [root]
    changed_count = 0
    errors = 0
    for path in paths:
        try:
            if migrate_file(path, stats, dry_run=args.dry_run,
                            verbose=args.verbose):
                changed_count += 1
                if not args.verbose:
                    print('  %s: %s' % (
                        'would migrate' if args.dry_run else 'migrated', path))
        except Exception as e:                       # noqa: BLE001
            errors += 1
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
    if stats['duplicate_color_chunks']:
        print('  %d chunks spell `color` more than once (dead lines; `colorspace` '
              'went after the LAST, which is the one that derives)'
              % stats['duplicate_color_chunks'], file=sys.stderr)
    if stats['unterminated'] or errors:
        # NON-ZERO EXIT.  An unterminated chunk means part of a file went
        # unscanned; a read/write error means a file went unprocessed.  Either
        # way the corpus is NOT fully migrated, and a caller (or a CI step)
        # that only checks the exit status must not be told otherwise.
        print('  %d unterminated chunk(s), %d file error(s) -- the corpus was NOT '
              'fully migrated' % (stats['unterminated'], errors), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
