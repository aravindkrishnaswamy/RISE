#!/bin/zsh
# Parallel-efficiency benchmark harness.
#
# Uses zsh's `time` builtin so user/sys values correctly sum CPU time
# across all threads (macOS's /usr/bin/time -l is broken for this).
# Values larger than 60 s come back as "mm:ss.ff" — to_seconds() below
# converts back to pure seconds for arithmetic.
#
# Benchmark override: render_thread_reserve_count=0 gives every P and
# E core a worker (the production default reserves 1 E-core for the
# system, which we turn off here for clean measurement).
# See docs/PERFORMANCE.md "Thread priority policy".

SCENE="$1"
ITERATIONS="${2:-3}"

if [ ! -f "$SCENE" ]; then
    echo "ERROR: scene file $SCENE not found"
    exit 1
fi

export RISE_MEDIA_PATH="$(pwd)/"
TIMEFMT='%*E %*U %*S'

BENCH_OPTS=$(mktemp /tmp/rise_bench_opts.XXXXXX)
cat > "$BENCH_OPTS" <<OPT_EOF
render_thread_reserve_count 0
force_all_threads_low_priority false
OPT_EOF
export RISE_OPTIONS_FILE="$BENCH_OPTS"

# Worker count for efficiency calc = total CPUs on the machine.
# On Apple Silicon `hw.logicalcpu` returns P + E.  Override via the
# THREADS environment variable if you want to report against a
# different denominator (e.g. P-cores only).
if [ -z "$THREADS" ]; then
    if [ "$(uname)" = "Darwin" ]; then
        THREADS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 10)
    else
        THREADS=$(nproc 2>/dev/null || echo 10)
    fi
fi

to_seconds() {
    echo "$1" | awk -F: '{ if (NF==2) print $1*60 + $2; else print $1 }'
}

# Warm up once
printf "render\nquit\n" | ./bin/rise "$SCENE" > /dev/null 2>&1

total_wall=0
total_cpu=0
for i in $(seq 1 $ITERATIONS); do
    output=$({ time (printf "render\nquit\n" | ./bin/rise "$SCENE" > /dev/null 2>&1); } 2>&1)
    wall_raw=$(echo "$output" | awk '{print $1}')
    user_raw=$(echo "$output" | awk '{print $2}')
    sys_raw=$(echo "$output" | awk '{print $3}')
    wall=$(to_seconds "$wall_raw")
    user=$(to_seconds "$user_raw")
    sys=$(to_seconds "$sys_raw")
    cpu=$(echo "$user + $sys" | bc -l)
    echo "  run $i: wall=${wall}s cpu=${cpu}s  (user=${user}s sys=${sys}s)"
    total_wall=$(echo "$total_wall + $wall" | bc -l)
    total_cpu=$(echo "$total_cpu + $cpu" | bc -l)
done
avg_wall=$(echo "scale=3; $total_wall / $ITERATIONS" | bc -l)
avg_cpu=$(echo "scale=3; $total_cpu / $ITERATIONS" | bc -l)
avg_parallelism=$(echo "scale=2; $avg_cpu / $avg_wall" | bc -l)
efficiency=$(echo "scale=1; $avg_parallelism / $THREADS * 100" | bc -l)
printf "  Summary: wall=%.2fs cpu=%.2fs parallelism=%.2fx efficiency=%.1f%% (%d threads)\n" \
    "$avg_wall" "$avg_cpu" "$avg_parallelism" "$efficiency" "$THREADS"

rm -f "$BENCH_OPTS"
