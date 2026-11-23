#!/bin/zsh
# Simulation M3b demonstration (docs/SIMULATION_M3B_RESULTS.md): bake the M3a cloth
# demonstration with the CPU cloth solver and render a few frames of
# scenes/Tests/Simulation/simulated_cloth_drape.RISEscene with and without
# deformation motion blur.
#
#   tools/simulation/render_cloth_drape.sh [frame ...]     (default frames: 12 18 24)
#
# Bulk outputs stay outside git:
#   cache    ~/.cache/rise/simulation-m3b/cloth_drape   (private, mode 700: the cache
#            writer refuses a directory with group-writable ancestors, such as a
#            shared volume, so the bake cannot live under the checkout on /Volumes/SSD)
#   renders  rendered/simulation_m3b/                   (gitignored)
# A finished bake is reused; an interrupted one is resumed.  REBAKE=1 bakes afresh
# (rise-sim --force).  Builds needed: bin/rise and bin/tools/rise_sim.
set -eu
repo=${0:A:h:h:h}
cd "$repo"
root="$HOME/.cache/rise/simulation-m3b"
cache="$root/cloth_drape"
mkdir -p "$root"
chmod 700 "$HOME/.cache/rise" "$root"

# The M3a demonstration (SIMULATION_M3A_RESULTS.md §7), 2 s at 30 Hz.
params="$root/cloth_drape.json"
cat > "$params" <<'EOF'
{"mode": "bake", "vertices_u": 51, "vertices_v": 51, "frames": 61,
 "pins": [{"select": "u0v0"}, {"select": "u1v0"}],
 "colliders": [{"type": "sphere", "center": [0, 0.5, 0], "radius": 0.3}]}
EOF

if [[ "${REBAKE:-0}" == 1 ]]; then
	bin/tools/rise_sim bake cloth.shell "$params" "$cache" --force
elif [[ -f "$cache/manifest.cbor" ]]; then
	bin/tools/rise_sim status "$cache"
	bin/tools/rise_sim resume "$cache" || true
else
	bin/tools/rise_sim bake cloth.shell "$params" "$cache"
fi
bin/tools/rise_sim status "$cache"

frames=( "$@" )
(( ${#frames} )) || frames=( 12 18 24 )
mkdir -p rendered/simulation_m3b
export RISE_MEDIA_PATH="$repo/"
scene=scenes/Tests/Simulation/simulated_cloth_drape.RISEscene
# The same scene with the shutter closed (exposure 0) and its own output name.
noblur=rendered/simulation_m3b/simulated_cloth_drape_noblur.RISEscene
sed -e 's/^\([[:space:]]*exposure[[:space:]]*\).*/\10/' \
	-e 's|rendered/simulation_m3b/cloth_drape|rendered/simulation_m3b/cloth_drape_noblur|' "$scene" > "$noblur"
for f in "${frames[@]}"; do
	# Renders are sequential (each takes every core).
	print "renderanimation $f\nquit" | bin/rise "$scene"
	print "renderanimation $f\nquit" | bin/rise "$noblur"
done
ls -l rendered/simulation_m3b/
