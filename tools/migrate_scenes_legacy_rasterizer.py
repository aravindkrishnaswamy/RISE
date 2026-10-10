#!/usr/bin/env python3
"""
migrate_scenes_legacy_rasterizer.py -- move scenes off the FROZEN legacy
pixel rasterizers (legacy-deprecation Phase 2, slice A; see
docs/LEGACY_DEPRECATION_ASSESSMENT.md section 11 and the owner rulings in
section 13).

`pixelpel_rasterizer` and `pixelintegratingspectral_rasterizer` drive the
legacy shader-op pipeline.  They are FROZEN (they render exactly as before
but receive no fixes).  This tool rewrites a scene onto the supported
`pathtracing_pel_rasterizer` / `pathtracing_spectral_rasterizer`.

MODES (pick one with --mode; every mode refuses what it does not handle):

  pt        A legacy rasterizer whose shader chains contain ONLY path-tracing
            ops (`pathtracing_shaderop`, `DefaultPathTracing`).  This is the
            legacy spelling of a path tracer; the look is preserved up to
            Monte-Carlo noise.

  direct    A legacy rasterizer whose shader chains are DIRECT LIGHTING ONLY
            (`DefaultDirectLighting`, optionally `DefaultEmission`, optionally
            a `transparency_shaderop` / `alpha_test_shaderop`, see below).
            Owner ruling #3(a): migrate to the path tracer with
            `max_diffuse_bounce 0` and accept the look change (specular paths
            and MIS-weighted emitter hits now contribute).  Every shader chain
            in the scene is rewritten to `DefaultPathTracing` so PT's SSS
            continuation (which still shades through the scene's default
            shader) is a path tracer, not a direct-lighting op.

  renames   Keyword-level renames with no transport change:
              mis_pathtracing_shaderop -> pathtracing_shaderop (alias);
              onb_pinhole_camera       -> pinhole_camera (va/vb/components
                                          turned into lookat/up; refused when
                                          any value is not a literal, and for
                                          `components VW`, whose legacy basis
                                          construction is degenerate).
            `3dsmesh_geometry` is REFUSED with advice: it needs the mesh file
            converted (plymesh_geometry / gltf_import), not a rename.

ALPHA OPS (direct mode).  A `transparency_shaderop` / `alpha_test_shaderop`
whose painter is a `uniformcolor_painter` with equal channels is turned into
the material parameters `alpha_coverage` / `alpha_mode` / `alpha_cutoff`
(DL-214, honoured by every integrator) on every material of every object
whose shader uses the op -- only when each such material is used by NO other
object (otherwise refused).  The op is removed from the shader chains and its
(now unreferenced) chunk is deleted.  Coverage = 1 - transparency; an
`alpha_test_shaderop` becomes `alpha_mode mask` with its `cutoff`.
`transparency_shaderop`'s `one_sided` has no material-coverage equivalent
(coverage applies to both faces): reported as a look change.

RASTERIZER PARAMETERS.  The path tracers do not accept `max_recursion`,
`lum_samples`, `luminary_sampler`, `luminary_sampler_param`, `filter_glossy`,
`integrate_rgb` or `rgb_spd*`.  `max_recursion` and `lum_samples` are dropped
(PT bounds paths itself at kDefaultPathTracingMaxDepth; `lum_samples` only
acts with a `luminary_sampler`, which no shipped scene sets).  A scene that
sets `luminary_sampler` (not none), a nonzero `filter_glossy`,
`integrate_rgb TRUE` or any `rgb_spd*` is REFUSED.  `samples` is carried
verbatim; when absent it is written as `samples 1`, because the legacy
default is 1 and the path tracers' default is 32.  `oidn_denoise` is carried
verbatim and NOT added when absent: the legacy pixel rasterizers ALSO default
to OIDN on (verified 2026-10-10: a default `pixelpel_rasterizer` render logs
"OIDN auto" and writes a `_denoised` output), so leaving it absent keeps the
scene's effective denoise setting.  A `pathtracing_shaderop` referenced by a
shader that enables SMS (`sms_enabled TRUE`) has its `sms_*` parameters
copied onto the rasterizer, where the path-tracing rasterizers read them.

OUT OF SCOPE (refused, left for later slices): photon maps / gathers,
`finalgather_shaderop`, `irradiance_cache`, point-cloud SSS ops,
`ambientocclusion_shaderop`, `distributiontracing_shaderop`, Whitted chains
(`DefaultReflection` / `DefaultRefraction`), `arealight_shaderop`, custom
`directlighting_shaderop`, `sms_shaderop`, direct volume rendering, MLT, and
scenes declaring more than one rasterizer.  Scenes in HELD below are reserved
for a later slice (Android catalogue / quickstart sample), and
scenes/Tests/Legacy/ (the frozen-feature regression corpus) and
scenes/Tests/ChunkCoverage/ (one-chunk parser coverage) are never touched.

Idempotent: a migrated scene has no legacy rasterizer and is reported
`unchanged`.  Structure is read from a COMMENT-STRIPPED view of each line
(same lexer rules as tools/migrate_scenes_light_colorspace.py); the bytes
written back are the originals, edited line by line.

Usage:
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode pt     [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode direct [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode renames [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --selftest
Paths default to `scenes/`; directories are walked for *.RISEscene.  Exit
status is 0 unless a file could not be read or parsed.
"""

