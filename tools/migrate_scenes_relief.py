#!/usr/bin/env python3
"""
migrate_scenes_relief.py — losslessly convert `bumpmap_modifier` chunks to
the `scalar_painter` + `relief_modifier` pair, per
docs/RELIEF_MODIFIER_DESIGN.md §7.2.

WHAT CHANGED.  `bumpmap_modifier` is DEPRECATED (§7.1): it samples an
`IFunction2D` at `ri.ptCoord` (UV only, R channel, through the fake-hit
`Painter::Evaluate` path) and tilts the normal `+T·scale·(f+ - f-)` (or
`/2*windowsize` when `normalize_gradient TRUE`).  `relief_modifier` is the
replacement: its `height` is any `IScalarPainter` (the `scalar_painter {
function2d F }` bridge samples the identical `F.Evaluate(ptCoord)` path, so
the sampled values are byte-identical to the legacy modifier), and it tilts
`N' = normalize(N - scale'*(h_T*T + h_B*B))` -- the OPPOSITE sign convention
of the legacy modifier (relief's positive `scale` rises along `+N`; the
legacy modifier's positive `scale` treats `f` as depth).  This script folds
that sign so a migrated scene renders bit-for-bit the same.

THE ALGEBRA (§7.2).  Legacy tilt magnitude along `T` is
    normalize_gradient TRUE:   +scale * (f+ - f-)
    normalize_gradient FALSE:  +scale * (f+ - f-) / (2*windowsize)
relief_modifier's tilt magnitude along `T` (domain uv, step = windowsize) is
    -scale' * (f+ - f-) / (2*windowsize)
Equating the two and solving for scale':
    normalize_gradient TRUE:   scale' = -scale
    normalize_gradient FALSE:  scale' = -scale * 2 * windowsize
Defaults when a param is absent: scale=1.0, windowsize=0.01,
normalize_gradient=FALSE (case-insensitive TRUE/FALSE).

NON-POSITIVE WINDOWSIZE IS A SPECIAL CASE, NOT A POINT ON THE ABOVE CURVE.
`windowsize <= 0` makes the legacy modifier INERT (`BumpMap::Modify`'s
central difference samples the same point on both sides and its
normalisation is gated on `dWindow > 0`), but `relief_modifier`'s `step 0`
means AUTO (a full footprint/1e-3-floor perturbation) -- the opposite of
inert. So this script does NOT fold such a chunk through the algebra above;
it emits `scale 0` instead (which neutralises the perturbation regardless of
`step`), with a WARN naming the file:line and the reason.

THE TRANSFORMATION.  For every
    bumpmap_modifier { name N  function F  scale S  windowsize W
                        normalize_gradient G }
this script emits, in place,
    scalar_painter  { name N__height  function2d F }
    relief_modifier { name N  height N__height  domain uv  step W  scale S' }
`domain uv` (not `surface`) keeps the migration LOSSLESS -- it is the exact
legacy sampling geometry (UV-domain step along `onb.u()/onb.v()`), not the
recommended mode; `domain surface` is the upgrade (§9), named in a per-file
note this script prints.  `N__height`, if already used as a chunk name
anywhere in the file, is suffixed `_2`, `_3`, ... (warned).  Any comment-only
line authored INSIDE the original chunk is carried into the emitted
`relief_modifier`, after its params, verbatim -- nothing authored is
dropped.  An unrecognized parameter line inside the chunk is warned
(file:line) and carried the same way as `# migrated: <line>`, so a
not-yet-anticipated bumpmap_modifier parameter is never silently lost.

FLOAT FORMATTING.  `scale'` is printed with `repr(float(x))` -- Python 3's
`repr` already emits the shortest decimal string that round-trips to the
same float (this is what turns `-(0.5*2*0.005)` into `-0.005`, not
`-0.0050000000000000001`), so `format(x, '.17g')` is not needed here and
`repr` is used for readability.

STRUCTURE.  Same idiom as `migrate_scenes_light_colorspace.py`: two parallel
views of the file, `lines` (verbatim, what gets written back) and `code`
(comment-stripped, what every structural decision -- chunk boundaries,
parameter recognition, the name-collision grep -- is made on), so a comment
can never hide a brace or a parameter line (see `strip_comments`).  An
unterminated chunk is diagnosed and makes the run exit non-zero rather than
silently abandoning the rest of the file.

Idempotent: a file with no `bumpmap_modifier` chunk is untouched (no write,
no timestamp change); the emitted `scalar_painter` / `relief_modifier` pair
is not itself a `bumpmap_modifier`, so re-running on migrated output is a
structural no-op.

A trailing comment on a RECOGNIZED parameter line (`scale 0.0075  # tuned`),
on the `bumpmap_modifier` keyword line itself (with or without the brace on
the same line), or on the closing `}` line is carried too, the same
`# migrated: <raw line>` idiom used for an unrecognized parameter, so a
hand-annotated chunk loses no authored text either.

Accepts `.RISEscene` and `.RISEscript` under `--root` (default `scenes`), or
explicit file paths as positional arguments (any extension) so a single
scene can be migrated directly.

Usage:
    python3 tools/migrate_scenes_relief.py [--dry-run] [--root scenes]
    python3 tools/migrate_scenes_relief.py --root scenes/Tests -v
    python3 tools/migrate_scenes_relief.py path/to/one_scene.RISEscene -v --dry-run
    python3 tools/migrate_scenes_relief.py --selftest
"""

