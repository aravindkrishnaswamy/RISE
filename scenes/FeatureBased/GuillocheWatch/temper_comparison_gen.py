#!/usr/bin/env python3
"""Generate single-dial temper-comparison scenes for the heat-tint showcase.

Each scene is one guilloché DISK (subtle relief) in the thin-film GGX material
for a chosen base metal, with the oxide thickness driven by the ABSOLUTE
radial temperature ramp (guilloche_oxide_painter output thickness_nm) and a
matte oxide-scale blend past the flaking temperature (output spall_mask ->
function2d_painter -> blend_painter on rd / rs / roughness).

Two render sets (driven by --mode):
  controlled : the SAME torch ramp temp_center_c -> temp_rim_c on every metal
               (default 200 -> 1000 C) -- which metal showcases the most colour.
  optimal    : each metal clamped to ITS OWN beautiful window (from the
               per-metal table, mirroring GuillocheField::MetalThermalModel).

Fully procedural -- nothing baked.  The renderer's Airy thin-film BSDF turns
the thickness field into colour from the real substrate/oxide n,k.
"""
import argparse, os, sys

# metal -> (substrate nk name, oxide nk name, metal0 char, temper anchors).
# The temper anchors (onsetC, optLoC, optHiC, flakeC, dLoNm, dHiNm) are the
# per-metal heat-tint model -- they MUST match the values the in-scene
# thickness/spall expressions below were validated against (the snapshot of
# the retired GuillocheField::MetalThermalModel; see tests/ThermalOxideExpr.h
# + tests/ThermalOxideExprTest.cpp).  The `optimal` mode clamps each metal to
# its own [optLoC, optHiC] window.
#                  sub      oxide    m0       onsetC optLoC optHiC flakeC dLoNm dHiNm
METALS = {
    "titanium":  ("Ti",    "TiO2",  "ti",     250,  300,   580,   650,   10,   73),
    "niobium":   ("Nb",    "Nb2O5", "nb",     200,  250,   520,   580,   12,   88),
    "tantalum":  ("Ta",    "Ta2O5", "ta",     250,  300,   560,   620,   10,   94),
    "stainless": ("Steel", "Fe3O4", "steel",  210,  230,   350,   420,   11,   93),
}
ORDER = ["titanium", "niobium", "tantalum", "stainless"]

NK = "colors/thinfilm/%s/%s.%s"   # (substrates|oxides, name, n|k)


DIALFN_UNIFORM = r"""expression_function2d
{
	name			dialfn
	param		R 20.6
	param		arms 12
	param		swirl 0
	param		seamJag 0.16
	param		seamJagFreq 3
	param		cell 0.9
	param		gridAmp 0.85
	param		petalAmp 0.3
	param		gridE0 0.12
	param		gridE1 0.5
	param		petalE0 0
	param		petalE1 0.82
	param		base 0.15
	param		landLevel 0.45
	param		reliefDepth 0.85
	param		centerRadius 0.03
	def			x (2*u-1)*R
	def			y (2*v-1)*R
	def			r hypot(x,y)
	def			rho clamp(r/R,0,1)
	def			theta atan2(y,x)
	def			jag seamJag*(2*abs(2*frac(seamJagFreq*rho)-1)-1)
	def			psi theta+swirl*rho+jag
	def			petal smoothstep(petalE0,petalE1,abs(cos(arms*psi)))
	def			wsec tau/arms
	def			q psi/wsec
	def			sector sign(q)*floor(abs(q)+0.5)
	def			thetaC sector*wsec-swirl*rho-jag
	def			cc cos(thetaC)
	def			ss sin(thetaC)
	def			xr cc*x+ss*y
	def			yr -ss*x+cc*y
	def			waxU (0.5/cell)*xr
	def			way0U (0.5/cell)*yr
	def			wrpU floor(2*waxU)
	def			wayU way0U+0.25*mod(wrpU,2)
	def			wgU smoothstep(gridE0,gridE1,abs(cos(tau*(waxU))))*smoothstep(gridE0,gridE1,abs(cos(tau*(wayU))))
	def			raw base+petalAmp*petal+gridAmp*wgU
	def			h0 clamp((raw-(0.14999999999999999))/(1.1499999999999999),0,1)
	def			h1 pow(h0,log(landLevel)/log(0.5))
	def			h2 0.5+(h1-0.5)*reliefDepth
	def			hub 0.5*(1-reliefDepth)
	def			rin max(centerRadius*R,0.000001)
	def			whub clamp(r/rin,0,1)
	def			hfin (1-whub*whub)*hub+whub*whub*h2
	expr			clamp(hfin,0,1)
}"""

