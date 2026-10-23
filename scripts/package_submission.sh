#!/bin/sh
# Generates the graded layout from design document section 8: one folder per
# case, each self-contained, where `make` creates exactly ./server and ./client.
set -eu

OUT=${1:-build/FinalSubmission_Group_064_091}
REPORT=${FINAL_REPORT_PATH:-output/pdf/FinalReport_Group_064_091.pdf}
out_parent=$(dirname "$OUT")
out_name=$(basename "$OUT")
archive="$out_parent/$out_name.tar.gz"

# Remove a previous archive before any preflight failure can leave it looking current.
rm -f "$archive"
if [ ! -f "$REPORT" ]; then
    echo "submission: missing final report: $REPORT" >&2
    exit 1
fi
if [ ! -d Results ]; then
    echo "submission: missing Results directory" >&2
    exit 1
fi

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

add_part1() {
    dir=$OUT/Part1
    mkdir -p "$dir/src/common" "$dir/src/client" "$dir/src/server"
    cp common/*.c common/*.h "$dir/src/common/"
    cp client/client.c client/client.h client/checkpoint.c client/checkpoint.h client/main.c "$dir/src/client/"
    cp server/server_common.c server/server_common.h server/fault_injection.c \
        server/fault_injection.h server/session_store.c server/session_store.h \
        server/server_single.c server/server_multi.c "$dir/src/server/"
    cat >"$dir/Makefile" <<'EOF'
CC ?= cc
CPPFLAGS ?= -Isrc/common -Isrc/client
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?= -pthread

CRYPTO_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)

ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif

COMMON_SOURCES := $(wildcard src/common/*.c)
SERVER_SOURCES := src/server/server_common.c src/server/fault_injection.c src/server/session_store.c $(COMMON_SOURCES)
CLIENT_SOURCES := src/client/client.c src/client/checkpoint.c $(COMMON_SOURCES)
HEADERS := $(wildcard src/common/*.h src/client/*.h src/server/*.h)

.PHONY: all multi single clean

all: multi

multi: client
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o server \
		src/server/server_multi.c $(SERVER_SOURCES) $(CRYPTO_LIBS) $(LDLIBS)

single: client
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o server \
		src/server/server_single.c $(SERVER_SOURCES) $(CRYPTO_LIBS) $(LDLIBS)

client: src/client/main.c $(CLIENT_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
		src/client/main.c $(CLIENT_SOURCES) $(CRYPTO_LIBS) $(LDLIBS)

clean:
	rm -f server client
EOF
}

rm -rf "$OUT"
mkdir -p "$OUT"
add_part1
add_case Part2_Case1 server/server_case1.c client/main_case1.c
add_case Part2_Case2 server/server_case2.c client/main_case2.c
add_case Part2_Case2_Enhanced server/server_enhanced.c client/main_enhanced.c
cp "$REPORT" "$OUT/FinalReport_Group_064_091.pdf"
mkdir -p "$OUT/Results"
cp -R Results/. "$OUT/Results/"

cat >"$OUT/README.md" <<'EOF'
# IT305 Socket Programming Project - Groups 064 and 091

Each case folder is self-contained; `make` inside it creates exactly two
executables, `server` and `client`. Requirements: a C11 compiler, pthreads,
and OpenSSL's libcrypto.

| Folder | Server | Description |
|---|---|---|
| `Part1` (`make single`) | sequential | completes one request before the next |
| `Part1` (`make` or `make multi`) | concurrent | one worker thread per client |
| `Part2_Case1` | concurrent + faults | no session state; client restarts from the beginning |
| `Part2_Case2` | concurrent + faults | session store + durable checkpoints; client resumes |
| `Part2_Case2_Enhanced` | concurrent + faults | parallel TCP connections and non-overlapping byte range reassembly |

```sh
cd Part1 && make                 # multi-threaded server (default)
./server <port> <topics_root_dir>
./client <server_ip> <server_port> <topic> <output_dir>
```

Run `make clean && make single` to rebuild `./server` in sequential mode.

Build and run each Part 2 case independently. The same required invocation
works without any extra arguments in all three folders:

```sh
cd Part2_Case1 && make
./server <port> <topics_root_dir> <failure_probability>
./client <server_ip> <server_port> <topic> <output_dir>

cd ../Part2_Case2 && make
./server <port> <topics_root_dir> <failure_probability>
./client <server_ip> <server_port> <topic> <output_dir>

cd ../Part2_Case2_Enhanced && make
./server <port> <topics_root_dir> <failure_probability>
./client <server_ip> <server_port> <topic> <output_dir>
```

The enhanced client optionally accepts a fifth argument for the number of
parallel connections, but defaults to four when the required invocation above
is used unchanged.

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
streaming from the earlier of the client's durable checkpoint and the server's
last stored ACK. This bounds retransmission to unacknowledged in-flight data;
the measured amount depends on the checkpoint interval and ACK window.
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
    if [ "$(basename "$case_dir")" = Part1 ] &&
       ! (cd "$scratch" && make clean >/dev/null 2>&1 && make single >/dev/null 2>&1 &&
          make clean >/dev/null 2>&1 && make multi >/dev/null 2>&1); then
        rm -rf "$scratch"
        echo "submission: Part1 single or multi mode does not build" >&2
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

tar -czf "$archive" -C "$out_parent" "$out_name"

tar -tzf "$archive" | grep -q "^$out_name/FinalReport_Group_064_091.pdf$" || {
    echo "submission: archive is missing the final report" >&2
    exit 1
}
for required in Part1 Part2_Case1 Part2_Case2 Part2_Case2_Enhanced Results README.md; do
    tar -tzf "$archive" | grep -Eq "^$out_name/$required(/|\$)" || {
        echo "submission: archive is missing $required" >&2
        exit 1
    }
done

echo "submission directory: $OUT"
echo "submission archive: $archive"
