#!/bin/zsh
# r273 run A: resume tier8_y030 step 13769 (22.46 s) for 2.5 s with gross soot yield 0.3 and soot radiation
# (cap $2, default 0.45); sheet oxidation gate $3 (default 0). Control: r272 sooty_y300 (same start, both opt-ins off).
# usage: run_tier8_a.sh <arm name> [cap] [sheet 0|1]
R=/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn
CK=/Volumes/SSD/RISE/fire_production_calibration/r272_soot/tier8_y030/checkpoint_history/step_0000013769.checkpoint
ARM=$1; CAP=${2:-0.45}; SHEET=${3:-0}
[ -e $R/$ARM ] && { echo "$ARM exists; refusing to overwrite"; exit 1; }
unset RISE_MEDIA_PATH RISE_FIRE_SOOT_SHEET_OXIDATION
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=0.3
export RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=$CAP
export RISE_R273_FRAME_ORIGIN_S=10.466040127156969   # the tier-8 statistics start, so frames line up with sooty_y300
[ "$SHEET" = 1 ] && export RISE_FIRE_SOOT_SHEET_OXIDATION=1
cd /Volumes/SSD/Working/RISE
exec /usr/bin/caffeinate -i $R/build/ab_driver_tier $CK $R/$ARM 2.5 8 > $R/$ARM.log 2>&1
