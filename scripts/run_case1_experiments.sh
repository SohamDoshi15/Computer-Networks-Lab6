#!/bin/sh
# Part 2 Case 1 experiment: sweep the failure probability and record, for every
# run, the client's METRICS line plus the server's per-connection log.
#
# Usage: scripts/run_case1_experiments.sh <topics_root> <topic> [runs] [out_dir]
#
#   runs     repetitions per probability (default 5)
#   out_dir  where CSV files go (default Results)
#
# Server and client run on this machine (loopback). For the three-computer
# setup, start build/server_case1 on the server node with CN_SERVER_METRICS_CSV
# and run build/client_case1 with CN_METRICS_CSV on the client nodes.
# The probability lists can be overridden with CONNECTION_PS and CHUNK_PS.
# Fault seeds are fixed per (probability, run) so every run is repeatable.
set -eu

ROOT=${1:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
TOPIC=${2:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
RUNS=${3:-5}
OUT=${4:-Results}
BUILD_DIR=${BUILD_DIR:-build}
CONNECTION_PS=${CONNECTION_PS:-"0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9"}
CHUNK_PS=${CHUNK_PS:-"0 0.0005 0.001 0.002 0.005"}
PORT=$((30000 + $$ % 20000))
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cn_case1_exp.XXXXXX")
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
CLIENT_CSV=$OUT/Part2_Case1_runs.csv
SERVER_CSV=$OUT/Part2_Case1_server_connections.csv
BASELINE_CSV=$OUT/Part2_Case1_part1_baseline.csv
rm -f "$CLIENT_CSV" "$SERVER_CSV" "$BASELINE_CSV"
echo "fault_mode,failure_probability,run,seed,topic,status,attempts,files,transmitted_bytes,sent_bytes,data_bytes_received,useful_bytes,redundant_bytes,wasted_bytes,overhead,completion_s,goodput_MBps,throughput_MBps" >"$CLIENT_CSV"
echo "server,run,topic,files,useful_bytes,completion_s,goodput_MBps" >"$BASELINE_CSV"

# start_server <binary> [args...] with environment already exported by the caller.
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

# Detached server workers append their CSV row just after the client exits.
# Wait for every attempt before stopping the process so the final row is not lost.
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

# Converts "METRICS a=1 b=2 ..." into "1,2,..." (field order is fixed).
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
                CN_SERVER_METRICS_CSV=$WORK/server.csv start_server server_case1 "$p"
            rm -rf "$WORK/out"
            set +e
            CN_QUIET=1 "$BUILD_DIR/client_case1" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/out" \
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
            echo "case1 mode=$mode p=$p run=$run: $(grep '^METRICS' "$WORK/client.log")"
            run=$((run + 1))
        done
    done
}

# Warm the page cache once so the first measured run is not penalised.
start_server server_multi
"$BUILD_DIR/client" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/out" >/dev/null
stop_server

# Part 1 baseline: the same topic through the Part 1 thread-per-client server.
run=1
while [ "$run" -le "$RUNS" ]; do
    start_server server_multi
    rm -rf "$WORK/out"
    line=$("$BUILD_DIR/client" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/out" | tail -1)
    stop_server
    files=$(echo "$line" | sed -n 's/^Received \([0-9]*\) file.*/\1/p')
    bytes=$(echo "$line" | sed -n 's/.* \([0-9]*\) verified byte.*/\1/p')
    secs=$(echo "$line" | sed -n 's/.* in \([0-9.]*\) s .*/\1/p')
    mbps=$(echo "$line" | sed -n 's/.*(\([0-9.]*\) MB\/s).*/\1/p')
    echo "server_multi,$run,$TOPIC,$files,$bytes,$secs,$mbps" >>"$BASELINE_CSV"
    echo "part1 run=$run: $line"
    run=$((run + 1))
done

# shellcheck disable=SC2086
run_sweep connection $CONNECTION_PS
# shellcheck disable=SC2086
run_sweep chunk $CHUNK_PS
echo "wrote $CLIENT_CSV, $SERVER_CSV and $BASELINE_CSV"
