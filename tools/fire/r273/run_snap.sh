#!/bin/zsh
# r273 diagnostic: short tier-8 replay of run A (resume tier8_y030 step 13769, yield 0.3, sheet gate off) that keeps
# a full checkpoint every $3 accepted steps, so the collapse can be examined with frozen_response/radiation_census.
# Needs ab_driver_tier built by build_tool.py into a second directory (snapshot patch; unset it changes nothing).
# usage: run_snap.sh <arm name> <cap> <window s> <every n steps> <build dir> [extra VAR=value ...]
R=/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn
CK=/Volumes/SSD/RISE/fire_production_calibration/r272_soot/tier8_y030/checkpoint_history/step_0000013769.checkpoint
ARM=$1; CAP=$2; WINDOW=$3; EVERY=$4; BIN=$5; shift 5
[ -e $R/$ARM ] && { echo "$ARM exists; refusing to overwrite"; exit 1; }
unset RISE_MEDIA_PATH RISE_FIRE_SOOT_SHEET_OXIDATION RISE_FIRE_SOOT_SELF_ABSORPTION
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=0.3
export RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=$CAP
export RISE_R273_FRAME_ORIGIN_S=10.466040127156969
export RISE_R273_SNAPSHOT_EVERY=$EVERY RISE_R273_SNAPSHOT_DIR=$R/$ARM.snap
for kv in "$@"; do export "$kv"; done
mkdir -p $R/$ARM.snap
cd /Volumes/SSD/Working/RISE
exec /usr/bin/caffeinate -i $BIN/ab_driver_tier $CK $R/$ARM $WINDOW 8 > $R/$ARM.log 2>&1
