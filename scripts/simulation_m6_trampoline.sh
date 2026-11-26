#!/bin/zsh
# Simulation M6: bake the two-way rigid <-> cloth trampoline
# (scenes/Tests/Simulation/rigid_cloth_trampoline.RISEscene) as one coupled
# run and render a few frames, CPU only, to rendered/simulation_m6/.
# The scene and its caches are copied to a private directory under
# ~/.cache/rise-m6 (the cache writer refuses directories others can modify).
set -euo pipefail
repo=${0:A:h:h}
work=$HOME/.cache/rise-m6/trampoline-render
out=$repo/rendered/simulation_m6
mkdir -p $HOME/.cache/rise-m6 $work $out
chmod 700 $HOME/.cache/rise-m6 $work
cp $repo/scenes/Tests/Simulation/rigid_cloth_trampoline.RISEscene $work/trampoline.RISEscene
$repo/bin/tools/rise_sim bake $work/trampoline.RISEscene trampoline
$repo/bin/tools/rise_sim status $work/trampoline.RISEscene
# A render copy: small film, an animation over the bake (frames 0, 10, 20, 30 at 30 Hz), PNG frames.
{
  cat $work/trampoline.RISEscene
  print "film\n{\n\twidth\t200\n\theight\t150\n}\n"
  print "animation_options\n{\n\ttime_start\t0\n\ttime_end\t1\n\tnum_frames\t4\n}\n"
  print "file_rasterizeroutput\n{\n\tpattern\trendered/simulation_m6/trampoline\n\tmultiple\tTRUE\n\ttype\tPNG\n\tbpp\t8\n}\n"
} > $work/trampoline_render.RISEscene
cd $repo
export RISE_MEDIA_PATH="$repo/"
printf "renderanimation\nquit\n" | ./bin/rise $work/trampoline_render.RISEscene
ls -l $out