import argparse
import pathlib
import re
import sys

KIND = 'bumpmap_modifier'

# The keyword on its own line, optionally with the opening brace on the SAME
# line (both spellings tokenise identically per RISE's lexer -- see
# strip_comments).  Matched against the COMMENT-STRIPPED view.
KIND_RE = re.compile(r'^\s*(' + KIND + r')\s*(\{)?\s*$')

# One regex per recognized bumpmap_modifier parameter.  Value is `\S+` (not
# a numeric-only class) so a malformed token is still captured for a
# targeted diagnostic rather than falling through to "unknown parameter".
PARAM_RES = {
    'name': re.compile(r'^(\s*)name(\s+)(\S+)\s*$'),
    'function': re.compile(r'^(\s*)function(\s+)(\S+)\s*$'),
    'scale': re.compile(r'^(\s*)scale(\s+)(\S+)\s*$'),
    'windowsize': re.compile(r'^(\s*)windowsize(\s+)(\S+)\s*$'),
    'normalize_gradient': re.compile(r'^(\s*)normalize_gradient(\s+)(\S+)\s*$'),
}
# Order matters only for readability of the fallback scan below; every key
# word is distinct so at most one regex matches a given line.
PARAM_ORDER = ('name', 'function', 'scale', 'windowsize', 'normalize_gradient')

# Any `name X` line anywhere in the file (any chunk kind) -- used to build
# the collision set for the minted `N__height` chunk name (§7.2: "grep the
# comment-stripped view").  Matched against the COMMENT-STRIPPED view.
NAME_ANY_RE = re.compile(r'^\s*name\s+(\S+)\s*$')

DEFAULT_SCALE = 1.0
DEFAULT_WINDOWSIZE = 0.01
DEFAULT_NORMALIZE = False


def strip_comments(lines):
    """Return a same-length list of lines with COMMENT TEXT removed.

    Mirrors RISE's own lexer (`Tokenize` in src/Library/Cst/Cst.cpp), which
    absorbs `#`-to-end-of-line comments and `/* ... */` block comments as
    trivia ANYWHERE -- including immediately after a brace.  The scene
    language has no quoted strings (a word token stops at whitespace, `#`,
    `{`, `}` or `/*`), so a `#` is unconditionally the start of a comment and
    stripping it can never eat data.

    Doing the brace-depth walk on this view (character by character, not
    whole-line equality) is what makes `}\t# end of the crease bump` and
    `{ # note` -- and a brace sharing a line with anything else -- count
    correctly instead of silently truncating the scan of the rest of the
    file (the bug `migrate_scenes_light_colorspace.py` was rewritten to
    avoid; ported here verbatim).

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


def format_float(x):
    """Shortest decimal string that round-trips to the same float.

    Python 3's `repr(float)` already computes this (David Gay's shortest
    round-trip algorithm), so `-(0.5*2*0.005)` prints as `-0.005`, not
    `-0.0050000000000000001` -- `format(x, '.17g')` would also round-trip
    but is not chosen: it always prints (up to) 17 significant digits, which
    is correct but not "readable" per the design's own preference.
    """
    return repr(float(x))


def compute_sprime(scale, windowsize, normalize_gradient):
    """§7.2 derivation: fold the legacy sign/normalization into relief's."""
    if normalize_gradient:
        return -scale
    return -(scale * 2.0 * windowsize)


