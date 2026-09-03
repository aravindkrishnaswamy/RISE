#!/usr/bin/env python3
"""Measurement harness for docs/CLOTH_FABRIC_DESIGN.md section 9.9 gates 9, 9b and 10.

Three measurements, one subject family, one reader.  All three are RECORDING
gates -- they exist to put a number on a limit the design already admits to, not
to pass or fail -- so this script prints tables meant to be pasted into the
design doc rather than exiting non-zero on a threshold.

    terminator   gate 9   -- the cue-(c) terminator profile of a rim-lit velvet
                             swatch, against the bare-substrate reference.
    aniso        gate 9b  -- the cue-(b) highlight-anisotropy proxy: how much of
                             an anisotropic GGX substrate's directional highlight
                             survives being wrapped in an ISOTROPIC Charlie sheen.
    variance     gate 10  -- K-trial pixel variance at fixed spp across sheen
                             alpha, and the spp each alpha needs to reach a fixed
                             relative-noise floor.

WHY A SPHERE AND NOT THE SHOWCASE DRAPE.  Gates 9 and 10 both need the shading
geometry to be ANALYTIC, because the numbers they report are indexed by incidence
angle.  On a unit sphere at the origin viewed from far down +Z with a narrow fov
(near-orthographic), the visible point on the horizontal centre scanline at
normalized screen abscissa s has

    N = (s, 0, sqrt(1 - s^2)),   V = (0, 0, 1),   and with L = (1, 0, 0):
    cos(theta_i) = N . L = s          -- so theta_i = acos(s)
    cos(theta_o) = N . V = sqrt(1-s^2)

i.e. one render scanline sweeps incidence from 0 degrees at the right silhouette
to 90 degrees at the disc centre (the terminator), with the VIEW simultaneously
sweeping the other way.  That is exactly the configuration the sheen lobe lives
in, and it needs no geometry-aware unprojection to read.  The drape is the
subject of the VISUAL half of gate 9b; this script measures the half that is a
number.

EXR, NOT PNG, AND NO DISPLAY TRANSFORM.  `file_rasterizeroutput` applies an ACES
tone curve to LDR formats by default (`display_transform`'s own default is "aces
(LDR) / none (HDR)"), which is right for a showcase PNG and fatal for a profile
measurement -- it is a nonlinear, highlight-compressing remap of the exact
quantity being measured.  Every render below writes 32-bit EXR, whose default
display transform is `none`.

OIDN IS OFF EVERYWHERE (`oidn_denoise FALSE`).  Gate 10 is a variance
measurement and the denoiser's entire job is to destroy variance; gate 9's
terminator and gate 9b's highlight are both structures OIDN is free to smooth.

SEEDING.  Each render is a separate `bin/rise` process, and bin/rise's main
(src/RISE/commandconsole.cpp:624) calls `srand( GetMilliseconds() )` at startup
-- so K trials launched in sequence start from K different clock states and are
independent draws.  That is a property of the CLI entry point specifically: a
test binary that calls `RISE_CreateJobPriv` directly never runs that main and is
NOT wall-clock seeded, which is why tests/FabricRenderTest.cpp sets its own seed
base instead of relying on the same assumption.  No PT rasterizer exposes a seed
parameter, so gate 9's fabric-vs-bare pairs are NOT seed-matched; at 1024-4096
spp on the smooth quantities being profiled that is acceptable, but the two
sides of each ratio are independent draws rather than common-random-number
pairs, and a small part of every ratio quoted is therefore MC noise rather than
signal.

Usage:
    python3 tools/fabric_sheen_measure.py terminator
    python3 tools/fabric_sheen_measure.py aniso
    python3 tools/fabric_sheen_measure.py variance
    python3 tools/fabric_sheen_measure.py all

Run from the repo root (it sets RISE_MEDIA_PATH itself and invokes ./bin/rise).
"""

import math
import os
import subprocess
import sys

import numpy as np
import OpenEXR

REPO = os.getcwd()
BIN = os.path.join(REPO, "bin", "rise")
# Outputs go under the repo, RELATIVE, because `file_rasterizeroutput.pattern`
# is resolved against RISE_MEDIA_PATH -- an absolute path there is PREFIXED with
# it, fails to open, and the render lands in an "emergency file" in the cwd.
OUT_REL = os.path.join("rendered", "_fabric_measure")
OUT = os.path.join(REPO, OUT_REL)


# ----------------------------------------------------------------------------
# Rendering + EXR reading
# ----------------------------------------------------------------------------

def render(scene_text, name):
    """Render `scene_text` to <OUT>/<name>.exr and return it as a float32 HxWx3.

    bin/rise exits non-zero on the `render\\nquit` path even on success (see
    tools/pixel_gate_global_medium.py's note), so the produced file -- removed
    first, so a stale one cannot masquerade as a fresh render -- is the success
    signal.
    """
    os.makedirs(OUT, exist_ok=True)
    exr = os.path.join(OUT, name + ".exr")
    for stale in (exr,):
        try:
            os.remove(stale)
        except OSError:
            pass

    scene_path = os.path.join(OUT, name + ".RISEscene")
    with open(scene_path, "w") as f:
        f.write(scene_text.replace("@OUT@", os.path.join(OUT_REL, name)))

    env = dict(os.environ)
    env["RISE_MEDIA_PATH"] = REPO + "/"
    subprocess.run([BIN, scene_path], input="render\nquit\n", text=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env=env, timeout=1800)
    if not os.path.exists(exr):
        raise RuntimeError("render produced no EXR: " + exr)
    return read_exr(exr)


