# Fault Injection, Part 2 Case 1, and Transfer Metrics

Author: Tirth. Report subsections for the fault-injection mechanism, Part 2
Case 1 (no session management), and the metrics used for every Part 2 case.

Contribution line (cover page): *Tirth: configurable server-side fault
injection, Case 1 reconnect-and-restart client and server, byte and time
accounting (transmitted, useful, redundant bytes, completion time,
throughput), Case 1 experiments and analysis.*

## 1. Fault injection

### 1.1 Requirements

The assignment asks the server to terminate an active client connection
during file transfer with a probability given by its third argument,
`./server <port> <topics_root_dir> <failure probability>`. It also asks us to
document when and how the failure decision is made. The mechanism also has to
be repeatable for experiments and the same in every Part 2 case, so that
Case 1, Case 2 and Case 2 Enhanced are compared under the same faults.

### 1.2 Design

Fault injection lives in one server module, `server/fault_injection.{h,c}`.
Each worker thread creates a `struct cn_fault_injector` for its connection, so
no locking is needed and no state carries over from one connection to the
next.

**When the decision is made (default `connection` mode).** The server decides
once per connection, immediately after it sends the `MANIFEST`. At that point
it knows how many file-data bytes, `D`, it is about to send on this connection
(for Case 2 this will be the bytes remaining after the resume offset). The
decision has two steps:

1. A Bernoulli trial: draw `u` uniformly from [0, 1). If `u < p`, this
   connection will fail.
2. If it fails, draw the cut point `c` uniformly from [0, D). The connection
   is interrupted after exactly `c` file-data bytes.

So `p` means exactly "the probability that a connection is interrupted
before it finishes", and the failure point is uniform over the transfer.
Topics with no file data (`D = 0`) are never cut, because no transfer is in
progress.

**How the connection is interrupted.** Every DATA frame goes through
`cn_fault_send_data_frame()`. When the frame contains byte `c`, the server
sends the 28-byte frame header, the 16 bytes of fixed DATA fields, and only
the first `c - sent` data bytes. Then it terminates the connection:

* `close` (default): `shutdown(SHUT_RDWR)`. The client receives every byte
  sent so far and then EOF in the middle of a frame (`TRUNCATED`). This is
  the hardest case for the receive path, and it makes byte accounting exact
  on both sides.
* `reset` (`CN_FAULT_ACTION=reset`): `SO_LINGER {1, 0}` makes the worker's
  `close()` send a TCP RST, as when a host crashes. The client may lose bytes
  it had not read yet. We also observed that the RST is not always processed
  by the client (TCP does not guarantee it). In that case the client notices
  the failure only through its receive timeout, just as it would after a real
  crash or a pulled cable. The e2e test covers this path with
  `CN_TIMEOUT_SECONDS=2`.

**Alternative `chunk` mode** (`CN_FAULT_MODE=chunk`). An independent Bernoulli
trial with probability `q` runs before every DATA frame (at most 64 KiB of
data), and the cut point is uniform inside the chosen frame. Here `q` is a
per-chunk hazard: the chance that a connection survives is `(1 - q)^n` for a
transfer of `n` frames, so large topics fail far more often. This models a
link with a constant failure rate. It shows why Case 1 does not scale, but it
makes `p` depend on topic size, so the main comparison uses `connection` mode.

**Randomness and repeatability.** Each connection has its own splitmix64
generator seeded with `seed XOR (connection_id * constant)`. The seed is taken
from `CN_FAULT_SEED`, or from the clock if it is not set; the server prints
the seed at startup. With a fixed seed, connection *k* always makes the same
decision whatever the thread timing, so an experiment can be rerun exactly.
`rand()` is not used because it is shared across threads.

**Observability.** Each finished connection prints one line with the
connection id, topic, outcome (`complete`, `injected_fault`, `failed`), files
acknowledged, file bytes sent out of planned, wire bytes in each direction,
duration and the cut point. `CN_SERVER_METRICS_CSV=<file>` appends the same
fields to a CSV file.

### 1.3 Alternatives considered

| Alternative | Why we did not use it |
|---|---|
| Kill the server process | Kills every client at once and cannot run on the multi-threaded server the assignment requires; no per-connection probability |
| Decide per byte or per `send()` call | Makes `p` depend on buffer sizes; a per-frame hazard is available as `chunk` mode |
| Cut only between frames or files | Never exercises truncated-frame handling, and for Case 2 never tests a resume offset inside a file |
| External tools (`iptables`, `tc`, `pfctl`) | Need root, differ between Linux and macOS, and are not controlled by the required `<failure probability>` argument |
| Shared `rand()` | Not thread-safe, and the results depend on thread scheduling |

### 1.4 Verification

