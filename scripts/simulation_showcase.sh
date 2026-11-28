#!/bin/zsh
# Simulation showcase driver: bake and render the scenes in scenes/Showcase/Simulation/
# (docs/SIMULATION_SHOWCASE.md).
#
#   scripts/simulation_showcase.sh proof|final <scene|all> [bake|render|both] [frame ...]
#
#   proof   a value written `<final value>  # proof <proof value>` in the scene takes the proof value:
#           coarse grids, small cloth meshes, the lowest fire tier, 640 px, few samples.
#   final   the scene as committed (the final values).
#   scene   the file stem (hearth, unveiling, ...) or `all`.
#   step    bake (simulations only), render (frames only, from an existing bake) or both (default).
#   frame   frame indices to render; default the scene's `showcase:` header line (hero frame and the
#           others) in proof mode, the hero frame in final mode, or `animation` for every frame.
#
# Each mode writes its own copy of the scene, rendered/showcase/<scene>/<scene>.<mode>.RISEscene,
# bakes it with rise_sim and renders it with rise from the same path (the default cache folder of a
# simulation is keyed by the scene's path, so bake and render must read the same file).  Caches go to
# $RISE_SIMULATION_CACHE_DIR when it is set, otherwise to the per-user default; frames go to
# rendered/showcase/<scene>/<mode>/ (outside git).
#
# Environment:
#   RISE_BIN, RISE_SIM_BIN   the renderer and the bake tool (default bin/rise, bin/tools/rise_sim);
#                            point them at private copies when another agent may relink bin/.
#   RISE_SIMULATION_CACHE_DIR, RISE_SIMULATION_TRUSTED_ROOTS   passed through to both.
#   SHOWCASE_SIM_ARGS        extra rise_sim options (e.g. --checkpoint-seconds 120).
set -euo pipefail

repo=${0:A:h:h}
mode=${1:-}
which=${2:-}
step=${3:-both}
(( $# >= 3 )) && shift 3 || shift $#
if [[ $mode != proof && $mode != final ]] || [[ -z $which ]] || [[ $step != bake && $step != render && $step != both ]]; then
  print -u2 "usage: $0 proof|final <scene|all> [bake|render|both] [frame ... | animation]"
  exit 2
fi
rise=${RISE_BIN:-$repo/bin/rise}
rise_sim=${RISE_SIM_BIN:-$repo/bin/tools/rise_sim}
src_dir=$repo/scenes/Showcase/Simulation
export RISE_MEDIA_PATH="$repo/"

if [[ $which == all ]]; then
  scenes=(${src_dir}/*.RISEscene(:t:r))
else
  scenes=($which)
fi

# The scene variant for a mode: in proof mode `<key> <final ...>  # proof <proof ...>` becomes `<key> <proof ...>`.
variant() {
  local in=$1 out=$2
  if [[ $mode == proof ]]; then
    sed -E 's/^([[:space:]]*)([A-Za-z_]+)([[:space:]]+)[^#]*#[[:space:]]*proof[[:space:]]+(.*[^[:space:]])[[:space:]]*$/\1\2\3\4/' $in > $out
  else
    cp $in $out
  fi
}

# The simulations to bake: simulation_group names, plus producers that are no group's member.
bake_targets() {
  awk '
    /^[a-z_]+$/ { chunk = $1; next }
    /^}/ { chunk = ""; next }
    $1 == "name" && (chunk == "simulation_group" || chunk ~ /_simulation$/) { kind[$2] = chunk; order[++n] = $2 }
    $1 == "members" && chunk == "simulation_group" { member[$2] = 1 }
    END { for (i = 1; i <= n; i++) { s = order[i]; if (kind[s] == "simulation_group" || !(s in member)) print s } }
  ' $1
}

for scene in $scenes; do
  src=$src_dir/$scene.RISEscene
  [[ -f $src ]] || { print -u2 "no scene $src"; exit 2; }
  out_dir=$repo/rendered/showcase/$scene
  mkdir -p $out_dir/$mode
  work=$out_dir/$scene.$mode.RISEscene
  variant $src $work
  header=$(grep -m1 '#[[:space:]]*showcase:' $src || true)
  hero=$(print -r -- $header | sed -nE 's/.*hero[[:space:]]+([0-9]+).*/\1/p')
  others=($(print -r -- $header | sed -nE 's/.*frames[[:space:]]+([0-9 ]+).*/\1/p'))

  if [[ $step != render ]]; then
    for target in $(bake_targets $work); do
      print "== $scene ($mode): bake $target"
      /usr/bin/time -l $rise_sim bake $work $target ${=SHOWCASE_SIM_ARGS:-} 2>&1 | tee $out_dir/$mode/bake_$target.log
      [[ ${pipestatus[1]} == 0 ]] || { print -u2 "bake of $target failed (log: $out_dir/$mode/bake_$target.log)"; exit 1; }
    done
    $rise_sim status $work | tee $out_dir/$mode/status.txt
  fi

  if [[ $step != bake ]]; then
    if (( $# > 0 )); then frames=($@)
    elif [[ $mode == proof ]]; then frames=($hero $others)
    else frames=($hero)
    fi
    cd $repo
    if [[ ${frames[1]} == animation ]]; then
      print "== $scene ($mode): render every frame"
      printf "renderanimation\nquit\n" | $rise $work 2>&1 | tee $out_dir/$mode/render_animation.log
    else
      for f in $frames; do
        print "== $scene ($mode): render frame $f"
        printf "renderanimation $f\nquit\n" | $rise $work > $out_dir/$mode/render_$f.log 2>&1
        grep -E "Total Rasterization Time|rror" $out_dir/$mode/render_$f.log | head -5 || true
      done
    fi
    ls -l $out_dir/$mode | grep -E '\.(png|exr)$' || true
  fi
done
