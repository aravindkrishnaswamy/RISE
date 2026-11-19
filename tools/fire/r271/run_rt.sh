#!/bin/bash
# r271 section 25 Rayleigh-Taylor arm (RISE): xp_driver from the fresh ambient state with rt_state.py's hot layer.
# $1 = n (cells per wavelength), $2 = window s. Inert plume and compact stencil as the reference arm.
set -u
B=/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale;ARM=rt_n$1;OUT=$B/$ARM
cd /Volumes/SSD/Working/RISE
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
RISE_R271_BT_N=$1 RISE_R271_XP_STATE=$B/rt_state_n${1}_rise.bin RISE_R271_XP_CKPT=none RISE_FIRE_INERT_PLUME=1 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 \
 /usr/bin/caffeinate -i $B/bt_build/xp_driver "$OUT" "$2" > $B/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"finished\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
exit $rc