def parse_bool_tf(token):
    """Case-insensitive TRUE/FALSE. Returns None if the token is neither."""
    t = token.strip().lower()
    if t == 'true':
        return True
    if t == 'false':
        return False
    return None


def collect_existing_names(code_lines):
    """All `name X` tokens anywhere in the comment-stripped file view."""
    names = set()
    for line in code_lines:
        m = NAME_ANY_RE.match(line)
        if m:
            names.add(m.group(1))
    return names


def mint_height_name(base, existing_names, stats, path_label, line_no):
    """`base` = `N__height`; suffix _2, _3, ... on collision, warn, reserve."""
    candidate = base
    suffix = 1
    while candidate in existing_names:
        suffix += 1
        candidate = '%s_%d' % (base, suffix)
    if suffix > 1:
        stats['name_collisions'] += 1
        print('  WARN %s:%d: chunk name `%s` already used in this file; '
              'the migrated height painter is named `%s` instead'
              % (path_label or '<text>', line_no, base, candidate),
              file=sys.stderr)
    existing_names.add(candidate)
    return candidate


def _leading_ws(line):
    m = re.match(r'^[\t ]*', line)
    return m.group(0)


def migrate_text(text, stats, path_label=''):
    """Return (new_text, changed).  Mutates `stats` in place.

    Two parallel views: `lines` (verbatim -- written back byte-for-byte
    outside touched chunks) and `code` (comment-stripped -- every structural
    decision is made on this view).
    """
    lines = text.split('\n')
    code = strip_comments(lines)
    existing_names = collect_existing_names(code)
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

        brace_on_keyword_line = m.group(2) is not None

        if brace_on_keyword_line:
            start = i
            head_offset = m.start(2)
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

        # Brace-depth walk over the comment-stripped text, character by
        # character (not whole-line equality) -- so `}\t# note` and
        # `{ # note` are both handled correctly.
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
            stats['unterminated'] += 1
            print('  ERROR %s: unterminated `%s` chunk starting at line %d '
                  '(no matching `}`); the rest of the file was NOT scanned'
                  % (path_label or '<text>', KIND, i + 1), file=sys.stderr)
            out.extend(lines[i:])
            i = n
            break

        stats['chunks_seen'] += 1

        # Interior lines: strictly between the opening-brace line and the
        # closing-brace line (both excluded -- they carry no parameters in
        # the repo's chunk-brace convention).
        interior_start = start + 1
        interior_end = end                      # exclusive
        raw_interior = lines[interior_start:interior_end]
        code_interior = code[interior_start:interior_end]

        # Indentation: reuse the leading-whitespace style of the first
        # interior line that actually has one (tabs if the original used
        # tabs); fall back to a single tab if the chunk had no such line.
        # Computed up front so both preserved comments (kept verbatim, with
        # their OWN original indentation) and "# migrated: ..." carries for
        # unknown parameters (which have no original indentation of their
        # own once unwrapped from the param line) render consistently.
        indent = None
        for raw in raw_interior:
            ws = _leading_ws(raw)
            if ws:
                indent = ws
                break
        if indent is None:
            indent = '\t'
        gap = '\t' if '\t' in indent else ' '

        params = {}
        extra_lines = []           # ready-to-emit strings: preserved
                                    # comments (verbatim) and
                                    # "# migrated: ..." unknown lines

        for offset, (raw, cline) in enumerate(zip(raw_interior, code_interior)):
            line_no = interior_start + offset + 1     # 1-based
            stripped_code = cline.strip()
            if stripped_code == '':
                if raw.strip() == '':
                    continue                          # truly blank: drop
                extra_lines.append(raw)                # authored comment: keep
                continue
            matched_key = None
            matched_groups = None
            for key in PARAM_ORDER:
                pm = PARAM_RES[key].match(cline)
                if pm:
                    matched_key = key
                    matched_groups = pm
                    break
            if matched_key is None:
                stats['unknown_params'] += 1
                print('  WARN %s:%d: unrecognized `%s` parameter line inside '
                      '`bumpmap_modifier`; migrated as a comment so nothing '
                      'is silently lost'
                      % (path_label or '<text>', line_no, KIND), file=sys.stderr)
                extra_lines.append('%s# migrated: %s' % (indent, raw.strip()))
                continue
            # Last-wins on a duplicated parameter (matches RISE's own
            # ParseStateBag::SetSingle overwrite semantics).
            params[matched_key] = (matched_groups.group(3), matched_groups.group(1),
                                    matched_groups.group(2), line_no)
            # A RECOGNIZED parameter line can still carry a trailing comment
            # (`scale 0.0075  # hand-tuned`); `cline` has already had it
            # stripped by strip_comments, so `raw != cline` is exactly the
            # signal that one was present.  Carried the same way an
            # unrecognized line is: verbatim, as `# migrated: <raw>`, so
            # nothing authored is dropped here either.
            if raw != cline:
                extra_lines.append('%s# migrated: %s' % (indent, raw.strip()))
                stats['comments_carried'] += 1

        # The keyword line (`bumpmap_modifier`, or `bumpmap_modifier {` when
        # the brace shares it) and the closing `}` line are OUTSIDE
        # raw_interior/code_interior (that slice excludes both by
        # convention), so a trailing comment on either would otherwise be
        # silently dropped -- the same `raw != code` signal, carried the
        # same way, prepended/appended so the two keep their original
        # position relative to the interior comments.
        if lines[i] != code[i]:
            extra_lines.insert(0, '%s# migrated: %s' % (indent, lines[i].strip()))
            stats['comments_carried'] += 1
        if lines[end] != code[end]:
            extra_lines.append('%s# migrated: %s' % (indent, lines[end].strip()))
            stats['comments_carried'] += 1

        name_val = params.get('name')
        func_val = params.get('function')

        if name_val is None or func_val is None:
            missing = []
            if name_val is None:
                missing.append('name')
            if func_val is None:
                missing.append('function')
            stats['skipped_missing_required'] += 1
            print('  WARN %s: `bumpmap_modifier` chunk starting at line %d is '
                  'missing required parameter(s) %s; left UNMIGRATED'
                  % (path_label or '<text>', i + 1, ', '.join(missing)),
                  file=sys.stderr)
            out.extend(lines[i:end + 1])
            i = end + 1
            continue

        orig_name = name_val[0]
        function_token = func_val[0]

        # scale
        if 'scale' in params:
            tok, _, _, ln = params['scale']
            try:
                scale = float(tok)
            except ValueError:
                stats['malformed_params'] += 1
                print('  WARN %s:%d: `scale %s` is not a number; using the '
                      'default %s' % (path_label or '<text>', ln, tok,
                                       format_float(DEFAULT_SCALE)),
                      file=sys.stderr)
                scale = DEFAULT_SCALE
        else:
            scale = DEFAULT_SCALE

        # windowsize
        windowsize_nonpositive = False
        if 'windowsize' in params:
            tok, _, _, ln = params['windowsize']
            try:
                windowsize = float(tok)
                if windowsize <= 0:
                    # Legacy `BumpMap::Modify` is INERT at windowsize <= 0:
                    # the central difference samples the same point on both
                    # sides (step 0) and its normalisation is gated on
                    # `dWindow > 0`, so the net perturbation is identically
                    # zero.  The migrated `step W` with W <= 0 means AUTO in
                    # relief_modifier (§3.3: `step_user > 0 ? step_user :
                    # ...`), i.e. a full footprint/1e-3-floor perturbation --
                    # the opposite of inert.  `scale 0` is what actually
                    # reproduces "inert" losslessly: it neutralises the
                    # perturbation regardless of what `step` ends up being.
                    windowsize_nonpositive = True
                    stats['nonpositive_windowsize'] += 1
                    print('  WARN %s:%d: `windowsize %s` is not positive -- '
                          'the legacy modifier is INERT there (BumpMap.cpp '
                          'gates its normalisation on dWindow > 0; the '
                          'central difference samples the same point on '
                          'both sides), but a migrated `step 0` means AUTO '
                          '(full perturbation) in relief_modifier, not '
                          'inert.  Emitting `scale 0` instead of the folded '
                          'algebra so the migrated chunk stays inert, '
                          'matching the legacy behaviour.'
                          % (path_label or '<text>', ln, tok), file=sys.stderr)
            except ValueError:
                stats['malformed_params'] += 1
                print('  WARN %s:%d: `windowsize %s` is not a number; using '
                      'the default %s'
                      % (path_label or '<text>', ln, tok,
                         format_float(DEFAULT_WINDOWSIZE)), file=sys.stderr)
                windowsize = DEFAULT_WINDOWSIZE
                tok = format_float(DEFAULT_WINDOWSIZE)  # don't propagate garbage
            step_token = tok
        else:
            windowsize = DEFAULT_WINDOWSIZE
            step_token = format_float(DEFAULT_WINDOWSIZE)

        # normalize_gradient
        if 'normalize_gradient' in params:
            tok, _, _, ln = params['normalize_gradient']
            parsed = parse_bool_tf(tok)
            if parsed is None:
                stats['malformed_params'] += 1
                print('  WARN %s:%d: `normalize_gradient %s` is neither TRUE '
                      'nor FALSE; treating as FALSE'
                      % (path_label or '<text>', ln, tok), file=sys.stderr)
                normalize_gradient = DEFAULT_NORMALIZE
            else:
                normalize_gradient = parsed
        else:
            normalize_gradient = DEFAULT_NORMALIZE

        if windowsize_nonpositive:
            sprime_str = '0'          # bare zero, matching the scene
                                       # language's own convention (`step 0`)
                                       # rather than `format_float`'s `0.0`
        else:
            sprime = compute_sprime(scale, windowsize, normalize_gradient)
            sprime_str = format_float(sprime)

        height_base = '%s__height' % orig_name
        height_name = mint_height_name(height_base, existing_names,
                                        stats, path_label, i + 1)

        new_lines = []
        new_lines.append('scalar_painter')
        new_lines.append('{')
        new_lines.append('%sname%s%s' % (indent, gap, height_name))
        new_lines.append('%sfunction2d%s%s' % (indent, gap, function_token))
        new_lines.append('}')
        new_lines.append('')
        new_lines.append('relief_modifier')
        new_lines.append('{')
        new_lines.append('%sname%s%s' % (indent, gap, orig_name))
        new_lines.append('%sheight%s%s' % (indent, gap, height_name))
        new_lines.append('%sdomain%suv' % (indent, gap))
        new_lines.append('%sstep%s%s' % (indent, gap, step_token))
        new_lines.append('%sscale%s%s' % (indent, gap, sprime_str))
        for extra in extra_lines:
            new_lines.append(extra)
        new_lines.append('}')

        stats['chunks_migrated'] += 1
        if path_label:
            stats['files_touched_set'].add(path_label)
        if stats.get('_verbose'):
            print('  %s: bumpmap_modifier `%s` (scale=%s windowsize=%s '
                  'normalize_gradient=%s) -> scale\' = %s'
                  % (path_label or '<text>', orig_name, format_float(scale),
                     format_float(windowsize), normalize_gradient, sprime_str))
            print('\n'.join('    ' + l for l in new_lines))
        changed = True
        out.extend(new_lines)
        i = end + 1

    return '\n'.join(out), changed


