#!/bin/zsh
# r273 sooty showcase: tier-8 full 25 s window from zero, accepted r271 candidate, gross soot yield 0.3, soot
# radiation (cap 0.45) with the self-absorption closure, sheet gate off. Private solver copy as r272's launcher.
# The capstone's closing check that the radiative fraction is 0.20 +/- 0.02 is expected to fail; frames are the product.
R=/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn
cd $R
[ -e tier8_y300_sa ] && { echo "tier8_y300_sa exists; refusing to overwrite"; exit 1; }
mkdir -p bin_showcase && cp /Volumes/SSD/Working/RISE/bin/tests/FireSequenceTest bin_showcase/fire_y300sa_solver
git -C /Volumes/SSD/Working/RISE rev-parse HEAD > tier8_y300_sa.commit
shasum -a 256 bin_showcase/fire_y300sa_solver >> tier8_y300_sa.commit
unset RISE_MEDIA_PATH RISE_FIRE_SOOT_SHEET_OXIDATION
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=0.3
export RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=0.45 RISE_FIRE_SOOT_SELF_ABSORPTION=1
exec /usr/bin/caffeinate -i bin_showcase/fire_y300sa_solver \
  --fire-production-temporal-capstone 8 0.0625 tier8_y300_sa > tier8_y300_sa.log 2>&1
