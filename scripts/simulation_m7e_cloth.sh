#!/bin/zsh
# Simulation M7e: bake a dam-break wave hitting a hanging cotton cloth (a sheet
# coupled implicitly with liquid.mac, cloth.shell taking the liquid's steps;
# scenes/Tests/Simulation/liquid_cloth_wave.RISEscene, `simulation_group flume`) and render frames of
# the water surface (dielectric, ior 1.33) and the cloth (simulated_mesh_geometry)
# with path tracing, CPU only, to rendered/simulation_m7/ (outside git).
# The scene lives in a private directory under ~/.cache/rise-m7e
# (the cache writer refuses directories others can modify).
#   scripts/simulation_m7e_cloth.sh [resolution]   (default 64 cells along x)
set -euo pipefail
repo=${0:A:h:h}
resolution=${1:-64}
work=$HOME/.cache/rise-m7e/cloth-$resolution
out=$repo/rendered/simulation_m7
mkdir -p $HOME/.cache/rise-m7e $work $out
chmod 700 $HOME/.cache/rise-m7e $work
sed "s/^\tresolution\t\t64$/\tresolution\t\t$resolution/" $repo/scenes/Tests/Simulation/liquid_cloth_wave.RISEscene > $work/cloth.RISEscene
time $repo/bin/tools/rise_sim bake $work/cloth.RISEscene flume
$repo/bin/tools/rise_sim status $work/cloth.RISEscene
# A render copy: film, frames 0, 12, 24, 36, 48, 60 (t = 0 ... 2 s), PNG output.
{
  cat $work/cloth.RISEscene
  print "film\n{\n\twidth\t640\n\theight\t400\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t2\n\tnum_frames\t6\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m7/cloth_$resolution\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n\tcolor_space\tsRGB\n}\n"
} > $work/cloth.render.tmp
# The render copy keeps the baked scene's path: the default cache directory is keyed by it.
mv $work/cloth.render.tmp $work/cloth.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/cloth.RISEscene
ls -l $out
