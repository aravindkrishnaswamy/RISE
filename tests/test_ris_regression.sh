#!/bin/bash
#
# test_ris_regression.sh - Regression test for RIS path guiding.
#
# Renders a Cornell box at 128x128 / 64 SPP with both RIS and MIS
# guiding, then compares mean luminance and firefly counts.
#
# Requirements: RISE binary, Python 3 with PIL/Pillow and numpy.
#
# Usage:
#   cd <RISE root>
#   bash tests/test_ris_regression.sh
#
# Exit code 0 on pass, 1 on failure.
#
set -euo pipefail

RISE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${RISE_ROOT}/bin/rise"
export RISE_MEDIA_PATH="${RISE_ROOT}/"

TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "${TMPDIR}" "${RISE_ROOT}/rendered/_test_ris" 2>/dev/null; }
trap cleanup EXIT

SCENE_BASE="${TMPDIR}/base.RISEscene"

# -------------------------------------------------------------------
# Generate a minimal Cornell box scene template
# -------------------------------------------------------------------
cat > "${SCENE_BASE}" <<'SCENE'
RISE ASCII SCENE 5

> set accelerator B 10 8

pathtracing_shaderop
{
	name		pt
	branch		false
}

standard_shader
{
	name		global
	shaderop	pt
}

pixelpel_rasterizer
{
	max_recursion		6
	samples				64
	lum_samples			1
	ior_stack			TRUE
	pathguiding			true
	pathguiding_iterations	3
	pathguiding_spp			2
	pathguiding_alpha		0.5
	pathguiding_max_depth	5
	__SAMPLING_TYPE__
}

file_rasterizeroutput
{
	pattern				__OUTPUT__
	type				PNG
	bpp					8
	color_space			sRGB
}

pinhole_camera
{
	location			278 200 -200
	lookat				278 200 0
	up					0 1 0
	width				128
	height				128
	fov					80.0
}

omni_light
{
	name				thelight
	power				200000
	color				1.0 0.95 0.80
	position			278 520 278
}

uniformcolor_painter
{
	name				pnt_white
	color				0.73 0.71 0.68
}

uniformcolor_painter
{
	name				pnt_red
	color				0.63 0.06 0.04
}

uniformcolor_painter
{
	name				pnt_green
	color				0.12 0.45 0.15
}

lambertian_material
{
	name				white
	reflectance			pnt_white
}

lambertian_material
{
	name				red
	reflectance			pnt_red
}

lambertian_material
{
	name				green
	reflectance			pnt_green
}

clippedplane_geometry
{
	name				floorgeom
	pta					556 0 0
	ptb					0 0 0
	ptc					0 0 556
	ptd					556 0 556
}

clippedplane_geometry
{
	name				ceilinggeom
	pta					556 549 0
	ptb					556 549 556
	ptc					0 549 556
	ptd					0 549 0
}

clippedplane_geometry
{
	name				backwallgeom
	pta					556 0 556
	ptb					556 549 556
	ptc					0 549 556
	ptd					0 0 556
}

clippedplane_geometry
{
	name				leftwallgeom
	pta					0 0 556
	ptb					0 0 0
	ptc					0 549 0
	ptd					0 549 556
}

clippedplane_geometry
{
	name				rightwallgeom
	pta					556 0 0
	ptb					556 0 556
	ptc					556 549 556
	ptd					556 549 0
}

standard_object
{
	name				floor
	geometry			floorgeom
	material			white
}

standard_object
{
	name				ceiling
	geometry			ceilinggeom
	material			white
}

standard_object
{
	name				backwall
	geometry			backwallgeom
	material			white
}

standard_object
{
	name				leftwall
	geometry			leftwallgeom
	material			red
}

standard_object
{
	name				rightwall
	geometry			rightwallgeom
	material			green
}

sphere_geometry
{
	name				spheregeom
	radius				60
}

standard_object
{
	name				sphere
	geometry			spheregeom
	position			278 60 278
	material			white
}
SCENE

# -------------------------------------------------------------------
# Create RIS and MIS variants
# -------------------------------------------------------------------
RIS_SCENE="${TMPDIR}/ris.RISEscene"
MIS_SCENE="${TMPDIR}/mis.RISEscene"

# RISE prepends RISE_MEDIA_PATH to relative output patterns,
# so we use a relative path under the RISE root for output.
RENDER_DIR="${RISE_ROOT}/rendered/_test_ris"
mkdir -p "${RENDER_DIR}"
RIS_OUT="rendered/_test_ris/ris"
MIS_OUT="rendered/_test_ris/mis"

RIS_IMG="${RENDER_DIR}/ris.png"
MIS_IMG="${RENDER_DIR}/mis.png"

