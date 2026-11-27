#!/bin/zsh
# Simulation M7: bake the liquid dam break against a box obstacle
# (scenes/Tests/Simulation/liquid_dam_break.RISEscene, solver liquid.mac) and
# render a few frames of its surface as water (dielectric, ior 1.33) with
# path tracing, CPU only, to rendered/simulation_m7/ (outside git).
# The scene lives in a private directory under ~/.cache/rise-m7
# (the cache writer refuses directories others can modify).
#   scripts/simulation_m7_dam_break.sh [resolution]   (default 48 cells along x)
set -euo pipefail
repo=${0:A:h:h}
resolution=${1:-48}
work=$HOME/.cache/rise-m7/dam-break-$resolution
out=$repo/rendered/simulation_m7
mkdir -p $HOME/.cache/rise-m7 $work $out
chmod 700 $HOME/.cache/rise-m7 $work
sed "s/^\tresolution\t\t48$/\tresolution\t\t$resolution/" $repo/scenes/Tests/Simulation/liquid_dam_break.RISEscene > $work/dam_break.RISEscene
time $repo/bin/tools/rise_sim bake $work/dam_break.RISEscene pool
$repo/bin/tools/rise_sim status $work/dam_break.RISEscene
# A render copy: film, frames 0, 9, 18, 27, 36, 45 (t = 0 ... 1.5 s), PNG output.
{
  cat $work/dam_break.RISEscene
  print "film\n{\n\twidth\t640\n\theight\t400\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t1.5\n\tnum_frames\t6\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m7/dam_break_$resolution\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n\tcolor_space\tsRGB\n}\n"
} > $work/dam_break.render.tmp
# The render copy keeps the baked scene's path: the default cache directory is keyed by it.
mv $work/dam_break.render.tmp $work/dam_break.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/dam_break.RISEscene
ls -l $out
