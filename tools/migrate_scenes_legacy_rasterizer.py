#!/usr/bin/env python3
"""
migrate_scenes_legacy_rasterizer.py -- move scenes off the FROZEN legacy
pixel rasterizers (legacy-deprecation Phase 2, slices A and B1; see
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

  full      (slice B1) Any other legacy chain: Whitted `DefaultReflection` /
            `DefaultRefraction`, `distributiontracing_shaderop`,
            `finalgather_shaderop`, the photon maps and gathers,
            `irradiance_cache`, `ambientocclusion_shaderop` (dropped, owner
            ruling #6), the point-cloud SSS ops, `arealight_shaderop`,
            `directlighting_shaderop`, path-tracing ops and alpha ops.
            `--integrator pt|bdpt|vcm` picks the target (default pt; CLAUDE.md
            "Integrator selection": PT by default, BDPT for strong indirect,
            VCM for caustics); `--sms` (pt only) also enables specular
            manifold sampling.  What it does:
              * a chain of only direct lighting / emission / Whitted
                reflection / refraction / area-light ops -- or distribution
                tracing with `diffuse false` (direct + glossy) -- maps to the
                path tracer with `max_diffuse_bounce 0` (the ruling #3a rule;
                glossy bounces are capped separately); any indirect op (DT
                with diffuse on, FG, photon map, irradiance cache, AO, path
                tracing, point-cloud SSS) gets full global illumination;
              * `samples` = legacy samples x the largest per-op sample count
                (arealight `samples`, DT / FG hemisphere samples, AO numtheta
                x numphi: what the legacy op took per shading point and what
                set its noise floor), floored at 16 (direct) / 64 (indirect),
                capped 1024 (`lum_samples` is inert and does not count; nor
                does a CACHED gather -- FG `cache`, default TRUE, DT
                `irradiance_caching`, AO `irradiance_cache` -- or an op that
                only feeds a point-cloud SSS op's irradiance shader, which
                runs per build point, not per pixel);
              * `--integrator vcm` REFUSES a scene with an `ambient_light` or
                `directional_light` (VCM renders neither) unless
                `--vcm-drop-unsupported-lights` is given, which reports the
                loss and writes a NOTE above the rasterizer;
              * every shader becomes a `standard_shader` with the single op
                `DefaultPathTracing`; every op chunk, photon map, gather,
                irradiance cache, and shader left unreferenced is deleted,
                with the comment lines directly above it, any section banner
                left empty, and commented-out `shaderop` lines in the
                rewritten chains;
              * no chain had an emission op but the scene has luminaire
                materials: `show_luminaires FALSE` (the legacy camera never
                saw them);
              * `diffusion_approximation_sss_shaderop` -> a
                `subsurfacescattering_material` (a diffusion BSSRDF, the
                closest match to the op's dipole; a random walk differs most
                on thin features) with the op's own sigma_s /
                sigma_a (per mm) x 1000 x geometric_scale, its ior and g;
              * `simple_sss_shaderop` (a phenomenological exp falloff) -> a
                `subsurfacescattering_material` (Christensen-Burley) whose
                albedo -- its total diffuse reflectance -- is the op's colour
                times its per-channel response 1/extinction^2 (normalized to
                the least-extinct channel; the integral of the op's exp
                kernel is 2 pi / extinction^2) and whose profile tail falls
                off at the op's rate (extinction x 1000 x geometric_scale).
                The op's colour is the object material's uniform base colour
                only where the legacy op applied it (`multiplybsdf TRUE`, or
                an irradiance shader that shades with the object's own BSDF:
                DefaultDirectLighting or a directlighting_shaderop with no /
                `none` bsdf, path-tracing / final-gather / distribution-
                tracing / photon-map ops, AO with multiplybrdf); a direct-
                lighting op with an override `bsdf` takes that material's
                literal colour (non-literal: refused), and otherwise it is
                untinted (0.95 grey);
                a base colour that is not a literal
                uniform painter is REFUSED unless `--sss-albedo R G B` gives
                one or `--sss-keep-material` keeps the authored material;
              * `arealight_shaderop` (N 1, no hotspot) -> a `rect_light` of
                exitance `power` (at N 1 the op is a Lambertian panel of
                radiance power/pi).
            The point-cloud SSS ops' brightness was set by an `irrad_scale`
            fudge factor and the photon maps' by `power_scale`; the migrated
            materials and integrators are physically normalized, so those
            looks change (reported).

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
(coverage applies to both faces): reported as a look change.  Material
coverage is honoured by shadow rays too, so a coverage below 1 also makes the
object's SHADOWS that transmissive (the legacy op only blended what the camera
saw): also reported as a look change.

RASTERIZER PARAMETERS.  The path tracers do not accept `max_recursion`,
`lum_samples`, `luminary_sampler`, `luminary_sampler_param`, `filter_glossy`,
`integrate_rgb` or `rgb_spd*`.  `max_recursion` is dropped (PT bounds paths
itself at kDefaultPathTracingMaxDepth).  `lum_samples`, `luminary_sampler` and
`luminary_sampler_param` are dropped because they are INERT: the rasterizer
builds the sampler and hands it to LuminaryManager::SetLuminaireSampling,
which stores `pLumSampling` and never reads it (verified 2026-10-10,
src/Library/Rendering/LuminaryManager.cpp).  A scene that sets a nonzero
`filter_glossy`, `integrate_rgb TRUE` or any `rgb_spd*` is REFUSED.  `samples`
is carried verbatim in --mode pt / direct; when absent it is written as
`samples 1`, because the legacy default is 1 and the path tracers' default is
32 (--mode full has its own rule, see below).  `oidn_denoise` is carried
verbatim and NOT added when absent: the legacy pixel rasterizers ALSO default
to OIDN on (verified 2026-10-10: a default `pixelpel_rasterizer` render logs
"OIDN auto" and writes a `_denoised` output), so leaving it absent keeps the
scene's effective denoise setting.

SMS (--mode pt).  The SMS settings of the `pathtracing_shaderop`s a chain
references move to the rasterizer's `sms_*` parameters, with the op's
EFFECTIVE values written explicitly: an op leaves out a parameter at ITS
default, and the op's `sms_max_chain_depth` default is 10 where the
rasterizer's is 30 (SMSConfig::maxChainDepth), so copying only the lines the
op spells out would silently triple the chain depth.  The rasterizer setting
is global, so every referenced path-tracing op (DefaultPathTracing included,
whose SMS is off) must agree; otherwise the scene is REFUSED.

OUT OF SCOPE.  --mode pt / direct refuse every family --mode full handles.
Every mode refuses `sms_shaderop`, `donner_jensen_skin_sss_shaderop`, the
translucent / shadow photon maps, direct volume rendering, MLT, scenes
declaring more than one rasterizer, and an OBJECT-level `radiance_map` (only
the legacy chain honours it; the supported integrators read only the
rasterizer's map; per-object radiance_map is frozen and its one scene,
different_rmaps, lives unmigrated in scenes/Tests/Legacy/).
scenes/Tests/Legacy/ (the frozen-feature regression corpus) and
scenes/Tests/ChunkCoverage/ (one-chunk parser coverage) are never touched.

Idempotent: a migrated scene has no legacy rasterizer and is reported
`unchanged`.  Structure is read from a COMMENT-STRIPPED view of each line
(same lexer rules as tools/migrate_scenes_light_colorspace.py); the bytes
written back are the originals, edited line by line.

Usage:
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode full [--integrator pt|bdpt|vcm] [--sms]
                                    [--sss-albedo R G B | --sss-keep-material] [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode pt     [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode direct [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --mode renames [--dry-run] [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --audit [paths...]
    python3 tools/migrate_scenes_legacy_rasterizer.py --selftest
Every mode also prints a FLAG line for a scene with more than one live
rasterizer chunk (unnamed rasterizers derive last-wins, so only the LAST one
renders) or whose active rasterizer is frozen.  --audit sweeps for exactly
that, plus frozen rasterizers left inside comments.
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

# Every FROZEN rasterizer chunk (legacy pixel rasterizers + MLT).
FROZEN_RASTERIZERS = frozenset(('pixelpel_rasterizer', 'pixelintegratingspectral_rasterizer',
                                'mlt_rasterizer', 'mlt_spectral_rasterizer'))

RAW_RASTERIZER_RE = re.compile(r'^\s*([A-Za-z0-9_]+_rasterizer)\s*\{?\s*$')


def rasterizer_audit(text):
    """(active, declared, commented_frozen) for a scene.  `declared` lists the
    LIVE rasterizer chunks in file order; unnamed rasterizer chunks derive
    last-wins (Job::Set*Rasterizer replaces the previous one), so the ACTIVE
    rasterizer is the last declared one (None if there is none).
    `commented_frozen` lists frozen rasterizer keywords that appear only inside
    comments (inert, reported for cleanup)."""
    lines = text.split('\n')
    code = strip_comments(lines)
    declared = [c.kw for c in parse_chunks(code) if c.kw.endswith('_rasterizer')]
    raw = []
    for ln in lines:
        m = RAW_RASTERIZER_RE.match(ln.lstrip('#').lstrip('/*'))
        if m:
            raw.append(m.group(1))
    commented = [k for k in raw if k in FROZEN_RASTERIZERS and raw.count(k) > declared.count(k)]
    return (declared[-1] if declared else None), declared, sorted(set(commented))


def rasterizer_flag(rel, text):
    """A FLAG line when a scene declares more than one live rasterizer chunk
    (only the last one renders) or its ACTIVE rasterizer is frozen; else None."""
    try:
        active, declared, _ = rasterizer_audit(text)
    except ParseError:
        return None
    msgs = []
    if len(declared) > 1:
        msgs.append('%d rasterizer chunks (%s); the ACTIVE one is the last declared, `%s`' % (
            len(declared), ', '.join(declared), active))
    if active in FROZEN_RASTERIZERS:
        msgs.append('the active rasterizer `%s` is FROZEN' % active)
    return ('FLAG      %s: %s' % (rel, '; '.join(msgs))) if msgs else None


def audit(paths):
    """--audit: report every scene whose ACTIVE (last-declared) rasterizer is
    frozen, every scene with more than one live rasterizer chunk, and every
    scene that carries a frozen rasterizer only inside a comment.  The
    frozen-feature corpus (scenes/Tests/Legacy/) is listed separately; it is
    frozen on purpose."""
    rc = 0
    n = {'frozen': 0, 'multi': 0, 'commented': 0, 'corpus': 0}
    for f in collect(paths):
        rel = repo_rel(f)
        try:
            with open(f, 'r', encoding='utf-8', newline='') as fh:
                text = fh.read()
            active, declared, commented = rasterizer_audit(text)
        except (IOError, UnicodeDecodeError, ParseError) as e:
            print('ERROR     %s: %s' % (rel, e))
            rc = 1
            continue
        corpus = rel.startswith(LEGACY_CORPUS) or rel.startswith(COVERAGE_CORPUS)
        if active in FROZEN_RASTERIZERS:
            if corpus:
                n['corpus'] += 1
            else:
                n['frozen'] += 1
                print('FROZEN    %s: active rasterizer `%s` is frozen' % (rel, active))
        if len(declared) > 1:
            n['multi'] += 1
            print('MULTI     %s: %s; active (last-declared) `%s`' % (rel, ', '.join(declared), active))
        if commented and not corpus:
            n['commented'] += 1
            print('COMMENTED %s: frozen %s only inside a comment (inert)' % (rel, ', '.join(commented)))
    print('audit: %d active-frozen (plus %d in the frozen corpus), %d multi-rasterizer, '
          '%d with a commented-out frozen rasterizer' % (n['frozen'], n['corpus'], n['multi'], n['commented']))
    return rc

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
_BIDIR_COMMON = {
    'defaultshader', 'samples', 'show_luminaires', 'oidn_denoise', 'oidn_quality',
    'oidn_device', 'oidn_prefilter', 'choose_one_light', 'blue_noise_sampler',
    'pixel_sampler', 'pixel_sampler_param', 'pixel_filter', 'pixel_filter_width',
    'pixel_filter_height', 'pixel_filter_paramA', 'pixel_filter_paramB',
    'radiance_map', 'radiance_scale', 'radiance_background', 'radiance_orient',
    'adaptive_max_samples', 'adaptive_threshold', 'show_adaptive_map',
    'direct_clamp', 'indirect_clamp', 'rr_min_depth', 'rr_threshold',
    'max_diffuse_bounce', 'max_glossy_bounce', 'max_transmission_bounce',
    'max_translucent_bounce', 'max_volume_bounce', 'light_bvh',
    'progressive_rendering', 'progressive_samples_per_pass',
    'max_eye_depth', 'max_light_depth',
}
_SPECTRAL_CORE = {'spectral_samples', 'num_wavelengths', 'nmbegin', 'nmend', 'hwss'}
_VCM_EXTRA = {'merge_radius', 'vc_enabled', 'vm_enabled'}
TARGET_PARAMS.update({
    'bdpt_pel_rasterizer': _BIDIR_COMMON,
    'bdpt_spectral_rasterizer': _BIDIR_COMMON | _SPECTRAL_CORE,
    'vcm_pel_rasterizer': _BIDIR_COMMON | _VCM_EXTRA,
    'vcm_spectral_rasterizer': _BIDIR_COMMON | _VCM_EXTRA | _SPECTRAL_CORE,
})
# --mode full: legacy rasterizer -> target, by --integrator.
FULL_TARGETS = {
    'pt':   {'pixelpel_rasterizer': 'pathtracing_pel_rasterizer',
             'pixelintegratingspectral_rasterizer': 'pathtracing_spectral_rasterizer'},
    'bdpt': {'pixelpel_rasterizer': 'bdpt_pel_rasterizer',
             'pixelintegratingspectral_rasterizer': 'bdpt_spectral_rasterizer'},
    'vcm':  {'pixelpel_rasterizer': 'vcm_pel_rasterizer',
             'pixelintegratingspectral_rasterizer': 'vcm_spectral_rasterizer'},
}

# Dropped silently: max_recursion (PT bounds paths itself); lum_samples and
# luminary_sampler_param (inert, see the docstring).
DROPPED = {'max_recursion', 'lum_samples', 'luminary_sampler_param'}
SMS_OP_PARAMS = ('sms_enabled', 'sms_max_iterations', 'sms_threshold',
                 'sms_max_chain_depth', 'sms_biased')
# pathtracing_shaderop's own SMS defaults (ChunkParserRegistry.cpp
# PathTracingShaderOpAsciiChunkParser::Finalize; Job.cpp DefaultPathTracing).
PT_OP_SMS_DEFAULTS = {'sms_enabled': 'FALSE', 'sms_max_iterations': '20', 'sms_threshold': '1e-5',
                      'sms_max_chain_depth': '10', 'sms_biased': 'TRUE'}

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

# Scenes reserved for a later slice.  Empty since slice C (Phase 2 step 6,
# 2026-10-10) migrated the Android catalogue scenes and the quickstart sample
# (shapes, kaleidoscope_atrium, tidepools) that were held here.
HELD = {}

# The frozen-feature regression corpus (owner ruling #17): originals kept
# so frozen chunks stay rendered and derived.  Never migrated.
LEGACY_CORPUS = 'scenes/Tests/Legacy/'
# Parser coverage scenes exist to exercise ONE chunk each; renaming a
# deprecated chunk out of its own coverage scene would drop that coverage.
COVERAGE_CORPUS = 'scenes/Tests/ChunkCoverage/'

BANNER_RE = re.compile(r'^\s*#{5,}\s*$')
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


def _norm_val(name, toks):
    """Canonical form of an sms_* value for comparison."""
    v = ' '.join(toks)
    if name in ('sms_enabled', 'sms_biased'):
        return 'TRUE' if v.strip('"').upper() in ('TRUE', '1') else 'FALSE'
    try:
        return repr(float(v))
    except ValueError:
        return v


def sms_to_rasterizer(chunks, chains, named, have, ind, additions, notes):
    """The SMS settings of the path-tracing ops a chain references move to
    the rasterizer.  An op's EFFECTIVE values are written -- including the
    op's own defaults for any it leaves out, because three of them differ from
    the rasterizer's (ChunkParserRegistry.cpp PathTracingShaderOpAsciiChunkParser
    / Job.cpp DefaultPathTracing: max_iterations 20, threshold 1e-5,
    max_chain_depth 10, biased TRUE; SMSConfig::maxChainDepth is 30).  The
    op's other solver fields take ManifoldSolverConfig's defaults, which equal
    the rasterizer's SMSConfig defaults, so they need no line.  Every
    referenced path-tracing op (DefaultPathTracing included: SMS off) must
    agree, because the rasterizer setting is global; otherwise REFUSED.
    Returns a refusal string or None."""
    referenced = []
    seen = set()
    for _, ops in chains:
        for _, opn in ops:
            if opn not in seen and classify_op(opn, named) == 'pathtracing_shaderop':
                seen.add(opn)
                referenced.append(opn)
    configs = {}
    for opn in referenced:
        chunk = next((c for c in chunks if c.kw in ('pathtracing_shaderop', 'mis_pathtracing_shaderop')
                      and c.name() == opn), None)
        cfg = {}
        en = chunk.get('sms_enabled') if chunk else None
        cfg['sms_enabled'] = _norm_val('sms_enabled', en) if en else 'FALSE'
        if cfg['sms_enabled'] == 'TRUE':
            for p in SMS_OP_PARAMS[1:]:
                v = chunk.get(p)
                cfg[p] = _norm_val(p, v) if v is not None else _norm_val(p, [PT_OP_SMS_DEFAULTS[p]])
        configs[opn] = (cfg, chunk)
    enabled = [o for o in referenced if configs[o][0]['sms_enabled'] == 'TRUE']
    if not enabled:
        return None
    first = configs[enabled[0]][0]
    for o in referenced:
        if configs[o][0] != first:
            return ('REFUSED: path-tracing ops `%s` and `%s` have different SMS settings; the '
                    'rasterizer\'s sms_* parameters are global' % (enabled[0], o))
    chunk = configs[enabled[0]][1]
    for p in SMS_OP_PARAMS:
        v = chunk.get(p)
        text = ' '.join(v) if v is not None else PT_OP_SMS_DEFAULTS[p]
        if p in have:
            if _norm_val(p, have[p][1][1:]) != first[p]:
                return 'REFUSED: rasterizer %s disagrees with pathtracing_shaderop `%s`' % (p, enabled[0])
            continue
        additions.append(ind + '%s\t\t%s' % (p, text))
    notes.append('SMS settings of pathtracing_shaderop `%s` copied to the rasterizer (op defaults '
                 'written explicitly)' % enabled[0])
    return None


# --------------------------------------------------------------------------
# Rasterizer migration (modes pt / direct)
# --------------------------------------------------------------------------

def per_object_radiance_map_refusal(chunks):
    """A `radiance_map` on an OBJECT is consulted only by the legacy shader-op
    chain (the ops forward RayIntersection::pRadianceMap to the rays they
    cast); the path tracer, BDPT and VCM read only the rasterizer's global
    map.  Migrating would silently drop the feature, so refuse."""
    for c in chunks:
        if c.kw.endswith('_object') and c.get('radiance_map'):
            return ('REFUSED: object `%s` sets its own radiance_map, which only the legacy '
                    'shader-op chain honours (the supported integrators read only the '
                    'rasterizer\'s radiance_map)' % c.name())
    return None


def migrate_rasterizer(text, mode, report):
    lines = text.split('\n')
    code = strip_comments(lines)
    chunks = parse_chunks(code)

    rasters = [c for c in chunks if c.kw.endswith('_rasterizer')]
    legacy = [c for c in rasters if c.kw in LEGACY_RASTERIZERS]
    if not legacy:
        return None, 'unchanged (no legacy pixel rasterizer)'
    if len(rasters) != 1:
        return None, ('REFUSED: %d rasterizer chunks (%s); only the last declared (`%s`) renders -- '
                      'remove the others first' % (len(rasters), ', '.join(c.kw for c in rasters), rasters[-1].kw))
    for c in chunks:
        if c.kw in OUT_OF_SCOPE_CHUNKS:
            return None, 'REFUSED: uses %s (`%s`), a later slice' % (OUT_OF_SCOPE_CHUNKS[c.kw], c.kw)
        if c.kw.endswith('_photonmap') or c.kw.endswith('_gather'):
            return None, 'REFUSED: uses photon mapping (`%s`), a later slice' % c.kw

    res = per_object_radiance_map_refusal(chunks)
    if res:
        return None, res
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
            # Inert (LuminaryManager stores the sampler and never reads it).
            if val and val[0].strip('"') != 'none':
                notes.append('luminary_sampler %s dropped (inert: never read)' % val[0])
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

    # A chain that runs two path-tracing ops at the same depth ADDS them (a
    # standard_shader runs every op; an advanced_shader op with `+` adds over
    # its [min, max] depth range), so the legacy render counted the path
    # tracer twice there.  The migrated scene counts it once: report it.
    if mode == 'pt':
        for c, ops in chains:
            pt_lines = [(idx, opn) for idx, opn in ops
                        if classify_op(opn, named) == 'pathtracing_shaderop']
            if len(pt_lines) < 2:
                continue
            if c.kw == 'standard_shader':
                overlap = True
            else:
                spans = []
                for idx, _ in pt_lines:
                    toks = code[idx].split()
                    try:
                        spans.append((int(toks[2]), int(toks[3])))
                    except (IndexError, ValueError):
                        spans.append((0, 1 << 30))
                overlap = any(a[0] <= b[1] and b[0] <= a[1]
                              for i, a in enumerate(spans) for b in spans[i + 1:])
            if overlap:
                notes.append('LOOK CHANGE: shader `%s` runs %d path-tracing ops at the same '
                             'depth; the legacy chain summed them (a double count), the '
                             'path tracer counts once' % (c.name(), len(pt_lines)))

    # SMS on a referenced pathtracing_shaderop moves to the rasterizer.
    if mode == 'pt':
        res = sms_to_rasterizer(chunks, chains, named, have, ind, additions, notes)
        if res is not None:
            return None, res

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
        if coverage < 1.0:
            notes.append('LOOK CHANGE: material coverage also applies to SHADOW rays, so the object\'s '
                         'shadows become %s%% transmissive (the legacy op only blended the camera-visible '
                         'surface; its shadows stayed opaque)' % fmt_num(round(100.0 * (1.0 - coverage), 6)))
    return None


# --------------------------------------------------------------------------
# --mode full (legacy-deprecation Phase 2 slice B1)
# --------------------------------------------------------------------------

# Op families --mode full understands (classify_op's names).
FULL_DIRECT_OPS = {'directlighting', 'directlighting_shaderop', 'emission',
                   'reflection (Whitted)', 'refraction (Whitted)', 'arealight_shaderop'}
FULL_INDIRECT_OPS = {'pathtracing_shaderop', 'distributiontracing_shaderop', 'finalgather_shaderop',
                     'ambientocclusion_shaderop', 'photon map'}
SSS_OPS = {'simple_sss_shaderop', 'diffusion_approximation_sss_shaderop'}
FULL_KNOWN_OPS = FULL_DIRECT_OPS | FULL_INDIRECT_OPS | SSS_OPS | ALPHA_OPS
# Chunks that only feed the legacy chain; deleted by --mode full.
FULL_DELETED_KINDS = ('_photonmap', '_gather')
# Sample floors for --mode full (see the docstring).
FULL_SAMPLE_FLOOR_DIRECT = 16
FULL_SAMPLE_FLOOR_INDIRECT = 64
FULL_SAMPLE_CAP = 1024
# Index of refraction the simple (phenomenological) SSS op is converted at.
SIMPLE_SSS_IOR = 1.3


def _painter_rgb(chunks, name):
    """Linear Rec.709 RGB of a literal uniformcolor_painter, or None."""
    for c in chunks:
        if c.kw == 'uniformcolor_painter' and c.name() == name:
            col = c.get('color')
            if not col or len(col) != 3 or not all(is_literal_number(x) for x in col):
                return None
            v = [float(x) for x in col]
            cs = (c.get('colorspace') or ['Rec709RGB_Linear'])[0]
            if cs in ('Rec709RGB_Linear', 'sRGB_Linear', 'RISERGB'):
                return v
            if cs == 'sRGB':
                return [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in v]
            return None
    return None


def _burley_s(A):
    """Christensen-Burley scaling factor (BurleyNormalizedDiffusionProfile.h)."""
    return 1.9 - A + 3.5 * (A - 0.8) ** 2


def simple_sss_coefficients(albedo, sigma_tr):
    """Per channel: (absorption, scattering) for subsurfacescattering_material
    (Christensen-Burley normalized diffusion, src/Library/Materials/
    BurleyNormalizedDiffusionProfile.h).  Its total diffuse reflectance IS
    the albedo A = sigma_s / (sigma_s + sigma_a) (g = 0), and its profile's
    long tail decays as exp(-s(A) r / (3 d)) with d = 1 / sigma_t, so the
    legacy op's exp(-sigma_tr r) falloff maps to sigma_t = 3 sigma_tr / s(A)."""
    absorption, scattering = [], []
    for A, tr in zip(albedo, sigma_tr):
        A = min(max(A, 0.01), 0.95)
        st = 3.0 * tr / _burley_s(A)
        scattering.append(A * st)
        absorption.append((1.0 - A) * st)
    return absorption, scattering


