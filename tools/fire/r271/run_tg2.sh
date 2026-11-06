#!/bin/bash
# r271 section 25: Taylor-Green momentum damping at 3-6 cells on the compact viscous stencil (tg_build2, current tree).
# $1 = arm, $2 = modes m (wavelength 2*69/m cells), $3 = U m/s, $4 = Vreman Cv override or "production". 0.5 s window.
set -u
B=/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale;ARM=$1;OUT=$B/$ARM
cd /Volumes/SSD/Working/RISE
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
[ "$4" != production ] && export RISE_R271_CV=$4
RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 RISE_R271_TG_MODES=$2 RISE_R271_TG_U=$3 \
  /usr/bin/caffeinate -i /Volumes/SSD/RISE/fire_production_calibration/r271_compact_stencil/tg_build2/tg_driver "$OUT" 0.5 > $B/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"finished\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
exit $rc