def migrate_file(path, stats, dry_run=False, verbose=False):
    text = path.read_text()
    stats['_verbose'] = verbose and dry_run  # printed emission is meaningful
                                              # in --dry-run -v; a real run's
                                              # emission is just the file.
    new_text, changed = migrate_text(text, stats, path_label=str(path))
    stats['_verbose'] = False
    if changed and not dry_run:
        path.write_text(new_text)
    if changed and verbose:
        print('  %s: %s' % ('would migrate' if dry_run else 'migrated', path))
    if changed:
        print('  NOTE %s: migrated relief_modifier chunk(s) keep `domain uv` '
              '(lossless); `domain surface` with a 3D field is the upgrade -- '
              'see docs/RELIEF_MODIFIER_DESIGN.md §9' % path)
    return changed


def new_stats():
    return {
        'chunks_seen': 0,
        'chunks_migrated': 0,
        'skipped_missing_required': 0,
        'unknown_params': 0,
        'malformed_params': 0,
        'name_collisions': 0,
        'unterminated': 0,
        'comments_carried': 0,
        'nonpositive_windowsize': 0,
        'files_touched_set': set(),
        '_verbose': False,
    }


#######################################################################
# Self-test.  `python3 tools/migrate_scenes_relief.py --selftest`
#######################################################################