def _fmt3(v):
    return ' '.join('%.6g' % x for x in v)


def _material_base_color(chunks, mat):
    """The diffuse base colour of a material (the colour the legacy SSS op's
    multiplybsdf tinted by), as linear RGB, or None."""
    slot = {'lambertian_material': 'reflectance', 'translucent_material': 'ref',
            'ward_anisotropic_material': 'rd', 'ward_isotropic_material': 'rd',
            'orennayar_material': 'reflectance'}.get(mat.kw)
    if slot is None:
        return None
    p = mat.get(slot)
    return _painter_rgb(chunks, p[0]) if p else None


def per_op_samples(c):
    """Samples a legacy op takes per shading point, or 1 (None: non-literal).
    Defaults from ChunkParserRegistry.cpp: DT 16, FG theta 15 x phi 47 (or
    `samples`), AO numtheta 5 x numphi 15 (or `samples`), arealight 9.  A
    cached gather (FG `cache`, default TRUE; DT `irradiance_caching`; AO
    `irradiance_cache`) counts 1."""
    def num(name, d):
        v = c.get(name)
        if v is None:
            return d
        return int(float(v[0])) if is_literal_number(v[0]) else None
    if c.kw == 'arealight_shaderop':
        return num('samples', 9)
    def flag(name, d):
        v = c.get(name)
        return d if v is None else v[0].upper() in ('TRUE', '1')
    # A CACHED gather samples only at sparse cache records, which the
    # interpolation smooths: it does not set the per-pixel noise floor.
    if c.kw == 'distributiontracing_shaderop':
        return 1 if flag('irradiance_caching', False) else num('samples', 16)
    if c.kw == 'finalgather_shaderop':
        if flag('cache', True):
            return 1
        if c.get('samples') is not None:
            return num('samples', 0)
        t, ph = num('theta_samples', 15), num('phi_samples', 47)
        return None if t is None or ph is None else t * ph
    if c.kw == 'ambientocclusion_shaderop':
        if flag('irradiance_cache', False):
            return 1
        if c.get('samples') is not None:
            return num('samples', 0)
        t, ph = num('numtheta', 5), num('numphi', 15)
        return None if t is None or ph is None else t * ph
    return 1


