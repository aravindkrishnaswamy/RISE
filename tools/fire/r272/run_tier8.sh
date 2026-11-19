#!/bin/zsh
# r272 tier-8 full 25 s window from zero: accepted r271 candidate (compact stencil, limited carrier,
# Vreman zone below:16:0.2857, floor emissivity 1) plus methane gross soot yield 0.03.
cd /Volumes/SSD/RISE/fire_production_calibration/r272_soot
[ -e tier8_y030 ] && { echo "tier8_y030 exists; refusing to overwrite"; exit 1; }
git -C /Volumes/SSD/Working/RISE rev-parse HEAD > tier8_y030.commit
shasum -a 256 /Volumes/SSD/Working/RISE/bin/tests/FireSequenceTest >> tier8_y030.commit
git -C /Volumes/SSD/Working/RISE diff -- src tests | shasum -a 256 >> tier8_y030.commit
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=0.03
exec /usr/bin/caffeinate -i /Volumes/SSD/Working/RISE/bin/tests/FireSequenceTest \
  --fire-production-temporal-capstone 8 0.0625 tier8_y030 > tier8_y030.log 2>&1
