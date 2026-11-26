#!/bin/zsh
# r272 sooty showcase: tier-8 full 25 s window from zero, accepted r271 candidate plus gross soot yield 0.3.
# Runs a private copy of FireSequenceTest (bin_y300/fire_y300_solver) so rebuilding the tree cannot disturb it.
cd /Volumes/SSD/RISE/fire_production_calibration/r272_soot
[ -e tier8_y300 ] && { echo "tier8_y300 exists; refusing to overwrite"; exit 1; }
git -C /Volumes/SSD/Working/RISE rev-parse HEAD > tier8_y300.commit
shasum -a 256 bin_y300/fire_y300_solver >> tier8_y300.commit
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=0.3
exec /usr/bin/caffeinate -i bin_y300/fire_y300_solver \
  --fire-production-temporal-capstone 8 0.0625 tier8_y300 > tier8_y300.log 2>&1
