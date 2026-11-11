#!/bin/bash
# r271 non-reacting plume, RISE arm: production capstone from t=0 with RISE_FIRE_INERT_PLUME=1.
# $1 = arm name, $2 = compact viscous stencil 0/1, $3 = window s (default 14), $4 = frame origin s (default 2),
# $5 = heat-source seed amplitude (RISE_FIRE_INERT_SEED; default none).
# Env RISE_R271_TIER (6|8|10, default 8) picks the resolution tier (section 33).
# Env RISE_R271_INERT_BUILD (default inert_build) picks the driver build; RISE_R271_CV and RISE_FIRE_SCALAR_VREMAN_CV pass through.
# The child exits non-zero at the end by design (frame-count and capstone-ignition checks assume a reacting full window).
set -u
I=/Volumes/SSD/RISE/fire_production_calibration/r271_inert
ARM=$1;COMPACT=$2;WINDOW=${3:-14};ORIGIN=${4:-2};SEED=${5:-};OUT=$I/$ARM
cd /Volumes/SSD/Working/RISE
[ "$COMPACT" = 1 ] && export RISE_FIRE_COMPACT_VISCOUS_STENCIL=1
[ -n "$SEED" ] && export RISE_FIRE_INERT_SEED=$SEED
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\",\"compact\":$COMPACT,\"window_s\":$WINDOW,\"seed\":\"$SEED\"}" > $I/${ARM}_status.json
RISE_FIRE_INERT_PLUME=1 /usr/bin/caffeinate -i $I/${RISE_R271_INERT_BUILD:-inert_build}/inert_driver "$OUT" "$WINDOW" "$ORIGIN" ${RISE_R271_TIER:-} > $I/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"finished_stopped_for_review\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $I/${ARM}_status.json
exit $rc
