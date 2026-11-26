#!/bin/zsh
# Simulation M7c: bake the same dam break in honey (viscosity 10 Pa s, surface
# tension 0.07 N/m) and in water (inviscid), side by side
# (scenes/Tests/Simulation/liquid_honey_water.RISEscene, solver liquid.mac),
# and render frames of both surfaces as dielectrics with path tracing, CPU
# only, to rendered/simulation_m7/ (outside git).  The scene and its caches live
# in a private directory under ~/.cache/rise-m7c (the cache writer refuses
# directories others can modify).
#   scripts/simulation_m7c_honey.sh [resolution]   (default 48 cells along x)
set -euo pipefail
repo=${0:A:h:h}
resolution=${1:-48}
work=$HOME/.cache/rise-m7c/honey-$resolution
out=$repo/rendered/simulation_m7
mkdir -p $HOME/.cache/rise-m7c $work $out
chmod 700 $HOME/.cache/rise-m7c $work
sed "s/^\tresolution\t\t48$/\tresolution\t\t$resolution/" $repo/scenes/Tests/Simulation/liquid_honey_water.RISEscene > $work/honey.RISEscene
time $repo/bin/tools/rise_sim bake $work/honey.RISEscene water
time $repo/bin/tools/rise_sim bake $work/honey.RISEscene honey
$repo/bin/tools/rise_sim status $work/honey.RISEscene
# A render copy: film, frames 0, 12, 24, 36, 48, 60 (t = 0 ... 2 s), PNG output.
{
  cat $work/honey.RISEscene
  print "film\n{\n\twidth\t640\n\theight\t400\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t2\n\tnum_frames\t6\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m7/honey_$resolution\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n\tcolor_space\tsRGB\n}\n"
} > $work/honey_render.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/honey_render.RISEscene
ls -l $out
