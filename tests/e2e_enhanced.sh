#!/bin/sh
# End-to-end checks for Part 2 Case 2 Enhanced: parallel TCP connections and range reassembly.
set -eu

BUILD_DIR=${BUILD_DIR:-build}
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cn_e2e_enhanced.XXXXXX")
DATA_DIR=$WORK_DIR/data
SERVER_PID=
PORT=$((30000 + $$ % 15000))

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "e2e_enhanced: FAIL: $*" >&2
    exit 1
}

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

start_server() {
    probability=$1
    PORT=$((PORT + 1))
    "$BUILD_DIR/server_enhanced" "$PORT" "$DATA_DIR" "$probability" >"$WORK_DIR/server.log" 2>&1 &
    SERVER_PID=$!
    tries=0
    while kill -0 "$SERVER_PID" 2>/dev/null; do
        if grep -q "Enhanced server listening" "$WORK_DIR/server.log" 2>/dev/null; then
            return 0
        fi
        tries=$((tries + 1))
        if [ "$tries" -ge 40 ]; then break; fi
        sleep 0.05
    done
    cat "$WORK_DIR/server.log" >&2
    fail "enhanced server failed to start on port $PORT"
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=
    fi
}

mkdir -p "$DATA_DIR/Cane"
echo "Enhanced parallel test file 1" > "$DATA_DIR/Cane/file1.txt"
# Create a larger file to trigger range splitting
head -c 2097152 /dev/urandom > "$DATA_DIR/Cane/large.bin" 2>/dev/null || dd if=/dev/zero of="$DATA_DIR/Cane/large.bin" bs=1024 count=2048 2>/dev/null

echo "--- 1. Clean parallel transfer (p=0.0, N=4) ---"
start_server 0.0
expect_exit 0 "$BUILD_DIR/client_enhanced" 127.0.0.1 "$PORT" Cane "$WORK_DIR/out1" 4
cmp -s "$DATA_DIR/Cane/file1.txt" "$WORK_DIR/out1/Cane/file1.txt" || fail "file1 mismatch"
cmp -s "$DATA_DIR/Cane/large.bin" "$WORK_DIR/out1/Cane/large.bin" || fail "large.bin mismatch"
stop_server

echo "--- 2. Single connection fallback (p=0.0, N=1) ---"
start_server 0.0
expect_exit 0 "$BUILD_DIR/client_enhanced" 127.0.0.1 "$PORT" Cane "$WORK_DIR/out2" 1
cmp -s "$DATA_DIR/Cane/large.bin" "$WORK_DIR/out2/Cane/large.bin" || fail "N=1 large.bin mismatch"
stop_server

echo "--- 3. Parallel transfer under fault injection (p=0.20, N=4) ---"
start_server 0.20
expect_exit 0 "$BUILD_DIR/client_enhanced" 127.0.0.1 "$PORT" Cane "$WORK_DIR/out3" 4
cmp -s "$DATA_DIR/Cane/large.bin" "$WORK_DIR/out3/Cane/large.bin" || fail "fault recovered large.bin mismatch"
stop_server

echo "--- 4. Invalid topic handling ---"
start_server 0.0
expect_exit 4 "$BUILD_DIR/client_enhanced" 127.0.0.1 "$PORT" NonExistentTopic "$WORK_DIR/out4" 4
stop_server

echo "e2e_enhanced: ALL CHECKS PASSED"
