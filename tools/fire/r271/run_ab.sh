#!/bin/bash
# r271 A/B arm: resume r270 step 9223 (t=14.984 s) for 4 s, floor on.
# $1 = arm name (default ab_compact_floor_on), $2 = driver build dir (default ab_build),
# $3 = compact viscous stencil 0/1 (default 1), $4 = FDS mixing time 0/1 (default 0).
# Keep $3 equal across arms that test $4: the mixing-time nu_t follows the stencil.
# Baseline is r270's own frames (switch off is value-identical; see resume_check_off and §14).
set -u
B=/Volumes/SSD/RISE/fire_production_calibration/r271_compact_stencil
CK=/Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation/tier8_eps1/checkpoint_history/step_0000009223.checkpoint
ARM=${1:-ab_compact_floor_on};BUILD=${2:-ab_build};COMPACT=${3:-1};FDSMIX=${4:-0}
OUT=$B/$ARM
cd /Volumes/SSD/Working/RISE
[ "$COMPACT" = 1 ] && export RISE_FIRE_COMPACT_VISCOUS_STENCIL=1
[ "$FDSMIX" = 1 ] && export RISE_FIRE_FDS_MIXING_TIME=1
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\",\"compact\":$COMPACT,\"fds_mixing\":$FDSMIX}" > $B/${ARM}_status.json
RISE_FIRE_FLOOR_EMISSIVITY=1 /usr/bin/caffeinate -i \
  $B/$BUILD/ab_driver "$CK" "$OUT" 4 > $B/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"$([ $rc = 0 ] && echo complete_stopped_for_review || echo stopped_error)\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
exit $rc