`tests/test_fault.c` (`make test`) checks the following:

* `p = 0` never fires.
* `p = 1` always fires, and exactly `c` data bytes plus the partial frame
  reach the peer.
* The byte counters match what the peer received.
* Chunk mode at `q = 1` cuts the first frame.
* An empty transfer is never cut.
* The same seed gives the same decisions, and a different seed differs.
* Over 20,000 trials the observed failure rate is within ±0.02 of `p` for
  p ∈ {0.1, 0.3, 0.5, 0.9}, and the mean cut point is within 0.45–0.55 of `D`.
* Invalid `CN_FAULT_*` values are rejected.

## 2. Part 2 Case 1: no session management

### 2.1 Server

`server/server_case1.c` is the thread-per-client server from Part 1. It takes
the failure probability as its third argument and passes a fresh fault
injector to the shared `part1_serve_client_ex()`. It keeps no record of any
transfer after its connection ends. A client that reconnects is handled like
a new client.

### 2.2 Client: reconnect and restart

`client/main_case1.c` wraps the common client (`cn_client_fetch_topic`) in a
retry loop. Every attempt does the following:

1. Opens a new TCP connection and generates a new random transfer ID.
2. Sends `GET <topic>` and receives the full `MANIFEST`.
3. Receives every file again, from file 0 byte 0, through the common
   temporary-file and SHA-256 path.

When an attempt fails, the common client has already deleted the partial
`.part` file and sent no ACK for it. The Case 1 loop throws away everything
else it knows and starts the next attempt from the beginning. Files verified
in a failed attempt stay on disk, but they are downloaded and verified again,
because Case 1 keeps no record that they are complete.

Retry policy:

| Failure (exit code) | Action |
|---|---|
| network: EOF, truncated frame, RST, timeout (3) | retry after `CN_RETRY_DELAY_MS` (50 ms) |
| protocol violation (4), integrity / SHA-256 mismatch (5) | retry (the next attempt re-fetches the data) |
| server `ERROR`, e.g. unknown topic (2); bad arguments (1); local disk error (6) | stop: retrying cannot help |
| connect refused / network unreachable | exponential backoff (50 ms up to 2 s); give up once connects have failed for `CN_CONNECT_GIVEUP_SECONDS` (30 s), so a short outage is survived |
| attempt limit `CN_MAX_ATTEMPTS` (100) reached | stop with the last error |

The attempt limit keeps the client from looping forever when the probability
is so high that a full pass almost never succeeds (see chunk mode below).

### 2.3 Verification

`tests/e2e_case1.sh` (`make e2e_case1`) runs real processes over TCP and
checks the following:

* Server argument validation.
* `p = 0` needs exactly 1 attempt with 0 redundant bytes, and the client's
  transmitted bytes equal the server's.
* At `p = 0.7` with both `close` and `reset`, the client restarts and ends
  with a byte-identical tree and no `.part` files.
* The server logs one connection per client attempt, and every failed attempt
  is an injected fault.
* The client's transmitted bytes equal the sum of the server's wire bytes
  (`close` mode).
* Chunk mode works.
* 6 parallel clients each restart independently and all finish.
* At `p = 1` the client stops at `CN_MAX_ATTEMPTS`.
* A topic with no data is never cut.
* An unknown topic is not retried, and a stopped server gives exit code 3
  after the connect give-up window.

Across separate hosts (three Linux containers, 100 Mbit/s links with 1 ms
delay) we also checked the following; details are in
`docs/THREE_COMPUTER_RUN.md`:
* 6 concurrent clients on 2 client nodes at p = 0.5, with both `close` and
  `reset`, all ended with identical trees, and the byte counts matched across
  the network.
* A client whose network cable was pulled for 6 s mid-transfer timed out,
  retried, reconnected and finished.

### 2.4 TCP_NODELAY

Testing across hosts exposed a stall of about 40 ms per file on Linux, caused
by Nagle's algorithm meeting delayed ACK:
* Frames are written as a header followed by a payload.
* Every file ends in a turn: the server sends FILE_END, the client answers
  with ACK.

One client took 96 s for a 25 MB topic. All connection sockets now set
`TCP_NODELAY` (`cn_socket_set_nodelay`), and the same transfer takes 1.8 s.
This affects every case, so Part 1 and Case 2 measurements must use binaries
built after this fix.

## 3. Metrics

All byte counts are exact. `cn_send_all()` and `cn_recv_all()` add every byte
they move to per-thread counters (`cn_io_set_counters`), including the bytes
of a frame that a fault cuts short. The client counts across all of its
attempts; the server counts per connection.

