# Running Part 2 Case 1 on Three Computers

One computer runs the server. The other two each run several clients at the
same time. Nothing in the code depends on running on one machine:
* The server listens on every interface (`INADDR_ANY`).
* The client connects to whatever IP address it is given.
* The port, dataset path and IP all come from the command line.

## 1. Prepare all three computers

```sh
git clone https://github.com/SohamDoshi15/Computer-Networks-Lab6
cd Computer-Networks-Lab6
make            # needs gcc/cc, make, pkg-config, OpenSSL (Ubuntu: sudo apt install build-essential libssl-dev pkg-config)
make test       # all three suites should print "passed"
```

Only the server needs the dataset (`raw-img/`).

On the **server** computer:

```sh
hostname -I            # Linux: note the LAN address, e.g. 10.100.x.y   (macOS: ipconfig getifaddr en0)
```

On **each client** computer, check that the server is reachable:

```sh
ping -c 3 <server_ip>
```

## 2. One run (fixed failure probability)

**Server** (start fresh for every probability, with a fixed seed so the run
can be repeated):

```sh
cd /path/to/raw-img/scoiattolo && sha256sum * | sort > ~/scoiattolo.sha256 && cd -
CN_FAULT_SEED=42 CN_SERVER_METRICS_CSV=Results/Part2_Case1_lab_server_p0.5.csv \
    ./build/server_case1 9000 /path/to/raw-img 0.5
```

Copy `~/scoiattolo.sha256` to both client computers (with `scp` or a USB stick).
The helper uses it to confirm every file arrived intact.

**Both client computers, started at the same time** (3 clients each in this example):

```sh
CN_FAULT_SEED=42 RUN_LABEL=1 \
    sh scripts/case1_client_node.sh <server_ip> 9000 scoiattolo 3 0.5 ~/scoiattolo.sha256
```

Each client node prints one `METRICS` line per client and `all files match the
server`. It also appends the metrics, using the same columns as the loopback
runner, to `Results/Part2_Case1_lab_runs_<hostname>.csv`. Increase `RUN_LABEL`
for each repetition and keep `CN_FAULT_SEED` equal to the seed used by the
server. Stop the server with Ctrl-C, then repeat for the next probability: 0,
0.1, 0.2, ... 0.9. Copy both client-node CSVs into the final `Results/`
directory; the report generator discovers and combines every
`Part2_Case1_lab_runs_*.csv` file.

Sanity checks after each run:

* **Connections:** the server prints one `conn=` line per client connection.
  The number of `result=injected_fault` lines equals the total of
  (attempts − 1) over all clients.
* **Bytes:** with the default `close` action, the sum of the clients'
  `transmitted_bytes` equals the sum of the server's `wire_bytes_sent`.

## 3. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `cannot connect ... Connection refused` | server not running, or wrong port |
| `cannot connect ... timed out` / `No route to host` | firewall on the server computer. Allow the port, e.g. `sudo ufw allow 9000/tcp`, or ask the lab admin |
| `bind/listen: Address already in use` | an old server is still running (`pkill server_case1`), or pick another port |
| transfer far slower than the link (about 0.3 MB/s) | a binary built before `TCP_NODELAY` was added. Pull and run `make` again (see §4) |
| client exits 3 after "server unreachable for 30 s" | the network was down longer than `CN_CONNECT_GIVEUP_SECONDS`; raise it if needed |

## 4. What was verified before the lab run

We had no access to three physical computers, so the setup was checked with
three Linux machines (Ubuntu 24.04, GCC 13) as Docker containers on a private
network. Each container had its own IP (server 172.30.0.10, clients .11 and
.12), and every link was shaped to 100 Mbit/s with 1 ms delay (`tc netem`).
Results:

| Test | Result |
|---|---|
| Build on Linux with `-Werror` | clean, 0 warnings; `make test`, `make e2e`, `make e2e_case1` pass |
| p = 0, 3 clients on each client node | 6/6 trees byte-identical; 155.5 MB in about 14 s, which fills the 100 Mbit/s link; client bytes = server bytes exactly |
| p = 0.5, `close` | 6 injected faults over 12 connections; 6/6 identical trees; client bytes = server bytes exactly (207,971,476) |
| p = 0.5, `reset` | 3 injected faults; 6/6 identical trees |
| real outage: client node unplugged from the network for 6 s mid-transfer | client timed out, retried connects while unreachable, reconnected, restarted, identical tree |
| Part 1 `server_multi` and `server_single` with the Part 1 client across nodes | correct |

Two problems appeared only across machines, and both are fixed:

1. **Nagle / delayed-ACK stall (affects every case, Part 1 included).**
   * **Cause:** frames are written as a header and then a payload, and each
     file ends with FILE_END → ACK. On Linux, each file stalled about 40 ms
     waiting for a delayed TCP ACK.
   * **Effect:** one client took 96 s for a topic that takes 0.4 s locally.
   * **Fix:** `cn_socket_set_nodelay()` (TCP_NODELAY) on every connection
     socket, in the client's connect and in the server's per-client handler.
     After the fix the same transfer took 1.8 s.
2. **The Case 1 client gave up too fast while the network was down.**
   * **Cause:** it allowed only 5 connect attempts, under 1 s in total.
   * **Fix:** it now keeps retrying with backoff for
     `CN_CONNECT_GIVEUP_SECONDS` (default 30 s).

One property to expect, not a bug: the protocol waits for an ACK after every
file. A single client therefore pays one round trip per file (1,862 files ×
about 5 ms ≈ 9.5 s in the simulation). On a lab LAN with a 0.2–0.5 ms round
trip this costs well under a second, and with several clients in parallel
the link stays fully used.