def _mk_bumpmap(name='creases', function='crease_field', scale=None,
                 windowsize=None, normalize_gradient=None, extra_lines=()):
    body = ['bumpmap_modifier', '{', '\tname\t\t\t\t%s' % name,
            '\tfunction\t\t\t%s' % function]
    if scale is not None:
        body.append('\tscale\t\t\t\t%s' % scale)
    if windowsize is not None:
        body.append('\twindowsize\t\t\t%s' % windowsize)
    if normalize_gradient is not None:
        body.append('\tnormalize_gradient\t%s' % normalize_gradient)
    for l in extra_lines:
        body.append(l)
    body.append('}')
    return '\n'.join(body) + '\n'


def selftest():
    failures = 0

    def check(label, cond, detail=''):
        nonlocal failures
        if cond:
            print('  ok: %s' % label)
        else:
            failures += 1
            print('  FAIL: %s%s' % (label, (' -- ' + detail) if detail else ''),
                  file=sys.stderr)

    # 1. All params present, normalize_gradient TRUE -> scale' = -scale
    text = _mk_bumpmap(scale='0.0075', windowsize='0.004',
                        normalize_gradient='TRUE')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t1>')
    check('all params present, G TRUE -> scale\' = -S',
          changed and stats['chunks_migrated'] == 1
          and '\tscale\t-0.0075\n}' in new_text
          and 'relief_modifier' in new_text and 'domain\tuv' in new_text
          and 'step\t0.004' in new_text and 'height\tcreases__height' in new_text,
          repr(new_text))

    # 2. G FALSE -> scale' = -S*2*W
    text = _mk_bumpmap(name='clay_bump', function='pnt_clay_bump',
                        scale='0.5', windowsize='0.005')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t2>')
    check('G FALSE -> scale\' = -S*2*W',
          changed and '\tscale\t-0.005\n}' in new_text,
          repr(new_text))

    # 2b. windowsize <= 0 is a special case (P2-6): the legacy modifier is
    # INERT there, but a migrated `step 0` means AUTO (full perturbation) --
    # so the migrator must emit `scale 0`, not the folded algebra, and warn.
    text = _mk_bumpmap(name='inert_bump', function='f', scale='5.0',
                        windowsize='0')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t2b>')
    check('windowsize 0 -> scale\' = 0 (inert), not the folded algebra',
          changed and stats['nonpositive_windowsize'] == 1
          and '\tscale\t0\n}' in new_text
          and 'relief_modifier' in new_text,
          repr(new_text))

    # 2c. Same for a negative windowsize.
    text = _mk_bumpmap(name='neg_window', function='f', scale='2.0',
                        windowsize='-0.01')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t2c>')
    check('negative windowsize -> scale\' = 0 (inert)',
          changed and stats['nonpositive_windowsize'] == 1
          and '\tscale\t0\n}' in new_text,
          repr(new_text))

    # 3. Defaults applied (scale, windowsize, normalize_gradient all absent)
    text = _mk_bumpmap(name='bare', function='f')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t3>')
    check('defaults applied -> scale\' = -1*2*0.01 = -0.02',
          changed and '\tscale\t-0.02\n}' in new_text
          and '\tstep\t0.01\n' in new_text,
          repr(new_text))

    # 4. Idempotency on migrated output
    stats2 = new_stats()
    again_text, again_changed = migrate_text(new_text, stats2, path_label='<t3b>')
    check('idempotency on migrated output',
          not again_changed and again_text == new_text
          and stats2['chunks_seen'] == 0)

    # 5. `}` hidden behind a trailing `# }` comment does not truncate parsing
    text = ('bumpmap_modifier\n{\nname a\nfunction f\n}\t# }\n'
            'bumpmap_modifier\n{\nname b\nfunction g\n}\n')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t5>')
    check('trailing `# }` comment on the closing brace does not truncate parsing',
          changed and stats['chunks_seen'] == 2 and stats['chunks_migrated'] == 2
          and new_text.count('relief_modifier') == 2,
          'chunks_seen=%d chunks_migrated=%d' % (stats['chunks_seen'], stats['chunks_migrated']))

    # 6. Two bumpmap chunks in one file (pool.RISEscene shape)
    text = (_mk_bumpmap(name='bumpchecker', function='pnt_check',
                         scale='0.05', windowsize='0.01')
            + '\n'
            + _mk_bumpmap(name='waterbump', function='pnt_waterbump',
                           scale='0.2', windowsize='0.002'))
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t6>')
    check('two bumpmap chunks in one file both migrate',
          changed and stats['chunks_seen'] == 2 and stats['chunks_migrated'] == 2
          and 'bumpchecker__height' in new_text and 'waterbump__height' in new_text
          and '\tscale\t-0.001\n' in new_text and '\tscale\t-0.0008\n' in new_text,
          repr(new_text))

    # 7. Name collision on N__height
    text = ('scalar_painter\n{\nname creases__height\nfunction2d other\n}\n\n'
            + _mk_bumpmap(name='creases', function='crease_field',
                          scale='0.0075', windowsize='0.004',
                          normalize_gradient='TRUE'))
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t7>')
    check('name collision on N__height suffixes _2',
          changed and stats['name_collisions'] == 1
          and 'name\tcreases__height_2' in new_text,
          repr(new_text))

    # 8. Authored comment inside the chunk is preserved
    text = _mk_bumpmap(name='creases', function='crease_field',
                        scale='0.0075', windowsize='0.004',
                        normalize_gradient='TRUE',
                        extra_lines=['\t# hand-tuned for the velvet weave'])
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t8>')
    check('authored comment inside the chunk is carried into relief_modifier',
          changed and '# hand-tuned for the velvet weave' in new_text
          and new_text.index('# hand-tuned') > new_text.index('relief_modifier'),
          repr(new_text))

    # 8b. A RECOGNIZED parameter line's trailing comment is carried too, not
    # silently dropped (P2-5: only the value token used to survive).
    text = ('bumpmap_modifier\n{\nname creases\nfunction crease_field\n'
            'scale 0.0075  # hand-tuned for the velvet weave\n'
            'windowsize 0.004\nnormalize_gradient TRUE\n}\n')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t8b>')
    check('trailing comment on a recognized parameter line is carried',
          changed and stats['comments_carried'] == 1
          and '# migrated: scale 0.0075  # hand-tuned for the velvet weave' in new_text
          and '\tscale\t-0.0075\n' in new_text,
          repr(new_text))

    # 8c. A trailing comment on the `bumpmap_modifier` KEYWORD line itself
    # (brace on the next line) is carried too.
    text = ('bumpmap_modifier  # the crease bump\n{\nname creases\n'
            'function crease_field\n}\n')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t8c>')
    check('trailing comment on the keyword line is carried',
          changed and stats['comments_carried'] == 1
          and '# migrated: bumpmap_modifier  # the crease bump' in new_text,
          repr(new_text))

    # 8c-bis. Same, with the brace on the keyword line itself.
    text = 'bumpmap_modifier { # the crease bump\nname creases\nfunction crease_field\n}\n'
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t8c2>')
    check('trailing comment on a combined keyword+brace line is carried',
          changed and stats['comments_carried'] == 1
          and '# migrated: bumpmap_modifier { # the crease bump' in new_text,
          repr(new_text))

    # 8d. A trailing comment on the closing `}` line is carried too.
    text = ('bumpmap_modifier\n{\nname creases\nfunction crease_field\n'
            '}\t# end of the crease bump\n')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t8d>')
    check('trailing comment on the closing brace line is carried',
          changed and stats['comments_carried'] == 1
          and '# migrated: }\t# end of the crease bump' in new_text,
          repr(new_text))

    # 9. A file with no bumpmap chunk is byte-identical after a run
    text = ('lambertian_material\n{\nname m\nreflectance 1 1 1\n}\n'
            '# a bumpmap_modifier mentioned only in a comment\n')
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t9>')
    check('no bumpmap chunk -> byte-identical, no change reported',
          not changed and new_text == text and stats['chunks_seen'] == 0)

    # Bonus: unknown parameter is warned and carried as a comment, not dropped
    text = _mk_bumpmap(name='n', function='f', extra_lines=['\tfrobnicate 3'])
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t10>')
    check('unknown parameter is carried as `# migrated: ...`, not dropped',
          changed and stats['unknown_params'] == 1
          and '# migrated: frobnicate 3' in new_text)

    # Bonus: missing required parameter (function) leaves the chunk untouched
    text = 'bumpmap_modifier\n{\nname only_a_name\n}\n'
    stats = new_stats()
    new_text, changed = migrate_text(text, stats, path_label='<t11>')
    check('missing required `function` leaves the chunk unmigrated',
          not changed and new_text == text
          and stats['skipped_missing_required'] == 1)

    # Bonus: an unterminated chunk is diagnosed, not swallowed
    stats = new_stats()
    migrate_text('bumpmap_modifier\n{\nname a\nfunction f\n', stats,
                 path_label='<t12>')
    check('unterminated chunk is diagnosed', stats['unterminated'] == 1)

    print('selftest: %d failure(s)' % failures, file=sys.stderr)
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='*',
                     help='explicit scene file path(s) to migrate; if given, '
                          '--root is ignored')
    ap.add_argument('--root', default='scenes')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--selftest', action='store_true',
                    help='run the built-in scanner tests and exit')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    stats = new_stats()

    if args.paths:
        paths = [pathlib.Path(p) for p in args.paths]
    else:
        root = pathlib.Path(args.root)
        if root.is_dir():
            paths = sorted(set(root.rglob('*.RISEscene'))
                           | set(root.rglob('*.RISEscript')))
        else:
            paths = [root]

    changed_count = 0
    errors = 0
    for path in paths:
        try:
            if migrate_file(path, stats, dry_run=args.dry_run,
                            verbose=args.verbose):
                changed_count += 1
        except Exception as e:                       # noqa: BLE001
            errors += 1
            print('  ERROR %s: %s' % (path, e), file=sys.stderr)

    print('', file=sys.stderr)
    print('%d files scanned, %d bumpmap_modifier chunks seen'
          % (len(paths), stats['chunks_seen']), file=sys.stderr)
    print('  %d chunks %smigrated in %d files'
          % (stats['chunks_migrated'],
             'would be ' if args.dry_run else '', changed_count),
          file=sys.stderr)
    if stats['skipped_missing_required']:
        print('  %d chunks left UNMIGRATED: missing required name/function'
              % stats['skipped_missing_required'], file=sys.stderr)
    if stats['unknown_params']:
        print('  %d unrecognized parameter line(s) carried as `# migrated: ...`'
              % stats['unknown_params'], file=sys.stderr)
    if stats['malformed_params']:
        print('  %d malformed parameter value(s) fell back to their default'
              % stats['malformed_params'], file=sys.stderr)
    if stats['name_collisions']:
        print('  %d height-painter name(s) suffixed to avoid a collision'
              % stats['name_collisions'], file=sys.stderr)
    if stats['comments_carried']:
        print('  %d trailing comment(s) on a recognized parameter/keyword/'
              'closing-brace line carried as `# migrated: ...`'
              % stats['comments_carried'], file=sys.stderr)
    if stats['nonpositive_windowsize']:
        print('  %d chunk(s) had a non-positive `windowsize` -- emitted '
              '`scale 0` (inert) instead of the folded algebra to match the '
              'legacy INERT behaviour' % stats['nonpositive_windowsize'],
              file=sys.stderr)
    if stats['unterminated'] or errors:
        print('  %d unterminated chunk(s), %d file error(s) -- the corpus was '
              'NOT fully migrated' % (stats['unterminated'], errors),
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