import argparse
import math
import os
import re
import sys

LEGACY_RASTERIZERS = {
    'pixelpel_rasterizer': 'pathtracing_pel_rasterizer',
    'pixelintegratingspectral_rasterizer': 'pathtracing_spectral_rasterizer',
}

# Parameters each target accepts (from the chunk descriptors,
# src/Library/Parsers/ChunkParserRegistry.cpp, 2026-10-10).
_COMMON = {
    'defaultshader', 'samples', 'show_luminaires', 'oidn_denoise', 'oidn_quality',
    'oidn_device', 'oidn_prefilter', 'choose_one_light', 'blue_noise_sampler',
    'pixel_sampler', 'pixel_sampler_param', 'pixel_filter', 'pixel_filter_width',
    'pixel_filter_height', 'pixel_filter_paramA', 'pixel_filter_paramB',
    'radiance_map', 'radiance_scale', 'radiance_background', 'radiance_orient',
    'sms_enabled', 'sms_max_iterations', 'sms_threshold', 'sms_max_chain_depth',
    'sms_biased', 'sms_bernoulli_trials', 'sms_multi_trials', 'sms_photon_count',
    'sms_two_stage', 'sms_target_bounces', 'sms_extended', 'sms_seeding',
    'adaptive_max_samples', 'adaptive_threshold', 'show_adaptive_map',
    'direct_clamp', 'indirect_clamp', 'rr_min_depth', 'rr_threshold',
    'max_diffuse_bounce', 'max_glossy_bounce', 'max_transmission_bounce',
    'max_translucent_bounce', 'max_volume_bounce', 'light_bvh',
    'progressive_rendering', 'progressive_samples_per_pass', 'transparent_shadows',
}
TARGET_PARAMS = {
    'pathtracing_pel_rasterizer': _COMMON | {
        'pathguiding', 'pathguiding_iterations', 'pathguiding_spp',
        'pathguiding_combine_training', 'pathguiding_online',
        'pathguiding_warmup_iterations', 'pathguiding_alpha',
        'pathguiding_learned_alpha', 'pathguiding_max_depth',
        'pathguiding_light_max_depth', 'pathguiding_sampling_type',
        'pathguiding_ris_candidates', 'pathguiding_complete_paths',
        'pathguiding_complete_path_strategy_selection',
        'pathguiding_complete_path_strategy_samples',
        'optimal_mis', 'optimal_mis_training_iterations', 'optimal_mis_tile_size',
    },
    'pathtracing_spectral_rasterizer': _COMMON | {
        'spectral_samples', 'num_wavelengths', 'nmbegin', 'nmend', 'hwss', 'use_hwss',
    },
}
DROPPED = {'max_recursion', 'lum_samples', 'luminary_sampler_param'}
SMS_OP_PARAMS = ('sms_enabled', 'sms_max_iterations', 'sms_threshold',
                 'sms_max_chain_depth', 'sms_biased')