def read_exr(path):
    # `exr_with_alpha` defaults TRUE, so the writer emits one interleaved "RGBA"
    # channel-group rather than a bare "RGB"; accept either and drop alpha.
    with OpenEXR.File(path) as f:
        ch = f.channels()
        grp = ch.get("RGBA") or ch["RGB"]
        px = grp.pixels
    return np.asarray(px, dtype=np.float64)[:, :, :3]


def luminance(img):
    # Rec.709 luminance; the film resolves to RISEPel == Rec709RGBPel
    # (CLAUDE.md, colour-space migration Stage B), so these are the right
    # weights for this buffer and not a generic guess.
    return 0.2126 * img[:, :, 0] + 0.7152 * img[:, :, 1] + 0.0722 * img[:, :, 2]


# ----------------------------------------------------------------------------
# Scene construction
# ----------------------------------------------------------------------------

# Near-orthographic: the camera sits 40 units out with a fov that just contains
# the unit sphere, so the largest view-ray parallax across the disc is ~1.45
# degrees and the analytic (s -> theta) mapping in the module docstring holds to
# better than the pixel grid can resolve.
CAM_Z = 40.0
FRAME_HALF = 1.01
FOV_DEG = 2.0 * math.degrees(math.atan(FRAME_HALF / CAM_Z))


def _preamble(width, height):
    return (
        "RISE ASCII SCENE 7\n"
        "standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
        "film\n{\n\twidth %d\n\theight %d\n}\n\n"
        "pinhole_camera\n{\n\tlocation 0 0 %g\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov %.6f\n}\n\n"
        % (width, height, CAM_Z, FOV_DEG)
    )


def _painters(dye, sheen, sheen_alpha):
    return (
        "uniformcolor_painter\n{\n\tname pnt_dye\n\tcolor %g %g %g\n\tcolorspace Rec709RGB_Linear\n}\n\n"
        "uniformcolor_painter\n{\n\tname pnt_sheen\n\tcolor %g %g %g\n\tcolorspace Rec709RGB_Linear\n}\n\n"
        "uniformcolor_painter\n{\n\tname pnt_f0\n\tcolor 0.04 0.04 0.04\n\tcolorspace Rec709RGB_Linear\n}\n\n"
        "scalar_painter\n{\n\tname pnt_alpha\n\tvalue %g\n}\n\n"
        % (dye[0], dye[1], dye[2], sheen[0], sheen[1], sheen[2], sheen_alpha)
    )


def _output():
    # 32-bit EXR, so no display transform and no quantization (see the module
    # docstring).  `exr_compression none` keeps the reader trivially happy.
    return ("file_rasterizeroutput\n{\n\tpattern @OUT@\n\ttype EXR\n\tbpp 32\n"
            "\tcolor_space Rec709RGB_Linear\n\texr_compression none\n}\n")


def _rasterizer_pt(samples, env_lit):
    s = ("pathtracing_pel_rasterizer\n{\n\tsamples %d\n\toidn_denoise FALSE\n" % samples)
    if env_lit:
        s += "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background FALSE\n"
    s += "}\n\n"
    return s


def _axial_light():
    # The light placed ON the camera axis.  On a sphere this is the geometry in
    # which the Charlie lobe actually peaks: with wi == wo == +Z, the half vector
    # is the constant (0,0,1), so at screen radius r the visible point has
    #     cos(theta_h) = cos(theta_i) = cos(theta_o) = sqrt(1 - r^2)
    # i.e. theta_h = theta_i = theta_o = asin(r), sweeping 0 at the disc centre to
    # 90 degrees at the silhouette.  D_Charlie goes as sin(theta_h)^(1/alpha), so
    # the whole grazing halo is laid out along the radius, and the image is
    # rotationally symmetric -- which lets the profile be RADIALLY BINNED instead
    # of read off one scanline, buying back the resolution that
    # ds/dtheta = cos(theta) throws away near the silhouette.
    #
    # The 90-degree rim configuration below CANNOT show this: there
    # theta_i + theta_o = 90 everywhere on the central scanline, so theta_h sits
    # near 45 degrees and the lobe is never near its peak.  The two
    # configurations are complementary and gate 9 records both.
    return ("directional_light\n{\n\tname axial\n\tpower 1.0\n\tcolor 1.0 1.0 1.0\n"
            "\tdirection 0.0 0.0 1.0\n}\n\n")


