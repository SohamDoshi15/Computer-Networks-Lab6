#!/bin/sh
# Generates the graded layout from design document section 8: one folder per
# case, each self-contained, where `make` creates exactly ./server and ./client.
set -eu

OUT=${1:-build/FinalSubmission_Group_064_091}

# add_case <folder> <server main source> [client main source]
add_case() {
    client_main=${3:-client/main.c}
    dir=$OUT/$1
    # Sources live under src/ so they cannot collide with the server and client executables.
    mkdir -p "$dir/src/common" "$dir/src/client" "$dir/src/server"
    cp common/*.c common/*.h "$dir/src/common/"
    cp client/client.c client/client.h client/checkpoint.c client/checkpoint.h "$client_main" "$dir/src/client/"
    cp server/server_common.c server/server_common.h server/fault_injection.c \
        server/fault_injection.h server/session_store.c server/session_store.h "$2" "$dir/src/server/"
    sed -e "s|@SERVER_MAIN@|src/server/$(basename "$2")|" \
        -e "s|@CLIENT_MAIN@|src/client/$(basename "$client_main")|" >"$dir/Makefile" <<'EOF'
CC ?= cc
CPPFLAGS ?= -Isrc/common -Isrc/client
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?= -pthread

# SHA-256 comes from OpenSSL's libcrypto; pkg-config also finds Homebrew installs.
CRYPTO_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)

# macOS hides O_NOFOLLOW and asprintf when _POSIX_C_SOURCE is defined.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif

COMMON_SOURCES := $(wildcard src/common/*.c)
SERVER_SOURCES := src/server/server_common.c src/server/fault_injection.c src/server/session_store.c $(COMMON_SOURCES)
CLIENT_SOURCES := src/client/client.c src/client/checkpoint.c $(COMMON_SOURCES)
HEADERS := $(wildcard src/common/*.h src/client/*.h src/server/*.h)

.PHONY: all clean

all: server client

server: @SERVER_MAIN@ $(SERVER_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(CRYPTO_LIBS) $(LDLIBS)

client: @CLIENT_MAIN@ $(CLIENT_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(CRYPTO_LIBS) $(LDLIBS)

clean:
	rm -f server client
EOF
}

rm -rf "$OUT"
mkdir -p "$OUT"
add_case Part1/single_threaded server/server_single.c
add_case Part1/multi_threaded server/server_multi.c
add_case Part2_Case1 server/server_case1.c client/main_case1.c
add_case Part2_Case2 server/server_case2.c client/main_case2.c
add_case Part2_Case2_Enhanced server/server_enhanced.c client/main_enhanced.c

cat >"$OUT/README.md" <<'EOF'
# IT305 Socket Programming Project - Groups 064 and 091

Each case folder is self-contained; `make` inside it creates exactly two
executables, `server` and `client`. Requirements: a C11 compiler, pthreads,
and OpenSSL's libcrypto.

| Folder | Server | Description |
|---|---|---|
| `Part1/single_threaded` | sequential | completes one request before the next |
| `Part1/multi_threaded` | concurrent | one worker thread per client |
| `Part2_Case1` | concurrent + faults | no session state; client restarts from the beginning |
| `Part2_Case2` | concurrent + faults | session store + durable checkpoints; client resumes |
| `Part2_Case2_Enhanced` | concurrent + faults | parallel TCP connections and non-overlapping byte range reassembly |

```sh
cd Part1/multi_threaded && make
./server <port> <topics_root_dir>
./client <server_ip> <server_port> <topic> <output_dir>
```

Part 2 servers take the failure probability as a third argument:
`./server <port> <topics_root_dir> <failure_probability>`.

## Fault injection (Part 2)

The server interrupts a connection while it is sending file data. By default
(`CN_FAULT_MODE=connection`) it makes one decision per connection, right after
sending the MANIFEST: with probability p the connection will be cut, at a file
data byte drawn uniformly from the bytes it is about to send. The cut happens
inside a DATA frame, so the client receives a truncated frame and then EOF.
Optional environment variables: `CN_FAULT_MODE=chunk` (decide before every
64 KiB DATA frame instead), `CN_FAULT_ACTION=reset` (abortive close with TCP
RST instead of FIN), `CN_FAULT_SEED=<n>` (repeatable decisions). Each finished
connection is logged on stdout; `CN_SERVER_METRICS_CSV=<file>` also writes it
to a CSV file.

## Part 2 Case 1 client

Without session management, the client reconnects after any network, protocol
or integrity failure and requests the topic again from the beginning (new
connection, new transfer ID, nothing reused). It prints a `METRICS` line with
the attempts, transmitted, useful and redundant bytes, completion time, and
throughput; `CN_METRICS_CSV=<file>` appends the same values to a CSV file.

## Part 2 Case 2 client

With session management, the client persists progress to a durable atomic checkpoint
`<output_dir>/<topic>/.checkpoint`. After an interrupted connection, it truncates
any uncheckpointed tail and sends `RESUME` with its checkpointed state. The server
validates the session against its in-memory SessionStore and replies `RESUME_OK`,
streaming only the remaining bytes and reducing duplicate transmission to near zero.
If the server was restarted or the session expired, the client safely falls back to `GET`.

| Client exit | Meaning |
|---:|---|
| 0 | every file received and verified |
| 1 | usage error or invalid topic |
| 2 | server answered with an error, e.g. unknown topic |
| 3 | network failure: connect, disconnect, truncated frame, timeout |
| 4 | protocol violation by the server |
| 5 | integrity failure: size or SHA-256 mismatch |
| 6 | local file system failure |
EOF

# Each case must build on its own and produce exactly server and client.
for makefile in $(find "$OUT" -name Makefile | sort); do
    case_dir=$(dirname "$makefile")
    scratch=$(mktemp -d "${TMPDIR:-/tmp}/cn_submission.XXXXXX")
    cp -R "$case_dir"/. "$scratch"
    if ! (cd "$scratch" && make >/dev/null 2>&1); then
        rm -rf "$scratch"
        echo "submission: ${case_dir#"$OUT"/} does not build" >&2
        exit 1
    fi
    produced=$(cd "$scratch" && find . -maxdepth 1 -type f ! -name Makefile | sort | tr '\n' ' ')
    rm -rf "$scratch"
    if [ "$produced" != "./client ./server " ]; then
        echo "submission: ${case_dir#"$OUT"/} produced: $produced" >&2
        exit 1
    fi
    echo "submission: ${case_dir#"$OUT"/} builds ./server and ./client"
done
echo "submission written to $OUT (add FinalReport_Group_064_091.pdf and Results/)"
