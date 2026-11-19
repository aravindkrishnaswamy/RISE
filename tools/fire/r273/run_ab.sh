#!/bin/zsh
# r273 tier-6 A/B arm: resume the retained r270 tier-6 state (step 1466, 2.41 s, floor on, default stencil, no soot)
# for $5 s with gross soot yield $2, soot radiation cap $3 ("-" = off) and sheet oxidation gate $4 (0/1).
# usage: run_ab.sh <arm name> <yield> <cap|-> <sheet 0|1> <window s>
# The numerics stay those of the checkpoint (no compact stencil, carrier or Vreman zone), so the flame is not
# disturbed by anything except the soot settings. Never a tier-8 run.
R=/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn
CK=/Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation/tier6_eps1/checkpoint_history/step_0000001466.checkpoint
ARM=$1; YIELD=$2; CAP=$3; SHEET=$4; WINDOW=$5
[ -e $R/$ARM ] && { echo "$ARM exists; refusing to overwrite"; exit 1; }
unset RISE_MEDIA_PATH RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP RISE_FIRE_SOOT_SHEET_OXIDATION
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_METHANE_SOOT_YIELD=$YIELD
export RISE_R273_FRAME_ORIGIN_S=2.5   # frames at 2.5 s + k/16 s (the state starts at 2.414 s)
[ "$CAP" != "-" ] && export RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=$CAP
[ "$SHEET" = 1 ] && export RISE_FIRE_SOOT_SHEET_OXIDATION=1
cd /Volumes/SSD/Working/RISE
exec /usr/bin/caffeinate -i $R/build/ab_driver_tier $CK $R/$ARM $WINDOW 6 > $R/$ARM.log 2>&1
