#!/usr/bin/env bash
# =============================================================================
# generate_references.sh -- regenerate the four committed reference PNGs for the
# RISE image->scene-reconstruction eval (Wave 3) from the ground-truth scene.
#
# Deterministic and re-runnable.  For each of the four poses it derives a temp
# scene variant with that camera (the ground-truth scene's own camera IS view1),
# renders it through RISE's normal render->PNG path at 256x256 / 256 spp, and
# moves the PNG to evals/references/viewN.png.  The four poses are the machine-
# readable record in POSES.json; keep the two in sync if you edit either.
#
# Usage (from the repo root):
#   ./evals/references/generate_references.sh
#
# References MUST be produced by this pipeline (RISE's own render->PNG path) so
# the Wave-2 render checkpoint can compare grading renders against them with a
# pipeline-identical RMSE.
# =============================================================================
set -euo pipefail

# Resolve repo root (this script lives in evals/references/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

export RISE_MEDIA_PATH="$REPO_ROOT/"
RISE_BIN="$REPO_ROOT/bin/rise"
TRUTH="$SCRIPT_DIR/sdf_reconstruct_truth.RISEscene"
OUT_PNG="$REPO_ROOT/rendered/sdf_reconstruct_truth.png"

if [ ! -x "$RISE_BIN" ]; then
	echo "ERROR: $RISE_BIN not found. Build it: make -C build/make/rise -j8 all" >&2
	exit 1
fi

mkdir -p "$REPO_ROOT/rendered"
TMPDIR_R="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_R"' EXIT

# render_pose <viewName> <lx> <ly> <lz> <ax> <ay> <az> <fov>
render_pose() {
	local name="$1" lx="$2" ly="$3" lz="$4" ax="$5" ay="$6" az="$7" fov="$8"
	local scene="$TMPDIR_R/$name.RISEscene"
	# Replace the camera location/lookat/fov lines (unique to pinhole_camera).
	sed -E \
		-e "s/^([[:space:]]*)location[[:space:]].*/\1location\t$lx $ly $lz/" \
		-e "s/^([[:space:]]*)lookat[[:space:]].*/\1lookat\t$ax $ay $az/" \
		-e "s/^([[:space:]]*)fov[[:space:]].*/\1fov\t$fov/" \
		"$TRUTH" > "$scene"
	rm -f "$OUT_PNG"
	# rise returns a non-zero exit code even on a clean render, so don't gate on it;
	# verify the PNG was actually produced instead.
	printf "render\nquit\n" | "$RISE_BIN" "$scene" >/dev/null 2>&1 || true
	if [ ! -f "$OUT_PNG" ]; then
		echo "ERROR: render of $name produced no PNG at $OUT_PNG" >&2
		exit 1
	fi
	mv -f "$OUT_PNG" "$SCRIPT_DIR/$name.png"
	echo "wrote $SCRIPT_DIR/$name.png"
}

# The four poses (must match POSES.json).
# view1 -- primary front-3/4 (identical to the ground-truth scene camera).
render_pose view1  2.6 1.8  3.2   0 0.8 0  40.0
# view2 -- right SIDE (+X), the notch face-on.
render_pose view2  4.0 1.5  0.2   0 0.9 0  40.0
# view3 -- back-3/4 (+X,-Z; leans toward the notch side so it's measurably
# visible on the silhouette -- a pure (-X,-Z) back-left view self-occludes
# the +X notch entirely, see CALIBRATION.md).
render_pose view3  2.0 1.8 -3.2   0 0.8 0  40.0
# view4 -- elevated top-down-ish (~60 deg elevation).
render_pose view4  1.4 4.6  1.7   0 0.7 0  45.0

echo "All four reference views regenerated."
