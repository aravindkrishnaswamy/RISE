#!/bin/zsh
# r272 sooty-fuel check: resume tier8_y030 step 13769 for 2.5 s with gross soot yield $1 into $R/$2
R=/Volumes/SSD/RISE/fire_production_calibration/r272_soot
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=$1
exec /usr/bin/caffeinate -i /Volumes/SSD/RISE/fire_production_calibration/r271_compact_stencil/ab_build_soot50/ab_driver \
  $R/tier8_y030/checkpoint_history/step_0000013769.checkpoint $R/$2 2.5 > $R/$2.log 2>&1