def sss_tint_source(oc, chunks, mat):
    """Where a simple_sss_shaderop's colour came from.  The op sums the
    irradiance its `shader` computed at the sample points, shaded by that
    shader's ops.  Returns 'base' (tinted by the object's base colour), 'none'
    (untinted: only the op's extinction sets the hue), an RGB list (an
    override material's colour), or ('refuse', reason).
      * `multiplybsdf TRUE` multiplies the result by the object's BSDF: 'base'.
      * A direct-lighting op must be present for rule (a): DefaultDirectLighting,
        or a directlighting_shaderop with no `bsdf` or `bsdf none`, shades with
        the OBJECT's BSDF (the parser turns `none` into a null override and
        DirectLightingShaderOp falls back to the object's material): 'base';
        with `bsdf X` it shades with X: X's colour (a non-literal colour is
        refused).
      * Path-tracing, final-gather, distribution-tracing and photon-map ops
        shade with the object's BSDF: 'base'.
      * AO tints only with multiplybrdf (default TRUE): 'base'.
    Any 'base' wins; several different override colours are refused."""
    mb = oc.get('multiplybsdf')
    if mb and mb[0].upper() in ('TRUE', '1'):
        return 'base'
    shname = (oc.get('shader') or ['none'])[0]
    sh = next((c for c in chunks if c.kw in ('standard_shader', 'advanced_shader') and c.name() == shname), None)
    if sh is None:
        return 'none'
    named = {c.name(): c for c in chunks if c.kw.endswith('_shaderop') and c.name()}
    tints = []
    for idx, toks in sh.params:
        if toks[0] != 'shaderop' or len(toks) < 2:
            continue
        op = named.get(toks[1])
        if op is None:
            if toks[1] in ('DefaultDirectLighting', 'DefaultPathTracing') or 'PhotonMap' in toks[1]:
                tints.append('base')
            continue
        if op.kw == 'directlighting_shaderop':
            b = op.get('bsdf')
            if not b or b[0] == 'none':
                tints.append('base')
                continue
            m = next((c for c in chunks if c.kw.endswith('_material') and c.name() == b[0]), None)
            col = _material_base_color(chunks, m) if m else None
            if col is None:
                return ('refuse', 'directlighting_shaderop `%s` override bsdf `%s` has no literal uniform '
                        'colour' % (op.name(), b[0]))
            tints.append(col)
        elif op.kw in ('pathtracing_shaderop', 'mis_pathtracing_shaderop', 'finalgather_shaderop',
                       'distributiontracing_shaderop'):
            tints.append('base')
        elif op.kw == 'ambientocclusion_shaderop':
            v = op.get('multiplybrdf')
            if not (v and v[0].upper() in ('FALSE', '0')):
                tints.append('base')
    if 'base' in tints:
        return 'base'
    cols = []
    for t in tints:
        if t not in cols:
            cols.append(t)
    if len(cols) > 1:
        return ('refuse', 'irradiance shader `%s` mixes override BSDFs of different colours' % shname)
    return cols[0] if cols else 'none'

