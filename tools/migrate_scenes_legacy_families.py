#!/usr/bin/env python3
"""
migrate_scenes_legacy_families.py -- move scenes off the remaining legacy
CHUNK families (legacy-deprecation Phase 2 slice B2; see
docs/LEGACY_DEPRECATION_ASSESSMENT.md sections 11 and 13 and
docs/SCENE_CONVENTIONS.md section 11.5).

Sibling of tools/migrate_scenes_legacy_rasterizer.py (which moves scenes off
the legacy pixel rasterizers); it reuses that tool's lexer and chunk parser.
Every family is look-changing (owner rulings #5 / #8 / #9: accepted, reviewed
per scene).  Pick families with --family (repeatable; `all` = materials,
iridescent, ambient, mlt, in that order, which is the order they compose in).

FAMILIES

  materials   The seven FROZEN legacy BRDF chunks, per the SCENE_CONVENTIONS
              11.5 table.  The material keeps its NAME (so every reference
              keeps working); a comment above it records the old chunk.
                cooktorrance_material  -> ggx_material (conductor):
                    rd, rs, ior, extinction (CT default 1 written when absent),
                    fresnel_mode carried; alphax = alphay = facets (same GGX D).
                isotropic_phong_material -> ggx_material (schlick_f0):
                    rd, rs (F0 tint); alpha = 1/sqrt(2N+1).  The Phong lobe is
                    cos^N about the REFLECTION vector, whose angle is twice
                    the half-vector's, so the half-vector exponent is 4N and
                    the Blinn-Phong/Beckmann rule alpha = sqrt(2/(n+2)) gives
                    sqrt(2/(4N+2)).
                ashikminshirley_anisotropicphong_material -> ggx_material
                    (schlick_f0): alphax = sqrt(2/(nu+2)), alphay =
                    sqrt(2/(nv+2)) (its exponents already act on the half
                    vector).
                schlick_material -> ggx_material (schlick_f0): alpha =
                    sqrt(roughness) (Schlick's Z(t) is the GGX D with r =
                    alpha^2).  isotropy p < 1 has no exact translation.
                    Schlick's azimuthal factor A(w) is a function of w =
                    onb.v . h_t (SchlickBRDF.cpp): 1/sqrt(p) at w = 0 (half
                    vectors along onb.u, the tangent) down to sqrt(p) at
                    w = 1, so the facets spread along onb.u.  The tool keeps
                    the geometric-mean width and stretches the GGX lobe
                    along u: alphax = sqrt(r) p^(-1/4), alphay = sqrt(r)
                    p^(1/4) (each clamped to 1) -- a heuristic, reported
                    (rendered at p 0.1: Schlick's highlight x/y spread 1.67,
                    this mapping 1.60).
                ward_isotropic_material -> ggx_material (schlick_f0):
                    alpha -> alphax = alphay.
                ward_anisotropic_material -> ggx_material (schlick_f0):
                    alphax / alphay carried.
                polished_material -> coated_material over a
                    lambertian_material `<name>_base` with the same
                    reflectance; coat_ior = ior; coat_roughness = 1/sqrt(2 scattering + 1)
                    (cos^N about the reflection vector, the Phong rule), 1e-3
                    for the delta case (scattering >= 1e6);
                    coat_weight = tau (an `r g b` tau -> its Rec.709
                    luminance, reported: tau multiplies only the coat
                    reflection and has no exact equivalent); tau 0 (no coat)
                    -> a plain lambertian_material.  An ior above
                    coated_material's range (3) makes the coat a near-total
                    reflector (ior 10000: F0 0.9996), so that case becomes
                    ggx_material schlick_f0 with F0 = tau ((n-1)/(n+1))^2,
                    rd = reflectance and alpha = the coat roughness.  A scattering bound to a
                    single-expression `scalar_painter` (the wetness scenes)
                    is rewritten in place to the same 1/sqrt(2n+1) of its
                    expression, provided nothing else binds that painter.
                Per-channel (`r g b`) roughness is REFUSED unless
                --collapse-per-channel is given: the legacy models honoured
                all three channels, but ggx_material reads only the FIRST
                channel of alphax / alphay under the RGB rasterizers
                (GGXBRDF.cpp / GGXSPF.cpp) while the spectral path
                interpolates them per wavelength, so a per-channel alpha
                would silently collapse.  With the flag the converted
                alphas are replaced by their Rec.709-luminance-weighted
                mean, reported and noted in the scene.  Any other
                painter-bound scalar is REFUSED.

  iridescent  `iridescent_painter` (deprecated: a view-angle colour lerp,
              "a cheap look-alike only") -> the physical thin-film model,
              ggx_material fresnel_mode thinfilm with a TiO2 film, chosen by
              what the painter is bound to:
                perfectreflector_material.reflectance -> a near-mirror
                    (alpha 0.01) thin film over TITANIUM (anodized metal,
                    the colors/thinfilm data);
                lambertian_material.reflectance -> a PEARLESCENT ggx
                    (TiO2 film over a 1.5 dielectric, i.e. TiO2-coated mica,
                    alpha 0.3, a soft sheen) whose diffuse rd is the
                    painter's projected-area mean colour (see below);
                translucent_material.ref / .tau, and the diffuse rd of a
                    ggx_material (a polished mirror coat converted above)
                    -> the painter's mean colour (the angular colour shift
                    is lost there, reported).
              The film thickness is the one whose NORMAL-incidence RGB
              reflectance (ThinFilm::ReflectanceConductorRGBSpectral, the
              renderer's own RGB thin-film integral; table below, 30-300
              nm) has the chromaticity closest to the painter's own
              normal-incidence colour lerp(colora, colorb, clamp(1 + bias,
              0, 1)) (colora when colorb is not a uniform colour).  Thin-film interference shifts the
              hue toward thinner-film colours at grazing incidence, which
              is what the painter faked with colora.
              The mean colour: the painter returns lerp(colora, colorb, t),
              t = clamp(|cos| + bias, 0, 1); over a convex object seen by
              the camera |cos| has density 2 mu d mu, so the projected-area
              mean weight is w = integral_0^1 clamp(mu + bias, 0, 1) 2 mu
              d mu, and the mean is (1-w) colora + w colorb (a
              uniformcolor_painter for uniform colours, else a
              blend_painter with a constant mask).

  ambient     `ambient_light` (deprecated, owner ruling #5) -> a uniform
              environment on the rasterizer.  The ambient light adds
              color x power x f(n, wo) at every shading point (no cosine, no
              pi, no shadow); a uniform environment of radiance L gives an
              UNOCCLUDED Lambertian surface the radiance rho L, which is
              what the ambient gives it when L = color x power / pi.  So:
              a `uniformcolor_painter` `pnt_ambient_env` with the light's
              colour (and colorspace), `radiance_map pnt_ambient_env`,
              `radiance_scale power/pi`, `radiance_background FALSE` (the
              ambient light was never visible to the camera).  Several
              ambient lights sum.  When the rasterizer already has a
              radiance_map that is a uniformcolor_painter, the rasterizer is
              pointed at a NEW painter `<sky>_with_ambient` holding the sky
              plus the ambient term (in linear Rec.709, divided by the
              existing radiance_scale) -- the sky painter itself may be
              shared, e.g. by a luminaire's exitance; any other existing
              map is REFUSED, and so is a scene whose `hosek_wilkie_skylight`
              already installs the environment unless --ambient-with-sky
              drop is given, which deletes the ambient fill (the sky is
              already its occluded, physical version).  The environment is occluded, so creases and
              interiors get darker (accepted by the ruling).  VCM renders
              no ambient_light but does render a radiance_map, so a VCM
              scene GAINS the ambient term.

  mlt         `mlt_rasterizer` / `mlt_spectral_rasterizer` (frozen) ->
              `bdpt_{pel,spectral}_rasterizer` (default) or
              `vcm_{pel,spectral}_rasterizer` (--mlt-integrator vcm; for
              caustic scenes, per the integrator map).  max_eye_depth /
              max_light_depth and every parameter the target accepts are
              carried; `mutations_per_pixel` becomes `samples` (one
              mutation evaluates one bidirectional path sample per pixel);
              `bootstrap_samples`, `chains` and `large_step_prob` are
              dropped.  Any other parameter the target does not accept is
              REFUSED.

scenes/Benchmarks/sea_creatures (no .RISEscene extension: a recorded
agent-benchmark run's output with two iridescent_painters) is HELD as written.

Kept by owner ruling (KEEP below): FeatureBased/Animation/translucent_bunny keeps
its iridescent_painter (2026-10-10).

Never touched: scenes/Tests/Legacy/ (the frozen-feature regression corpus),
and scenes/Tests/ChunkCoverage/.  Idempotent: a migrated scene has
none of the family's chunks and is reported `unchanged`.  Structure is read
from a comment-stripped view; the bytes written back are the originals,
edited chunk by chunk.

Usage:
    python3 tools/migrate_scenes_legacy_families.py --family materials|iridescent|ambient|mlt|all
                                    [--mlt-integrator bdpt|vcm] [--ambient-with-sky drop]
                                    [--collapse-per-channel]
                                    [--dry-run] [-v] [paths...]
    python3 tools/migrate_scenes_legacy_families.py --selftest
Paths default to `scenes/`.  Exit status is 0 unless a file could not be read
or parsed.
"""

