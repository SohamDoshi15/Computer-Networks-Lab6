#!/bin/sh
# Local or three-computer-compatible Part 1 concurrency sweep.
# Usage: run_part1_experiments.sh <topics_root> <topic> [runs] [out_dir]
set -eu

ROOT=${1:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
TOPIC=${2:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
RUNS=${3:-3}
OUT=${4:-Results}
BUILD_DIR=${BUILD_DIR:-build}
CLIENT_COUNTS=${CLIENT_COUNTS:-"1 2 4 8"}
PORT=$((30000 + $$ % 20000))
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cn_part1_exp.XXXXXX")
SERVER_PID=

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

mkdir -p "$OUT"
CSV=$OUT/Part1_runs.csv
echo "server_mode,clients,run,topic,useful_bytes,completion_s,aggregate_throughput_MBps" >"$CSV"

topic_bytes=$(find "$ROOT/$TOPIC" -maxdepth 1 -type f -printf '%s\n' | awk '{s += $1} END {print s + 0}')
[ "$topic_bytes" -gt 0 ] || { echo "topic has no non-empty data: $ROOT/$TOPIC" >&2; exit 1; }

start_server() {
    binary=$1
    PORT=$((PORT + 1))
    "$BUILD_DIR/$binary" "$PORT" "$ROOT" >"$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    tries=0
    until grep -q listening "$WORK/server.log" 2>/dev/null; do
        kill -0 "$SERVER_PID" 2>/dev/null || { cat "$WORK/server.log" >&2; exit 1; }
        tries=$((tries + 1))
        [ "$tries" -le 100 ] || { echo "server failed to start" >&2; exit 1; }
        sleep 0.05
    done
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

for mode in single multi; do
    binary=server_$mode
    for clients in $CLIENT_COUNTS; do
        run=1
        while [ "$run" -le "$RUNS" ]; do
            start_server "$binary"
            rm -rf "$WORK/out"
            mkdir -p "$WORK/out"
            start_ns=$(date +%s%N)
            pids=""
            i=1
            while [ "$i" -le "$clients" ]; do
                "$BUILD_DIR/client" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/out/$i" \
                    >"$WORK/client_$i.log" 2>&1 &
                pids="$pids $!"
                i=$((i + 1))
            done
            status=0
            for pid in $pids; do
                wait "$pid" || status=1
            done
            end_ns=$(date +%s%N)
            stop_server
            [ "$status" -eq 0 ] || { echo "$mode/$clients run $run failed" >&2; exit 1; }
            useful=$((topic_bytes * clients))
            seconds=$(awk -v a="$start_ns" -v b="$end_ns" 'BEGIN {printf "%.6f", (b-a)/1000000000}')
            rate=$(awk -v bytes="$useful" -v seconds="$seconds" 'BEGIN {printf "%.3f", bytes/seconds/1000000}')
            echo "$mode,$clients,$run,$TOPIC,$useful,$seconds,$rate" >>"$CSV"
            echo "part1 mode=$mode clients=$clients run=$run throughput=$rate MB/s"
            run=$((run + 1))
        done
    done
done

echo "Part 1 experiments complete: $CSV"