def migrate_full(text, integrator, report, sss_albedo=None, sss_keep=False, sms=False,
                 vcm_drop_lights=False):
    lines = text.split('\n')
    code = strip_comments(lines)
    chunks = parse_chunks(code)

    rasters = [c for c in chunks if c.kw.endswith('_rasterizer')]
    legacy = [c for c in rasters if c.kw in LEGACY_RASTERIZERS]
    if not legacy:
        return None, 'unchanged (no legacy pixel rasterizer)'
    if len(rasters) != 1:
        return None, ('REFUSED: %d rasterizer chunks (%s); only the last declared (`%s`) renders -- '
                      'remove the others first' % (len(rasters), ', '.join(c.kw for c in rasters), rasters[-1].kw))
    for c in chunks:
        if c.kw in ('mlt_rasterizer', 'mlt_spectral_rasterizer', 'directvolumerendering_shader',
                    'spectraldirectvolumerendering_shader'):
            return None, 'REFUSED: uses %s, not a --mode full family' % c.kw
        if c.kw in ('donner_jensen_skin_sss_shaderop', 'sms_shaderop', 'translucent_pel_photonmap',
                    'shadow_photonmap'):
            return None, 'REFUSED: `%s` has no --mode full conversion' % c.kw

    res = per_object_radiance_map_refusal(chunks)
    if res:
        return None, res
    named = op_types(chunks)
    chains = shader_chains(chunks)
    ops_by_name = {c.name(): c for c in chunks if c.kw.endswith('_shaderop') and c.name()}
    mats_by_name = {c.name(): c for c in chunks if c.kw.endswith('_material') and c.name()}
    used = set()
    for _, ops in chains:
        for _, opn in ops:
            used.add(classify_op(opn, named))
    unknown = sorted(used - FULL_KNOWN_OPS)
    if unknown:
        return None, 'REFUSED for --mode full: shader chains use %s' % ', '.join(unknown)
    has_ic = any(c.kw == 'irradiance_cache' for c in chunks)
    has_pm = any(c.kw.endswith(FULL_DELETED_KINDS) for c in chunks)
    # A distribution-tracing op with `diffuse false` traces only glossy /
    # specular continuations: with direct lighting beside it the chain is
    # direct + glossy, which the path tracer reproduces at
    # max_diffuse_bounce 0 (glossy bounces are capped separately).
    referenced_ops = set(opn for _, ops in chains for _, opn in ops)
    def _dt_diffuse(c):
        v = c.get('diffuse')
        return not (v and v[0].upper() in ('FALSE', '0'))
    indirect_kinds = set(FULL_INDIRECT_OPS | SSS_OPS)
    if 'distributiontracing_shaderop' in used and not any(
            _dt_diffuse(c) for c in chunks
            if c.kw == 'distributiontracing_shaderop' and c.name() in referenced_ops):
        indirect_kinds.discard('distributiontracing_shaderop')
    indirect = bool(used & indirect_kinds) or has_ic
    direct_only = not indirect

    r = legacy[0]
    target = FULL_TARGETS[integrator][r.kw]
    accepted = TARGET_PARAMS[target]
    edits = {}
    notes = []

    # VCM samples neither ambient_light nor directional lights (it never emits
    # from them and its NEE skips them; VCMIntegrator.cpp, the
    # zero-exitance-light comment), so their light would silently vanish.
    if integrator == 'vcm':
        unseen = [c for c in chunks if c.kw in ('ambient_light', 'directional_light')]
        if unseen:
            names = ', '.join('%s `%s`' % (c.kw, c.name()) for c in unseen)
            if not vcm_drop_lights:
                return None, ('REFUSED: VCM renders neither ambient_light nor directional_light, so %s '
                              'would be dropped; choose another integrator or pass '
                              '--vcm-drop-unsupported-lights after measuring the dropped term' % names)
            notes.append('LOOK CHANGE (LIGHT DROPPED): VCM does not render %s; their light is gone '
                         '(--vcm-drop-unsupported-lights)' % names)
            dropped_light_comment = ('# NOTE: VCM renders neither ambient_light nor directional_light, so this '
                                     'scene\'s %s %s nothing here.' % (names, 'contributes' if len(unseen) == 1 else 'contribute'))
        else:
            dropped_light_comment = None
    else:
        dropped_light_comment = None

    have = {}
    for idx, toks in r.params:
        have[toks[0]] = (idx, toks)
    for name, (idx, toks) in have.items():
        val = toks[1:]
        if name == 'luminary_sampler':
            if val and val[0].strip('"') != 'none':
                notes.append('luminary_sampler %s dropped (inert: never read)' % val[0])
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
            return None, 'REFUSED: integrate_rgb TRUE has no equivalent'
        if name.startswith('rgb_spd'):
            return None, 'REFUSED: %s has no equivalent' % name
        if name in DROPPED or name == 'samples':
            if name != 'samples':
                edits[idx] = []
            continue
        if name not in accepted:
            return None, 'REFUSED: parameter `%s` is not accepted by %s' % (name, target)

    ind = '\t'
    for idx, toks in r.params:
        ind = indent_of(lines[idx]) or '\t'
        break
    additions = []

    # Samples: a legacy op that integrates over a domain takes its OWN sample
    # count per shading point (arealight_shaderop `samples`, a DT / FG op's
    # hemisphere samples, AO numtheta x numphi), and that sets the legacy noise
    # floor (an area light's penumbra, a glossy blur); the path tracer takes ONE
    # such sample per pixel sample.  So samples x the largest per-op count,
    # floored (16 direct, 64 indirect) and capped at 1024.  (lum_samples is
    # inert and does not count.)
    def ival(n, d):
        v = have.get(n)
        try:
            return int(float(v[1][1])) if v else d
        except (IndexError, ValueError):
            return None
    spp = ival('samples', 1)
    if spp is None:
        return None, 'REFUSED: samples is not a literal number'
    # Ops that only appear in a point-cloud SSS op's irradiance shader run
    # once per BUILD point, not per pixel, and do not set the pixel noise.
    sss_irr_shaders = set((c.get('shader') or ['none'])[0] for c in chunks if c.kw in SSS_OPS)
    pixel_ops = set(opn for c, ops in chains if c.name() not in sss_irr_shaders for _, opn in ops)
    op_samples, op_from = 1, None
    for c in chunks:
        if c.kw.endswith('_shaderop') and c.name() in pixel_ops:
            n = per_op_samples(c)
            if n is None:
                return None, 'REFUSED: `%s` `%s` has a non-literal sample count' % (c.kw, c.name())
            if n > op_samples:
                op_samples, op_from = n, '%s `%s`' % (c.kw, c.name())
    floor = FULL_SAMPLE_FLOOR_DIRECT if direct_only else FULL_SAMPLE_FLOOR_INDIRECT
    new_spp = min(max(spp * op_samples, floor), FULL_SAMPLE_CAP)
    if 'samples' in have:
        idx = have['samples'][0]
        edits[idx] = [indent_of(lines[idx]) + 'samples\t\t\t\t%d' % new_spp]
    else:
        additions.append(ind + 'samples\t\t\t\t%d' % new_spp)
    notes.append('samples %d (legacy samples %d x %d per-op samples%s, floor %d, cap %d)'
                 % (new_spp, spp, op_samples, ' from %s' % op_from if op_from else '', floor,
                    FULL_SAMPLE_CAP))

    if direct_only:
        if integrator != 'pt':
            return None, ('REFUSED: a direct / Whitted chain (no indirect op) maps to the path '
                          'tracer with max_diffuse_bounce 0; use --integrator pt')
        if 'max_diffuse_bounce' in have:
            idx = have['max_diffuse_bounce'][0]
            edits[idx] = [ind + 'max_diffuse_bounce\t\t0']
        else:
            additions.append(ind + 'max_diffuse_bounce\t\t0')
        notes.append('max_diffuse_bounce 0: the chain was direct lighting plus Whitted '
                     'reflection / refraction only (owner ruling #3a)')
    else:
        notes.append('full global illumination (%s): the chain integrated indirect / caustic light' % target)

    # Emitters the legacy camera never saw: no chain had an emission op.
    if 'emission' not in used and 'show_luminaires' not in have:
        lum_mats = [c for c in chunks if c.kw.endswith('_luminaire_material')]
        if lum_mats:
            additions.append(ind + 'show_luminaires\t\tFALSE')
            notes.append('show_luminaires FALSE: no chain had an emission op, so the legacy camera '
                         'never saw the luminaires (they still light the scene)')

    if integrator == 'pt' and 'pathtracing_shaderop' in used:
        res = sms_to_rasterizer(chunks, chains, named, have, ind, additions, notes)
        if res is not None:
            return None, res
    elif 'pathtracing_shaderop' in used:
        for c in chunks:
            if c.kw in ('pathtracing_shaderop', 'mis_pathtracing_shaderop'):
                en = c.get('sms_enabled')
                if en and en[0].upper() in ('TRUE', '1'):
                    return None, 'REFUSED: SMS is a path-tracer feature; use --integrator pt'

    if sms:
        if integrator != 'pt':
            return None, 'REFUSED: --sms needs --integrator pt (SMS is a path-tracer feature)'
        if any(a.split()[0] == 'sms_enabled' for a in additions) or 'sms_enabled' in have:
            return None, 'REFUSED: --sms on a scene whose path-tracing op already configures SMS'
        additions.append(ind + 'sms_enabled\t\tTRUE')
        notes.append('sms_enabled TRUE (--sms): specular manifold sampling carries the caustics the '
                     'photon map used to (rasterizer SMS defaults)')

    kl = lines[r.kw_line]
    edits[r.kw_line] = [indent_of(kl) + '# Migrated from %s by tools/migrate_scenes_legacy_rasterizer.py '
                        '(--mode full --integrator %s%s, legacy-deprecation Phase 2).'
                        % (r.kw, integrator, ' --sms' if sms else '')] + \
                       ([indent_of(kl) + dropped_light_comment] if dropped_light_comment else []) + [
                        kl.replace(r.kw, target, 1)]
    edits[r.close_line] = additions + [lines[r.close_line]]
    # Appended lines go just before the brace: drop blank lines that would
    # otherwise separate them from the chunk's other parameters.
    k = r.close_line - 1
    while additions and k > r.kw_line and k not in edits and lines[k].strip() == '':
        edits[k] = []
        k -= 1

    default_shader = (have['defaultshader'][1][1] if 'defaultshader' in have else 'global').strip('"')
    objects = [c for c in chunks if c.kw.endswith('_object')]

    def obj_shader(o):
        v = o.get('shader')
        return v[0] if v else default_shader

    # Point-cloud SSS ops -> subsurfacescattering_material on their objects.
    sss_ops = {}
    for c, ops in chains:
        for _, opn in ops:
            if classify_op(opn, named) in SSS_OPS:
                sss_ops.setdefault(opn, set()).add(c.name())
    new_mats = {}     # (op, material) -> new name
    insert_before = {}  # line index -> lines
    for opn in sorted(sss_ops):
        oc = ops_by_name[opn]
        shaders = sss_ops[opn]
        users = [o for o in objects if obj_shader(o) in shaders]
        if not users:
            continue
        gs = oc.get('geometric_scale')
        gs = float(gs[0]) if gs else 1.0
        k = 1000.0 * gs     # legacy: r_mm = r_scene * 1000 * geometric_scale
        for o in users:
            mv = o.get('material')
            if not mv:
                return None, 'REFUSED: SSS object `%s` has no material' % o.name()
            key = (opn, mv[0])
            if key not in new_mats:
                mat = mats_by_name.get(mv[0])
                if mat is None:
                    return None, 'REFUSED: material `%s` not found' % mv[0]
                nm = '%s_sss_%s' % (opn, mv[0])
                body = ['subsurfacescattering_material', '{', '\tname\t\t\t%s' % nm]
                if oc.kw == 'diffusion_approximation_sss_shaderop':
                    sc = oc.get('scattering') or ['2.19', '2.62', '3.0']
                    ab = oc.get('absorption') or ['0.0021', '0.0041', '0.0071']
                    ior = (oc.get('ior') or ['1.3'])[0]
                    g = (oc.get('g') or ['0.8'])[0]
                    scat = [float(x) * k for x in sc]
                    absr = [float(x) * k for x in ab]
                    body += ['\tior\t\t\t%s' % ior, '\tabsorption\t%s' % _fmt3(absr),
                             '\tscattering\t%s' % _fmt3(scat), '\tg\t\t\t%s' % g]
                    how = ('the op\'s own sigma_s / sigma_a (per mm) x 1000 x geometric_scale %s, '
                           'ior %s, g %s' % (fmt_num(gs), ior, g))
                else:
                    ext = oc.get('extinction') or ['0.02', '0.03', '0.09']
                    tr = [float(x) * k for x in ext]
                    tint = sss_tint_source(oc, chunks, mat)
                    if isinstance(tint, tuple):
                        return None, 'REFUSED: simple_sss_shaderop `%s`: %s' % (opn, tint[1])
                    if tint == 'none':
                        alb, src = [0.95, 0.95, 0.95], 'untinted (multiplybsdf FALSE, irradiance shader with no surface colour)'
                    elif isinstance(tint, list):
                        alb, src = tint, 'the irradiance shader\'s override BSDF'
                    else:
                        alb = _material_base_color(chunks, mat)
                        src = 'material `%s`' % mv[0]
                    if alb is None:
                        if sss_keep:
                            new_mats[key] = None
                            notes.append('simple_sss_shaderop `%s`: material `%s` (%s) KEPT as authored '
                                         '(--sss-keep-material: its colour is not a uniform painter, which '
                                         'the SSS materials\' scalar coefficients cannot carry); the op is '
                                         'dropped' % (opn, mv[0], mat.kw))
                            continue
                        if sss_albedo is None:
                            return None, ('REFUSED: simple_sss_shaderop `%s`: cannot read a uniform base '
                                          'colour from material `%s` (%s); pass --sss-albedo or '
                                          '--sss-keep-material' % (opn, mv[0], mat.kw))
                        alb, src = sss_albedo, '--sss-albedo'
                    # The op's response per channel is the base colour times
                    # the integral of its exp(-sigma r) kernel over the
                    # surface, 2 pi / sigma^2: channels with a lower
                    # extinction glow brighter (that is where the authored
                    # tint of a white-material SSS object comes from).  The
                    # least-extinct channel keeps the base colour.
                    emin = min(tr)
                    alb = [min(max(c * (emin / t) ** 2, 0.01), 0.95) for c, t in zip(alb, tr)]
                    absr, scat = simple_sss_coefficients(alb, tr)
                    body += ['\tior\t\t\t%s' % fmt_num(SIMPLE_SSS_IOR), '\tabsorption\t%s' % _fmt3(absr),
                             '\tscattering\t%s' % _fmt3(scat), '\tg\t\t\t0']
                    how = ('Christensen-Burley match: albedo %s (colour %s, x the op\'s 1/extinction^2 '
                           'channel weights, clamped to [0.01, 0.95]), profile tail falloff = extinction x 1000 x '
                           'geometric_scale %s = %s per unit'
                           % (_fmt3(alb), src, fmt_num(gs), _fmt3(tr)))
                body += ['}', '']
                hdr = ['# Converted from %s `%s` (point-cloud SSS, frozen) by '
                       'tools/migrate_scenes_legacy_rasterizer.py --mode full:' % (oc.kw, opn),
                       '#   %s.' % how]
                insert_before.setdefault(o.kw_line, []).extend(hdr + body)
                new_mats[key] = nm
                notes.append('%s `%s` -> subsurfacescattering_material `%s` (%s)' % (oc.kw, opn, nm, how))
            if new_mats[key] is None:
                continue
            for idx, toks in o.params:
                if toks[0] == 'material':
                    edits[idx] = [re.sub(r'^(\s*material\s+)\S+', lambda m: m.group(1) + new_mats[key],
                                         lines[idx], count=1)]

    if any(v is not None for v in new_mats.values()):
        notes.append('LOOK CHANGE: the point-cloud SSS brightness was set by each op\'s irrad_scale '
                     'fudge factor; the subsurfacescattering_material is physically normalized')

    # arealight_shaderop -> rect_light.
    for c in chunks:
        if c.kw != 'arealight_shaderop':
            continue
        if not any(opn == c.name() for _, ops in chains for _, opn in ops):
            continue
        Nv = c.get('N')
        if (Nv and not (is_literal_number(Nv[0]) and float(Nv[0]) == 1.0)) or c.get('hotspot'):
            return None, 'REFUSED: arealight_shaderop `%s` sets N / hotspot (no rect_light equivalent)' % c.name()
        loc = _vec(c.get('location')) or [0.0, 10.0, 0.0]
        if c.get('make_dir'):
            t = _vec(c.get('make_dir'))
            if t is None:
                return None, 'REFUSED: arealight_shaderop make_dir is not literal'
            d = _sub(t, loc)
        else:
            d = _vec(c.get('dir')) or [0.0, -1.0, 0.0]
        if math.sqrt(sum(x * x for x in d)) == 0.0:
            return None, 'REFUSED: arealight_shaderop `%s` has a zero direction' % c.name()
        col = _painter_rgb(chunks, (c.get('emission') or ['color_white'])[0])
        if col is None:
            return None, 'REFUSED: arealight_shaderop emission is not a literal uniformcolor_painter'
        power = float((c.get('power') or ['1.0'])[0])
        w = float((c.get('width') or ['1.0'])[0])
        h = float((c.get('height') or ['1.0'])[0])
        # The op adds emm*power*(N+1)cos_r^N/(2pi)*cos_l*A/d^2*f per sample
        # (AreaLightShaderOp::Shade; Job::AddAreaLightShaderOp normalizes
        # `dir`).  At N = 1 that is a Lambertian panel of radiance
        # emm*power/pi, i.e. a rect_light of exitance `power`.
        exitance = power
        body = ['# Converted from arealight_shaderop `%s` (frozen) by tools/migrate_scenes_legacy_rasterizer.py'
                ' --mode full:' % c.name(),
                '#   exitance = power (at N 1 the op is a Lambertian panel of radiance power/pi).',
                'rect_light', '{', '\tname\t\t%s' % c.name(),
                '\tcenter\t\t%s' % ' '.join(fmt_num(x) for x in loc),
                '\tsize\t\t%s %s' % (fmt_num(w), fmt_num(h)),
                '\tfacing\t\t%s' % ' '.join(fmt_num(x) for x in d),
                '\texitance\t%s' % ('%.6g' % exitance),
                '\tcolor\t\t%s' % _fmt3(col), '}']
        edits[c.kw_line] = body
        for kk in range(c.kw_line + 1, c.close_line + 1):
            edits[kk] = []
        notes.append('arealight_shaderop `%s` -> rect_light (exitance %.6g, centre %s, facing %s)'
                     % (c.name(), exitance, ' '.join(fmt_num(x) for x in loc), ' '.join(fmt_num(x) for x in d)))

    # Alpha ops -> material coverage (as --mode direct).
    alpha_ops = {}
    for _, ops in chains:
        for _, opn in ops:
            t = classify_op(opn, named)
            if t in ALPHA_OPS:
                alpha_ops[opn] = t
    if alpha_ops:
        res = convert_alpha_ops(lines, chunks, chains, named, alpha_ops, edits, notes)
        if res is not None:
            return None, res

    # Every shader chain becomes one DefaultPathTracing op (PT's SSS
    # continuation shades through the scene's shaders; the integrators
    # ignore the chain otherwise).
    dropped = sorted(used - {'pathtracing_shaderop'})
    for c, ops in chains:
        kl = lines[c.kw_line]
        if c.kw == 'advanced_shader':
            edits[c.kw_line] = [kl.replace('advanced_shader', 'standard_shader', 1)]
        first = True
        for idx, opn in ops:
            if first:
                edits[idx] = [indent_of(lines[idx]) + 'shaderop\t\tDefaultPathTracing']
                first = False
            else:
                edits[idx] = []
    notes.append('every shader chain -> DefaultPathTracing (dropped ops: %s)' % ', '.join(dropped))
    if 'ambientocclusion_shaderop' in used:
        notes.append('ambientocclusion_shaderop dropped with no replacement (owner ruling #6)')

    # Delete the chunks that only fed the legacy chain: photon maps, gathers,
    # the irradiance cache, every op chunk (none is referenced any more), and
    # shader chunks nothing references after the ops that named them go.
    referenced_shaders = set(obj_shader(o) for o in objects) | {default_shader, 'global'}
    for c in chunks:
        kill = (c.kw.endswith(FULL_DELETED_KINDS) or c.kw == 'irradiance_cache'
                or (c.kw.endswith('_shaderop') and c.kw != 'arealight_shaderop')
                or (c.kw in ('standard_shader', 'advanced_shader') and c.name() not in referenced_shaders))
        if kill:
            for kk in range(c.kw_line, c.close_line + 1):
                edits[kk] = []
            # ...the comment lines directly above it that describe it (a
            # banner line of only `#` ends the block) ...
            kk = c.kw_line - 1
            while kk >= 0 and kk not in edits and lines[kk].strip().startswith('#') \
                    and not BANNER_RE.match(lines[kk]) and code[kk].strip() == '':
                edits[kk] = []
                kk -= 1
            # ...and the blank line that separated it from the next chunk.
            nxt = c.close_line + 1
            if nxt < len(lines) and lines[nxt].strip() == '' and nxt not in edits:
                edits[nxt] = []
    # Commented-out shaderop lines inside the rewritten shader chains are stale.
    for c, _ops in chains:
        for kk in range(c.kw_line + 1, c.close_line):
            if kk not in edits and re.match(r'^\s*#\s*shaderop\b', lines[kk]):
                edits[kk] = []
    # A banner section (#### / # Title / ####) whose every chunk was deleted.
    headers = []    # (first line, first line after the header)
    i = 0
    while i < len(lines):
        if BANNER_RE.match(lines[i]):
            j = i + 1
            while j < len(lines) and lines[j].strip().startswith('#') and not BANNER_RE.match(lines[j]):
                j += 1
            if j > i + 1 and j < len(lines) and BANNER_RE.match(lines[j]):
                headers.append((i, j + 1))
                i = j + 1
                continue
        i += 1
    for bi, (b0, b1) in enumerate(headers):
        end = headers[bi + 1][0] if bi + 1 < len(headers) else len(lines)
        body = [k for k in range(b1, end) if lines[k].strip() != '']
        if body and all(k in edits and edits[k] == [] for k in body):
            for k in range(b0, end):
                edits[k] = []
    if has_pm or has_ic:
        notes.append('photon map / gather / irradiance-cache chunks removed (frozen, owner ruling #4)')

    out = []
    for i, line in enumerate(lines):
        if i in insert_before:
            out.extend(insert_before[i])
        out.extend(edits[i] if i in edits else [line])
    report.extend(notes)
    return '\n'.join(out), 'MIGRATED %s -> %s' % (r.kw, target)


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