import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from migrate_scenes_legacy_rasterizer import (  # noqa: E402
    strip_comments, parse_chunks, ParseError, is_literal_number, indent_of, fmt_num,
    collect, repo_rel, LEGACY_CORPUS, COVERAGE_CORPUS, TARGET_PARAMS, rasterizer_flag)

LEGACY_MATERIALS = (
    'cooktorrance_material', 'isotropic_phong_material',
    'ashikminshirley_anisotropicphong_material', 'schlick_material',
    'ward_isotropic_material', 'ward_anisotropic_material', 'polished_material')

# Scenes deliberately left as written.  (The four slice-C scenes -- the Android
# catalogue and the quickstart sample -- were held here until slice C migrated
# them, 2026-10-10.)  Repo-relative.
HELD = {
    # No .RISEscene extension, so directory walks never collect it; HELD when
    # passed explicitly.  It is a recorded agent-benchmark run's output (cited
    # in AgentSession.cpp), kept as the agent wrote it, iridescent_painters
    # included.
    'scenes/Benchmarks/sea_creatures': 'recorded agent-benchmark output (no .RISEscene extension), kept as written',
}

# Families a scene deliberately keeps (owner rulings).  Repo-relative path ->
# {family: reason}; the other families still migrate.
KEEP = {
    'scenes/FeatureBased/Animation/translucent_bunny.RISEscene': {
        'iridescent': 'owner ruling 2026-10-10: keeps the deprecated iridescent_painter '
                      '(translucent_material has no thin-film model)'},
}

# Per-scene film thickness overrides (owner rulings), repo-relative path ->
# {iridescent painter name: nm}.  The hue rule matches the painter's
# normal-incidence colour; on the two teapots (bias -0.5) most of the visible
# surface showed the purple end instead, and 125 nm (magenta-purple titanium
# anodizing, _TF_TI row 125) keeps that look.
FILM_OVERRIDE = {
    'scenes/FeatureBased/Geometry/teapot.RISEscene': {'pnt_iri': 125},
    'scenes/Tests/Geometry/teapot_analytic.RISEscene': {'pnt_iri': 125},
}

POLISHED_DELTA = 1e6          # kPhongDeltaThreshold
COAT_ROUGHNESS_FLOOR = 1e-3   # coated_material's clamp
COAT_IOR_MAX = 3.0            # coated_material's clamp
IRID_MIRROR_ALPHA = 0.01
IRID_PEARL_ALPHA = 0.3
IRID_PEARL_IOR = 1.5

# Normal-incidence RGB reflectance (linear Rec.709) of a TiO2 film
# (colors/thinfilm/oxides/TiO2.{n,k}) over titanium (substrates/Ti.{n,k}),
# and over a constant 1.5 dielectric, vs film thickness in nm; computed with
# ThinFilm::ReflectanceConductorRGBSpectral( 1, d, stack ) -- the same RGB
# integral GGXBRDF uses under the RGB rasterizers -- on 2026-10-10.
_TF_TI = (
    (30, 0.10933, 0.02402, 0.27847),
    (35, 0.00005, 0.06069, 0.45223),
    (40, 0.00000, 0.15904, 0.48202),
    (45, 0.00000, 0.28398, 0.64738),
    (50, 0.07923, 0.39923, 0.71051),
    (55, 0.19015, 0.49139, 0.73350),
    (60, 0.29873, 0.56088, 0.73832),
    (65, 0.39346, 0.61100, 0.72688),
    (70, 0.47129, 0.64568, 0.69756),
    (75, 0.53321, 0.66822, 0.64510),
    (80, 0.58168, 0.68094, 0.56120),
    (85, 0.61995, 0.68453, 0.44014),
    (90, 0.65257, 0.67623, 0.29571),
    (95, 0.68517, 0.64909, 0.17358),
    (100, 0.72045, 0.59639, 0.12246),
    (105, 0.75437, 0.51729, 0.15570),
    (110, 0.77692, 0.41752, 0.25329),
    (115, 0.77506, 0.30848, 0.38082),
    (120, 0.73535, 0.20629, 0.50682),
    (125, 0.64792, 0.12939, 0.61203),
    (130, 0.51234, 0.09330, 0.68855),
    (135, 0.34223, 0.10480, 0.73509),
    (140, 0.16389, 0.15969, 0.75290),
    (145, 0.00916, 0.24521, 0.74310),
    (150, 0.00000, 0.32781, 0.59649),
    (155, 0.00000, 0.41313, 0.55219),
    (160, 0.00000, 0.50280, 0.51296),
    (165, 0.00000, 0.59162, 0.43902),
    (170, 0.07818, 0.64851, 0.34329),
    (175, 0.20231, 0.67367, 0.28047),
    (180, 0.32793, 0.67808, 0.25557),
    (185, 0.44627, 0.66420, 0.26674),
    (190, 0.55252, 0.63434, 0.30908),
    (195, 0.64424, 0.59087, 0.37371),
    (200, 0.72008, 0.53634, 0.44858),
    (205, 0.77867, 0.47378, 0.52198),
    (210, 0.81772, 0.40717, 0.58485),
    (215, 0.83358, 0.34203, 0.63102),
    (220, 0.82199, 0.28516, 0.65626),
    (225, 0.77975, 0.24353, 0.65725),
    (230, 0.70659, 0.22240, 0.63249),
    (235, 0.60619, 0.22394, 0.58551),
    (240, 0.48627, 0.24671, 0.52769),
    (245, 0.35750, 0.28666, 0.47265),
    (250, 0.23205, 0.33852, 0.42962),
    (255, 0.12256, 0.39622, 0.40316),
    (260, 0.04007, 0.45417, 0.38897),
    (265, 0.00000, 0.50433, 0.38977),
    (270, 0.00000, 0.54396, 0.40447),
    (275, 0.01697, 0.57677, 0.43054),
    (280, 0.08091, 0.59086, 0.46503),
    (285, 0.16862, 0.59107, 0.50123),
    (290, 0.26993, 0.57886, 0.53323),
    (295, 0.37498, 0.55640, 0.55497),
    (300, 0.47526, 0.52619, 0.56244),
)
_TF_DIEL = (
    (30, 0.23686, 0.32111, 0.43790),
    (35, 0.27977, 0.36277, 0.46387),
    (40, 0.31762, 0.39351, 0.47012),
    (45, 0.34941, 0.41330, 0.45698),
    (50, 0.37479, 0.42239, 0.42392),
    (55, 0.39382, 0.42089, 0.37010),
    (60, 0.40675, 0.40861, 0.29624),
    (65, 0.41398, 0.38478, 0.20828),
    (70, 0.41570, 0.34815, 0.12149),
    (75, 0.41126, 0.29795, 0.05947),
    (80, 0.39806, 0.23600, 0.04314),
    (85, 0.37151, 0.16863, 0.07743),
    (90, 0.32705, 0.10617, 0.14949),
    (95, 0.26345, 0.05996, 0.23816),
    (100, 0.18521, 0.03862, 0.32426),
    (105, 0.10254, 0.04532, 0.39533),
    (110, 0.02872, 0.07730, 0.44542),
    (115, 0.00000, 0.12630, 0.41380),
    (120, 0.00000, 0.18040, 0.40230),
    (125, 0.00000, 0.24028, 0.41418),
    (130, 0.00000, 0.30258, 0.41254),
    (135, 0.03630, 0.35426, 0.36301),
    (140, 0.09711, 0.39224, 0.29037),
    (145, 0.16362, 0.41761, 0.21378),
    (150, 0.23173, 0.42879, 0.14767),
    (155, 0.29850, 0.42478, 0.10547),
    (160, 0.36130, 0.40576, 0.09439),
    (165, 0.41727, 0.37320, 0.11406),
    (170, 0.46324, 0.32976, 0.15821),
    (175, 0.49591, 0.27915, 0.21737),
    (180, 0.51200, 0.22592, 0.28127),
    (185, 0.50867, 0.17524, 0.34079),
    (190, 0.48401, 0.13243, 0.38885),
    (195, 0.43777, 0.10232, 0.42050),
    (200, 0.37204, 0.08841, 0.43265),
    (205, 0.29152, 0.09222, 0.42406),
    (210, 0.20339, 0.11297, 0.39584),
    (215, 0.11642, 0.14760, 0.35229),
    (220, 0.03988, 0.19135, 0.30107),
    (225, 0.00000, 0.23385, 0.24566),
    (230, 0.00000, 0.26876, 0.21110),
    (235, 0.00000, 0.30488, 0.19628),
    (240, 0.00000, 0.34030, 0.18914),
    (245, 0.00458, 0.37110, 0.18663),
    (250, 0.06397, 0.37809, 0.20738),
    (255, 0.13384, 0.37405, 0.23746),
    (260, 0.20771, 0.36026, 0.27191),
    (265, 0.27973, 0.33856, 0.30542),
    (270, 0.34495, 0.31114, 0.33304),
    (275, 0.39937, 0.28050, 0.35081),
    (280, 0.43988, 0.24931, 0.35638),
    (285, 0.46435, 0.22022, 0.34955),
    (290, 0.47171, 0.19554, 0.33262),
    (295, 0.46206, 0.17703, 0.31007),
    (300, 0.43661, 0.16579, 0.28693),
)
THINFILM_DATA = (
    ('tf_Ti_n', 'colors/thinfilm/substrates/Ti.n'),
    ('tf_Ti_k', 'colors/thinfilm/substrates/Ti.k'),
    ('tf_TiO2_n', 'colors/thinfilm/oxides/TiO2.n'),
    ('tf_TiO2_k', 'colors/thinfilm/oxides/TiO2.k'),
)

