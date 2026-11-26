#!/bin/zsh
# Simulation M7b: bake three rigid bodies dropped into a liquid tank
# (scenes/Tests/Simulation/liquid_rigid_bodies.RISEscene: liquid.mac owning
# rigid.bodies' exchange in `simulation_group tank`) and render frames of
# the water surface (dielectric, ior 1.33) and the bodies (transform_source)
# with path tracing, CPU only, to rendered/simulation_m7/ (outside git).
# The scene and its caches live in a private directory under ~/.cache/rise-m7b
# (the cache writer refuses directories others can modify).
#   scripts/simulation_m7b_bodies.sh [resolution]   (default 64 cells along x)
set -euo pipefail
repo=${0:A:h:h}
resolution=${1:-64}
work=$HOME/.cache/rise-m7b/bodies-$resolution
out=$repo/rendered/simulation_m7
mkdir -p $HOME/.cache/rise-m7b $work $out
chmod 700 $HOME/.cache/rise-m7b $work
sed "s/^\tresolution\t\t64$/\tresolution\t\t$resolution/" $repo/scenes/Tests/Simulation/liquid_rigid_bodies.RISEscene > $work/bodies.RISEscene
time $repo/bin/tools/rise_sim bake $work/bodies.RISEscene tank
$repo/bin/tools/rise_sim status $work/bodies.RISEscene
# A render copy: film, frames 0, 12, 24, 36, 48, 60 (t = 0 ... 2 s), PNG output.
{
  cat $work/bodies.RISEscene
  print "film\n{\n\twidth\t640\n\theight\t400\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t2\n\tnum_frames\t6\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m7/bodies_$resolution\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n\tcolor_space\tsRGB\n}\n"
} > $work/bodies_render.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/bodies_render.RISEscene
ls -l $out
