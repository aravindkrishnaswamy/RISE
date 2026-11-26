#!/bin/zsh
# r275 fast-mode validation: the r273 sooty showcase (tier 8, 25 s from zero, yield 0.3, cap 0.45, self-absorption
# closure $2, default 1 to match r273_poor_burn/tier8_y300_sa) with RISE_FIRE_FAST_MODE=1 and the source-step cap
# lifted. Uncertified by construction; compare window statistics against the certified run.
# usage: run_tier8_fast.sh <arm name> [self-absorption 1|2] [step cap s, default 0.0025]
# The cap stays finite: with inf the cold start takes 11.6 ms steps and the stage validation fails at step 10.
R=/Volumes/SSD/RISE/fire_production_calibration/r275_speed
ARM=$1; SELF=${2:-1}; CAP=${3:-0.0025}
mkdir -p $R/bin && cd $R
[ -e $ARM ] && { echo "$ARM exists; refusing to overwrite"; exit 1; }
cp /Volumes/SSD/Working/RISE/bin/tests/FireSequenceTest bin/${ARM}_solver
git -C /Volumes/SSD/Working/RISE rev-parse HEAD > $ARM.commit
shasum -a 256 bin/${ARM}_solver >> $ARM.commit
unset RISE_MEDIA_PATH RISE_FIRE_SOOT_SHEET_OXIDATION RISE_FIRE_WORKERS
export RISE_FIRE_FLOOR_EMISSIVITY=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
export RISE_FIRE_VREMAN_ZONE=below:16:0.2857 RISE_FIRE_METHANE_SOOT_YIELD=0.3
export RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=0.45 RISE_FIRE_SOOT_SELF_ABSORPTION=$SELF
export RISE_FIRE_FAST_MODE=1 RISE_FIRE_SOURCE_STEP_CAP_S=$CAP
exec /usr/bin/caffeinate -i bin/${ARM}_solver --fire-production-temporal-capstone 8 0.0625 $ARM > $ARM.log 2>&1