| Metric | Definition |
|---|---|
| attempts | connections the client opened for this topic |
| transmitted bytes | every byte the client received from the server over all attempts: headers, manifests, data, partial frames |
| useful bytes | file bytes delivered and verified at the end (equals the topic size) |
| data bytes received | file bytes in every complete DATA frame over all attempts |
| redundant bytes | data bytes received − useful bytes: file data sent more than once |
| wasted bytes | transmitted bytes in failed attempts |
| overhead | (transmitted − useful) / useful |
| completion time | `CLOCK_MONOTONIC` from the first connect to the last verified file, including failed attempts and retry pauses |
| goodput | useful bytes / completion time |
| raw throughput | transmitted bytes / completion time |

The same counters work for Case 2 and Case 2 Enhanced. Redundant bytes are
the quantity that session management should reduce.

## 4. Case 1 analysis

### 4.1 Setup

* **Machine:** server and client ran on one Apple-silicon Mac (8 cores,
  macOS 26.5) over loopback, using the final binaries (with `TCP_NODELAY`).
  **These are not the three-computer measurements.** The group's
  three-computer runs should replace these numbers in the final report; the
  same scripts work unchanged there; see `docs/THREE_COMPUTER_RUN.md` for the
  procedure and for the multi-host verification already done.
* **Topic:** `scoiattolo` from Animals-10: 1,862 files, 25,409,193 bytes. Every
  file is smaller than 64 KiB, so each file is exactly one DATA frame.
* **Runs:** 20 per point, each with its own fixed fault seed, so every row
  can be reproduced. The default client settings were used: 50 ms retry
  pause, at most 100 attempts.
* **Part 1 baseline:** 20 runs of the same topic through `server_multi` and
  the Part 1 client, after one warm-up fetch so the page cache is hot.
* **Commands:**
  `sh scripts/run_case1_experiments.sh ../dataset/raw-img scoiattolo 20 Results`,
  then `scripts/summarize_case1.py` and `scripts/plot_case1.py`.
* **Raw data:** `Results/Part2_Case1_runs.csv` (one row per client run),
  `Results/Part2_Case1_server_connections.csv` (one row per server
  connection), `Results/Part2_Case1_part1_baseline.csv`.
* **Summary and graphs:** `Results/Part2_Case1_summary.csv`,
  `Part2_Case1_throughput.png`, `Part2_Case1_overhead.png`,
  `Part2_Case1_bytes.png`.

### 4.2 Model

With per-connection probability `p`, the number of attempts `N` is geometric,
so `E[N] = 1/(1-p)`. Each failed attempt is cut at a uniform point, so it
carries on average `D/2` file bytes, all of which must be sent again. That
gives:

* `E[redundant] = (E[N] - 1) * D/2 = p / (2(1-p)) * D`
* `overhead ≈ p / (2(1-p))`, plus about 2% for frame headers, the manifest
  and ACKs
* if time is proportional to bytes, `goodput ≈ Part1 * (1-p) / (1 - p/2)`

In chunk mode, an attempt survives all `n` frames with probability
`(1-q)^n`, so `E[N] = (1-q)^-n`. This grows exponentially with the topic
size.

### 4.3 Results, connection mode (Part 1 baseline: 0.377 s, 67.8 MB/s)

| p | attempts (model) | transmitted MB | redundant MB (model) | overhead (model) | completion s | goodput MB/s | vs Part 1 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1.00 (1.00) | 25.91 | 0.0 (0.00) | 0.020 (0.02) | 0.358 | 71.0 | 1.05 |
| 0.1 | 1.05 (1.11) | 26.57 | 0.64 (1.41) | 0.046 (0.08) | 0.397 | 64.0 | 0.95 |
| 0.2 | 1.10 (1.25) | 27.59 | 1.64 (3.18) | 0.086 (0.15) | 0.396 | 64.1 | 0.95 |
| 0.3 | 1.25 (1.43) | 30.29 | 4.28 (5.44) | 0.192 (0.23) | 0.472 | 53.8 | 0.80 |
| 0.4 | 1.85 (1.67) | 35.14 | 8.96 (8.47) | 0.383 (0.35) | 0.585 | 43.5 | 0.65 |
| 0.5 | 1.70 (2.00) | 37.22 | 11.04 (12.70) | 0.465 (0.52) | 0.611 | 41.6 | 0.62 |
| 0.6 | 2.40 (2.50) | 41.74 | 15.39 (19.06) | 0.643 (0.77) | 0.740 | 34.4 | 0.51 |
| 0.7 | 2.45 (3.33) | 46.39 | 19.96 (29.64) | 0.826 (1.19) | 0.806 | 31.5 | 0.47 |
| 0.8 | 5.40 (5.00) | 80.32 | 52.96 (50.82) | 2.161 (2.02) | 1.590 | 16.0 | 0.24 |
| 0.9 | 10.25 (10.00) | 143.57 | 114.57 (114.34) | 4.650 (4.52) | 2.955 | 8.6 | 0.13 |