def build_scene(metal, temp_lo, temp_hi, out_pattern, width, height, samples):
    sub, ox, m0, onsetC, optLoC, optHiC, flakeC, dLoNm, dHiNm = METALS[metal]
    R = 20.6
    s = []
    s.append("RISE ASCII SCENE 6")
    s.append("# Temper-comparison dial -- %s, ramp %.0f-%.0f C (fully procedural)" % (metal, temp_lo, temp_hi))
    s.append("standard_shader\n{\n\tname\t\t\tglobal\n\tshaderop\t\tDefaultPathTracing\n}")
    s.append("hdr_painter\n{\n\tname\t\t\tpnt_env\n\tfile\t\t\tlightprobes/uffizi_probe.hdr\n}")
    s.append("pathtracing_spectral_rasterizer\n{\n\tsamples\t\t\t%d\n\tnmbegin\t\t\t380\n\tnmend\t\t\t720\n"
             "\tnum_wavelengths\t\t16\n\tspectral_samples\t1\n\toidn_denoise\t\ttrue\n\toidn_prefilter\t\taccurate\n"
             "\tradiance_map\t\tpnt_env\n\tradiance_scale\t\t1.30\n\tradiance_background\tFALSE\n}" % samples)
    s.append("file_rasterizeroutput\n{\n\tpattern\t\t\t%s\n\tmultiple\t\tTRUE\n\ttype\t\t\tPNG\n\tbpp\t\t\t8\n\tcolor_space\t\tsRGB\n}" % out_pattern)
    s.append("film\n{\n\twidth\t\t\t%d\n\theight\t\t\t%d\n}" % (width, height))
    # slight 3/4 tilt off the +Z dial normal so the specular thin-film
    # reflection sweeps a wider arc of the environment (the iridescence
    # reads across the whole face instead of a dead-on cone), but shallow
    # enough that the radial temper rings stay legible.
    s.append("pinhole_camera\n{\n\tlocation\t\t0 -16 53\n\tlookat\t\t\t0 0 0\n\tup\t\t\t0 1 0\n\tfov\t\t\t46.0\n}")
    # a soft key fill so the matte oxide-scale region reads as dark grey
    # rather than pure black (the env alone barely lights the rough scale).
    s.append("omni_light\n{\n\tname\t\t\tfill\n\tpower\t\t\t9000\n\tcolor\t\t\t1.0 0.97 0.92\n\tposition\t\t-22 -30 60\n}")

    # substrate + oxide complex index
    s.append("scalar_painter\n{\n\tname\t\t\tsub_n\n\tfile\t\t\t%s\n}" % (NK % ("substrates", sub, "n")))
    s.append("scalar_painter\n{\n\tname\t\t\tsub_k\n\tfile\t\t\t%s\n}" % (NK % ("substrates", sub, "k")))
    s.append("scalar_painter\n{\n\tname\t\t\tfilm_n\n\tfile\t\t\t%s\n}" % (NK % ("oxides", ox, "n")))
    s.append("scalar_painter\n{\n\tname\t\t\tfilm_k\n\tfile\t\t\t%s\n}" % (NK % ("oxides", ox, "k")))

    # absolute-temperature oxide thickness (nm) + spall mask, on a clean
    # linear radial ramp (cool centre -> hot rim).  Authored IN-SCENE as
    # expression_function2d (was guilloche_oxide_painter output
    # thickness_nm / spall_mask) -- the falloff is `linear` so heat = rho.
    # These two expressions are tests/ThermalOxideExpr.h's
    # BuildTemperThickness / BuildSpall verbatim (golden-checked ==
    # the retired GuillocheField physics by tests/ThermalOxideExprTest).
    s.append((
        "expression_function2d\n{\n"
        "\tname\t\t\toxfn_thk\n"
        "\tparam\t\tR %.4f\n"
        "\tparam\t\ttC %.1f\n"
        "\tparam\t\ttR %.1f\n"
        "\tparam\t\tonsetC %g\n"
        "\tparam\t\toptLoC %g\n"
        "\tparam\t\toptHiC %g\n"
        "\tparam\t\tflakeC %g\n"
        "\tparam\t\tdLoNm %g\n"
        "\tparam\t\tdHiNm %g\n"
        "\tdef\t\t\trho clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)\n"
        "\tdef\t\t\theat rho\n"
        "\tdef\t\t\tT tC+heat*(tR-tC)\n"
        "\tdef\t\t\ts1 dLoNm*(T-onsetC)/max(1,optLoC-onsetC)\n"
        "\tdef\t\t\ts2 dLoNm+(dHiNm-dLoNm)*(T-optLoC)/max(1,optHiC-optLoC)\n"
        "\tdef\t\t\ts3 dHiNm+dHiNm*(T-optHiC)/max(1,flakeC-optHiC)\n"
        "\tdef\t\t\ts4 min(2*dHiNm+dHiNm*(T-flakeC)/100,3.5*dHiNm)\n"
        "\texpr\t\tselect(T<onsetC,0,select(T<optLoC,s1,select(T<=optHiC,s2,select(T<=flakeC,s3,s4))))\n"
        "}") % (R, temp_lo, temp_hi, onsetC, optLoC, optHiC, flakeC, dLoNm, dHiNm))
    s.append("scalar_painter\n{\n\tname\t\t\tfilm_thk\n\tfunction2d\t\toxfn_thk\n\tscale\t\t\t1.0\n\tbias\t\t\t0.0\n}")
    s.append((
        "expression_function2d\n{\n"
        "\tname\t\t\toxfn_spall\n"
        "\tparam\t\tR %.4f\n"
        "\tparam\t\ttC %.1f\n"
        "\tparam\t\ttR %.1f\n"
        "\tparam\t\tflakeC %g\n"
        "\tdef\t\t\trho clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)\n"
        "\tdef\t\t\theat rho\n"
        "\tdef\t\t\tT tC+heat*(tR-tC)\n"
        "\texpr\t\tsmoothstep(flakeC-22,flakeC+22,T)\n"
        "}") % (R, temp_lo, temp_hi, flakeC))
    # spall mask as a COLOUR painter (for the rd/rs colour blends)...
    s.append("function2d_painter\n{\n\tname\t\t\tspall_col\n\tfunction2d\t\toxfn_spall\n}")
    # ...and as a physical SCALAR ramp for roughness: glossy 0.08 -> matte 0.52
    # (alphax/alphay require IScalarPainter, no colour blend allowed).
    s.append("scalar_painter\n{\n\tname\t\t\trough_thk\n\tfunction2d\t\toxfn_spall\n\tscale\t\t\t0.44\n\tbias\t\t\t0.08\n}")

    # palette: thin-film tint (rs white), no diffuse (rd black); matte oxide
    # scale (dark warm diffuse, dark spec) blended in past flaking.
    for nm, col in [("pnt_black", "0 0 0"), ("pnt_white", "1 1 1"),
                    ("pnt_scale_alb", "0.05 0.043 0.038"), ("pnt_scale_spec", "0.03 0.03 0.03")]:
        s.append("uniformcolor_painter\n{\n\tname\t\t\t%s\n\tcolor\t\t\t%s\n\tcolorspace\t\tRec709RGB_Linear\n}" % (nm, col))
    # blend = colora*mask + colorb*(1-mask); mask=1 spalled.
    s.append("blend_painter\n{\n\tname\t\t\trd_b\n\tcolora\t\t\tpnt_scale_alb\n\tcolorb\t\t\tpnt_black\n\tmask\t\t\tspall_col\n}")
    s.append("blend_painter\n{\n\tname\t\t\trs_b\n\tcolora\t\t\tpnt_scale_spec\n\tcolorb\t\t\tpnt_white\n\tmask\t\t\tspall_col\n}")

    s.append("ggx_material\n{\n\tname\t\t\ttf\n\trd\t\t\trd_b\n\trs\t\t\trs_b\n\talphax\t\t\trough_thk\n\talphay\t\t\trough_thk\n"
             "\tfresnel_mode\t\tthinfilm\n\tior\t\t\tsub_n\n\textinction\t\tsub_k\n"
             "\tfilm_ior\t\tfilm_n\n\tfilm_extinction\t\tfilm_k\n\tfilm_thickness\t\tfilm_thk\n}")

    # flat Cartesian base + the guilloché relief authored IN-SCENE as an
    # expression_function2d (== GuillocheField uniform to 1e-6;
    # tests/ExpressionFunction2DTest), displaced with uv_seam_fold FALSE.
    s.append("cartesian_disk_geometry\n{\n\tname\t\t\tdisk\n\tradius\t\t\t%.4f\n\tmesh_n\t\t\t460\n}" % R)
    s.append(DIALFN_UNIFORM)
    s.append("displaced_geometry\n{\n\tname\t\t\tdialdisp\n\tbase_geometry\t\tdisk\n\tdisplacement\t\tdialfn\n\tdisp_scale\t\t0.12\n\tuv_seam_fold\t\tFALSE\n}")
    s.append("standard_object\n{\n\tname\t\t\tdial\n\tgeometry\t\tdialdisp\n\tmaterial\t\ttf\n\tposition\t\t0 0 0\n}")
    return "\n\n".join(s) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--metal", choices=ORDER, required=True)
    ap.add_argument("--mode", choices=["controlled", "optimal"], default="controlled")
    ap.add_argument("--temp-lo", type=float, default=200.0)
    ap.add_argument("--temp-hi", type=float, default=1000.0)
    ap.add_argument("--out-pattern", default=None, help="file_rasterizeroutput pattern (no extension)")
    ap.add_argument("--scene-out", required=True)
    ap.add_argument("--width", type=int, default=1600)
    ap.add_argument("--height", type=int, default=1600)
    ap.add_argument("--samples", type=int, default=128)
    a = ap.parse_args()

    if a.mode == "optimal":
        # clamp to the metal's own optimal window [optLoC, optHiC].
        lo, hi = METALS[a.metal][4], METALS[a.metal][5]
    else:
        lo, hi = a.temp_lo, a.temp_hi
    out_pattern = a.out_pattern or ("rendered/temper_%s_%s" % (a.metal, a.mode))
    scene = build_scene(a.metal, lo, hi, out_pattern, a.width, a.height, a.samples)
    with open(a.scene_out, "w") as f:
        f.write(scene)
    print("%s  %s  %.0f-%.0f C  -> %s" % (a.metal, a.mode, lo, hi, a.scene_out))


if __name__ == "__main__":
    main()