def _rim_light():
    # `direction` is FROM the surface TO the light (docs/SCENE_CONVENTIONS.md 1),
    # so +X is a light sitting on the +X axis: exactly 90 degrees off the view,
    # which puts the terminator on the disc's vertical centre line and makes the
    # scanline sweep incidence from 0 to 90 degrees.
    return ("directional_light\n{\n\tname rim\n\tpower 1.0\n\tcolor 1.0 1.0 1.0\n"
            "\tdirection 1.0 0.0 0.0\n}\n\n")


def sphere_scene(kind, *, samples, width=768, height=768,
                 dye=(0.20, 0.05, 0.09), sheen=(0.90, 0.90, 0.90),
                 sheen_alpha=0.08, preset="velvet",
                 alphax=0.34, alphay=0.06, weave_rot=None, weave_scale=60.0,
                 env_lit=False, light=True, axial=False):
    """One sphere, one material.

    kind: "fabric_lambert" -- fabric_material over a lambertian_material
          "bare_lambert"   -- the same lambertian_material, unwrapped (gate 9's reference)
          "fabric_ggx"     -- fabric_material over an anisotropic ggx_material
          "bare_ggx"       -- the same ggx_material, unwrapped (gate 9b's reference)
          "bare_weave"     -- a weave_material alone (PHASE 2)
          "fabric_weave"   -- fabric_material over that weave_material (PHASE 2, the
                              composition the substrate allowlist was extended for)

    `weave_scale` matters for the two weave kinds and is passed explicitly by the
    caller rather than defaulted, because the whole point of the Phase-2 half of
    gate 9b is to measure the SAME material at a scale where its draft is
    resolvable and at one where it is not.
    """
    s = _preamble(width, height)
    s += _painters(dye, sheen, sheen_alpha)
    if env_lit:
        s += "uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"

    if kind in ("fabric_lambert", "bare_lambert"):
        s += "lambertian_material\n{\n\tname base_sub\n\treflectance pnt_dye\n}\n\n"
    elif kind in ("fabric_weave", "bare_weave"):
        # The author's dye on the WARP, the preset's own colour left on the
        # weft -- the same split `make_fabric` mints and the showcase scenes
        # author, so the number measured here is the one a scene actually gets.
        s += ("weave_material\n{\n\tname base_sub\n\tfabric %s\n\twarp_color pnt_dye\n"
              "\tweave_scale %g\n\tweave_rotation 0.0\n}\n\n" % (preset, weave_scale))
    else:
        s += ("ggx_material\n{\n\tname base_sub\n\trd pnt_dye\n\trs pnt_f0\n"
              "\talphax %g\n\talphay %g\n\tfresnel_mode schlick_f0\n}\n\n" % (alphax, alphay))

    if kind.startswith("fabric"):
        s += ("fabric_material\n{\n\tname mat\n\tfabric %s\n\tbase base_sub\n"
              "\tsheen_color pnt_sheen\n\tsheen_roughness pnt_alpha\n" % preset)
        if weave_rot is not None:
            s += "\tweave_rotation %g\n" % weave_rot
        s += "}\n\n"
        mat = "mat"
    else:
        mat = "base_sub"

    s += "sphere_geometry\n{\n\tname sph\n\tradius 1.0\n}\n\n"
    s += ("standard_object\n{\n\tname obj\n\tgeometry sph\n\tmaterial %s\n\tposition 0 0 0\n}\n\n" % mat)
    if light:
        s += _axial_light() if axial else _rim_light()
    s += _rasterizer_pt(samples, env_lit)
    s += _output()
    return s


# ----------------------------------------------------------------------------
# Gate 9 -- the cue-(c) terminator profile
# ----------------------------------------------------------------------------

# Where the profile is sampled.  90 degrees is the terminator itself; 0 is the
# right silhouette, where the LIGHT is head-on and the VIEW is fully grazing.
PROFILE_ANGLES = [20, 40, 60, 70, 80, 85, 88]


