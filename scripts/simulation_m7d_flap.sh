#!/bin/zsh
# Simulation M7d: bake a dam-break wave hitting a hanging flap (a sheet as a
# moving boundary of liquid.mac, the load returned to cloth.shell;
# scenes/Tests/Simulation/liquid_cloth_flap.RISEscene, `simulation_group flume`) and render frames of
# the water surface (dielectric, ior 1.33) and the flap (simulated_mesh_geometry)
# with path tracing, CPU only, to rendered/simulation_m7/ (outside git).
# The scene and its caches live in a private directory under ~/.cache/rise-m7d
# (the cache writer refuses directories others can modify).
#   scripts/simulation_m7d_flap.sh [resolution]   (default 64 cells along x)
set -euo pipefail
repo=${0:A:h:h}
resolution=${1:-64}
work=$HOME/.cache/rise-m7d/flap-$resolution
out=$repo/rendered/simulation_m7
mkdir -p $HOME/.cache/rise-m7d $work $out
chmod 700 $HOME/.cache/rise-m7d $work
sed "s/^\tresolution\t\t64$/\tresolution\t\t$resolution/" $repo/scenes/Tests/Simulation/liquid_cloth_flap.RISEscene > $work/flap.RISEscene
time $repo/bin/tools/rise_sim bake $work/flap.RISEscene flume
$repo/bin/tools/rise_sim status $work/flap.RISEscene
# A render copy: film, frames 0, 12, 24, 36, 48, 60 (t = 0 ... 2 s), PNG output.
{
  cat $work/flap.RISEscene
  print "film\n{\n\twidth\t640\n\theight\t400\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t2\n\tnum_frames\t6\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m7/flap_$resolution\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n\tcolor_space\tsRGB\n}\n"
} > $work/flap_render.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/flap_render.RISEscene
ls -l $out
