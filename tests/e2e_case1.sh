#!/bin/sh
# End-to-end checks for Part 2 Case 1: fault-injecting server, restarting client.
set -eu

BUILD_DIR=${BUILD_DIR:-build}
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cn_e2e_case1.XXXXXX")
DATA_DIR=$WORK_DIR/data
SERVER_PID=
PORT=$((20000 + $$ % 20000))

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "e2e_case1: FAIL: $*" >&2
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
    CN_QUIET=1 CN_RETRY_DELAY_MS=0 "$BUILD_DIR/client_case1" 127.0.0.1 "$PORT" "$@"
}

# start_server <probability> [VAR=value...]: a fresh server, so connection ids restart at 1.
start_server() {
    probability=$1
    shift
    rm -f "$WORK_DIR/server.csv"
    for attempt in 1 2 3 4 5; do
        PORT=$((PORT + 1))
        env CN_SERVER_METRICS_CSV="$WORK_DIR/server.csv" "$@" \
            "$BUILD_DIR/server_case1" "$PORT" "$DATA_DIR" "$probability" \
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
    kill "$SERVER_PID"
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

same_tree() {
    diff -r "$DATA_DIR/$1" "$2/$1" >/dev/null || fail "$2/$1 differs from the served copy"
    if find "$2/$1" -name '.*.part' | grep -q .; then
        fail "$2/$1 still contains temporary .part files"
    fi
}

# Sum of a server CSV column (1-based) over all rows.
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

# --- Server command line ----------------------------------------------------
expect_exit 1 "$BUILD_DIR/server_case1" 1 "$DATA_DIR"
expect_exit 1 "$BUILD_DIR/server_case1" 1 "$DATA_DIR" 1.5
expect_exit 1 "$BUILD_DIR/server_case1" 1 "$DATA_DIR" abc
expect_exit 1 "$BUILD_DIR/server_case1" 1 "$WORK_DIR/missing" 0.1
expect_exit 1 env CN_FAULT_MODE=sometimes "$BUILD_DIR/server_case1" 1 "$DATA_DIR" 0.1
echo "e2e_case1: server argument checks ok"

# --- p = 0 behaves like Part 1: one attempt, nothing redundant ---------------
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
echo "e2e_case1: p=0 ok"

# --- High failure probability: restarts until a clean pass -------------------
for action in close reset; do
    start_server 0.7 CN_FAULT_SEED=11 CN_FAULT_ACTION=$action
    out=$WORK_DIR/out_$action
    # An RST can be lost, leaving only the receive timeout to detect the cut.
    expect_exit 0 env CN_TIMEOUT_SECONDS=2 CN_QUIET=1 CN_RETRY_DELAY_MS=0 \
        "$BUILD_DIR/client_case1" 127.0.0.1 "$PORT" Grande "$out"
    same_tree Grande "$out"
    attempts=$(metric attempts)
    [ "$attempts" -gt 1 ] || fail "$action: expected restarts at p=0.7, seed 11"
    [ "$(metric redundant_bytes)" -gt 0 ] || fail "$action: restarts must cost redundant bytes"
    if [ "$action" = close ]; then
        # FIN after the partial frame: the client must notice the cut at once.
        awk -v s="$(metric completion_s)" 'BEGIN { exit !(s < 10) }' ||
            fail "$action: the client took $(metric completion_s) s to notice the cuts"
    fi
    rows=$(($(wc -l <"$WORK_DIR/server.csv") - 1))
    [ "$rows" = "$attempts" ] || fail "$action: server saw $rows connections, client $attempts"
    [ "$(grep -c ',injected_fault,' "$WORK_DIR/server.csv")" = $((attempts - 1)) ] ||
        fail "$action: every failed attempt must be an injected fault"
    if [ "$action" = close ]; then
        # With FIN the client reads every byte the server sent, including the
        # partial frame at each cut, so both counts must agree exactly.
        [ "$(metric transmitted_bytes)" = "$(server_sum 10)" ] ||
            fail "transmitted bytes $(metric transmitted_bytes) != server $(server_sum 10)"
    fi
    # Restart from the beginning: every attempt starts at data byte 0, so the
    # successful pass alone carries the whole topic.
    stop_server
    echo "e2e_case1: p=0.7 action=$action ok ($attempts attempts)"
done

# --- Chunk mode --------------------------------------------------------------
start_server 0.01 CN_FAULT_SEED=5 CN_FAULT_MODE=chunk
expect_exit 0 client Grande "$WORK_DIR/out_chunk"
same_tree Grande "$WORK_DIR/out_chunk"
stop_server
echo "e2e_case1: chunk mode ok ($(metric attempts) attempts)"

# --- Several clients at once, each restarting independently ------------------
start_server 0.5 CN_FAULT_SEED=3
pids=
for n in 1 2 3 4 5 6; do
    client Grande "$WORK_DIR/parallel_$n" >/dev/null 2>&1 &
    pids="$pids $!"
done
for pid in $pids; do
    wait "$pid" || fail "a parallel client failed"
done
for n in 1 2 3 4 5 6; do
    same_tree Grande "$WORK_DIR/parallel_$n"
done
stop_server
echo "e2e_case1: 6 parallel clients ok"

# --- Failure cases -------------------------------------------------------------
start_server 1
# Every connection is cut: the client stops at the attempt limit with a network error.
expect_exit 3 env CN_MAX_ATTEMPTS=3 CN_QUIET=1 CN_RETRY_DELAY_MS=0 \
    "$BUILD_DIR/client_case1" 127.0.0.1 "$PORT" Grande "$WORK_DIR/out_p1"
[ "$(metric attempts)" = 3 ] || fail "p=1 should stop after 3 attempts"
if find "$WORK_DIR/out_p1" -name '.*.part' | grep -q .; then
    fail "a cut transfer left a .part file behind"
fi
# A topic with no file data has no transfer to interrupt, even at p=1.
expect_exit 0 client Vuoto "$WORK_DIR/out_p1"
same_tree Vuoto "$WORK_DIR/out_p1"
# An unknown topic is the server's answer, not a fault: no retry.
expect_exit 2 client NoSuchTopic "$WORK_DIR/out_p1"
[ "$(metric attempts)" = 1 ] || fail "an unknown topic must not be retried"
[ ! -e "$WORK_DIR/out_p1/NoSuchTopic" ] || fail "directory created for an unknown topic"
# Invalid arguments never reach the network.
expect_exit 1 client .. "$WORK_DIR/out_p1"
expect_exit 1 client Cane "$WORK_DIR/out_p1" extra_argument
stop_server
# Nothing is listening: connects are retried for CN_CONNECT_GIVEUP_SECONDS,
# then the client reports a network error.
expect_exit 3 env CN_CONNECT_GIVEUP_SECONDS=1 CN_QUIET=1 \
    "$BUILD_DIR/client_case1" 127.0.0.1 "$PORT" Cane "$WORK_DIR/out_down"
grep -q "unreachable for 1 s" "$WORK_DIR/last.log" || fail "no give-up message"
[ "$(metric attempts)" -gt 1 ] || fail "a refused connect must be retried"
echo "e2e_case1: failure cases ok"

echo "all Case 1 end-to-end tests passed"
