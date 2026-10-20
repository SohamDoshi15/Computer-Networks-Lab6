#!/bin/sh
# End-to-end checks for Part 2 Case 2: session management, checkpointing, and resume.
set -eu

BUILD_DIR=${BUILD_DIR:-build}
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cn_e2e_case2.XXXXXX")
DATA_DIR=$WORK_DIR/data
SERVER_PID=
PORT=$((25000 + $$ % 20000))

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "e2e_case2: FAIL: $*" >&2
    exit 1
}

# expect_exit <status> <command...>; output stays in $WORK_DIR/last.log
expect_exit() {
    expected=$1
    shift
    set +e
    "$@" >"$WORK_DIR/last.log" 2>&1
    actual=$?
    set -e
    if [ "$actual" -ne "$expected" ]; then
        cat "$WORK_DIR/last.log" >&2
        fail "expected exit $expected, got $actual: $*"
    fi
}

# metric <name>: a field of the METRICS line in last.log
metric() {
    sed -n "s/^METRICS.* $1=\([^ ]*\).*/\1/p" "$WORK_DIR/last.log"
}

client() {
    CN_QUIET=1 CN_RETRY_DELAY_MS=0 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" "$@"
}

start_server() {
    probability=$1
    shift
    rm -f "$WORK_DIR/server.csv"
    for attempt in 1 2 3 4 5; do
        PORT=$((PORT + 1))
        env CN_SERVER_METRICS_CSV="$WORK_DIR/server.csv" "$@" \
            "$BUILD_DIR/server_case2" "$PORT" "$DATA_DIR" "$probability" \
            >"$WORK_DIR/server.log" 2>&1 &
        SERVER_PID=$!
        tries=0
        while kill -0 "$SERVER_PID" 2>/dev/null; do
            if grep -q listening "$WORK_DIR/server.log"; then
                return 0
            fi
            tries=$((tries + 1))
            [ "$tries" -le 50 ] || fail "server did not start"
            sleep 0.1
        done
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=
    done
    fail "server could not bind a port: $(cat "$WORK_DIR/server.log")"
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

same_tree() {
    diff -r "$DATA_DIR/$1" "$2/$1" >/dev/null || fail "$2/$1 differs from the served copy"
    if find "$2/$1" -name '.*.part' | grep -q .; then
        fail "$2/$1 still contains temporary .part files"
    fi
    if [ -f "$2/$1/.checkpoint" ]; then
        fail "$2/$1 still contains .checkpoint file after completion"
    fi
}

server_sum() {
    awk -F, -v c="$1" 'NR > 1 { s += $c } END { printf "%.0f\n", s }' "$WORK_DIR/server.csv"
}

cp -R tests/data "$DATA_DIR"
mkdir "$DATA_DIR/Grande"
head -c 3000000 /dev/urandom >"$DATA_DIR/Grande/random.bin"
head -c 65536 /dev/urandom >"$DATA_DIR/Grande/exact_chunk.bin"
head -c 65537 /dev/urandom >"$DATA_DIR/Grande/chunk_plus_one.bin"
i=0
while [ "$i" -lt 40 ]; do
    head -c $((1000 + i * 977)) /dev/urandom >"$DATA_DIR/Grande/small_$i.bin"
    i=$((i + 1))
done
: >"$DATA_DIR/Grande/empty"
mkdir "$DATA_DIR/Vuoto"
: >"$DATA_DIR/Vuoto/empty"
GRANDE_BYTES=$(cat "$DATA_DIR"/Grande/* | wc -c | tr -d ' ')

# --- Server command line checks ----------------------------------------------
expect_exit 1 "$BUILD_DIR/server_case2" 1 "$DATA_DIR"
expect_exit 1 "$BUILD_DIR/server_case2" 1 "$DATA_DIR" 1.5
expect_exit 1 "$BUILD_DIR/server_case2" 1 "$DATA_DIR" abc
expect_exit 1 "$BUILD_DIR/server_case2" 1 "$WORK_DIR/missing" 0.1
expect_exit 1 env CN_FAULT_MODE=sometimes "$BUILD_DIR/server_case2" 1 "$DATA_DIR" 0.1
echo "e2e_case2: server argument checks ok"

# --- p = 0: Single attempt, zero redundant bytes ------------------------------
start_server 0
out=$WORK_DIR/out_p0
expect_exit 0 client Grande "$out"
same_tree Grande "$out"
[ "$(metric attempts)" = 1 ] || fail "p=0 needed $(metric attempts) attempts"
[ "$(metric redundant_bytes)" = 0 ] || fail "p=0 reported redundant bytes"
[ "$(metric useful_bytes)" = "$GRANDE_BYTES" ] || fail "useful bytes != topic size"
[ "$(metric transmitted_bytes)" = "$(server_sum 10)" ] || fail "client and server byte counts differ"
expect_exit 0 client Cane "$out"
same_tree Cane "$out"
stop_server
echo "e2e_case2: p=0 ok"

# --- High failure probability: RESUME reduces retransmission ------------------
for action in close reset; do
    start_server 0.7 CN_FAULT_SEED=11 CN_FAULT_ACTION=$action
    out=$WORK_DIR/out_$action
    expect_exit 0 env CN_TIMEOUT_SECONDS=2 CN_QUIET=1 CN_RETRY_DELAY_MS=0 \
        "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" Grande "$out"
    same_tree Grande "$out"
    attempts=$(metric attempts)
    [ "$attempts" -gt 1 ] || fail "$action: expected restarts at p=0.7, seed 11"
    
    # In Case 2, resumption means redundant bytes should be tiny compared to topic size
    redundant=$(metric redundant_bytes)
    useful=$(metric useful_bytes)
    awk -v r="$redundant" -v u="$useful" 'BEGIN { exit !(r < u * 0.3) }' || \
        fail "$action: redundant bytes ($redundant) too high for Case 2 resumption (useful=$useful)"

    stop_server
    echo "e2e_case2: p=0.7 action=$action ok ($attempts attempts, redundant=$redundant bytes)"
done

# --- Safe fallback: Server restart invalidates in-memory session store --------
start_server 0.8 CN_FAULT_SEED=99
out=$WORK_DIR/out_restart
# First cut happens
set +e
env CN_MAX_ATTEMPTS=1 CN_TIMEOUT_SECONDS=1 CN_QUIET=1 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" Grande "$out" >/dev/null 2>&1
set -e
# Verify a checkpoint exists on client
[ -f "$out/Grande/.checkpoint" ] || fail "expected checkpoint file to be created during interrupted transfer"
# Stop server (killing in-memory session store)
stop_server
# Restart server on the same port with p=0
env CN_SERVER_METRICS_CSV="$WORK_DIR/server.csv" "$BUILD_DIR/server_case2" "$PORT" "$DATA_DIR" 0 >"$WORK_DIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 0.2
# Client reconnects: server rejects stale session -> client falls back to fresh GET -> finishes!
expect_exit 0 env CN_TIMEOUT_SECONDS=2 CN_QUIET=1 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" Grande "$out"
same_tree Grande "$out"
stop_server
echo "e2e_case2: server restart fallback ok"

# --- Safe fallback: Topic manifest changed on server -------------------------
start_server 0.8 CN_FAULT_SEED=77
out=$WORK_DIR/out_modified
set +e
env CN_MAX_ATTEMPTS=1 CN_TIMEOUT_SECONDS=1 CN_QUIET=1 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" Grande "$out" >/dev/null 2>&1
set -e
[ -f "$out/Grande/.checkpoint" ] || fail "expected checkpoint file for modified test"
# Alter a file on the server topic
echo "modified server data" >>"$DATA_DIR/Grande/random.bin"
GRANDE_BYTES_NEW=$(cat "$DATA_DIR"/Grande/* | wc -c | tr -d ' ')
# Client resumes: manifest identity mismatch -> server rejects -> fallback to fresh GET
expect_exit 0 env CN_TIMEOUT_SECONDS=2 CN_QUIET=1 "$BUILD_DIR/client_case2" 127.0.0.1 "$PORT" Grande "$out"
same_tree Grande "$out"
[ "$(metric useful_bytes)" = "$GRANDE_BYTES_NEW" ] || fail "useful bytes did not match modified size"
stop_server
echo "e2e_case2: modified manifest fallback ok"

# --- Multiple parallel clients resuming independently -------------------------
start_server 0.5 CN_FAULT_SEED=42
pids=
for n in 1 2 3 4; do
    client Grande "$WORK_DIR/parallel_$n" >/dev/null 2>&1 &
    pids="$pids $!"
done
for pid in $pids; do
    wait "$pid" || fail "a parallel client failed"
done
for n in 1 2 3 4; do
    same_tree Grande "$WORK_DIR/parallel_$n"
done
stop_server
echo "e2e_case2: 4 parallel clients ok"

# --- Failure cases ------------------------------------------------------------
start_server 0
# Unknown topic
expect_exit 2 client NoSuchTopic "$WORK_DIR/out_err"
[ ! -e "$WORK_DIR/out_err/NoSuchTopic" ] || fail "directory created for unknown topic"
# Invalid arguments
expect_exit 1 client .. "$WORK_DIR/out_err"
expect_exit 1 client Cane "$WORK_DIR/out_err" extra_arg
stop_server
echo "e2e_case2: failure cases ok"

echo "all Case 2 end-to-end tests passed"
