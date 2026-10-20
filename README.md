# Computer-Networks-Lab6

IT305 socket-programming project for Groups 064 and 091.

This repository contains the implementation of the fault-tolerant topic-based
file distribution service described in the project brief. Everything builds
from the single top-level `Makefile`.

## Layout

```text
common/        Shared protocol, serialization, socket I/O, and CLI helpers
server/        Part 1 servers, Part 2 Case 1 server, fault injection
client/        Client library used by every case, plus the client program
docs/          Protocol contract and integration rules
tests/         Unit tests, fault-injection tests, end-to-end script, sample topics
scripts/       Packaging, Case 1 experiment, summary and plotting scripts
Results/       Raw CSV logs and graphs from the experiments
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
make e2e_case1   # Case 1: fault injection, restarts, byte accounting over TCP
make e2e_case2   # Case 2: durable checkpoints, session resume over TCP
make e2e_enhanced # Enhanced Case 2: parallel TCP connections, range reassembly
make submission  # build/FinalSubmission_Group_064_091/, one folder per case
```

Development happens in this single shared tree. `make submission` generates the
graded layout from the design document: each case folder (for now
`Part1/single_threaded` and `Part1/multi_threaded`) is self-contained, and
`make` inside it creates exactly `./server` and `./client`. Part 2 cases are
added with one `add_case` line in `scripts/package_submission.sh`.

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

## Part 2 Case 1: fault injection and restart

```sh
./build/server_case1 9000 /path/to/dataset/raw-img 0.3   # p = 0.3
./build/client_case1 127.0.0.1 9000 cane downloads
```

The server makes one failure decision per connection, right after it sends the
MANIFEST: with probability p it cuts the connection at a file-data byte drawn
uniformly from the bytes it is about to send. The cut lands inside a DATA frame,
so the client sees a truncated frame. The client keeps nothing from a failed
attempt. It reconnects, sends a fresh `GET` with a new transfer ID, and
downloads the whole topic again. Optional environment variables:

| Variable | Side | Default | Meaning |
|---|---|---|---|
| `CN_FAULT_MODE` | server | `connection` | `chunk`: an independent decision before every 64 KiB DATA frame |
| `CN_FAULT_ACTION` | server | `close` | `reset`: abortive close (TCP RST) instead of FIN |
| `CN_FAULT_SEED` | server | clock | fixed seed, so the same decisions repeat |
| `CN_SERVER_METRICS_CSV` | server | off | one CSV row per finished connection |
| `CN_MAX_ATTEMPTS` | client | 100 | stop after this many attempts (0 = no limit) |
| `CN_RETRY_DELAY_MS` | client | 50 | pause before reconnecting |
| `CN_TIMEOUT_SECONDS` | client | 30 | per send/receive timeout |
| `CN_CONNECT_GIVEUP_SECONDS` | client | 30 | keep retrying refused/unreachable connects this long |
| `CN_METRICS_CSV` | client | off | append the run's metrics to a CSV file |
| `CN_QUIET` | client | 0 | `1` hides the per-file lines |

Every client run ends with a `METRICS` line: attempts, transmitted bytes,
useful bytes, redundant bytes, overhead, completion time, goodput and raw
throughput. `docs/FAULT_INJECTION_CASE1.md` defines these metrics and analyzes
the results. To repeat the experiment:

```sh
sh scripts/run_case1_experiments.sh /path/to/raw-img scoiattolo 20 Results
python3 scripts/summarize_case1.py Results
python3 scripts/plot_case1.py Results /path/to/raw-img/scoiattolo   # needs matplotlib
```

For the server plus two client computers setup, follow
`docs/THREE_COMPUTER_RUN.md`; `scripts/case1_client_node.sh` runs the clients
on each client computer.

Every connection socket uses `TCP_NODELAY`. Without it, Linux stalls each file
about 40 ms (Nagle's algorithm meets delayed ACK) whenever client and server
are on different hosts.

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