PT_OPS = {'pathtracing_shaderop'}
DIRECT_OPS = {'directlighting', 'emission', 'transparency_shaderop', 'alpha_test_shaderop'}
ALPHA_OPS = {'transparency_shaderop', 'alpha_test_shaderop'}
BUILTIN_OPS = {
    'DefaultReflection': 'reflection (Whitted)', 'DefaultRefraction': 'refraction (Whitted)',
    'DefaultEmission': 'emission', 'DefaultDirectLighting': 'directlighting',
    'DefaultPathTracing': 'pathtracing_shaderop',
    'DefaultCausticPelPhotonMap': 'photon map', 'DefaultCausticSpectralPhotonMap': 'photon map',
    'DefaultGlobalPelPhotonMap': 'photon map', 'DefaultGlobalSpectralPhotonMap': 'photon map',
    'DefaultTranslucentPelPhotonMap': 'photon map', 'DefaultShadowPhotonMap': 'photon map',
}
OUT_OF_SCOPE_CHUNKS = {
    'irradiance_cache': 'irradiance cache',
    'mlt_rasterizer': 'MLT', 'mlt_spectral_rasterizer': 'MLT',
    'directvolumerendering_shader': 'direct volume rendering',
    'spectraldirectvolumerendering_shader': 'direct volume rendering',
}

# Reserved for a later slice (Phase 2 step 6): Android catalogue + the
# CLAUDE.md / README quickstart sample.  Paths are repo-relative.
HELD = {
    'scenes/Tests/Geometry/shapes.RISEscene':
        'quickstart sample render and Android catalogue scene (Phase 2 step 6)',
    'scenes/FeatureBased/Parser/kaleidoscope_atrium.RISEscene':
        'Android catalogue scene (Phase 2 step 6)',
    'scenes/FeatureBased/Combined/tidepools.RISEscene':
        'Android catalogue scene (Phase 2 step 6)',
}

# The frozen-feature regression corpus (owner ruling #17): originals kept
# so frozen chunks stay rendered and derived.  Never migrated.
LEGACY_CORPUS = 'scenes/Tests/Legacy/'
# Parser coverage scenes exist to exercise ONE chunk each; renaming a
# deprecated chunk out of its own coverage scene would drop that coverage.
COVERAGE_CORPUS = 'scenes/Tests/ChunkCoverage/'

KW_RE = re.compile(r'^\s*([A-Za-z0-9_][A-Za-z0-9_]*)\s*(\{)?\s*$')


def strip_comments(lines):
    """Same-length list of lines with `#` and `/* */` comment text removed
    (RISE's lexer, src/Library/Cst/Cst.cpp `Tokenize`)."""
    out = []
    in_block = False
    for line in lines:
        buf = []
        i, n = 0, len(line)
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
                break
            buf.append(line[i])
            i += 1
        out.append(''.join(buf))
    return out


class Chunk(object):
    def __init__(self, kw, kw_line, close_line, params):
        self.kw = kw
        self.kw_line = kw_line        # index of the keyword line
        self.close_line = close_line  # index of the closing brace line
        self.params = params          # list of (line_index, [tokens])

    def get(self, name):
        for idx, toks in self.params:
            if toks[0] == name:
                return toks[1:]
        return None

    def name(self):
        v = self.get('name')
        return v[0] if v else None


class ParseError(Exception):
    pass


def parse_chunks(code):
    """Top-level chunks of a comment-stripped scene (bodies are flat)."""
    chunks = []
    i, n = 0, len(code)
    while i < n:
        m = KW_RE.match(code[i])
        if not m:
            i += 1
            continue
        kw = m.group(1)
        j = i
        if not m.group(2):
            j = i + 1
            while j < n and code[j].strip() == '':
                j += 1
            if j >= n or code[j].strip() != '{':
                i += 1
                continue
        params = []
        k = j + 1
        while k < n and code[k].strip() != '}':
            if '{' in code[k] or '}' in code[k]:
                raise ParseError('line %d: brace inside the body of `%s`' % (k + 1, kw))
            toks = code[k].split()
            if toks:
                params.append((k, toks))
            k += 1
        if k >= n:
            raise ParseError('line %d: unterminated chunk `%s`' % (i + 1, kw))
        chunks.append(Chunk(kw, i, k, params))
        i = k + 1
    return chunks


