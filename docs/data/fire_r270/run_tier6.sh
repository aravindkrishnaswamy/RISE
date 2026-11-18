#!/bin/zsh
# r270 short tier-6 floor-radiation run (epsilon=1), stopped at 2.5 s simulated or 50 min wall.
cd /Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation
rm -rf tier6_eps1
RISE_FIRE_FLOOR_EMISSIVITY=1 /usr/bin/caffeinate -i /Volumes/SSD/Working/RISE/bin/tests/FireSequenceTest \
  --fire-production-temporal-capstone 6 0.0625 tier6_eps1 > tier6_eps1.log 2>&1 &
pid=$!
start=$(date +%s); peak=0
while kill -0 $pid 2>/dev/null; do
  sleep 20
  child=$(pgrep -f "capstone 6 0.0625 tier6_eps1" | head -1)
  rss=$(ps -o rss= -p ${child:-$pid} 2>/dev/null | tr -d ' '); [ -n "$rss" ] && [ $rss -gt $peak ] && peak=$rss
  [ -n "$rss" ] && [ $rss -gt 31457280 ] && { echo "RSS ceiling"; break; }
  t=$(tail -1 tier6_eps1/budgets/floor_radiation.csv 2>/dev/null | cut -d, -f2)
  [ -n "$t" ] && [ "$t" != "time_s" ] && python3 -c "import sys; sys.exit(0 if float('$t')>=2.5 else 1)" && break
  [ $(( $(date +%s) - start )) -gt 3000 ] && { echo "wall limit"; break; }
done
pkill -f "capstone 6 0.0625 tier6_eps1"; wait $pid; code=$?; sleep 2; echo "exit $code"
echo "stopped after $(( $(date +%s) - start )) s, peak RSS $((peak/1024)) MB, last: $(tail -1 tier6_eps1/budgets/floor_radiation.csv)"