# MLT parameters with no bidirectional equivalent (dropped, reported).
# Chunks that install the global environment themselves (a rasterizer
# radiance_map would replace it).
ENV_CHUNKS = {'hosek_wilkie_skylight'}

MLT_DROPPED = {'bootstrap_samples', 'chains', 'large_step_prob'}
MLT_TARGETS = {
    'bdpt': {'mlt_rasterizer': 'bdpt_pel_rasterizer', 'mlt_spectral_rasterizer': 'bdpt_spectral_rasterizer'},
    'vcm': {'mlt_rasterizer': 'vcm_pel_rasterizer', 'mlt_spectral_rasterizer': 'vcm_spectral_rasterizer'},
}

STAMP = 'Legacy phase 2 slice B2'


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------

def literal_list(toks):
    """[floats] when every token is a number (1 or 3 of them), else None."""
    if not toks or len(toks) not in (1, 3) or not all(is_literal_number(t) for t in toks):
        return None
    return [float(t) for t in toks]


def fmt_list(vals, digits=6, collapse=True):
    out = []
    for v in vals:
        s = ('%.' + str(digits) + 'g') % v
        out.append(s)
    if collapse and len(out) == 3 and out[0] == out[1] == out[2]:
        return out[0]
    return ' '.join(out)


def srgb_decode(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def painter_linear_rgb(chunk):
    """Linear Rec.709 colour of a uniformcolor_painter (None if not literal or
    in an unsupported colour space)."""
    col = chunk.get('color')
    vals = literal_list(col) if col else None
    if vals is None:
        return None
    if len(vals) == 1:
        vals = vals * 3
    cs = (chunk.get('colorspace') or ['Rec709RGB_Linear'])[0]
    if cs == 'sRGB':
        return [srgb_decode(v) for v in vals]
    if cs in ('Rec709RGB_Linear', 'RISERGB'):
        return vals
    return None


def light_linear_rgb(chunk):
    col = chunk.get('color')
    vals = literal_list(col) if col else [0.0, 0.0, 0.0]
    if vals is None:
        return None
    if len(vals) == 1:
        vals = vals * 3
    cs = (chunk.get('colorspace') or ['Rec709RGB_Linear'])[0]
    if cs == 'sRGB':
        return [srgb_decode(v) for v in vals]
    if cs == 'Rec709RGB_Linear':
        return vals
    return None


def luminance(v):
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]


class Editor(object):
    """Line edits on the original text: replace a chunk (kw line .. close
    line) by new lines, insert lines before a line index."""

    def __init__(self, text):
        self.lines = text.split('\n')
        self.code = strip_comments(self.lines)
        self.chunks = parse_chunks(self.code)
        self.repl = {}     # start -> (end, [lines])
        self.ins = {}      # index -> [lines]

    def named(self):
        return dict((c.name(), c) for c in self.chunks if c.name())

    def replace(self, chunk, new_lines):
        self.repl[chunk.kw_line] = (chunk.close_line, new_lines)

    def delete(self, chunk):
        """Delete a chunk with the comment lines directly above it and one
        trailing blank line."""
        start = chunk.kw_line
        while start > 0 and self.lines[start - 1].strip().startswith('#') and \
                not self.lines[start - 1].strip().startswith('#####'):
            start -= 1
        end = chunk.close_line
        if end + 1 < len(self.lines) and self.lines[end + 1].strip() == '':
            end += 1
        self.repl[start] = (end, [])

    def insert_before(self, idx, new_lines):
        # Above the comment block that documents the chunk at idx.
        while idx > 0 and self.lines[idx - 1].strip().startswith('#') and \
                not self.lines[idx - 1].strip().startswith('#####'):
            idx -= 1
        self.ins.setdefault(idx, []).extend(new_lines)

    def text(self):
        out = []
        i = 0
        n = len(self.lines)
        while i < n:
            if i in self.ins:
                out.extend(self.ins[i])
            if i in self.repl:
                end, nl = self.repl[i]
                out.extend(nl)
                i = end + 1
                continue
            out.append(self.lines[i])
            i += 1
        return '\n'.join(out)


