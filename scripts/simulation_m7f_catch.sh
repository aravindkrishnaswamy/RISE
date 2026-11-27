#!/bin/zsh
# Simulation M7f: bake a cotton cloth (pinned along its four edges) catching a falling block of
# water (a sheet coupled implicitly with liquid.mac that holds the liquid on one side;
# scenes/Tests/Simulation/liquid_cloth_catch.RISEscene, `simulation_group catch`) and render frames
# of the water surface (dielectric, ior 1.33) and the cloth (simulated_mesh_geometry) with path
# tracing, CPU only, to rendered/simulation_m7/ (outside git).
# The scene lives in a private directory under ~/.cache/rise-m7f
# (the cache writer refuses directories others can modify).
#   scripts/simulation_m7f_catch.sh [resolution]   (default 64 cells along x)
set -euo pipefail
repo=${0:A:h:h}
resolution=${1:-64}
work=$HOME/.cache/rise-m7f/catch-$resolution
out=$repo/rendered/simulation_m7
mkdir -p $HOME/.cache/rise-m7f $work $out
chmod 700 $HOME/.cache/rise-m7f $work
sed "s/^\tresolution\t\t64$/\tresolution\t\t$resolution/" $repo/scenes/Tests/Simulation/liquid_cloth_catch.RISEscene > $work/catch.RISEscene
time $repo/bin/tools/rise_sim bake $work/catch.RISEscene catch
$repo/bin/tools/rise_sim status $work/catch.RISEscene
# A render copy: film, frames at t = 0, 0.2, ..., 1.6 s (9 frames), PNG output.
{
  cat $work/catch.RISEscene
  print "film\n{\n\twidth\t640\n\theight\t400\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t1.6\n\tnum_frames\t9\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m7/catch_$resolution\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n\tcolor_space\tsRGB\n}\n"
} > $work/catch.render.tmp
# The render copy keeps the baked scene's path: the default cache directory is keyed by it.
mv $work/catch.render.tmp $work/catch.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/catch.RISEscene
ls -l $out
