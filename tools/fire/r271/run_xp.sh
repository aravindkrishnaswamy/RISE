#!/bin/bash
# r271 section 25 transplant arm: inert plume, compact viscous stencil, state replaced at step 0 (xp_driver.cpp).
# $1 = arm name, $2 = window s, $3 = 1 for the control (keep the RISE checkpoint's own temperature and velocity).
# Env RISE_R271_XP_DRIVER (default xp_driver) picks the driver in bt_build; RISE_R271_CV, RISE_FIRE_SCALAR_VREMAN_CV and
# RISE_FIRE_HYPER_VREMAN pass through; RISE_R271_XP_STATE / RISE_R271_XP_CKPT override the state file and composition checkpoint (section 29; CKPT none = fresh start).
set -u
B=/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale;ARM=$1;OUT=$B/$ARM
CK=/Volumes/SSD/RISE/fire_production_calibration/r271_inert/inert_compact/checkpoint_history/step_0000007298.checkpoint
cd /Volumes/SSD/Working/RISE
echo "{\"state\":\"running\",\"started\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
RISE_R271_XP_OWN=${3:-0} RISE_R271_XP_STATE=${RISE_R271_XP_STATE:-$B/fds_state_9p75.bin} RISE_R271_XP_CKPT=${RISE_R271_XP_CKPT:-$CK} RISE_FIRE_INERT_PLUME=1 \
 RISE_FIRE_COMPACT_VISCOUS_STENCIL=1 /usr/bin/caffeinate -i $B/bt_build/${RISE_R271_XP_DRIVER:-xp_driver} "$OUT" "$2" > $B/$ARM.log 2>&1
rc=$?
echo "{\"state\":\"finished\",\"exit\":$rc,\"finished\":\"$(date -u +%FT%TZ)\",\"out\":\"$OUT\"}" > $B/${ARM}_status.json
exit $rc