def chunk_lines(kw, params, comment=None, ind='\t'):
    out = []
    if comment:
        for c in comment:
            out.append('# ' + c)
    out.append(kw)
    out.append('{')
    for k, v in params:
        pad = "\t" * max(1, (20 - len(k) + 3) // 4)
        out.append('%s%s%s%s' % (ind, k, pad, v))
    out.append('}')
    return out


def body_indent(ed, chunk):
    for idx, _ in chunk.params:
        return indent_of(ed.lines[idx]) or '\t'
    return '\t'


def ensure_painter(ed, created, name, rgb, before_line, ind, note=None):
    """Add a linear uniformcolor_painter `name` once."""
    if name in created or name in ed.named():
        return name
    created.add(name)
    ed.insert_before(before_line, chunk_lines('uniformcolor_painter',
                                              [('name', name), ('color', fmt_list(rgb, 5, False)),
                                               ('colorspace', 'Rec709RGB_Linear')],
                                              [note] if note else None, ind) + [''])
    return name


def unique_name(ed, created, base):
    names = set(ed.named()) | created
    n, k = base, 2
    while n in names:
        n = '%s_%d' % (base, k)
        k += 1
    return n


# --------------------------------------------------------------------------
# Family: legacy materials
# --------------------------------------------------------------------------

def _scal(c, key, default):
    toks = c.get(key)
    if toks is None:
        toks = [default]
    return literal_list(toks), toks


def _alpha_from(vals, fn):
    return [min(1.0, max(1e-4, fn(v))) for v in vals]


def _per_channel(name, ax, ay, collapse, report):
    """None when both alphas are single-valued; a refusal string, or (ax, ay,
    description) collapsed to the Rec.709-luminance-weighted mean."""
    def multi(v):
        return len(v) == 3 and len(set(v)) > 1
    if not (multi(ax) or multi(ay)):
        return None
    if not collapse:
        return ('REFUSED: `%s` has per-channel roughness (alphax %s, alphay %s); ggx_material reads only the '
                'first channel under RGB rasterizers, so it would collapse (--collapse-per-channel takes '
                'the luminance-weighted mean)' % (name, fmt_list(ax, 4), fmt_list(ay, 4)))
    cx = [luminance(ax) if len(ax) == 3 else ax[0]]
    cy = [luminance(ay) if len(ay) == 3 else ay[0]]
    d = ('PER-CHANNEL ROUGHNESS COLLAPSED: alphax %s alphay %s -> %s / %s (Rec.709-luminance-weighted mean; '
         'ggx_material reads only the first channel under RGB)' % (fmt_list(ax, 4), fmt_list(ay, 4),
                                                                   fmt_list(cx, 4), fmt_list(cy, 4)))
    report.append('WARNING `%s`: %s' % (name, d))
    return cx, cy, d


def migrate_materials(text, report, collapse=False):
    ed = Editor(text)
    mats = [c for c in ed.chunks if c.kw in LEGACY_MATERIALS]
    if not mats:
        return None, 'unchanged (no legacy material)'
    named = ed.named()
    created = set()
    done = []
    for c in mats:
        name = c.name() or 'noname'
        ind = body_indent(ed, c)
        p = []
        extra_before = []
        cmt = []
        kw = c.kw

        def carry(key, default='none'):
            v = c.get(key)
            return ' '.join(v) if v else default

        if kw in ('cooktorrance_material', 'isotropic_phong_material', 'ashikminshirley_anisotropicphong_material',
                  'schlick_material', 'ward_isotropic_material', 'ward_anisotropic_material'):
            p.append(('name', name))
            p.append(('rd', carry('rd')))
            p.append(('rs', carry('rs')))
            if kw == 'cooktorrance_material':
                fv, ft = _scal(c, 'facets', '0.15')
                if fv is None:
                    return None, 'REFUSED: `%s` facets `%s` is not a literal' % (name, ' '.join(ft))
                ax = ay = fv
                pc = _per_channel(name, ax, ay, collapse, report)
                if isinstance(pc, str):
                    return None, pc
                cdesc = ''
                if pc:
                    ax, ay, cdesc = pc
                p += [('alphax', fmt_list(ax)), ('alphay', fmt_list(ay))]
                p.append(('fresnel_mode', carry('fresnel_mode', 'conductor')))
                p.append(('ior', carry('ior', '2.45')))
                p.append(('extinction', carry('extinction', '1')))
                desc = 'cooktorrance_material -> ggx_material conductor, alpha = facets %s' % fmt_list(fv)
                if cdesc:
                    desc += '; ' + cdesc
            else:
                if kw == 'isotropic_phong_material':
                    nv, nt = _scal(c, 'N', '16.0')
                    if nv is None:
                        return None, 'REFUSED: `%s` N `%s` is not a literal' % (name, ' '.join(nt))
                    ax = ay = _alpha_from(nv, lambda n: 1.0 / math.sqrt(2.0 * n + 1.0))
                    desc = 'isotropic_phong N %s -> alpha 1/sqrt(2N+1) = %s' % (fmt_list(nv), fmt_list(ax, 4))
                elif kw == 'ashikminshirley_anisotropicphong_material':
                    nu, ut = _scal(c, 'nu', '10.0')
                    nvv, vt = _scal(c, 'nv', '100.0')
                    if nu is None or nvv is None:
                        return None, 'REFUSED: `%s` nu/nv is not a literal' % name
                    ax = _alpha_from(nu, lambda n: math.sqrt(2.0 / (n + 2.0)))
                    ay = _alpha_from(nvv, lambda n: math.sqrt(2.0 / (n + 2.0)))
                    desc = 'ashikmin-shirley nu %s nv %s -> alphax %s alphay %s (sqrt(2/(n+2)))' % (
                        fmt_list(nu), fmt_list(nvv), fmt_list(ax, 4), fmt_list(ay, 4))
                elif kw == 'schlick_material':
                    rv, rt = _scal(c, 'roughness', '0.05')
                    iv, it = _scal(c, 'isotropy', '1')
                    if rv is None or iv is None:
                        return None, 'REFUSED: `%s` roughness/isotropy is not a literal' % name
                    if len(rv) == 1 and len(iv) == 3:
                        rv = rv * 3
                    if len(iv) == 1 and len(rv) == 3:
                        iv = iv * 3
                    # A(w) peaks at w = 0, half vectors along onb.u: stretch alphax.
                    ax = [min(1.0, math.sqrt(r) * (i ** -0.25)) for r, i in zip(rv, iv)]
                    ay = [min(1.0, math.sqrt(r) * (i ** 0.25)) for r, i in zip(rv, iv)]
                    desc = 'schlick roughness %s -> alpha sqrt(r) = %s' % (
                        fmt_list(rv), fmt_list([math.sqrt(r) for r in rv], 4))
                    if any(abs(i - 1.0) > 1e-9 for i in iv):
                        desc += '; isotropy %s -> alphax %s alphay %s (heuristic: geometric-mean width kept, aspect 1/sqrt(isotropy))' % (
                            fmt_list(iv), fmt_list(ax, 4), fmt_list(ay, 4))
                        report.append('`%s`: schlick isotropy %s has no exact GGX translation (heuristic aspect)' % (
                            name, fmt_list(iv)))
                elif kw == 'ward_isotropic_material':
                    av, at = _scal(c, 'alpha', '0.1')
                    if av is None:
                        return None, 'REFUSED: `%s` alpha `%s` is not a literal' % (name, ' '.join(at))
                    ax = ay = av
                    desc = 'ward_isotropic alpha %s -> alphax = alphay' % fmt_list(av)
                else:
                    axv, _ = _scal(c, 'alphax', '0.1')
                    ayv, _ = _scal(c, 'alphay', '0.2')
                    if axv is None or ayv is None:
                        return None, 'REFUSED: `%s` alphax/alphay is not a literal' % name
                    ax, ay = axv, ayv
                    desc = 'ward_anisotropic alphax %s alphay %s carried' % (fmt_list(ax), fmt_list(ay))
                pc = _per_channel(name, ax, ay, collapse, report)
                if isinstance(pc, str):
                    return None, pc
                if pc:
                    ax, ay, cdesc = pc
                    desc += '; ' + cdesc
                p += [('alphax', fmt_list(ax, 5)), ('alphay', fmt_list(ay, 5)),
                      ('fresnel_mode', 'schlick_f0')]
                desc = desc + ', rs -> F0 (schlick_f0)'
            for k in ('variant',):
                if c.get(k):
                    p.append((k, carry(k)))
            cmt = ['%s: was %s; %s (SCENE_CONVENTIONS 11.5).' % (STAMP, kw, desc)]
            ed.replace(c, chunk_lines('ggx_material', p, cmt, ind))
            done.append('%s `%s` -> ggx_material' % (kw, name))
            report.append('`%s`: %s' % (name, desc))
            continue

        # polished_material
        refl = carry('reflectance')
        tau_toks = c.get('tau') or ['0.0']
        ior_toks = c.get('ior') or ['1.0']
        sc_toks = c.get('scattering') or ['64']
        if c.get('henyey-greenstein') and c.get('henyey-greenstein')[0].upper() in ('TRUE', '1'):
            return None, 'REFUSED: `%s` uses henyey-greenstein, which coated_material has no equivalent for' % name
        tau = literal_list(tau_toks)
        if tau is not None and max(tau) <= 0.0:
            cmt = ['%s: was polished_material with tau 0 (no coat: only the substrate) -> lambertian_material.' % STAMP]
            ed.replace(c, chunk_lines('lambertian_material', [('name', name), ('reflectance', refl)], cmt, ind))
            done.append('polished_material `%s` (tau 0) -> lambertian_material' % name)
            report.append('`%s`: polished tau 0 -> lambertian_material' % name)
            continue
        if tau is None:
            if len(tau_toks) == 1 and tau_toks[0] in named and named[tau_toks[0]].kw == 'scalar_painter':
                weight = tau_toks[0]
                wdesc = 'coat_weight = tau painter `%s`' % weight
            else:
                return None, 'REFUSED: `%s` tau `%s` is neither a literal nor a scalar_painter' % (name, ' '.join(tau_toks))
        else:
            wv = luminance(tau) if len(tau) == 3 else tau[0]
            wv = min(1.0, max(0.0, wv))
            weight = fmt_num(round(wv, 4))
            wdesc = 'coat_weight = tau %s%s' % (fmt_list(tau), ' (Rec.709 luminance; the tint is dropped)' if len(tau) == 3 else '')
        iv = literal_list(ior_toks)
        if iv is None or len(iv) != 1:
            return None, 'REFUSED: `%s` ior `%s` is not a single literal' % (name, ' '.join(ior_toks))
        coat_ior = min(COAT_IOR_MAX, max(1.0, iv[0]))
        idesc = 'coat_ior %s' % fmt_num(coat_ior)
        if iv[0] < 1.0:
            idesc += ' (ior %s raised to coated_material\'s minimum 1)' % fmt_num(iv[0])
            report.append('`%s`: ior %s raised to 1 (coated_material range)' % (name, fmt_num(iv[0])))
        sv = literal_list(sc_toks)
        if sv is not None:
            if len(sv) != 1:
                return None, 'REFUSED: `%s` per-channel scattering (coat_roughness is single-valued)' % name
            s = sv[0]
            rough = COAT_ROUGHNESS_FLOOR if s >= POLISHED_DELTA else max(COAT_ROUGHNESS_FLOOR, 1.0 / math.sqrt(2.0 * s + 1.0))
            rough_s = '%.4g' % rough
            rdesc = 'coat_roughness 1/sqrt(2*%s+1) = %s' % (fmt_num(s), rough_s)
        elif len(sc_toks) == 1 and sc_toks[0] in named and named[sc_toks[0]].kw == 'scalar_painter' \
                and named[sc_toks[0]].get('expression'):
            sp = named[sc_toks[0]]
            users = [d for d in ed.chunks if d is not c and d is not sp and any(sc_toks[0] in t[1:] for _, t in d.params)]
            if users:
                return None, 'REFUSED: `%s` scattering painter `%s` is also bound by `%s`' % (
                    name, sc_toks[0], users[0].name() or users[0].kw)
            expr_idx = [idx for idx, t in sp.params if t[0] == 'expression'][0]
            line = ed.lines[expr_idx]
            pos = line.find('expression') + len('expression')
            rest = line[pos:]
            lead = rest[:len(rest) - len(rest.lstrip())]
            body = rest.strip()
            hash_pos = body.find('#')
            if hash_pos >= 0:
                return None, 'REFUSED: `%s` expression has a trailing comment' % sc_toks[0]
            ed.repl[expr_idx] = (expr_idx, [
                '%s# %s: this was polished_material `%s`\'s Phong scattering exponent n; it is now coated_material\'s' % (
                    indent_of(line), STAMP, name),
                '%s# coat_roughness, 1/sqrt(2n+1) of the same expression (its `param` labels still describe n).' % indent_of(line),
                line[:pos] + lead + '1.0 / sqrt(2.0 * (%s) + 1.0)' % body])
            rough_s = sc_toks[0]
            rdesc = 'coat_roughness = 1/sqrt(2n+1) of scalar_painter `%s` (rewritten in place)' % sc_toks[0]
        else:
            return None, 'REFUSED: `%s` scattering `%s` is neither a literal nor a single-expression scalar_painter' % (
                name, ' '.join(sc_toks))
        if iv[0] > COAT_IOR_MAX:
            # Outside coated_material's index range the coat is a near-total
            # reflector (ior 10000: F0 = 0.9996), so the faithful translation
            # is a Schlick-F0 GGX lobe of F0 = tau ((n-1)/(n+1))^2 over the
            # substrate colour (its diffuse is coupled by (1-A(i))(1-A(o)),
            # which, like the polished coat, hides the substrate as F0 -> 1).
            if tau is None or sv is None:
                return None, 'REFUSED: `%s` ior %s > 3 with painter-bound tau/scattering' % (name, fmt_num(iv[0]))
            f0 = ((iv[0] - 1.0) / (iv[0] + 1.0)) ** 2
            t3 = tau * 3 if len(tau) == 1 else tau
            f0c = [min(1.0, t * f0) for t in t3]
            pn = unique_name(ed, created, name + '_f0')
            ensure_painter(ed, created, pn, f0c, c.kw_line, ind,
                           '%s: F0 of `%s` = tau x ((ior-1)/(ior+1))^2.' % (STAMP, name))
            cmt = ['%s: was polished_material with ior %s, beyond coated_material\'s [1, 3] range: the coat is a' % (STAMP, fmt_num(iv[0])),
                   'near-total reflector, so -> ggx_material schlick_f0 with F0 = tau x ((n-1)/(n+1))^2 = %s, rd = reflectance,' % fmt_list(f0c, 4),
                   'alpha = %s (SCENE_CONVENTIONS 11.5 roughness rule).' % rdesc]
            ed.replace(c, chunk_lines('ggx_material', [('name', name), ('rd', refl), ('rs', pn), ('alphax', rough_s),
                                                       ('alphay', rough_s), ('fresnel_mode', 'schlick_f0')], cmt, ind))
            done.append('polished_material `%s` (ior %s) -> ggx_material' % (name, fmt_num(iv[0])))
            report.append('`%s`: polished ior %s > 3 -> ggx schlick_f0, F0 %s, %s' % (name, fmt_num(iv[0]), fmt_list(f0c, 4), rdesc))
            continue
        if tau is not None and len(tau) == 3 and len(set(tau)) > 1:
            report.append('`%s`: tau %s tints only the coat reflection; coat_weight %s is its luminance' % (
                name, fmt_list(tau), weight))
        base = unique_name(ed, created, name + '_base')
        created.add(base)
        cmt = ['%s: was polished_material (Fresnel dielectric coat over a Lambertian substrate) ->' % STAMP,
               'coated_material over `%s`; %s, %s, %s (SCENE_CONVENTIONS 11.5).' % (base, wdesc, idesc, rdesc)]
        newl = chunk_lines('lambertian_material', [('name', base), ('reflectance', refl)],
                           ['%s: substrate of `%s` (was polished_material reflectance).' % (STAMP, name)], ind)
        newl.append('')
        newl += chunk_lines('coated_material', [('name', name), ('base', base), ('coat_weight', weight),
                                                ('coat_ior', fmt_num(coat_ior)), ('coat_roughness', rough_s)], cmt, ind)
        ed.replace(c, newl)
        done.append('polished_material `%s` -> coated_material' % name)
        report.append('`%s`: polished -> coated over lambertian; %s, %s, %s' % (name, wdesc, idesc, rdesc))
    return ed.text(), 'MIGRATED (materials): ' + '; '.join(done)


# --------------------------------------------------------------------------
# Family: iridescent_painter
# --------------------------------------------------------------------------

def mean_weight(bias):
    """w = integral_0^1 clamp(mu + bias, 0, 1) 2 mu d mu (Simpson, 2000)."""
    n = 2000
    h = 1.0 / n
    s = 0.0
    for i in range(n + 1):
        mu = i * h
        f = min(1.0, max(0.0, mu + bias)) * 2.0 * mu
        s += f * (1 if i in (0, n) else (4 if i % 2 else 2))
    return s * h / 3.0


def match_thickness(rgb, table):
    def chrom(v):
        t = sum(v)
        return [x / t for x in v] if t > 1e-9 else [1 / 3.0] * 3
    tc = chrom(rgb)
    best = None
    for d, r, g, b in table:
        c = chrom((r, g, b))
        dist = sum((a - b2) ** 2 for a, b2 in zip(tc, c))
        if best is None or dist < best[0]:
            best = (dist, d)
    return best[1]


def migrate_iridescent(text, report, rel=None):
    ed = Editor(text)
    iris = [c for c in ed.chunks if c.kw == 'iridescent_painter']
    if not iris:
        return None, 'unchanged (no iridescent_painter)'
    named = ed.named()
    created = set()
    done = []
    need_data = []          # first line to insert the thin-film data painters before
    for P in iris:
        pname = P.name()
        ind = body_indent(ed, P)
        a_name = (P.get('colora') or ['none'])[0]
        b_name = (P.get('colorb') or ['none'])[0]
        bias = float((P.get('bias') or ['0'])[0])
        A = named.get(a_name)
        B = named.get(b_name)
        a_rgb = painter_linear_rgb(A) if A is not None and A.kw == 'uniformcolor_painter' else None
        b_rgb = painter_linear_rgb(B) if B is not None and B.kw == 'uniformcolor_painter' else None
        w = mean_weight(bias)
        users = []
        for d in ed.chunks:
            if d is P:
                continue
            for idx, t in d.params:
                if pname in t[1:]:
                    users.append((d, idx, t[0]))
        if not users:
            return None, 'REFUSED: iridescent_painter `%s` is unreferenced' % pname
        for d, _, slot in users:
            ok = (d.kw == 'perfectreflector_material' and slot == 'reflectance') or \
                 (d.kw == 'lambertian_material' and slot == 'reflectance') or \
                 (d.kw == 'translucent_material' and slot in ('ref', 'tau')) or \
                 (d.kw == 'ggx_material' and slot == 'rd')
            if not ok:
                return None, 'REFUSED: iridescent_painter `%s` is bound to %s.%s, which has no thin-film translation' % (
                    pname, d.kw, slot)
        # The painter's own NORMAL-incidence colour: t = clamp(1 + bias, 0, 1).
        t1 = min(1.0, max(0.0, 1.0 + bias))
        if a_rgb is not None and b_rgb is not None:
            target = [(1 - t1) * x + t1 * y for x, y in zip(a_rgb, b_rgb)]
            hue_src = 'its normal-incidence colour %s' % fmt_list(target, 3)
        elif b_rgb is not None and t1 >= 1.0:
            target, hue_src = b_rgb, 'colorb'
        elif a_rgb is not None:
            target, hue_src = a_rgb, 'colora (colorb is not a uniform colour)'
        else:
            return None, 'REFUSED: iridescent_painter `%s`: no literal uniform colour to match a film to' % pname
        # Mean colour painter, replacing the iridescent painter in place.
        avg = pname
        if a_rgb is not None and b_rgb is not None:
            mean = [(1 - w) * x + w * y for x, y in zip(a_rgb, b_rgb)]
            ed.replace(P, chunk_lines('uniformcolor_painter',
                                      [('name', avg), ('color', fmt_list(mean, 5, False)), ('colorspace', 'Rec709RGB_Linear')],
                                      ['%s: was iridescent_painter (colora `%s`, colorb `%s`, bias %s); now its' % (
                                          STAMP, a_name, b_name, fmt_num(bias)),
                                       'projected-area mean colour (1-w) colora + w colorb, w = %.4f; the view-dependent' % w,
                                       'colour shift moved to the thin-film ggx_material(s) that bind it.'], ind))
            mdesc = 'mean colour %s (w %.3f)' % (fmt_list(mean, 3), w)
        else:
            mask = unique_name(ed, created, pname + '_mask')
            ensure_painter(ed, created, mask, [1 - w] * 3, P.kw_line, ind,
                           '%s: constant blend weight 1-w for `%s`.' % (STAMP, pname))
            ed.replace(P, chunk_lines('blend_painter',
                                      [('name', avg), ('colora', a_name), ('colorb', b_name), ('mask', mask)],
                                      ['%s: was iridescent_painter (bias %s); now its projected-area mean' % (STAMP, fmt_num(bias)),
                                       '(1-w) colora + w colorb, w = %.4f (mask = 1-w selects colora).' % w], ind))
            mdesc = 'mean blend w %.3f' % w
        for d, idx, slot in users:
            dind = body_indent(ed, d)
            if d.kw in ('translucent_material', 'ggx_material'):
                report.append('`%s`.%s: iridescent `%s` -> %s; the angular colour shift is LOST there' % (
                    d.name(), slot, pname, mdesc))
                done.append('%s `%s` keeps the mean colour' % (d.kw, d.name()))
                continue
            if d.kw_line in ed.repl:
                continue   # already rewritten (bound twice)
            white = ensure_painter(ed, created, 'pnt_tf_white', [1, 1, 1], d.kw_line, dind,
                                   '%s: white F0 tint for the thin-film materials.' % STAMP)
            for nm_, fn in THINFILM_DATA:
                if nm_ not in created and nm_ not in named:
                    created.add(nm_)
                    ed.insert_before(d.kw_line, chunk_lines('scalar_painter', [('name', nm_), ('file', fn)], None, dind) + [''])
            if d.kw == 'perfectreflector_material':
                th = match_thickness(target, _TF_TI)
                th_matched = th
                ov = FILM_OVERRIDE.get(rel, {}).get(pname)
                override_note = ''
                if ov is not None:
                    th = ov
                    override_note = ('; OVERRIDDEN to %d nm by owner ruling 2026-10-10 (keeps the purple look; '
                                     'FILM_OVERRIDE)' % ov)
                params = [('name', d.name()), ('rd', 'none'), ('rs', white), ('alphax', fmt_num(IRID_MIRROR_ALPHA)),
                          ('alphay', fmt_num(IRID_MIRROR_ALPHA)), ('fresnel_mode', 'thinfilm'), ('ior', 'tf_Ti_n'),
                          ('extinction', 'tf_Ti_k'), ('film_ior', 'tf_TiO2_n'), ('film_extinction', 'tf_TiO2_k'),
                          ('film_thickness', str(th))]
                desc = ('perfectreflector with iridescent `%s` -> near-mirror (alpha %s) TiO2 film over titanium, '
                        'thickness %d nm matching %s%s' % (pname, fmt_num(IRID_MIRROR_ALPHA), th_matched, hue_src,
                                                           override_note))
            else:
                th = match_thickness(target, _TF_DIEL)
                params = [('name', d.name()), ('rd', avg), ('rs', white), ('alphax', fmt_num(IRID_PEARL_ALPHA)),
                          ('alphay', fmt_num(IRID_PEARL_ALPHA)), ('fresnel_mode', 'thinfilm'),
                          ('ior', fmt_num(IRID_PEARL_IOR)), ('extinction', '0'), ('film_ior', 'tf_TiO2_n'),
                          ('film_extinction', 'tf_TiO2_k'), ('film_thickness', str(th))]
                desc = ('lambertian with iridescent `%s` -> pearlescent ggx: rd = %s, TiO2 film %d nm over a %s dielectric '
                        '(alpha %s), thickness matching %s' % (pname, mdesc, th, fmt_num(IRID_PEARL_IOR),
                                                                       fmt_num(IRID_PEARL_ALPHA), hue_src))
            ed.replace(d, chunk_lines('ggx_material', params, ['%s: was %s; %s.' % (STAMP, d.kw, desc)], dind))
            report.append('`%s`: %s' % (d.name(), desc))
            done.append('%s `%s` -> thin-film ggx_material' % (d.kw, d.name()))
    return ed.text(), 'MIGRATED (iridescent): ' + '; '.join(done)


# --------------------------------------------------------------------------
# Family: ambient_light
# --------------------------------------------------------------------------

def migrate_ambient(text, report, drop_with_sky=False):
    ed = Editor(text)
    amb = [c for c in ed.chunks if c.kw == 'ambient_light']
    if not amb:
        return None, 'unchanged (no ambient_light)'
    rasters = [c for c in ed.chunks if c.kw.endswith('_rasterizer')]
    if len(rasters) != 1:
        return None, ('REFUSED: %d rasterizer chunks (%s); only the last declared (`%s`) renders' % (
            len(rasters), ', '.join(c.kw for c in rasters), rasters[-1].kw))
    R = rasters[0]
    if R.kw.startswith('pixel') or R.kw.startswith('mlt'):
        return None, 'REFUSED: rasterizer `%s` is frozen (migrate it first)' % R.kw
    named = ed.named()
    skies = [c for c in ed.chunks if c.kw in ENV_CHUNKS]
    if skies:
        if not drop_with_sky:
            return None, ('REFUSED: `%s` already installs the environment, which a radiance_map would replace '
                          '(--ambient-with-sky drop deletes the ambient fill instead)' % skies[0].kw)
        for a in amb:
            ed.delete(a)
        report.append('ambient_light dropped: `%s` already supplies the (occluded) sky light the ambient faked' % skies[0].kw)
        return ed.text(), 'MIGRATED (ambient): %d ambient_light dropped (scene has %s)' % (len(amb), skies[0].kw)
    total = [0.0, 0.0, 0.0]
    for a in amb:
        if a.get('position') or a.get('direction'):
            pass
        rgb = light_linear_rgb(a)
        if rgb is None:
            return None, 'REFUSED: ambient_light `%s` colour is not a literal in a supported colorspace' % a.name()
        pw = literal_list(a.get('power') or ['1.0'])
        if pw is None or len(pw) != 1:
            return None, 'REFUSED: ambient_light `%s` power is not a literal' % a.name()
        for d in ed.chunks:
            if d is not a and a.name() and any(a.name() in t[1:] for _, t in d.params):
                return None, 'REFUSED: ambient_light `%s` is referenced by `%s`' % (a.name(), d.kw)
        for i in range(3):
            total[i] += rgb[i] * pw[0] / math.pi
    ind = body_indent(ed, R)
    have = dict((t[0], (idx, t)) for idx, t in R.params)
    if 'radiance_map' in have:
        mname = have['radiance_map'][1][1]
        M = named.get(mname)
        if M is None or M.kw != 'uniformcolor_painter':
            return None, 'REFUSED: the rasterizer already has a non-uniform radiance_map `%s`' % mname
        sky = painter_linear_rgb(M)
        if sky is None:
            return None, 'REFUSED: radiance_map painter `%s` colour is not a literal' % mname
        scale = float(have['radiance_scale'][1][1]) if 'radiance_scale' in have else 1.0
        new = [s_ + t / scale for s_, t in zip(sky, total)]
        # A NEW painter: the sky painter may be shared (e.g. a luminaire's
        # exitance), which must not change.
        pname = unique_name(ed, set(), mname + '_with_ambient')
        ed.insert_before(R.kw_line, chunk_lines('uniformcolor_painter',
                                                [('name', pname), ('color', fmt_list(new, 5, False)),
                                                 ('colorspace', 'Rec709RGB_Linear')],
                                                ['%s: the radiance_map `%s` %s (linear) plus the former ambient_light term %s' % (
                                                    STAMP, mname, fmt_list(sky, 4), fmt_list(total, 4)),
                                                 '/ radiance_scale %s (ambient color x power / pi: the uniform radiance that lights an' % fmt_num(scale),
                                                 'unoccluded Lambertian surface as the ambient light did; the environment is occluded,',
                                                 'so creases darken).  A new painter: `%s` itself is left as authored (other chunks may bind it).' % mname], ind) + [''])
        idx = have['radiance_map'][0]
        line = ed.lines[idx]
        ed.repl[idx] = (idx, [line[:line.index('radiance_map')] + 'radiance_map' +
                              line[line.index('radiance_map') + len('radiance_map'):].replace(mname, pname, 1)])
        report.append('ambient %s (linear radiance) added to the uniform radiance_map `%s` as a NEW painter `%s` (%s; also the background)' % (
            fmt_list(total, 4), mname, pname, fmt_list(new, 4)))
    else:
        for k in ('radiance_scale', 'radiance_background', 'radiance_orient'):
            if k in have:
                return None, 'REFUSED: rasterizer sets `%s` without a radiance_map' % k
        pname = unique_name(ed, set(), 'pnt_ambient_env')
        tot_y = max(total)
        col = [t / tot_y for t in total] if tot_y > 0 else [0, 0, 0]
        ed.insert_before(R.kw_line, chunk_lines('uniformcolor_painter',
                                                [('name', pname), ('color', fmt_list(col, 5, False)),
                                                 ('colorspace', 'Rec709RGB_Linear')],
                                                ['%s: the former ambient_light, as a uniform environment (radiance_map' % STAMP,
                                                 'below).  radiance = ambient color x power / pi: the uniform radiance that lights an',
                                                 'unoccluded Lambertian surface as the ambient light did.  The environment is occluded,',
                                                 'so creases and interiors get darker (owner ruling #5).'], ind) + [''])
        last = R.params[-1][0] if R.params else R.kw_line + 1
        add = ['%sradiance_map\t\t\t%s' % (ind, pname),
               '%sradiance_scale\t\t\t%s' % (ind, '%.6g' % tot_y),
               '%sradiance_background\t\tFALSE' % ind]
        ed.repl[last] = (last, [ed.lines[last]] + add)
        report.append('ambient -> radiance_map `%s`, radiance %s (linear), radiance_background FALSE' % (
            pname, fmt_list(total, 4)))
    for a in amb:
        ed.delete(a)
    if R.kw.startswith('vcm'):
        report.append('VCM: renders no ambient_light but does render the radiance_map, so this ADDS the ambient term')
    return ed.text(), 'MIGRATED (ambient): %d ambient_light -> uniform environment on `%s`' % (len(amb), R.kw)


# --------------------------------------------------------------------------
# Family: MLT
# --------------------------------------------------------------------------

def migrate_mlt(text, report, integrator='bdpt'):
    ed = Editor(text)
    rasters = [c for c in ed.chunks if c.kw.endswith('_rasterizer')]
    mlt = [c for c in rasters if c.kw in MLT_TARGETS['bdpt']]
    if not mlt:
        return None, 'unchanged (no MLT rasterizer)'
    if len(rasters) != 1:
        return None, ('REFUSED: %d rasterizer chunks (%s); only the last declared (`%s`) renders -- '
                      'remove the others first' % (len(rasters), ', '.join(c.kw for c in rasters), rasters[-1].kw))
    R = mlt[0]
    target = MLT_TARGETS[integrator][R.kw]
    accepted = TARGET_PARAMS[target]
    ind = body_indent(ed, R)
    newp = []
    dropped = []
    for idx, t in R.params:
        k = t[0]
        if k in MLT_DROPPED:
            dropped.append('%s %s' % (k, ' '.join(t[1:])))
            continue
        if k == 'mutations_per_pixel':
            newp.append(('samples', ' '.join(t[1:])))
            continue
        if k not in accepted:
            return None, 'REFUSED: `%s` %s is not accepted by %s' % (R.kw, k, target)
        newp.append((k, ' '.join(t[1:])))
    cmt = ['%s: was %s (frozen) -> %s; mutations_per_pixel -> samples%s.' % (
        STAMP, R.kw, target, (', dropped ' + ', '.join(dropped)) if dropped else '')]
    ed.replace(R, chunk_lines(target, newp, cmt, ind))
    report.append('%s -> %s; dropped %s' % (R.kw, target, ', '.join(dropped) or 'nothing'))
    return ed.text(), 'MIGRATED (mlt): %s -> %s' % (R.kw, target)


# --------------------------------------------------------------------------

FAMILIES = ('materials', 'iridescent', 'ambient', 'mlt')


def run(families, paths, dry_run, verbose, mlt_integrator, ambient_with_sky=None, collapse=False):
    counts = {'migrated': 0, 'refused': 0, 'unchanged': 0, 'held': 0}
    rc = 0
    for f in collect(paths):
        rel = repo_rel(f)
        if rel.startswith(LEGACY_CORPUS) or rel.startswith(COVERAGE_CORPUS):
            counts['held'] += 1
            continue
        if rel in HELD:
            counts['held'] += 1
            print('HELD      %s: %s' % (rel, HELD[rel]))
            continue
        try:
            with open(f, 'r', encoding='utf-8', newline='') as fh:
                text = fh.read()
        except (IOError, UnicodeDecodeError) as e:
            print('ERROR     %s: %s' % (rel, e))
            rc = 1
            continue
        flag = rasterizer_flag(rel, text)
        if flag:
            # More than one live rasterizer (only the last renders) or a
            # frozen ACTIVE rasterizer: reported whatever the family.
            print(flag)
        cur = text
        statuses = []
        notes = []
        refused = None
        try:
            for fam in families:
                if fam in KEEP.get(rel, {}):
                    if verbose:
                        print('KEPT      %s: %s family -- %s' % (rel, fam, KEEP[rel][fam]))
                    continue
                fn = {'materials': migrate_materials, 'iridescent': migrate_iridescent,
                      'ambient': migrate_ambient}.get(fam)
                if fam == 'iridescent':
                    new, st = migrate_iridescent(cur, notes, rel)
                elif fam == 'mlt':
                    new, st = migrate_mlt(cur, notes, mlt_integrator)
                elif fam == 'ambient':
                    new, st = migrate_ambient(cur, notes, ambient_with_sky == 'drop')
                elif fam == 'materials':
                    new, st = migrate_materials(cur, notes, collapse)
                else:
                    new, st = fn(cur, notes)
                if new is None:
                    if not st.startswith('unchanged'):
                        refused = st
                        break
                    continue
                statuses.append(st[len('MIGRATED '):])
                cur = new
        except ParseError as e:
            print('ERROR     %s: %s' % (rel, e))
            rc = 1
            continue
        if refused:
            counts['refused'] += 1
            print('REFUSED   %s: %s' % (rel, refused[len('REFUSED: '):] if refused.startswith('REFUSED: ') else refused))
            continue
        if not statuses:
            counts['unchanged'] += 1
            if verbose:
                print('UNCHANGED %s' % rel)
            continue
        if rel in HELD:
            counts['held'] += 1
            print('HELD      %s: %s' % (rel, HELD[rel]))
            continue
        counts['migrated'] += 1
        print('MIGRATED  %s: %s' % (rel, ' | '.join(statuses)))
        for n in notes:
            print('          - %s' % n)
        if not dry_run:
            with open(f, 'w', encoding='utf-8', newline='') as fh:
                fh.write(cur)
    print('%s: %d migrated, %d refused, %d held, %d unchanged%s' % (
        '+'.join(families), counts['migrated'], counts['refused'], counts['held'], counts['unchanged'],
        ' (dry run, nothing written)' if dry_run else ''))
    return rc


def selftest():
    s = ('RISE ASCII SCENE 7\nuniformcolor_painter\n{\n\tname w\n\tcolor 1 1 1\n}\n'
         'isotropic_phong_material\n{\n\tname m\n\trd w\n\trs w\n\tN 32\n}\n'
         'polished_material\n{\n\tname p\n\treflectance w\n\ttau 1\n\tior 1.5\n\tscattering 64\n}\n'
         'polished_material\n{\n\tname q\n\treflectance w\n\ttau 0\n}\n'
         'ambient_light\n{\n\tname a\n\tcolor 1 1 1\n\tpower 3.14159265\n}\n'
         'pathtracing_pel_rasterizer\n{\n\tsamples 4\n}\n')
    new, st = migrate_materials(s, [])
    assert st.startswith('MIGRATED'), st
    code = '\n'.join(strip_comments(new.split('\n')))
    assert 'isotropic_phong_material' not in code and 'polished_material' not in code, new
    assert 'coated_material' in code and 'p_base' in code, new
    assert re.search(r'coat_roughness\s+0\.08805\b', new), new   # 1/sqrt(2*64+1)
    assert re.search(r'alphax\s+0\.12403\b', new), new          # 1/sqrt(2*32+1)
    assert migrate_materials(new, [])[0] is None
    new2, st2 = migrate_ambient(new, [])
    assert st2.startswith('MIGRATED'), st2
    code2 = '\n'.join(strip_comments(new2.split('\n')))
    assert 'ambient_light' not in code2 and re.search(r'radiance_map\s+pnt_ambient_env', new2), new2
    assert re.search(r'radiance_scale\s+1\n', new2), new2
    assert migrate_ambient(new2, [])[0] is None
    m = 'mlt_rasterizer\n{\n\tmax_eye_depth 4\n\tchains 64\n\tmutations_per_pixel 200\n}\n'
    new3, st3 = migrate_mlt(m, [])
    assert 'bdpt_pel_rasterizer' in new3 and 'samples' in new3 and 'chains' not in '\n'.join(strip_comments(new3.split('\n'))), new3
    assert abs(mean_weight(0.0) - 2.0 / 3.0) < 1e-6
    assert abs(mean_weight(-0.5) - 5.0 / 24.0) < 1e-6      # int_.5^1 (mu-.5) 2mu dmu
    ir = ('uniformcolor_painter\n{\n\tname a\n\tcolor 0.5 0.1 0.4\n}\nuniformcolor_painter\n{\n\tname b\n\tcolor 0 0.5 1\n}\n'
          'iridescent_painter\n{\n\tname i\n\tcolora a\n\tcolorb b\n}\n'
          'perfectreflector_material\n{\n\tname r\n\treflectance i\n}\n')
    new4, st4 = migrate_iridescent(ir, [])
    assert re.search(r'fresnel_mode\s+thinfilm', new4) and 'tf_Ti_n' in new4, new4
    assert migrate_iridescent(new4, [])[0] is None
    # Schlick isotropy < 1: facets spread along onb.u, so alphax is the LARGER.
    sch = 'schlick_material\n{\n\tname s\n\troughness 0.04\n\tisotropy 0.1\n}\n'
    new5, _ = migrate_materials(sch, [])
    axv = float(re.search(r'alphax\s+(\S+)', new5).group(1))
    ayv = float(re.search(r'alphay\s+(\S+)', new5).group(1))
    assert axv > ayv and abs(axv * ayv - 0.04) < 1e-4, new5        # 0.2 * 0.1^(-/+1/4)
    # Per-channel roughness: refused by default, collapsed on request.
    pcs = 'ward_isotropic_material\n{\n\tname w\n\talpha 0.1 0.2 0.3\n}\n'
    assert migrate_materials(pcs, [])[0] is None and 'per-channel' in migrate_materials(pcs, [])[1]
    rep = []
    new6, _ = migrate_materials(pcs, rep, collapse=True)
    assert re.search(r'alphax\s+0\.18596\n', new6) and any('COLLAPSED' in r for r in rep), new6
    # Ambient into an existing uniform sky: a NEW painter, the sky untouched.
    amb = ('uniformcolor_painter\n{\n\tname sky\n\tcolor 0.2 0.2 0.2\n}\n'
           'lambertian_luminaire_material\n{\n\tname lum\n\texitance sky\n}\n'
           'ambient_light\n{\n\tname a\n\tcolor 1 1 1\n\tpower 3.14159265\n}\n'
           'pathtracing_pel_rasterizer\n{\n\tradiance_map sky\n\tradiance_scale 2\n}\n')
    new7, _ = migrate_ambient(amb, [])
    assert '\tcolor 0.2 0.2 0.2' in new7 and re.search(r'radiance_map\s+sky_with_ambient', new7), new7
    assert re.search(r'name\s+sky_with_ambient\n\tcolor\s+0\.7 0\.7 0\.7', new7), new7
    print('selftest OK')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--family', action='append', choices=FAMILIES + ('all',))
    ap.add_argument('--mlt-integrator', choices=('bdpt', 'vcm'), default='bdpt')
    ap.add_argument('--ambient-with-sky', choices=('drop',),
                    help='ambient: when a hosek_wilkie_skylight already installs the environment, delete the '
                         'ambient fill (the sky is the occluded version of it) instead of refusing')
    ap.add_argument('--collapse-per-channel', action='store_true',
                    help='materials: collapse a per-channel roughness to its luminance-weighted mean '
                         '(ggx_material reads only the first channel under RGB) instead of refusing')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('paths', nargs='*', default=['scenes'])
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.family:
        ap.error('--family is required')
    fams = FAMILIES if 'all' in a.family else tuple(f for f in FAMILIES if f in a.family)
    return run(fams, a.paths, a.dry_run, a.verbose, a.mlt_integrator, a.ambient_with_sky, a.collapse_per_channel)


if __name__ == '__main__':
    sys.exit(main())