def run(mode, paths, dry_run, verbose, integrator='pt', sss_albedo=None, sss_keep=False, sms=False,
        vcm_drop_lights=False):
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
            elif mode == 'full':
                new, status = migrate_full(text, integrator, notes, sss_albedo, sss_keep, sms,
                                           vcm_drop_lights)
            else:
                new, status = migrate_rasterizer(text, mode, notes)
        except ParseError as e:
            print('ERROR     %s: %s' % (rel, e))
            rc = 1
            continue
        held = HELD.get(rel)
        flag = rasterizer_flag(rel, text)
        if flag and not (rel.startswith(LEGACY_CORPUS) or rel.startswith(COVERAGE_CORPUS)):
            print(flag)
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
    # SMS: the op's own defaults are written (chain depth 10, not the
    # rasterizer's 30), and disagreeing ops are refused.
    sm = ('pathtracing_shaderop\n{\n\tname a\n\tsms_enabled TRUE\n}\n'
          'standard_shader\n{\n\tname global\n\tshaderop a\n}\npixelpel_rasterizer\n{\n}\n')
    new, st = migrate_rasterizer(sm, 'pt', [])
    assert 'sms_max_chain_depth\t\t10' in new and new.count('sms_enabled') == 2, new
    assert 'sms_max_iterations\t\t20' in new and 'sms_biased\t\tTRUE' in new, new
    sm2 = sm + 'standard_shader\n{\n\tname other\n\tshaderop DefaultPathTracing\n}\n'
    assert 'different SMS settings' in migrate_rasterizer(sm2, 'pt', [])[1]
    # --mode full: Whitted chain -> PT direct; photon map -> chunks deleted.
    wh = ('standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n\tshaderop DefaultReflection\n}\n'
          'pixelpel_rasterizer\n{\n\tsamples 4\n\tlum_samples 9\n}\n')
    new, st = migrate_full(wh, 'pt', [])
    assert 'max_diffuse_bounce\t\t0' in new and 'samples\t\t\t\t16' in new, new  # lum_samples inert
    assert 'DefaultReflection' not in new and new.count('DefaultPathTracing') == 1, new
    assert migrate_full(wh, 'vcm', [])[0] is None
    pm = ('standard_shader\n{\n\tname global\n\tshaderop DefaultCausticPelPhotonMap\n}\n'
          'pixelpel_rasterizer\n{\n}\ncaustic_pel_photonmap\n{\n\tnum 10\n}\n'
          'caustic_pel_gather\n{\n\tmax_photons 5\n}\n')
    new, st = migrate_full(pm, 'vcm', [])
    assert 'vcm_pel_rasterizer' in new and 'photonmap' not in new.split('Migrated')[1].replace('Migrated', ''), new
    assert '_gather' not in new and 'samples\t\t\t\t64' in new and 'max_diffuse_bounce' not in new, new
    # arealight_shaderop -> rect_light, exitance = power x |dir|.
    al = ('uniformcolor_painter\n{\n\tname e\n\tcolor 1 1 1\n}\narealight_shaderop\n{\n\tname l\n\temission e\n'
          '\tlocation 0 4 0\n\tmake_dir 0 0 0\n\tpower 2\n}\nstandard_shader\n{\n\tname global\n\tshaderop l\n}\n'
          'pixelpel_rasterizer\n{\n}\n')
    new, st = migrate_full(al, 'pt', [])
    assert 'rect_light' in new and 'exitance\t2' in new and 'facing\t\t0 -4 0' in new, new
    assert 'arealight_shaderop\n' not in new, new
    # diffusion SSS op -> subsurfacescattering_material on its object.
    ss = ('lambertian_material\n{\n\tname m\n}\ndiffusion_approximation_sss_shaderop\n{\n\tname d\n'
          '\tgeometric_scale 0.1\n\tscattering 1 2 3\n\tabsorption 0.1 0.2 0.3\n}\n'
          'standard_shader\n{\n\tname s\n\tshaderop d\n}\nstandard_shader\n{\n\tname global\n'
          '\tshaderop DefaultDirectLighting\n}\npixelpel_rasterizer\n{\n}\n'
          'standard_object\n{\n\tname o\n\tmaterial m\n\tshader s\n}\n')
    new, st = migrate_full(ss, 'pt', [])
    assert 'subsurfacescattering_material' in new and 'scattering\t100 200 300' in new, new
    assert 'material d_sss_m' in new and 'absorption\t10 20 30' in new, new
    # DT with `diffuse false` + direct lighting is direct + glossy: bounce 0,
    # and its per-op samples set the spp (4 x 32).
    dt = ('distributiontracing_shaderop\n{\n\tname dt\n\tsamples 32\n\tdiffuse false\n}\n'
          'standard_shader\n{\n\tname global\n\tshaderop dt\n\tshaderop DefaultDirectLighting\n}\n'
          'pixelpel_rasterizer\n{\n\tsamples 4\n}\n')
    new, st = migrate_full(dt, 'pt', [])
    assert 'max_diffuse_bounce\t\t0' in new and 'samples\t\t\t\t128' in new, new
    new, st = migrate_full(dt.replace('\tdiffuse false\n', ''), 'pt', [])
    assert 'max_diffuse_bounce' not in new, new
    # VCM refuses a scene whose ambient / directional light it would drop.
    amb = pm + 'ambient_light\n{\n\tname a\n\tpower 1\n}\n'
    assert 'REFUSED' in migrate_full(amb, 'vcm', [])[1]
    notes = []
    assert migrate_full(amb, 'vcm', notes, vcm_drop_lights=True)[0] is not None
    assert any('LIGHT DROPPED' in n for n in notes), notes
    # simple SSS tint sources.  `bsdf none` is a NULL override: the op shades
    # with the object's own BSDF, so the base colour tints (rule (a)).
    base_ = ('uniformcolor_painter\n{\n\tname g\n\tcolor 0.1 0.8 0.1\n}\n'
             'uniformcolor_painter\n{\n\tname w\n\tcolor 0.7 0.7 0.7\n}\n'
             'lambertian_material\n{\n\tname m\n\treflectance g\n}\n'
             'lambertian_material\n{\n\tname wm\n\treflectance w\n}\n'
             '@OPS@'
             'standard_shader\n{\n\tname irr\n@IRR@}\n'
             'simple_sss_shaderop\n{\n\tname s\n\textinction 0.01 0.02 0.04\n\tshader irr\n@MB@}\n'
             'standard_shader\n{\n\tname global\n\tshaderop s\n}\npixelpel_rasterizer\n{\n}\n'
             'standard_object\n{\n\tname o\n\tmaterial m\n}\n')
    def sss_case(ops, irr, mb=''):
        return migrate_full(base_.replace('@OPS@', ops).replace('@IRR@', irr).replace('@MB@', mb), 'pt', [])
    dl_none = 'directlighting_shaderop\n{\n\tname dl\n\tbsdf none\n}\n'
    new, st = sss_case(dl_none, '\tshaderop dl\n')
    assert 'albedo 0.1 0.2 0.01 (colour material `m`' in new, new                 # bsdf none -> base
    new, st = sss_case('directlighting_shaderop\n{\n\tname dl\n\tbsdf wm\n}\n', '\tshaderop dl\n')
    assert "albedo 0.7 0.175 0.04375 (colour the irradiance shader's override" in new, new  # override colour
    new, st = sss_case('', '\tshaderop DefaultDirectLighting\n')
    assert '(colour material `m`' in new, new                                     # default DL -> base
    new, st = sss_case('finalgather_shaderop\n{\n\tname fg\n}\n', '\tshaderop fg\n')
    assert '(colour material `m`' in new, new                                     # FG uses the object BSDF
    new, st = sss_case('', '\tshaderop DefaultPathTracing\n')
    assert '(colour material `m`' in new, new                                     # PT uses the object BSDF
    new, st = sss_case('ambientocclusion_shaderop\n{\n\tname ao\n\tmultiplybrdf false\n}\n', '\tshaderop ao\n')
    assert 'albedo 0.95 0.2375 0.059375 (colour untinted' in new, new             # no tinting op
    new, st = sss_case('directlighting_shaderop\n{\n\tname dl\n\tbsdf wm\n}\n', '\tshaderop dl\n', '\tmultiplybsdf TRUE\n')
    assert '(colour material `m`' in new, new                                     # multiplybsdf -> base
    bad = base_.replace('@OPS@', 'checker_painter\n{\n\tname ck\n\tcolora g\n\tcolorb w\n}\n'
                        'lambertian_material\n{\n\tname cm\n\treflectance ck\n}\n'
                        'directlighting_shaderop\n{\n\tname dl\n\tbsdf cm\n}\n').replace('@IRR@', '\tshaderop dl\n').replace('@MB@', '')
    assert 'no literal uniform colour' in migrate_full(bad, 'pt', [])[1]          # non-literal override refused
    # Samples: a CACHED final gather and an op that only feeds an SSS
    # irradiance shader do not set the per-pixel spp.
    fg = ('finalgather_shaderop\n{\n\tname fg\n\tsamples 512\n}\n'
          'standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n\tshaderop fg\n}\n'
          'pixelpel_rasterizer\n{\n\tsamples 4\n}\n')
    assert 'samples\t\t\t\t64' in migrate_full(fg, 'pt', [])[0]
    assert 'samples\t\t\t\t1024' in migrate_full(fg.replace('samples 512', 'samples 512\n\tcache false'), 'pt', [])[0]
    new, st = sss_case('distributiontracing_shaderop\n{\n\tname dt\n\tsamples 500\n}\n', '\tshaderop dt\n')
    assert 'samples\t\t\t\t64' in new, new
    # Active rasterizer = the LAST declared one; commented-out frozen chunks are inert.
    two = ('bdpt_pel_rasterizer\n{\n}\nmlt_rasterizer\n{\n}\n/*\npixelpel_rasterizer\n{\n}\n*/\n')
    act, decl, com = rasterizer_audit(two)
    assert act == 'mlt_rasterizer' and decl == ['bdpt_pel_rasterizer', 'mlt_rasterizer'], (act, decl)
    assert com == ['pixelpel_rasterizer'], com
    fl = rasterizer_flag('x', two)
    assert 'ACTIVE one is the last declared, `mlt_rasterizer`' in fl and 'is FROZEN' in fl, fl
    assert rasterizer_flag('x', 'pathtracing_pel_rasterizer\n{\n}\n/*\nmlt_rasterizer\n{\n}\n*/\n') is None
    assert rasterizer_audit('/*\nmlt_rasterizer\n{\n}\n*/\nbdpt_pel_rasterizer\n{\n}\n')[2] == ['mlt_rasterizer']
    print('selftest: OK')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--mode', choices=('pt', 'direct', 'renames', 'full'))
    ap.add_argument('--integrator', choices=('pt', 'bdpt', 'vcm'), default='pt',
                    help='--mode full: the integrator to migrate to (default pt)')
    ap.add_argument('--sss-albedo', nargs=3, type=float, metavar=('R', 'G', 'B'),
                    help='--mode full: diffuse albedo for a simple_sss_shaderop whose object '
                         'material has no readable uniform base colour')
    ap.add_argument('--sms', action='store_true',
                    help='--mode full --integrator pt: also enable specular manifold sampling '
                         '(caustics from point / spot / area lights through smooth dielectrics)')
    ap.add_argument('--vcm-drop-unsupported-lights', action='store_true',
                    help='--mode full --integrator vcm: migrate even though VCM will not render the '
                         'scene\'s ambient_light / directional_light (reported as LIGHT DROPPED)')
    ap.add_argument('--sss-keep-material', action='store_true',
                    help='--mode full: keep (rather than convert) the material of a simple_sss_shaderop '
                         'object whose base colour is not a uniform painter; the op is dropped')
    ap.add_argument('--dry-run', action='store_true', help='report only, write nothing')
    ap.add_argument('-v', '--verbose', action='store_true', help='also list unchanged files')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--audit', action='store_true',
                    help='report scenes whose ACTIVE (last-declared) rasterizer is frozen, scenes with more '
                         'than one rasterizer chunk, and commented-out frozen rasterizers; writes nothing')
    ap.add_argument('paths', nargs='*', default=['scenes'])
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.audit:
        return audit(a.paths)
    if not a.mode:
        ap.error('--mode is required')
    return run(a.mode, a.paths, a.dry_run, a.verbose, a.integrator, a.sss_albedo, a.sss_keep_material, a.sms,
               a.vcm_drop_unsupported_lights)


if __name__ == '__main__':
    sys.exit(main())