def _scanline(img):
    """Central horizontal scanline as (theta_i in degrees, luminance), inside the disc.

    Returns arrays ordered by increasing theta_i (i.e. right silhouette first).
    """
    h, w = img.shape[:2]
    lum = luminance(img)
    # Average a thin BAND of rows about the equator instead of a single one.  At
    # |sy| < 0.012 the sampled points' normals differ from the equator's by less
    # than 0.7 degrees -- far inside one bin of the profile -- while the band
    # cuts the per-column MC noise by sqrt(rows).
    band = max(1, int(round(0.012 * h / FRAME_HALF)))
    lo = max(0, h // 2 - band)
    hi = min(h, h // 2 + band + 1)
    row = lum[lo:hi, :].mean(axis=0)
    xs = ((np.arange(w) + 0.5) / w * 2.0 - 1.0) * FRAME_HALF
    inside = np.abs(xs) < 0.999
    s = xs[inside]
    v = row[inside]
    theta = np.degrees(np.arccos(np.clip(s, -1.0, 1.0)))
    order = np.argsort(theta)
    return theta[order], v[order], (s[order])


def _at(theta, vals, want):
    return float(np.interp(want, theta, vals))


def _crossing(theta, vals, frac, ref):
    """First theta (increasing) at which `vals` falls below frac*ref."""
    target = frac * ref
    for i in range(1, len(theta)):
        if vals[i - 1] >= target > vals[i]:
            t0, t1 = theta[i - 1], theta[i]
            v0, v1 = vals[i - 1], vals[i]
            if v0 == v1:
                return float(t0)
            return float(t0 + (t1 - t0) * (v0 - target) / (v0 - v1))
    return float("nan")


def _radial_profile(img, nbins=180):
    """Radially bin the disc into equal-theta bins, theta = asin(r).

    Only valid for the AXIAL-light configuration, where the image is
    rotationally symmetric about the disc centre.  Binning instead of reading a
    scanline is what makes the near-silhouette angles measurable at all: along a
    scanline ds/dtheta = cos(theta) collapses to 0 there, so one pixel spans
    degrees; binning by theta gives every bin the pixels of a whole annulus.
    """
    lum = luminance(img)
    h, w = lum.shape
    yy, xx = np.mgrid[0:h, 0:w]
    # Screen coordinates in the same normalized units as the scanline reader.
    sx = ((xx + 0.5) / w * 2.0 - 1.0) * FRAME_HALF
    sy = ((yy + 0.5) / h * 2.0 - 1.0) * FRAME_HALF
    r = np.hypot(sx, sy)
    inside = r < 0.9995
    theta = np.degrees(np.arcsin(np.clip(r[inside], 0.0, 1.0)))
    vals = lum[inside]

    edges = np.linspace(0.0, 90.0, nbins + 1)
    idx = np.clip(np.digitize(theta, edges) - 1, 0, nbins - 1)
    sums = np.bincount(idx, weights=vals, minlength=nbins)
    cnts = np.bincount(idx, minlength=nbins).astype(float)
    ok = cnts > 0
    centres = 0.5 * (edges[:-1] + edges[1:])
    return centres[ok], sums[ok] / cnts[ok]


HALO_ANGLES = [20, 40, 60, 70, 80, 85, 88]


def gate9(samples=4096):
    print("=" * 78)
    print("GATE 9 -- cue (c): terminator profile of a rim-lit velvet swatch")
    print("=" * 78)
    print("subject: unit sphere, dye 0.20/0.05/0.09, sheen colour 0.90 white,")
    print("         sheen alpha 0.08 (the `velvet` preset's own value)")
    print("light:   ONE directional light at 90 deg to the view (`direction 1 0 0`),")
    print("         no environment, no ambient -- rim only")
    print("render:  %d spp, 768x768, EXR 32-bit, oidn_denoise FALSE, display_transform none"
          % samples)
    print("mapping: theta_i = acos(s) along the central scanline; theta_o = 90 - theta_i")
    print()

    fab = render(sphere_scene("fabric_lambert", samples=samples), "g9_fabric")
    bare = render(sphere_scene("bare_lambert", samples=samples), "g9_bare")

    tf, vf, sf = _scanline(fab)
    tb, vb, sb = _scanline(bare)

    # Both profiles are normalized by their own value at theta_i = 20 deg, which
    # is well inside the lit cap and far from both the terminator and the
    # silhouette -- so the shape comparison below is about the FALLOFF and not
    # about the two materials' overall brightness.
    ref_f = _at(tf, vf, 20.0)
    ref_b = _at(tb, vb, 20.0)

    print("  theta_i   theta_o   fabric L      bare L        fabric/bare   fab/ref   bare/ref")
    for a in PROFILE_ANGLES:
        lf = _at(tf, vf, float(a))
        lb = _at(tb, vb, float(a))
        print("   %5.1f     %5.1f   %.6e  %.6e   %8.4f     %7.4f   %7.4f"
              % (a, 90.0 - a, lf, lb, (lf / lb if lb > 0 else float("nan")),
                 lf / ref_f, lb / ref_b))
    print()

    # Falloff width: how far, in incidence angle, the profile takes to go from
    # 90% to 10% of its theta_i = 20 deg value.  A Lambertian's is fixed by
    # cos(theta_i) alone and is the yardstick the sheen's is read against.
    w = fab.shape[1]
    deg_per_px = None
    for name, (t, v, s), ref in (("fabric", (tf, vf, sf), ref_f), ("bare  ", (tb, vb, sb), ref_b)):
        t90 = _crossing(t, v, 0.90, ref)
        t50 = _crossing(t, v, 0.50, ref)
        t10 = _crossing(t, v, 0.10, ref)
        # Pixels per degree at the terminator: ds/dtheta = sin(theta) = 1 at 90 deg,
        # and one pixel is 2*FRAME_HALF/w in s.
        px_per_deg = (math.pi / 180.0) / (2.0 * FRAME_HALF / w)
        deg_per_px = 1.0 / px_per_deg
        print("  %s falloff (fraction of its own theta_i=20 value):" % name)
        print("      90%%  at theta_i = %6.2f deg      50%%  at %6.2f deg      10%%  at %6.2f deg"
              % (t90, t50, t10))
        print("      90->10 width = %6.2f deg = %6.1f px at the terminator (%.4f deg/px)"
              % (t10 - t90, (t10 - t90) * px_per_deg, deg_per_px))
    print()
    print("  (one pixel spans %.4f deg of incidence AT the terminator, where ds/dtheta = 1;"
          % deg_per_px)
    print("   nearer the silhouette a pixel spans more, so the widths above are lower bounds")
    print("   on angular resolution, not on the measurement.)")
    print()

    # ---------------------------------------------------------------- config B
    print("-" * 78)
    print("CONFIGURATION B -- the GRAZING HALO (light ON the camera axis)")
    print("-" * 78)
    print("Configuration A above has theta_i + theta_o == 90 on its scanline, so the")
    print("half vector never leaves the neighbourhood of 45 degrees and the Charlie lobe")
    print("is never near its peak -- A measures the TERMINATOR, and correctly says the")
    print("sheen does nothing to soften it.  B puts the light on the camera axis, where")
    print("theta_h == theta_i == theta_o == asin(r), so the lobe's whole grazing ramp")
    print("lays itself out along the disc radius.  The image is rotationally symmetric,")
    print("so the profile is radially binned (0.5 deg bins) rather than read off a")
    print("scanline.")
    print()

    fabA = render(sphere_scene("fabric_lambert", samples=samples, axial=True), "g9b_axial_fabric")
    barA = render(sphere_scene("bare_lambert", samples=samples, axial=True), "g9b_axial_bare")
    tf2, vf2 = _radial_profile(fabA)
    tb2, vb2 = _radial_profile(barA)

    print("  theta (= theta_i = theta_o = theta_h)   fabric L      bare L        fabric/bare")
    ratios = []
    for a in HALO_ANGLES:
        lf = _at(tf2, vf2, float(a))
        lb = _at(tb2, vb2, float(a))
        rr = lf / lb if lb > 0 else float("nan")
        ratios.append((a, rr))
        print("            %5.1f                      %.6e  %.6e   %8.4f" % (a, lf, lb, rr))
    print()

    # Where the halo switches on: the smallest theta at which the fabric exceeds
    # the bare substrate by each factor.  This is the number an LTC (multiple-
    # scattering) sheen would move most -- it would push the onset EARLIER and
    # raise the plateau.
    rt = np.array([_at(tf2, vf2, t) / max(_at(tb2, vb2, t), 1e-30) for t in tf2])
    for f in (1.10, 1.50, 2.00, 3.00):
        hit = tf2[rt >= f]
        onset = float(hit[0]) if hit.size else float("nan")
        print("      fabric/bare first reaches %.2f at theta = %6.2f deg" % (f, onset))
    print("      peak fabric/bare over the disc = %.3f at theta = %.2f deg"
          % (float(rt.max()), float(tf2[int(np.argmax(rt))])))
    print()


# ----------------------------------------------------------------------------
# Gate 9b -- the cue-(b) highlight-anisotropy proxy
# ----------------------------------------------------------------------------

def _disc_radius_map(lum):
    h, w = lum.shape
    yy, xx = np.mgrid[0:h, 0:w]
    sx = ((xx + 0.5) / w * 2.0 - 1.0) * FRAME_HALF
    sy = ((yy + 0.5) / h * 2.0 - 1.0) * FRAME_HALF
    return np.hypot(sx, sy)


def _highlight_extent(img, weave_axis_is_x=True, rmax=0.65):
    """Half-max extent of the SUBSTRATE highlight, along x and along y, in pixels.

    Restricted to the inner disc (r < rmax).  That restriction is not tidying:
    outside it the ISOTROPIC Charlie halo -- which on a wrapped row is an order
    of magnitude brighter than the substrate's own highlight -- becomes the
    image maximum, and an unrestricted threshold would measure the halo's
    silhouette ring (extent ~ the whole disc, in both axes, ratio ~ 1) instead of
    the highlight.  Measuring the halo is worth doing and IS done, separately, by
    `_rim_lift` below; conflating the two would report "the anisotropy vanished"
    when what actually happened is that a different feature took over the max.

    The diffuse pedestal is removed by subtracting the MEDIAN of the region
    before thresholding, so what is measured is the highlight's own footprint
    rather than the lit cap's.
    """
    lum = luminance(img)
    r = _disc_radius_map(lum)
    region = r < rmax
    if not region.any():
        return float("nan"), float("nan"), float("nan")

    pedestal = float(np.median(lum[region]))
    resid = np.where(region, lum - pedestal, 0.0)
    peak = float(resid.max())
    if peak <= 0:
        return float("nan"), float("nan"), float("nan")
    mask = resid >= 0.5 * peak
    if not mask.any():
        return float("nan"), float("nan"), float("nan")

    ys, xs = np.nonzero(mask)
    ex = xs.max() - xs.min() + 1.0
    ey = ys.max() - ys.min() + 1.0
    along, across = (ex, ey) if weave_axis_is_x else (ey, ex)
    return along, across, (along / across if across > 0 else float("nan"))


def _rim_lift(img, lo=0.90, hi=0.995):
    """Mean luminance in the outer annulus -- where the isotropic sheen halo lives."""
    lum = luminance(img)
    r = _disc_radius_map(lum)
    ring = (r >= lo) & (r < hi)
    return float(lum[ring].mean()) if ring.any() else float("nan")


def _pattern_energy(img, rmax=0.65, k=9):
    """PATTERN-SCALE ENERGY: relative RMS of the high-passed inner disc.

    Gate 9b's original proxy measures the highlight's ANISOTROPY, and its
    verdict was that anisotropy was never the problem -- 95-99 % of the
    substrate's survived the sheen and the frame still read as brushed metal.
    What was missing was structure AT THE SCALE OF A YARN CROSSING, and that has
    no anisotropy signature at all: a weave and a smooth lobe can have identical
    highlight extents.  So the Phase-2 half of this gate needs its own number.

    This one is deliberately crude and deliberately CONTROLLED.  It box-blurs the
    luminance over `k` pixels, subtracts, and reports rms(residual)/mean over the
    inner disc.  That statistic cannot by itself tell pattern from MONTE CARLO
    NOISE, which is also high-frequency -- so it is never read alone.  Every
    weave row below is paired with a control row that is the SAME material at a
    weave scale fine enough to put the draft below one pixel, where the fade
    takes over and the only high-frequency content left is the noise.  The
    difference between the two is the structure.
    """
    lum = luminance(img)
    r = _disc_radius_map(lum)
    region = r < rmax
    if not region.any():
        return float("nan")
    # Separable box blur via a cumulative sum, with the disc's own mean used
    # outside it so the blur does not drag the silhouette in.
    fill = float(lum[region].mean())
    a2 = np.where(region, lum, fill)
    pad = k // 2
    ap = np.pad(a2, pad, mode="edge")
    cs = np.cumsum(np.cumsum(ap, axis=0), axis=1)
    cs = np.pad(cs, ((1, 0), (1, 0)), mode="constant")
    h, w = a2.shape
    blur = (cs[k:k + h, k:k + w] - cs[0:h, k:k + w]
            - cs[k:k + h, 0:w] + cs[0:h, 0:w]) / float(k * k)
    resid = (a2 - blur)[region]
    return float(np.sqrt((resid ** 2).mean()) / fill) if fill > 0 else float("nan")


def gate9b(samples=2048):
    print("=" * 78)
    print("GATE 9b -- cue (b): does an ISOTROPIC sheen over an ANISOTROPIC GGX")
    print("            substrate still read as satin?")
    print("=" * 78)
    print("subject: unit sphere, anisotropic ggx_material substrate, ONE directional")
    print("         light 25 deg off the view axis so the highlight sits inside the disc")
    print("proxy 1: half-max extent of the substrate highlight, measured ALONG the weave")
    print("         axis and ACROSS it, over the INNER disc (r < 0.65).  The BARE")
    print("         substrate is the ceiling; the wrapped row says how much of that")
    print("         anisotropy survives the sheen.")
    print("proxy 2: mean luminance of the outer annulus (r in [0.90, 0.995]) -- the")
    print("         ISOTROPIC halo the sheen adds.  Cue (b) is about whether the")
    print("         DIRECTIONAL structure survives; this number is what it has to")
    print("         compete with.")
    print("render:  %d spp, 768x768, EXR 32-bit, oidn_denoise FALSE" % samples)
    print()

    # A light 25 degrees off the view axis, in the x-z plane: the mirror
    # direction then lands inside the disc rather than at the silhouette, so the
    # highlight's whole footprint is measurable.
    a = math.radians(25.0)
    lightdir = "\tdirection %g 0.0 %g\n" % (math.sin(a), math.cos(a))

    rows = [
        ("silk",  "bare",   "bare_ggx",   0.30, 0.10, 0.20),
        ("silk",  "fabric", "fabric_ggx", 0.30, 0.10, 0.20),
        ("satin", "bare",   "bare_ggx",   0.34, 0.06, 0.12),
        ("satin", "fabric", "fabric_ggx", 0.34, 0.06, 0.12),
    ]

    print("  preset  wrap     along(px)  across(px)   ratio    rim annulus L")
    res = {}
    for preset, wrap, kind, ax, ay, alpha in rows:
        sc = sphere_scene(kind, samples=samples, preset=preset,
                          alphax=ax, alphay=ay,
                          weave_rot=(0.0 if kind.startswith("fabric") else None),
                          dye=(0.18, 0.16, 0.15), sheen=(0.92, 0.90, 0.88),
                          sheen_alpha=alpha)
        sc = sc.replace("\tdirection 1.0 0.0 0.0\n", lightdir)
        img = render(sc, "g9b_%s_%s" % (preset, wrap))
        along, across, ratio = _highlight_extent(img)
        rim = _rim_lift(img)
        res[(preset, wrap)] = (ratio, rim)
        print("  %-7s %-7s %8.1f  %10.1f  %7.3f    %.6e"
              % (preset, wrap, along, across, ratio, rim))
    print()
    for preset in ("silk", "satin"):
        rb, rimb = res[(preset, "bare")]
        rf, rimf = res[(preset, "fabric")]
        surv = 100.0 * (rf - 1.0) / (rb - 1.0) if rb > 1.0 else float("nan")
        print("  %-5s: highlight anisotropy %.3f bare -> %.3f wrapped"
              "  (%.0f%% of the substrate's anisotropy survives the sheen)"
              % (preset, rb, rf, surv))
        print("         isotropic rim annulus %.4e bare -> %.4e wrapped  (x%.1f)"
              % (rimb, rimf, rimf / rimb if rimb > 0 else float("nan")))
    print()

    # ------------------------------------------------------------------
    # PHASE 2.  The block above measures ANISOTROPY, and its Phase-1 verdict
    # was that anisotropy was never the deficit.  This block measures the thing
    # that was -- PATTERN SCALE -- on the same subject, with the Phase-1 rows as
    # the floor and a deliberately-minified weave as the noise control.
    # ------------------------------------------------------------------
    print("-" * 78)
    print("PHASE 2 -- the deficit gate 9b actually named: PATTERN SCALE")
    print("-" * 78)
    print("proxy 3: relative RMS of the high-passed inner disc (9-px box high-pass,")
    print("         r < 0.65).  High-frequency content is pattern OR Monte-Carlo")
    print("         noise, so it is never read alone: the last row of each preset is")
    print("         the SAME weave at a scale fine enough to put its draft below one")
    print("         pixel, where the material's own footprint fade takes over and the")
    print("         only high-frequency content left IS the noise.  The gap between a")
    print("         weave row and its own control is the structure.")
    print()
    print("  THE `aniso ratio` COLUMN IS NOT MEANINGFUL ON THE WEAVE ROWS and is")
    print("  printed only so the phase-1 rows can be read against the block above.")
    print("  Proxy 1 takes the bounding box of everything above half-max after a")
    print("  median pedestal subtraction; on a PATTERNED disc the above-threshold set")
    print("  is the yarn field rather than one highlight, so the box measures the")
    print("  draft's own footprint and the ratio says nothing about a lobe.  The")
    print("  proxy that speaks to the weave rows is `pattern rms`.")
    print()
    print("  preset  configuration              aniso ratio   pattern rms   rim annulus L")

    p2rows = [
        ("silk",  "bare ggx (phase 1)",       "bare_ggx",    0.0,    0.30, 0.10, 0.20),
        ("silk",  "fabric/ggx (phase 1)",     "fabric_ggx",  0.0,    0.30, 0.10, 0.20),
        ("silk",  "weave, scale 60",          "bare_weave",  60.0,   0.30, 0.10, 0.20),
        ("silk",  "fabric/weave, scale 60",   "fabric_weave",60.0,   0.30, 0.10, 0.20),
        ("silk",  "weave, scale 4000 (ctrl)", "bare_weave",  4000.0, 0.30, 0.10, 0.20),
        ("satin", "bare ggx (phase 1)",       "bare_ggx",    0.0,    0.34, 0.06, 0.12),
        ("satin", "fabric/ggx (phase 1)",     "fabric_ggx",  0.0,    0.34, 0.06, 0.12),
        ("satin", "weave, scale 60",          "bare_weave",  60.0,   0.34, 0.06, 0.12),
        ("satin", "fabric/weave, scale 60",   "fabric_weave",60.0,   0.34, 0.06, 0.12),
        ("satin", "weave, scale 4000 (ctrl)", "bare_weave",  4000.0, 0.34, 0.06, 0.12),
    ]
    p2 = {}
    for preset, label, kind, wscale, ax, ay, alpha in p2rows:
        sc = sphere_scene(kind, samples=samples, preset=preset,
                          alphax=ax, alphay=ay, weave_scale=wscale,
                          weave_rot=(0.0 if kind.startswith("fabric") else None),
                          dye=(0.18, 0.16, 0.15), sheen=(0.92, 0.90, 0.88),
                          sheen_alpha=alpha)
        sc = sc.replace("\tdirection 1.0 0.0 0.0\n", lightdir)
        tag = "g9b2_%s_%s_%g" % (preset, kind, wscale)
        img = render(sc, tag.replace(".", "p"))
        _, _, ratio = _highlight_extent(img)
        pe = _pattern_energy(img)
        rim = _rim_lift(img)
        p2[(preset, label)] = (ratio, pe, rim)
        print("  %-7s %-26s %8.3f   %11.5f   %.6e" % (preset, label, ratio, pe, rim))
    print()
    print("  A CONFOUND, STATED BEFORE THE NUMBERS ARE READ.  The weave rows are")
    print("  materially DARKER than the phase-1 rows -- their rim annulus runs 3-8x")
    print("  lower -- so at a fixed sample count their RELATIVE Monte-Carlo noise is")
    print("  correspondingly higher, and a raw rms comparison across the two shapes")
    print("  would be measuring brightness as much as structure.  That is exactly why")
    print("  each weave row is compared against ITS OWN minified control, which shares")
    print("  its brightness and therefore its noise.  Pattern and noise are independent,")
    print("  so they add IN QUADRATURE and the structure alone is")
    print("      sqrt( rms(weave)^2 - rms(control)^2 ).")
    print("  The phase-1 rows need no such subtraction: they have no periodic content")
    print("  to separate out, so their rms IS their noise floor.")
    print()
    for preset in ("silk", "satin"):
        base = p2[(preset, "fabric/ggx (phase 1)")][1]
        weave = p2[(preset, "fabric/weave, scale 60")][1]
        ctrl = p2[(preset, "weave, scale 4000 (ctrl)")][1]
        print("  %-5s: pattern rms  phase-1 fabric/ggx %.5f   phase-2 fabric/weave %.5f"
              "   minified control %.5f" % (preset, base, weave, ctrl))
        if weave > ctrl:
            struct = math.sqrt(weave * weave - ctrl * ctrl)
            print("         -> STRUCTURE ALONE (quadrature-subtracted): %.5f, against a"
                  " phase-1 shape that has none" % struct)
        else:
            print("         -> NO structure above the noise floor -- the weave row did not"
                  " exceed its own control, which would mean the draft is not resolving")
    print()


# ----------------------------------------------------------------------------
# Gate 10 -- variance vs sheen alpha
# ----------------------------------------------------------------------------

NOISE_FLOOR = 0.01   # target relative per-pixel standard deviation


def gate10(K=6, samples=32, width=256, height=256):
    print("=" * 78)
    print("GATE 10 -- variance: the cosine-sampling cost across sheen alpha")
    print("=" * 78)
    print("K-trial protocol per docs/skills/variance-measurement.md: %d independent" % K)
    print("renders per cell at %d spp, per-pixel standard deviation across trials," % samples)
    print("averaged over the lit disc and normalized by the per-pixel mean.")
    print()
    print("WHY THE TRIALS ARE INDEPENDENT: every render here is a separate `bin/rise`")
    print("process, and bin/rise's own main -- src/RISE/commandconsole.cpp:624 -- calls")
    print("`srand( GetMilliseconds() )` at startup, so each process begins from a")
    print("different millisecond-clock state.  (That is a property of the CLI, not of the")
    print("renderer: a test binary calling RISE_CreateJobPriv directly never runs that")
    print("main and is NOT wall-clock seeded -- see tests/FabricRenderTest.cpp's header,")
    print("which sets its seed base explicitly for exactly this reason.)")
    print()
    print("NEE ON/OFF: `pathtracing_pel_rasterizer` EXPOSES NO NEE TOGGLE -- its")
    print("descriptor carries the base, filter, radiance-map, SMS, guiding, adaptive,")
    print("stability, transparent-shadow, optimal-MIS and progressive parameter blocks,")
    print("and none of them can switch next-event estimation off (`choose_one_light` is")
    print("a legacy no-op).  The gate's requested A/B is therefore run as the closest")
    print("HONEST substitute, which is arguably the sharper experiment anyway:")
    print("  DELTA   -- a single directional light.  NEE is the ONLY route to it;")
    print("             BSDF sampling can never hit a delta light, so this row isolates")
    print("             the cosine sampler's cost in the INDIRECT bounces alone.")
    print("  ENV     -- a uniform radiance map and no directional light.  Both routes")
    print("             live and MIS combines them, so the mismatch between a")
    print("             cosine-hemisphere proposal and a peaked Charlie lobe is")
    print("             actually exercised against the light.  This is the row section")
    print("             9.4's argument (\"MIS against NEE carries the variance\") is about.")
    print("render:  %dx%d, EXR 32-bit, oidn_denoise FALSE" % (width, height))
    print()

    print("  config  sheen alpha   mean L      rel.sigma    spp for %.0f%% rel.sigma" % (NOISE_FLOOR * 100))
    for env_lit in (False, True):
        cfg = "ENV  " if env_lit else "DELTA"
        for alpha in (0.08, 0.5, 1.0):
            trials = []
            for k in range(K):
                sc = sphere_scene("fabric_lambert", samples=samples,
                                  width=width, height=height,
                                  sheen_alpha=alpha, env_lit=env_lit,
                                  light=not env_lit)
                img = render(sc, "g10_%s_%s_%d" % (cfg.strip(), str(alpha).replace(".", "p"), k))
                trials.append(luminance(img))
            stack = np.stack(trials, axis=0)
            mean = stack.mean(axis=0)
            std = stack.std(axis=0, ddof=1)
            lit = mean > 1e-4
            rel = float((std[lit] / mean[lit]).mean())
            need = samples * (rel / NOISE_FLOOR) ** 2
            print("  %s   %8.2f   %.6e   %8.5f     %10.0f"
                  % (cfg, alpha, float(mean[lit].mean()), rel, need))
    print()
    print("  (spp scales as 1/N, so the last column is samples * (rel.sigma / %.2f)^2 -- the"
          % NOISE_FLOOR)
    print("   sample count this alpha would need to reach the same %.0f%% relative per-pixel"
          % (NOISE_FLOOR * 100))
    print("   noise floor.)")
    print()


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if not os.path.exists(BIN):
        print("bin/rise not found -- run from the repo root after a build", file=sys.stderr)
        return 1
    if what in ("terminator", "gate9", "all"):
        gate9()
    if what in ("aniso", "gate9b", "all"):
        gate9b()
    if what in ("variance", "gate10", "all"):
        gate10()
    return 0


if __name__ == "__main__":
    sys.exit(main())
