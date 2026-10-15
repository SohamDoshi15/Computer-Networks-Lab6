#!/bin/sh
# Generates the graded layout from design document section 8: one folder per
# case, each self-contained, where `make` creates exactly ./server and ./client.
set -eu

OUT=${1:-build/FinalSubmission_Group_064_091}

# add_case <folder> <server main source>
add_case() {
    dir=$OUT/$1
    # Sources live under src/ so they cannot collide with the server and client executables.
    mkdir -p "$dir/src/common" "$dir/src/client" "$dir/src/server"
    cp common/*.c common/*.h "$dir/src/common/"
    cp client/client.c client/client.h client/main.c "$dir/src/client/"
    cp server/server_common.c server/server_common.h "$2" "$dir/src/server/"
    sed "s|@SERVER_MAIN@|src/server/$(basename "$2")|" >"$dir/Makefile" <<'EOF'
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

COMMON_SOURCES := src/common/protocol.c src/common/serialization.c src/common/socket_io.c \
	src/common/cli.c
HEADERS := $(wildcard src/common/*.h src/client/*.h src/server/*.h)

.PHONY: all clean

all: server client

server: @SERVER_MAIN@ src/server/server_common.c $(COMMON_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(CRYPTO_LIBS) $(LDLIBS)

client: src/client/main.c src/client/client.c $(COMMON_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(CRYPTO_LIBS) $(LDLIBS)

clean:
	rm -f server client
EOF
}

rm -rf "$OUT"
mkdir -p "$OUT"
add_case Part1/single_threaded server/server_single.c
add_case Part1/multi_threaded server/server_multi.c
# Part2_Case1, Part2_Case2 and Part2_Case2_Enhanced are added here as they land.

cat >"$OUT/README.md" <<'EOF'
# IT305 Socket Programming Project - Groups 064 and 091

Each case folder is self-contained; `make` inside it creates exactly two
executables, `server` and `client`. Requirements: a C11 compiler, pthreads,
and OpenSSL's libcrypto.

| Folder | Server |
|---|---|
| `Part1/single_threaded` | sequential: completes one request before the next |
| `Part1/multi_threaded` | one worker thread per client |

```sh
cd Part1/multi_threaded && make
./server <port> <topics_root_dir>
./client <server_ip> <server_port> <topic> <output_dir>
```

The client writes the topic to `<output_dir>/<topic>/`. Every file is written
to a temporary name and published only after its size and SHA-256 match the
server's manifest.

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
