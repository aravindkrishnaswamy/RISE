#!/bin/bash
# r271 operator damping: Taylor-Green array in ambient air above the burner, real tier-8 production capstone from t=0.
# $1 = arm name, $2 = modes m (wavelength 2L/m), $3 = U (m/s), $4 = Vreman Cv override or "production". 0.5 s window.
set -u
B=/Volumes/SSD/RISE/fire_production_calibration/r271_compact_stencil
ARM=$1;OUT=$B/$ARM
cd /Volumes/SSD/Working/RISE
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
[ "$4" != production ] && export RISE_R271_CV=$4
RISE_R271_TG_MODES=$2 RISE_R271_TG_U=$3 \
  /usr/bin/caffeinate -i $B/tg_build/tg_driver "$OUT" 0.5 > $B/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"finished\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
exit $rc
