# Computer-Networks-Lab6

IT305 socket-programming project for Groups 064 and 091.

This repository contains the implementation of the fault-tolerant topic-based
file distribution service described in the project brief. Everything builds
from the single top-level `Makefile`.

## Layout

```text
common/        Shared protocol, serialization, socket I/O, and CLI helpers
server/        Part 1 servers: single-threaded and thread-per-client
client/        Client library used by every case, plus the client program
docs/          Protocol contract and integration rules
tests/         Unit tests, fault-injection tests, end-to-end script, sample topics
sources/       Read-only assignment and design references (not edited)
```

## Build and test

The project targets a POSIX environment with a C11 compiler, pthreads, and
OpenSSL's libcrypto (found through `pkg-config` when available; on macOS install
it with `brew install openssl`).

```sh
make          # build/server_single, build/server_multi, build/client, test binaries
make test     # protocol unit tests and client fault-injection tests
make e2e      # real client against both real servers over TCP
```

## Running

A dataset root holds one directory per topic; regular files directly inside a
topic directory are served. Topic names are the directory names exactly as they
appear on disk (the Animals-10 dataset uses lowercase: `cane`, `gatto`, ...).

```sh
./build/server_multi 9000 /path/to/dataset/raw-img
./build/client 127.0.0.1 9000 cane downloads        # writes downloads/cane/
```

Client usage: `client <server_ip> <server_port> <topic> <output_dir>`, the
interface required for every case. On success it prints the number of verified
files and bytes, the completion time from first request to last verified file,
and the throughput. Each blocking send and receive times out after 30 s.

| Exit | Meaning |
|---:|---|
| 0 | every file received and verified |
| 1 | usage error or invalid topic (nothing sent) |
| 2 | server answered with `ERROR`, e.g. unknown topic |
| 3 | network failure: connect, disconnect, truncated frame, timeout |
| 4 | protocol violation by the server |
| 5 | integrity failure: size or SHA-256 mismatch |
| 6 | local file system failure |

Codes 3 to 5 are the ones a retrying case (Part 2 Case 1) should retry.

## Client file integrity

- The manifest is validated before anything is written: sequential indexes,
  unique single-component names (no `/`, `\`, `.`, `..`, control characters, or
  embedded NUL), no trailing bytes, and no size overflow.
- `output_dir/topic/` is created only after the server accepts the topic.
- Each file is written to `output_dir/topic/.<name>.part`, flushed, and then its
  on-disk size and SHA-256 are checked against the manifest. Only a verified file
  is renamed to its final name, and the `ACK` is sent after that rename is
  flushed. A previous copy of the file is never replaced by a bad one.
- On disconnect, timeout, or corruption the partial file is deleted, no `ACK` is
  sent for it, and the client sends a best-effort `ERROR` explaining why.
- Two manifest names that resolve to the same file on the local file system
  (for example `README` and `readme` on macOS) are rejected instead of
  overwriting each other.

The library in `client/client.h` exposes these steps separately (manifest
parsing, the temporary-file sink, SHA-256 of a file, connect) so the Part 2
cases can reuse them for reconnects, resume, and byte ranges.
