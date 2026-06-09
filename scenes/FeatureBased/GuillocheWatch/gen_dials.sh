#!/usr/bin/env bash
# Regenerate EVERY guilloché-watch dial mesh.  The dial*.raw2 meshes are large
# (tens of MB) and gitignored — THIS SCRIPT IS THE SOURCE OF TRUTH for the exact
# parameters of each shipped dial pattern.  Run it after cloning, or whenever you
# want to rebuild the dials:
#
#     ./gen_dials.sh
#
# Every pattern shares the stock Cartesian UV, so every oxide map / palette /
# metal applies to each — a pattern only changes the RELIEF.  The scene
# (watch_dial.RISEscene) carries each as a `rawmesh2_geometry dialmesh_<name>`;
# the dial object's `geometry` is a LIVE GUI rebind (SceneEdit::SetObjectGeometry).
set -euo pipefail
cd "$(dirname "$0")"

echo "[1/6] uniform  — stock single-cell woven dial            -> dial.raw2"
python3 dial_mesh_gen.py        # also (re)bakes the committed oxide maps (deterministic)

echo "[2/6] lightning — MING hero: 11 zigzag bolts of a tight cube on a uniform rung ground -> dial_lightning.raw2"
python3 dial_variants_gen.py --field lightning --num-arms 11 --cell-mode select \
  --lightning-relief 0.6 --lightning-lo 0.45 --lightning-hi 0.65 --center-radius 0.015 \
  --zigzag-amp 0.16 --zigzag-freq 3.0 --bolt-style rung --rung-len 0.65 --rung-width 0.82 \
  --field-cell 0.50 --field-frame global --mesh-n 880 --out dial_lightning.raw2

echo "[3/6] radial    — earlier swirled-petal lightning at a coarser bolt cell -> dial_radial.raw2"
python3 dial_variants_gen.py --field radial --cell-mode select --lightning-cell-scale 1.8 \
  --lightning-lo 0.25 --lightning-hi 0.65 --out dial_radial.raw2

echo "[4/6] iris      — 007 camera aperture (8 blades)         -> dial_iris.raw2"
python3 dial_variants_gen.py --field iris --num-arms 8 --iris-aperture 0.13 --iris-swirl 0.6 \
  --cell 0.8 --grid-amp 0.95 --lightning-relief 0.0 --center-radius 0.0 --mesh-n 820 --out dial_iris.raw2

echo "[5/6] swirl     — log-spiral guilloché                   -> dial_swirl.raw2"
python3 dial_variants_gen.py --field swirl --num-arms 6 --swirl-turns 7.0 --cell 0.8 \
  --grid-amp 0.95 --center-radius 0.02 --mesh-n 820 --out dial_swirl.raw2

echo "[6/6] varwidth  — alternating fine/coarse sunburst sectors -> dial_varwidth.raw2"
python3 dial_variants_gen.py --field varwidth --num-arms 8 --lightning-cell-scale 2.6 --cell 0.6 \
  --grid-amp 0.95 --center-radius 0.02 --mesh-n 820 --out dial_varwidth.raw2

echo "done — all dial meshes regenerated (dial*.raw2 stay gitignored)."
