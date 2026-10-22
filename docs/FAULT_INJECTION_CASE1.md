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

## 4. Validation measurements

The CSV files and figures under `Results/` were regenerated on 22 October
2026 from the current binaries. The validation workload contains ten random
binary files totaling 12,582,910 bytes. Server and clients ran on one WSL2
host over loopback; three repetitions were collected at connection failure
probabilities 0, 0.2, and 0.5. `Results/README.md` records the exact scope and
commands.

| p | mean attempts | mean goodput (MB/s) | mean redundant data (MB) |
|---:|---:|---:|---:|
| 0.0 | 1.00 | 21.54 | 0.00 |
| 0.2 | 1.33 | 19.78 | 0.99 |
| 0.5 | 1.33 | 25.63 | 2.18 |

The p=0.5 goodput is higher than p=0 in this small three-run sample because
two of the three seeded runs completed without a cut. It is sampling noise,
not evidence that faults improve performance. The byte counters still expose
the cost of the interrupted run: mean redundant data rose from zero to 2.18
MB. A larger repeated sweep is required for a stable high-p estimate.

With per-connection probability `p`, the theoretical attempt count is
geometric: `E[N] = 1/(1-p)`. A failed attempt cut uniformly through a topic of
size `D` carries about `D/2` bytes that Case 1 must later resend, giving
`E[redundant] = pD/(2(1-p))`. Case 2 avoids full restart by acknowledging and
persisting checkpoints.

These loopback measurements verify the instrumentation and comparison
pipeline. They do not replace the assignment's required Animals-10 run across
three computers. Use `docs/THREE_COMPUTER_RUN.md` and rerun the same scripts
on the lab machines before treating the performance section as final evidence.