Notes on the columns:

* Useful bytes are 25.41 MB in every run, because every run finished with a
  byte-identical, SHA-256-verified copy.
* Goodput here is useful bytes / mean completion time. The mean of the
  per-run ratios would overweight lucky runs.
* The model column for overhead includes the 2% framing cost.

### 4.4 Results, chunk mode (per-frame hazard q, n = 1,862 frames)

| q | attempts (model `(1-q)^-n`) | completed runs | transmitted MB | redundant MB | completion s | goodput MB/s |
|---:|---:|---:|---:|---:|---:|---:|
| 0      | 1.00 (1.0)     | 20/20 | 25.91  | 0.00   | 0.366  | 69.4 |
| 0.0005 | 2.65 (2.5)     | 20/20 | 43.19  | 16.78  | 0.766  | 33.2 |
| 0.001  | 7.80 (6.4)     | 20/20 | 82.34  | 54.57  | 1.796  | 14.2 |
| 0.002  | 45.95 (41.6)   | 18/20 | 308.63 | 236.25 | 8.603  | 3.4 among completed runs |
| 0.005  | 100 (≈ 11,300) | 0/20  | 284.78 | n/a    | 13.801 | 0, gave up |

At q = 0.002, two runs hit the 100-attempt limit. At q = 0.005, the expected
number of attempts is about 11,300, so no run finished. On average each
attempt there got through only about 2.8 MB (11%) before it was cut.

### 4.5 Observations

1. **The model predicts Case 1 well.** At p = 0.8 and p = 0.9, the measured
   attempts (5.4, 10.25), redundant bytes (53.0 MB, 114.6 MB) and overhead
   (2.16, 4.65) are within a few percent of `1/(1-p)` and `p/(2(1-p))`. At
   lower p, 20 runs of a geometric variable leave visible sampling noise. For
   example, p = 0.7 measured 2.45 attempts against 3.33 expected. The full
   per-run spread is shown by the min-max bars in `Part2_Case1_overhead.png`.
2. **Cost grows as 1/(1-p), not linearly.** Up to p ≈ 0.3, Case 1 loses about
   20% or less of the Part 1 goodput. At p = 0.9 it transmits 5.6× the topic
   size and delivers only 13% of the Part 1 goodput. At p = 0.9, 80% of all
   bytes on the wire are redundant (`Part2_Case1_bytes.png`).
3. **Completion time varies a lot.** At p = 0.9, completion time ranged from
   0.36 s to 10.2 s across runs (standard deviation 2.7 s around a mean of
   3.0 s). A Case 1 user cannot predict when a download will finish. The
   best case is one lucky pass; the worst run needed 35 attempts.
4. **Restarts have a fixed cost on top of the bytes.** Raw throughput
   (transmitted / time) also drops, from 73 to 53 MB/s, so the slowdown is
   not caused by the bytes alone. Regressing the 569 server connections in
   connection mode gives about 53 ms per connection plus 13.2 ms per MB:
   * The 53 ms is mostly the server rebuilding the manifest, which hashes
     all 1,862 files again for every new `GET`.
   * Each restart also includes the client's 50 ms retry pause.

   At p = 0.9 the 9.25 restarts cost about 1.0 s of the 3.0 s. Together,
   1.9 s of byte transfer (143.6 MB) plus 0.54 s of connection setup plus
   0.46 s of pauses predicts 2.9 s, against 2.96 s measured.
5. **Case 1 does not scale with topic size.** Chunk mode is a constant
   failure rate per unit of data. There, the chance of finishing a whole
   topic in one pass falls exponentially with its size:
   * At q = 0.001, `scoiattolo` needs about 8 passes.
   * At q = 0.005 it practically never finishes; the model predicts about
     11,000 passes.
   * The largest topic, `gatto` (147 MB), would be far worse at the same q.

   This is the main argument for Case 2. A checkpoint turns the cost from
   *exponential in topic size* into *a bounded re-send per failure*.
6. **Correctness under faults.** Every completed run produced a
   byte-identical tree with no `.part` files left behind. No partial or
   corrupted file was ever published under its final name. Faults landing in
   the middle of a frame, with both FIN and RST, were all detected as network
   errors and retried.

### 4.6 What Case 2 should be compared on

Case 2 must run the same sweep: the same seeds, the same topic and
`CN_FAULT_MODE=connection`. Its redundant bytes should drop from
`p/(2(1-p)) * D` toward the re-send of at most one checkpoint interval per
failure. Its chunk-mode completion should stay finite at q = 0.005.
