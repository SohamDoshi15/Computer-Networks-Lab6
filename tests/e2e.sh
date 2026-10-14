#!/bin/sh
# End-to-end checks: the real client against both Part 1 servers over TCP.
set -eu

BUILD_DIR=${BUILD_DIR:-build}
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cn_e2e.XXXXXX")
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
    echo "e2e: FAIL: $*" >&2
    exit 1
}

# expect_exit <status> <command...>
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

client() {
    "$BUILD_DIR/client" 127.0.0.1 "$PORT" "$@"
}

# start_server <binary>: tries a few ports in case one is taken.
start_server() {
    for attempt in 1 2 3 4 5; do
        PORT=$((PORT + 1))
        "$BUILD_DIR/$1" "$PORT" "$DATA_DIR" >"$WORK_DIR/server.log" 2>&1 &
        SERVER_PID=$!
        tries=0
        while kill -0 "$SERVER_PID" 2>/dev/null; do
            if grep -q listening "$WORK_DIR/server.log"; then
                return 0
            fi
            tries=$((tries + 1))
            [ "$tries" -le 50 ] || fail "$1 did not start"
            sleep 0.1
        done
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=
    done
    fail "$1 could not bind a port: $(cat "$WORK_DIR/server.log")"
}

stop_server() {
    kill "$SERVER_PID"
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

same_tree() {
    diff -r "$DATA_DIR/$1" "$2/$1" >/dev/null || fail "$2/$1 differs from the served copy"
}

# The checked-in topics plus a generated one with multi-chunk files.
cp -R tests/data "$DATA_DIR"
mkdir "$DATA_DIR/Grande"
head -c 5000000 /dev/urandom >"$DATA_DIR/Grande/random.bin"
head -c 65536 /dev/urandom >"$DATA_DIR/Grande/exact_chunk.bin"
head -c 65537 /dev/urandom >"$DATA_DIR/Grande/chunk_plus_one.bin"
: >"$DATA_DIR/Grande/empty"

for server in server_single server_multi; do
    start_server "$server"
    out=$WORK_DIR/out_$server

    for topic in Cane Gatto Grande; do
        expect_exit 0 client "$topic" "$out"
        same_tree "$topic" "$out"
    done

    # Fetching again replaces a damaged local copy.
    printf 'damaged' >"$out/Cane/hello.txt"
    expect_exit 0 client Cane "$out"
    same_tree Cane "$out"

    # Unknown topic: the server's ERROR is reported and no directory is created.
    expect_exit 2 client NoSuchTopic "$out"
    [ ! -e "$out/NoSuchTopic" ] || fail "directory created for an unknown topic"

    # Invalid topics never reach the server.
    expect_exit 1 client .. "$out"
    expect_exit 1 client a/b "$out"
    expect_exit 1 client "" "$out"

    # Several clients at once, each into its own output directory.
    pids=
    for n in 1 2 3 4 5 6; do
        client Grande "$WORK_DIR/parallel_${server}_$n" >/dev/null 2>&1 &
        pids="$pids $!"
    done
    for pid in $pids; do
        wait "$pid" || fail "$server: a parallel client failed"
    done
    for n in 1 2 3 4 5 6; do
        same_tree Grande "$WORK_DIR/parallel_${server}_$n"
    done

    stop_server
    # Nothing is listening any more: a network failure, not a hang.
    expect_exit 3 client Cane "$out"
    echo "e2e: $server ok"
done

echo "all end-to-end tests passed"
