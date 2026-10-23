#!/bin/sh
# Part 2 Case 2 experiment: sweep the failure probability and record, for every
# run, the client's METRICS line plus the server's per-connection log with session resumption.
#
# Usage: scripts/run_case2_experiments.sh <topics_root> <topic> [runs] [out_dir]
#
#   runs     repetitions per probability (default 5)
#   out_dir  where CSV files go (default Results)
set -eu

ROOT=${1:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
TOPIC=${2:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
RUNS=${3:-5}
OUT=${4:-Results}
BUILD_DIR=${BUILD_DIR:-build}
CONNECTION_PS=${CONNECTION_PS:-"0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9"}
CHUNK_PS=${CHUNK_PS:-"0 0.0005 0.001 0.002 0.005"}
PORT=$((35000 + $$ % 20000))
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cn_case2_exp.XXXXXX")
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
CLIENT_CSV=$OUT/Part2_Case2_runs.csv
SERVER_CSV=$OUT/Part2_Case2_server_connections.csv
rm -f "$CLIENT_CSV" "$SERVER_CSV"
echo "fault_mode,failure_probability,run,seed,topic,status,attempts,files,transmitted_bytes,sent_bytes,data_bytes_received,useful_bytes,redundant_bytes,wasted_bytes,overhead,completion_s,goodput_MBps,throughput_MBps" >"$CLIENT_CSV"

start_server() {
    binary=$1
    shift
    PORT=$((PORT + 1))
    "$BUILD_DIR/$binary" "$PORT" "$ROOT" "$@" >"$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    tries=0
    until grep -q listening "$WORK/server.log" 2>/dev/null; do
        kill -0 "$SERVER_PID" 2>/dev/null || { cat "$WORK/server.log" >&2; exit 1; }
        tries=$((tries + 1))
        [ "$tries" -le 100 ] || { echo "server did not start" >&2; exit 1; }
        sleep 0.05
    done
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

wait_for_server_rows() {
    expected=$1
    tries=0
    while :; do
        rows=0
        if [ -f "$WORK/server.csv" ]; then
            rows=$(( $(wc -l <"$WORK/server.csv") - 1 ))
        fi
        [ "$rows" -ge "$expected" ] && return 0
        tries=$((tries + 1))
        if [ "$tries" -gt 200 ]; then
            echo "server metrics timeout: expected $expected row(s), found $rows" >&2
            return 1
        fi
        sleep 0.05
    done
}

metrics_to_csv() {
    sed -n 's/^METRICS //p' | tr ' ' '\n' | sed 's/^[^=]*=//' | paste -sd, -
}

run_sweep() {
    mode=$1
    shift
    for p in "$@"; do
        run=1
        while [ "$run" -le "$RUNS" ]; do
            seed=$((run * 1000 + $(printf '%s' "$p" | cksum | cut -c1-4)))
            CN_FAULT_MODE=$mode CN_FAULT_SEED=$seed \
                CN_SERVER_METRICS_CSV=$WORK/server.csv start_server server_case2 "$p"
            rm -rf "$WORK/out"
            set +e
            CN_QUIET=1 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/out" \
                >"$WORK/client.log" 2>"$WORK/client.err"
            set -e
            row=$(metrics_to_csv <"$WORK/client.log")
            attempts=$(sed -n 's/^METRICS //p' "$WORK/client.log" | tr ' ' '\n' | sed -n 's/^attempts=//p')
            [ -n "$attempts" ] || { cat "$WORK/client.err" >&2; exit 1; }
            wait_for_server_rows "$attempts"
            stop_server
            echo "$mode,$p,$run,$seed,$TOPIC,$row" >>"$CLIENT_CSV"
            if [ ! -f "$SERVER_CSV" ]; then
                echo "run,seed,$(head -1 "$WORK/server.csv")" >"$SERVER_CSV"
            fi
            tail -n +2 "$WORK/server.csv" | sed "s/^/$run,$seed,/" >>"$SERVER_CSV"
            rm -f "$WORK/server.csv"
            echo "case2 mode=$mode p=$p run=$run: $(grep '^METRICS' "$WORK/client.log")"
            run=$((run + 1))
        done
    done
}

# Warm cache
start_server server_case2 0
CN_QUIET=1 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/warmup" >/dev/null 2>&1 || true
stop_server
rm -rf "$WORK/warmup"

run_sweep connection $CONNECTION_PS
run_sweep chunk $CHUNK_PS

echo "Case 2 experiments complete: $CLIENT_CSV and $SERVER_CSV written."
