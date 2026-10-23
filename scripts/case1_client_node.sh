#!/bin/sh
# Run on each CLIENT computer of the three-computer Case 1 experiment.
# Starts N clients in parallel against the server, then appends their METRICS
# to one canonical-schema CSV per node and checks every downloaded file against
# the server's checksum list when one is given.
#
# Usage: scripts/case1_client_node.sh <server_ip> <port> <topic> <clients> <p_label> [ref.sha256]
#   p_label    the probability the server is running with, used in file names
#   ref.sha256 optional: output of `cd <topic_dir> && sha256sum * | sort` on the server
set -eu

SERVER=${1:?usage: $0 <server_ip> <port> <topic> <clients> <p_label> [ref.sha256]}
PORT=${2:?port}
TOPIC=${3:?topic}
CLIENTS=${4:?clients}
P=${5:?p_label}
REF=${6:-}
NODE=$(hostname -s 2>/dev/null || hostname)
BUILD_DIR=${BUILD_DIR:-build}
RESULTS=${RESULTS:-Results}
STAMP=$(date +%Y%m%d_%H%M%S)
WORK=lab_runs/${STAMP}_p${P}_${NODE}
CSV=$RESULTS/Part2_Case1_lab_runs_${NODE}.csv
RUN_LABEL=${RUN_LABEL:-1}
SEED_LABEL=${CN_FAULT_SEED:-lab}

mkdir -p "$WORK" "$RESULTS"
if [ ! -f "$CSV" ]; then
    echo "fault_mode,failure_probability,run,seed,topic,status,attempts,files,transmitted_bytes,sent_bytes,data_bytes_received,useful_bytes,redundant_bytes,wasted_bytes,overhead,completion_s,goodput_MBps,throughput_MBps" >"$CSV"
fi

metrics_to_csv() {
    sed -n 's/^METRICS //p' | tr ' ' '\n' | sed 's/^[^=]*=//' | paste -sd, -
}

i=1
pids=
while [ "$i" -le "$CLIENTS" ]; do
    CN_QUIET=1 "$BUILD_DIR/client_case1" "$SERVER" "$PORT" "$TOPIC" \
        "$WORK/c$i" >"$WORK/c$i.log" 2>&1 &
    pids="$pids $!"
    i=$((i + 1))
done
failed=0
for pid in $pids; do
    wait "$pid" || failed=$((failed + 1))
done

i=1
while [ "$i" -le "$CLIENTS" ]; do
    grep '^METRICS' "$WORK/c$i.log" | cut -c1-160 | sed "s/^/c$i /"
    row=$(metrics_to_csv <"$WORK/c$i.log")
    if [ -n "$row" ]; then
        echo "connection,$P,$RUN_LABEL.$i,$SEED_LABEL,$TOPIC,$row" >>"$CSV"
    else
        echo "c$i: missing METRICS line" >&2
        failed=$((failed + 1))
    fi
    if [ -n "$REF" ] && [ -d "$WORK/c$i/$TOPIC" ]; then
        if (cd "$WORK/c$i/$TOPIC" && sha256sum -- * | sort) | cmp -s - "$REF"; then
            echo "c$i: all files match the server"
        else
            echo "c$i: FILES DIFFER from the server copy"
            failed=$((failed + 1))
        fi
    fi
    i=$((i + 1))
done
echo "$NODE: $CLIENTS client(s), $failed failure(s); metrics appended to $CSV"
[ "$failed" -eq 0 ]