def is_literal_number(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


def indent_of(line):
    return line[:len(line) - len(line.lstrip())]


def fmt_num(v):
    if abs(v) < 5e-16:
        v = 0.0
    s = repr(float(v))
    if s.endswith('.0'):
        s = s[:-2]
    return s


# --------------------------------------------------------------------------
# Shared analysis
# --------------------------------------------------------------------------

def op_types(chunks):
    """name -> op type for every named shader op in the file."""
    named = {}
    for c in chunks:
        if c.kw.endswith('_shaderop') and c.name():
            named[c.name()] = c.kw
    return named


def shader_chains(chunks):
    """List of (chunk, [(line_index, op_name)]) for standard/advanced shaders."""
    out = []
    for c in chunks:
        if c.kw in ('standard_shader', 'advanced_shader'):
            ops = [(idx, toks[1]) for idx, toks in c.params
                   if toks[0] == 'shaderop' and len(toks) > 1]
            out.append((c, ops))
    return out


def classify_op(op_name, named):
    if op_name in named:
        kw = named[op_name]
        if kw == 'mis_pathtracing_shaderop':
            return 'pathtracing_shaderop'
        return kw
    if op_name in BUILTIN_OPS:
        return BUILTIN_OPS[op_name]
    return 'unknown op `%s`' % op_name


# --------------------------------------------------------------------------
# Rasterizer migration (modes pt / direct)
# --------------------------------------------------------------------------

def migrate_rasterizer(text, mode, report):
    lines = text.split('\n')
    code = strip_comments(lines)
    chunks = parse_chunks(code)

    rasters = [c for c in chunks if c.kw.endswith('_rasterizer')]
    legacy = [c for c in rasters if c.kw in LEGACY_RASTERIZERS]
    if not legacy:
        return None, 'unchanged (no legacy pixel rasterizer)'
    if len(rasters) != 1:
        return None, 'REFUSED: %d rasterizer chunks (%s); the active one is ambiguous' % (
            len(rasters), ', '.join(c.kw for c in rasters))
    for c in chunks:
        if c.kw in OUT_OF_SCOPE_CHUNKS:
            return None, 'REFUSED: uses %s (`%s`), a later slice' % (OUT_OF_SCOPE_CHUNKS[c.kw], c.kw)
        if c.kw.endswith('_photonmap') or c.kw.endswith('_gather'):
            return None, 'REFUSED: uses photon mapping (`%s`), a later slice' % c.kw

    named = op_types(chunks)
    chains = shader_chains(chunks)
    used = set()
    for _, ops in chains:
        for _, opn in ops:
            used.add(classify_op(opn, named))
    allowed = PT_OPS if mode == 'pt' else DIRECT_OPS
    bad = sorted(used - allowed)
    if bad:
        return None, 'REFUSED for --mode %s: shader chains use %s' % (mode, ', '.join(bad))
    if mode == 'pt' and 'pathtracing_shaderop' not in used:
        return None, 'REFUSED for --mode pt: no path-tracing op in any shader chain'
    if mode == 'direct' and 'directlighting' not in used:
        return None, 'REFUSED for --mode direct: no DefaultDirectLighting in any shader chain'

    r = legacy[0]
    target = LEGACY_RASTERIZERS[r.kw]
    accepted = TARGET_PARAMS[target]
    edits = {}          # line index -> replacement list of lines (empty = delete)
    notes = []

    have = {}
    for idx, toks in r.params:
        have[toks[0]] = (idx, toks)
    for name, (idx, toks) in have.items():
        val = toks[1:]
        if name == 'luminary_sampler':
            if val and val[0].strip('"') != 'none':
                return None, 'REFUSED: luminary_sampler %s has no path-tracer equivalent' % val[0]
            edits[idx] = []
            continue
        if name == 'filter_glossy':
            if val and is_literal_number(val[0]) and float(val[0]) == 0.0:
                edits[idx] = []
                continue
            return None, 'REFUSED: filter_glossy is not accepted by %s' % target
        if name == 'integrate_rgb':
            if val and val[0].upper() in ('FALSE', '0'):
                edits[idx] = []
                continue
            return None, 'REFUSED: integrate_rgb TRUE has no path-tracer equivalent'
        if name.startswith('rgb_spd'):
            return None, 'REFUSED: %s has no path-tracer equivalent' % name
        if name in DROPPED:
            edits[idx] = []
            continue
        if name not in accepted:
            return None, 'REFUSED: parameter `%s` is not accepted by %s' % (name, target)

    ind = '\t'
    for idx, toks in r.params:
        ind = indent_of(lines[idx]) or '\t'
        break
    additions = []
    if 'samples' not in have:
        additions.append(ind + 'samples\t\t\t\t1')
        notes.append('samples 1 written (legacy default 1, path-tracer default 32)')

    # SMS on a referenced pathtracing_shaderop moves to the rasterizer.
    if mode == 'pt':
        referenced = set(opn for _, ops in chains for _, opn in ops)
        for c in chunks:
            if c.kw in ('pathtracing_shaderop', 'mis_pathtracing_shaderop') and c.name() in referenced:
                en = c.get('sms_enabled')
                if en and en[0].upper() in ('TRUE', '1'):
                    for p in SMS_OP_PARAMS:
                        v = c.get(p)
                        if v is not None and p not in have:
                            additions.append(ind + '%s\t\t%s' % (p, ' '.join(v)))
                    notes.append('SMS settings of pathtracing_shaderop `%s` copied to the rasterizer' % c.name())

    if mode == 'direct':
        if 'max_diffuse_bounce' in have:
            idx = have['max_diffuse_bounce'][0]
            edits[idx] = [ind + 'max_diffuse_bounce\t\t0']
        else:
            additions.append(ind + 'max_diffuse_bounce\t\t0')
        notes.append('max_diffuse_bounce 0 (direct-only, owner ruling #3a)')

    # Rewrite the keyword line (keep any trailing comment / brace / indent).
    kl = lines[r.kw_line]
    edits[r.kw_line] = [kl.replace(r.kw, target, 1)]
    header = (indent_of(kl) + '# Migrated from %s by tools/migrate_scenes_legacy_rasterizer.py '
              '(--mode %s, legacy-deprecation Phase 2).' % (r.kw, mode))
    edits[r.kw_line] = [header] + edits[r.kw_line]
    # Additions go just before the closing brace.
    cl = r.close_line
    edits[cl] = additions + [lines[cl]]

    # Shader chains.
    if mode == 'direct':
        res = rewrite_direct_chains(lines, chunks, chains, named, edits, notes)
        if res is not None:
            return None, res

    out = []
    for i, line in enumerate(lines):
        if i in edits:
            out.extend(edits[i])
        else:
            out.append(line)
    new = '\n'.join(out)
    report.extend(notes)
    return new, 'MIGRATED %s -> %s' % (r.kw, target)


def rewrite_direct_chains(lines, chunks, chains, named, edits, notes):
    """Direct mode: chains become DefaultPathTracing; alpha ops become
    material coverage.  Returns a refusal string or None."""
    # Alpha ops first (needs the original chains).
    alpha_ops = {}
    for _, ops in chains:
        for _, opn in ops:
            t = classify_op(opn, named)
            if t in ALPHA_OPS:
                alpha_ops[opn] = t
    if alpha_ops:
        res = convert_alpha_ops(lines, chunks, chains, named, alpha_ops, edits, notes)
        if res is not None:
            return res

    for c, ops in chains:
        seen_pt = False
        for idx, opn in ops:
            t = classify_op(opn, named)
            if t in ALPHA_OPS:
                edits[idx] = []
                continue
            if t in ('directlighting', 'emission'):
                if seen_pt:
                    edits[idx] = []
                else:
                    edits[idx] = [lines[idx].replace(opn, 'DefaultPathTracing', 1)]
                    seen_pt = True
    notes.append('shader chains rewritten to DefaultPathTracing')
    return None


def _uniform_grey(chunks, painter):
    for c in chunks:
        if c.name() == painter and c.kw == 'uniformcolor_painter':
            col = c.get('color')
            if col and len(col) == 3 and all(is_literal_number(x) for x in col):
                vals = [float(x) for x in col]
                if max(vals) - min(vals) < 1e-12:
                    cs = c.get('colorspace')
                    if cs and cs[0] not in ('Rec709RGB_Linear', 'sRGB_Linear', 'RISERGB'):
                        # A non-linear colorspace changes the value; only
                        # 0 and 1 are fixed points.
                        if vals[0] not in (0.0, 1.0):
                            return None
                    return vals[0]
    return None


def convert_alpha_ops(lines, chunks, chains, named, alpha_ops, edits, notes):
    op_chunks = {c.name(): c for c in chunks if c.kw in ALPHA_OPS}
    shaders_using = {}
    for c, ops in chains:
        for _, opn in ops:
            if opn in alpha_ops:
                shaders_using.setdefault(opn, set()).add(c.name())
    objects = [c for c in chunks if c.kw.endswith('_object')]
    mat_users = {}
    for o in objects:
        m = o.get('material')
        if m:
            mat_users.setdefault(m[0], []).append(o)
    materials = {c.name(): c for c in chunks if c.kw.endswith('_material')}
    for opn, t in alpha_ops.items():
        oc = op_chunks.get(opn)
        if oc is None:
            return 'REFUSED: alpha op `%s` not found' % opn
        if t == 'transparency_shaderop':
            p = oc.get('transparency')
            g = _uniform_grey(chunks, p[0]) if p else None
            if g is None:
                return 'REFUSED: transparency_shaderop `%s` painter is not a uniform grey' % opn
            coverage, mode, cutoff = 1.0 - g, 'blend', None
            os_ = oc.get('one_sided')
            if os_ and os_[0].upper() in ('TRUE', '1'):
                notes.append('LOOK CHANGE: transparency_shaderop `%s` one_sided has no '
                             'material-coverage equivalent (coverage applies to both faces)' % opn)
        else:
            p = oc.get('alpha')
            g = _uniform_grey(chunks, p[0]) if p else None
            if g is None:
                return 'REFUSED: alpha_test_shaderop `%s` painter is not a uniform grey' % opn
            cut = oc.get('cutoff')
            coverage, mode, cutoff = g, 'mask', (cut[0] if cut else None)
        shaders = shaders_using.get(opn, set())
        objs = [o for o in objects if o.get('shader') and o.get('shader')[0] in shaders]
        if not objs:
            return 'REFUSED: alpha op `%s` is used by no object' % opn
        for o in objs:
            m = o.get('material')
            if not m:
                return 'REFUSED: object `%s` (alpha op `%s`) has no material' % (o.name(), opn)
            others = [u for u in mat_users.get(m[0], []) if u not in objs]
            if others:
                return 'REFUSED: material `%s` is shared with objects outside alpha op `%s`' % (m[0], opn)
            mc = materials.get(m[0])
            if mc is None:
                return 'REFUSED: material `%s` not found' % m[0]
            if mc.get('alpha_coverage') or mc.get('alpha_mode'):
                return 'REFUSED: material `%s` already has alpha parameters' % m[0]
            if mc.close_line in edits:
                continue
            ind = indent_of(lines[mc.params[0][0]]) if mc.params else '\t'
            add = [ind + 'alpha_coverage\t\t' + fmt_num(coverage), ind + 'alpha_mode\t\t\t' + mode]
            if cutoff is not None:
                add.append(ind + 'alpha_cutoff\t\t' + cutoff)
            edits[mc.close_line] = add + [lines[mc.close_line]]
        # The op is now unreferenced (every chain that used it drops it):
        # delete its chunk so the scene no longer carries a frozen keyword.
        for k in range(oc.kw_line, oc.close_line + 1):
            edits[k] = []
        notes.append('%s `%s` -> alpha_coverage %s / alpha_mode %s on its materials (op chunk removed)'
                     % (t, opn, fmt_num(coverage), mode))
    return None


# --------------------------------------------------------------------------
# Renames
# --------------------------------------------------------------------------

def _vec(toks):
    if toks is None or len(toks) != 3 or not all(is_literal_number(x) for x in toks):
        return None
    return [float(x) for x in toks]


def _sub(a, b): return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
def _add(a, b): return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
def _cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _norm(a):
    L = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    if L == 0:
        raise ValueError('zero-length axis')
    return [a[0] / L, a[1] / L, a[2] / L]


def onb_from(components, a, b):
    """Mirror of OrthonormalBasis3D::CreateFrom* (src/Library/Utilities/
    OrthonormalBasis3D.cpp).  Returns (U, V, W)."""
    if components == 'UV':
        U = _norm(a); W = _norm(_cross(U, b)); V = _cross(W, U)
    elif components == 'VU':
        V = _norm(a); W = _norm(_cross(V, b)); U = _cross(W, V)
    elif components == 'UW':
        U = _norm(a); V = _norm(_cross(b, U)); W = _cross(U, V)
    elif components == 'WU':
        W = _norm(a); V = _norm(_cross(W, b)); U = _cross(V, W)
    elif components == 'WV':
        W = _norm(a); U = _norm(_cross(b, W)); V = _cross(W, U)
    else:
        raise ValueError(components)
    return U, V, W


def migrate_renames(text, report):
    lines = text.split('\n')
    code = strip_comments(lines)
    chunks = parse_chunks(code)
    edits = {}
    done = []
    for c in chunks:
        if c.kw == 'mis_pathtracing_shaderop':
            edits[c.kw_line] = [lines[c.kw_line].replace(c.kw, 'pathtracing_shaderop', 1)]
            done.append('mis_pathtracing_shaderop -> pathtracing_shaderop')
        elif c.kw == 'onb_pinhole_camera':
            comps = c.get('components')
            loc, va, vb = _vec(c.get('location')), _vec(c.get('va')), _vec(c.get('vb'))
            if not comps or loc is None or va is None or vb is None:
                return None, 'REFUSED: onb_pinhole_camera needs literal location/va/vb/components'
            if comps[0] == 'VW':
                return None, 'REFUSED: onb_pinhole_camera components VW (degenerate legacy basis)'
            try:
                U, V, W = onb_from(comps[0], va, vb)
            except ValueError as e:
                return None, 'REFUSED: onb_pinhole_camera: %s' % e
            lookat = _add(loc, W)
            for idx, toks in c.params:
                ind = indent_of(lines[idx])
                if toks[0] == 'va':
                    edits[idx] = [ind + 'lookat\t\t' + ' '.join(fmt_num(x) for x in lookat)]
                elif toks[0] == 'vb':
                    edits[idx] = [ind + 'up\t\t' + ' '.join(fmt_num(x) for x in V)]
                elif toks[0] == 'components':
                    edits[idx] = []
            edits[c.kw_line] = [lines[c.kw_line].replace(c.kw, 'pinhole_camera', 1)]
            done.append('onb_pinhole_camera -> pinhole_camera (lookat = location + W, up = V)')
        elif c.kw == '3dsmesh_geometry':
            return None, ('REFUSED: 3dsmesh_geometry needs its mesh converted '
                          '(plymesh_geometry or gltf_import), not a rename')
    if not done:
        return None, 'unchanged (nothing to rename)'
    out = []
    for i, line in enumerate(lines):
        out.extend(edits[i] if i in edits else [line])
    report.extend(done)
    return '\n'.join(out), 'MIGRATED (renames)'


# --------------------------------------------------------------------------

def collect(paths):
    files = []
    for p in paths:
        if os.path.isfile(p):
            files.append(p)
        else:
            for dp, _, fns in os.walk(p):
                for fn in fns:
                    if fn.endswith('.RISEscene'):
                        files.append(os.path.join(dp, fn))
    return sorted(files)


def repo_rel(path):
    ap = os.path.abspath(path).replace(os.sep, '/')
    i = ap.find('/scenes/')
    return ap[i + 1:] if i >= 0 else path


def run(mode, paths, dry_run, verbose):
    counts = {'migrated': 0, 'refused': 0, 'unchanged': 0, 'held': 0}
    rc = 0
    for f in collect(paths):
        rel = repo_rel(f)
        try:
            with open(f, 'r', encoding='utf-8', newline='') as fh:
                text = fh.read()
        except (IOError, UnicodeDecodeError) as e:
            print('ERROR     %s: %s' % (rel, e))
            rc = 1
            continue
        notes = []
        try:
            if mode == 'renames':
                new, status = migrate_renames(text, notes)
            else:
                new, status = migrate_rasterizer(text, mode, notes)
        except ParseError as e:
            print('ERROR     %s: %s' % (rel, e))
            rc = 1
            continue
        held = HELD.get(rel)
        if rel.startswith(LEGACY_CORPUS) or rel.startswith(COVERAGE_CORPUS):
            counts['held'] += 1
            if verbose:
                print('HELD      %s: %s' % (rel, 'frozen-feature regression corpus (see its README)'
                      if rel.startswith(LEGACY_CORPUS) else 'chunk-coverage scene'))
            continue
        if held is not None and new is not None:
            counts['held'] += 1
            print('HELD      %s: %s' % (rel, held))
            continue
        if new is None:
            key = 'unchanged' if status.startswith('unchanged') else 'refused'
            counts[key] += 1
            if key == 'refused' or verbose:
                print('%-9s %s: %s' % (key.upper(), rel, status))
            continue
        counts['migrated'] += 1
        print('MIGRATED  %s: %s' % (rel, status[len('MIGRATED '):]))
        for n in notes:
            print('          - %s' % n)
        if not dry_run:
            with open(f, 'w', encoding='utf-8', newline='') as fh:
                fh.write(new)
    print('%s: %d migrated, %d refused, %d held, %d unchanged%s' % (
        mode, counts['migrated'], counts['refused'], counts['held'], counts['unchanged'],
        ' (dry run, nothing written)' if dry_run else ''))
    return rc


def selftest():
    pt = ('RISE ASCII SCENE 7\nstandard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n'
          'pixelpel_rasterizer\n{\n\tmax_recursion 10\n\tlum_samples 1 # c\n}\n')
    notes = []
    new, st = migrate_rasterizer(pt, 'pt', notes)
    assert st.startswith('MIGRATED'), st
    assert 'pathtracing_pel_rasterizer' in new and 'max_recursion' not in new
    assert 'samples\t\t\t\t1' in new
    again, st2 = migrate_rasterizer(new, 'pt', [])
    assert again is None and st2.startswith('unchanged'), st2
    dl = ('standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n\tshaderop DefaultEmission\n}\n'
          'pixelintegratingspectral_rasterizer\n{\n\tsamples 4\n\tintegrate_rgb FALSE\n}\n')
    new, st = migrate_rasterizer(dl, 'direct', [])
    assert 'pathtracing_spectral_rasterizer' in new and 'max_diffuse_bounce\t\t0' in new, new
    assert new.count('DefaultPathTracing') == 1 and 'DefaultEmission' not in new, new
    assert migrate_rasterizer(dl, 'pt', [])[0] is None
    wh = 'standard_shader\n{\n\tname g\n\tshaderop DefaultReflection\n}\npixelpel_rasterizer\n{\n}\n'
    assert 'REFUSED' in migrate_rasterizer(wh, 'direct', [])[1]
    cm = '/*\nfoo_rasterizer\n{\n}\n*/\n' + pt
    assert migrate_rasterizer(cm, 'pt', [])[1].startswith('MIGRATED')
    onb = ('onb_pinhole_camera\n{\n\tlocation 0 0 -10\n\tva 1 0 0\n\tvb 0 1 0\n\tcomponents UV\n}\n'
           'mis_pathtracing_shaderop\n{\n\tname x\n}\n')
    new, st = migrate_renames(onb, [])
    assert 'pinhole_camera' in new and 'lookat\t\t0 0 -9' in new and 'up\t\t0 1 0' in new, new
    assert 'onb_' not in new and '\npathtracing_shaderop' in new
    print('selftest: OK')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--mode', choices=('pt', 'direct', 'renames'))
    ap.add_argument('--dry-run', action='store_true', help='report only, write nothing')
    ap.add_argument('-v', '--verbose', action='store_true', help='also list unchanged files')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('paths', nargs='*', default=['scenes'])
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.mode:
        ap.error('--mode is required')
    return run(a.mode, a.paths, a.dry_run, a.verbose)


if __name__ == '__main__':
    sys.exit(main())