# Remove any stale output so a failed render cannot silently
# reuse images from a previous run.
rm -f "${RIS_IMG}" "${MIS_IMG}"

sed -e "s|__SAMPLING_TYPE__|pathguiding_sampling_type\tris\n\tpathguiding_ris_candidates\t2|" \
    -e "s|__OUTPUT__|${RIS_OUT}|" \
    "${SCENE_BASE}" > "${RIS_SCENE}"

sed -e "s|__SAMPLING_TYPE__|pathguiding_sampling_type\tmis|" \
    -e "s|__OUTPUT__|${MIS_OUT}|" \
    "${SCENE_BASE}" > "${MIS_SCENE}"

# -------------------------------------------------------------------
# Render
# -------------------------------------------------------------------
echo "=== Rendering RIS ==="
printf "render\nquit\n" | "${BIN}" "${RIS_SCENE}" > "${TMPDIR}/ris_log.txt" 2>&1 || true
grep -E "Initialized|Training.*complete|Written|Time" "${TMPDIR}/ris_log.txt" || true

echo ""
echo "=== Rendering MIS ==="
printf "render\nquit\n" | "${BIN}" "${MIS_SCENE}" > "${TMPDIR}/mis_log.txt" 2>&1 || true
grep -E "Initialized|Training.*complete|Written|Time" "${TMPDIR}/mis_log.txt" || true

if [ ! -f "${RIS_IMG}" ] || [ ! -f "${MIS_IMG}" ]; then
    echo "FAIL: one or both renders did not produce output"
    exit 1
fi

# -------------------------------------------------------------------
# Numerical analysis
# -------------------------------------------------------------------
echo ""
echo "=== Regression Analysis ==="
python3 - "${RIS_IMG}" "${MIS_IMG}" <<'PYEOF'
import sys
import numpy as np
from PIL import Image

ris_path, mis_path = sys.argv[1], sys.argv[2]
ris = np.array(Image.open(ris_path).convert("RGB"), dtype=np.float64)
mis = np.array(Image.open(mis_path).convert("RGB"), dtype=np.float64)

def luminance(img):
    return img[:,:,0]*0.2126 + img[:,:,1]*0.7152 + img[:,:,2]*0.0722

ris_lum = luminance(ris)
mis_lum = luminance(mis)

ris_mean = ris_lum.mean()
mis_mean = mis_lum.mean()
lum_diff = abs(ris_mean - mis_mean) / max(mis_mean, 1e-10) * 100

# Firefly detection: lum > 250 and 5x5 neighbor mean < 150
def count_fireflies(lum, thresh_hi=250, thresh_nbr=150):
    from scipy.ndimage import uniform_filter
    nbr = uniform_filter(lum, size=5, mode='reflect')
    return int(np.sum((lum > thresh_hi) & (nbr < thresh_nbr)))

try:
    ris_ff = count_fireflies(ris_lum)
    mis_ff = count_fireflies(mis_lum)
except ImportError:
    # Fallback without scipy
    ris_ff = int(np.sum(ris_lum > 250))
    mis_ff = int(np.sum(mis_lum > 250))

# Floor variance (rows 90-120, cols 30-90)
ris_var = ris_lum[90:120, 30:90].var()
mis_var = mis_lum[90:120, 30:90].var()
var_ratio = ris_var / max(mis_var, 1e-10)

print(f"  RIS mean luminance:  {ris_mean:.2f}")
print(f"  MIS mean luminance:  {mis_mean:.2f}")
print(f"  Luminance diff:      {lum_diff:.2f}%")
print(f"  RIS fireflies:       {ris_ff}")
print(f"  MIS fireflies:       {mis_ff}")
print(f"  RIS floor variance:  {ris_var:.1f}")
print(f"  MIS floor variance:  {mis_var:.1f}")
print(f"  Variance ratio:      {var_ratio:.2f}")
print()

# Thresholds
FAIL = False
if lum_diff > 5.0:
    print(f"  FAIL: luminance difference {lum_diff:.2f}% exceeds 5% threshold")
    FAIL = True
else:
    print(f"  PASS: luminance within {lum_diff:.2f}%")

if ris_ff > mis_ff * 3 + 10:
    print(f"  FAIL: RIS has {ris_ff} fireflies vs MIS {mis_ff} (> 3x + 10)")
    FAIL = True
else:
    print(f"  PASS: firefly count comparable ({ris_ff} vs {mis_ff})")

if var_ratio > 2.0:
    print(f"  FAIL: RIS variance ratio {var_ratio:.2f} exceeds 2.0")
    FAIL = True
else:
    print(f"  PASS: variance ratio {var_ratio:.2f}")

print()
if FAIL:
    print("  VERDICT: FAIL")
    sys.exit(1)
else:
    print("  VERDICT: PASS")
    sys.exit(0)
PYEOF
