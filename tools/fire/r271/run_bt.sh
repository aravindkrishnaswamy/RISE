#!/bin/bash
# r271 small-scale response tests (section 24): real tier-8 production capstone from t=0, default stencil and settings.
# $1 = arm name, $2 = kind buoy|adv, $3 = cells per wavelength, $4 = amplitude K, $5 = window s, $6 = U m/s (adv).
set -u
B=/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale
ARM=$1;OUT=$B/$ARM
cd /Volumes/SSD/Working/RISE
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
[ -n "${6:-}" ] && export RISE_R271_BT_U=$6
RISE_R271_BT_KIND=$2 RISE_R271_BT_N=$3 RISE_R271_BT_A=$4 /usr/bin/caffeinate -i $B/bt_build/bt_driver "$OUT" "$5" > $B/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"finished\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
exit $rc
