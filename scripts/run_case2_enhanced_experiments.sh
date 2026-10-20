#!/bin/sh
# Part 2 Case 2 Enhanced experiment: sweep parallel connections (N) and failure
# probability (p), measuring completion time, throughput, wire overhead, and retransmission.
#
# Usage: scripts/run_case2_enhanced_experiments.sh <topics_root> <topic> [runs] [out_dir]
set -eu

ROOT=${1:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
TOPIC=${2:?usage: $0 <topics_root> <topic> [runs] [out_dir]}
RUNS=${3:-3}
OUT=${4:-Results}
BUILD_DIR=${BUILD_DIR:-build}
PARALLEL_NS=${PARALLEL_NS:-"1 2 4 8"}
PROBABILITIES=${PROBABILITIES:-"0 0.05 0.1 0.2 0.3 0.5"}
PORT=$((40000 + $$ % 15000))
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cn_enhanced_exp.XXXXXX")
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
CLIENT_CSV=$OUT/Part2_Case2_Enhanced_runs.csv
SUMMARY_CSV=$OUT/Part2_Case2_Enhanced_summary.csv
rm -f "$CLIENT_CSV" "$SUMMARY_CSV"

echo "parallel_N,failure_probability,run,topic,files,wire_bytes_received,wire_bytes_sent,data_bytes_received,useful_bytes,seconds,throughput_mbps,redundant_bytes,retransmission_ratio,overhead_ratio" >"$CLIENT_CSV"

start_server() {
    prob=$1
    PORT=$((PORT + 1))
    "$BUILD_DIR/server_enhanced" "$PORT" "$ROOT" "$prob" >"$WORK/server.log" 2>&1 &
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
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=
    fi
}

echo "=== Running Part 2 Case 2 Enhanced Experiments on topic '$TOPIC' ==="

for p in $PROBABILITIES; do
    echo "Starting server with fault probability p=$p"
    start_server "$p"

    for N in $PARALLEL_NS; do
        echo "  Testing with N=$N parallel connections ($RUNS runs)..."
        for r in $(seq 1 "$RUNS"); do
            rm -rf "$WORK/download"
            mkdir -p "$WORK/download"

            set +e
            "$BUILD_DIR/client_enhanced" 127.0.0.1 "$PORT" "$TOPIC" "$WORK/download" "$N" >"$WORK/client.log" 2>&1
            rc=$?
            set -e

            if [ "$rc" -ne 0 ]; then
                echo "    Run $r failed with exit code $rc" >&2
                cat "$WORK/client.log" >&2
                continue
            fi

            # Parse METRICS line:
            # files=A/B wire_bytes_received=C wire_bytes_sent=D data_bytes_received=E useful_bytes=F final_wire_bytes=G seconds=H throughput_mbps=I redundant_bytes=J retransmission_ratio=K overhead_ratio=L parallel_connections=M
            files=$(sed -n 's/.*files=\([^ ]*\).*/\1/p' "$WORK/client.log")
            wire_rx=$(sed -n 's/.*wire_bytes_received=\([^ ]*\).*/\1/p' "$WORK/client.log")
            wire_tx=$(sed -n 's/.*wire_bytes_sent=\([^ ]*\).*/\1/p' "$WORK/client.log")
            data_rx=$(sed -n 's/.*data_bytes_received=\([^ ]*\).*/\1/p' "$WORK/client.log")
            useful=$(sed -n 's/.*useful_bytes=\([^ ]*\).*/\1/p' "$WORK/client.log")
            sec=$(sed -n 's/.*seconds=\([^ ]*\).*/\1/p' "$WORK/client.log")
            mbps=$(sed -n 's/.*throughput_mbps=\([^ ]*\).*/\1/p' "$WORK/client.log")
            red=$(sed -n 's/.*redundant_bytes=\([^ ]*\).*/\1/p' "$WORK/client.log")
            ret_ratio=$(sed -n 's/.*retransmission_ratio=\([^ ]*\).*/\1/p' "$WORK/client.log")
            ovh_ratio=$(sed -n 's/.*overhead_ratio=\([^ ]*\).*/\1/p' "$WORK/client.log")

            echo "$N,$p,$r,$TOPIC,$files,$wire_rx,$wire_tx,$data_rx,$useful,$sec,$mbps,$red,$ret_ratio,$ovh_ratio" >>"$CLIENT_CSV"
        done
    done

    stop_server
done

echo "Experiments completed. Results saved to $CLIENT_CSV"
