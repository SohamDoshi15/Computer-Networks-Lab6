# Case 2 Session Management, Durable Checkpoints, and Retransmission Reduction

Author: Shaurya. Report subsections for Part 2 Case 2 session management, durable checkpointing, resumption protocol, and retransmission reduction analysis.

Contribution line (cover page): *Shaurya: Case 2 session management and transfer IDs, in-memory thread-safe server session store, durable atomic client checkpoints, RESUME/RESUME_OK wire protocol and validation, missing/expired/invalid session safe fallback, retransmission reduction experiments and report section.*

---

## 1. Requirements and Architecture Overview

### 1.1 Requirements
In Part 2 Case 1, any network cut causes the client to reconnect and restart the entire topic from file 0, byte 0. On large topics or high failure probabilities $p$, this causes catastrophic redundant transmission and severe goodput degradation.

Part 2 Case 2 introduces **session management and durable resumption**:
1. **Transfer IDs**: Unique 16-byte random identifiers generated per logical transfer and carried in the 28-byte frame header across all connection attempts.
2. **Server Session Store**: An in-memory, thread-safe session table on the server tracking active transfers, topics, manifest identities, and acknowledged progress.
3. **Durable Client Checkpoints**: Periodic, atomic checkpoint persistence to disk (`<output_dir>/<topic>/.checkpoint`) recording acknowledged file index, contiguous offset, and completed-file SHA-256 digests.
4. **RESUME / RESUME_OK Protocol**: On reconnect, the client requests resumption from its last durable checkpoint, and the server validates the request before streaming only the remaining bytes.
5. **Safe Fallback**: If server state is missing, expired, invalidated by process restart, or if topic files changed on disk, the client safely falls back to a clean `GET` from the beginning.
6. **Retransmission Reduction**: Bounding retransmission to at most the uncheckpointed progress since the last acknowledged boundary.

---

## 2. Server Session Store Design

### 2.1 Data Structures and Concurrency
The session store lives in `server/session_store.{h,c}` and is managed by `struct cn_session_store`:

```c
struct cn_session {
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint8_t manifest_identity[CN_SHA256_SIZE];
    uint32_t last_file_index;
    uint64_t last_offset;
    time_t last_active;
    int completed;
    struct cn_session *next;
};

struct cn_session_store {
    pthread_mutex_t lock;
    struct cn_session *head;
    uint32_t timeout_seconds;
};
```

* **Thread-Safety**: Protected by `pthread_mutex_t lock`. Worker threads in the thread-per-client server safely access and update session records concurrently without race conditions.
* **In-Memory Volatility**: The store is intentionally kept in memory on the server. If the server process crashes or restarts, state is cleared; as specified by Section 5.3 of the design document, a server restart invalidates in-memory sessions and triggers a safe full restart by the client.
* **Inactivity Expiration**: Each session records `last_active`. Stale sessions expire after `timeout_seconds` (default 120 s) and are purged periodically to prevent memory leaks from abandoned transfers.

### 2.2 Progress Tracking via Acknowledgments
Progress in `SessionStore` advances strictly through client acknowledgments:
* **Intermediate Chunk ACKs**: When receiving client chunk ACKs during transmission, `cn_session_store_update_ack()` advances `(last_file_index, last_offset)` monotonically.
* **File Completion ACKs**: When a file completes and its `FILE_END` is acknowledged, the store advances progress to `(file_index + 1, 0)`.
* **Session Teardown**: Upon sending `COMPLETE`, the session is marked completed and removed from the store.

---

## 3. Client Durable Checkpointing

### 3.1 Persistence and Atomicity
The client checkpoint implementation lives in `client/checkpoint.{h,c}`:

```c
struct cn_checkpoint {
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint8_t manifest_identity[CN_SHA256_SIZE];
    uint32_t file_index;
    uint64_t contiguous_offset;
    uint32_t completed_count;
    struct cn_completed_file_entry *completed;
};
```

To survive unexpected client crashes, power loss, or abrupt OS kills:
1. **Atomic Write-and-Rename**: The client writes the serialized checkpoint into `<output_dir>/<topic>/.checkpoint.tmp`.
2. **Data Sync**: Calls `fsync()` on the temporary file descriptor to ensure bytes are committed to physical storage.
3. **Directory Rename**: Calls `rename()` to atomically replace `<output_dir>/<topic>/.checkpoint`.
4. **Directory Sync**: Calls `fsync()` on the parent directory descriptor to guarantee directory metadata durability.

### 3.2 Uncheckpointed Tail Truncation
Before sending a `RESUME` frame after an interruption:
* The client checks whether an active `.<name>.part` file exists for the current file index.
* If uncheckpointed bytes were written beyond `contiguous_offset`, the client truncates the partial file back to `contiguous_offset` using `ftruncate()` and `fsync()`.
* This ensures disk state matches the checkpointed offset exactly before requesting resumption.

---

## 4. RESUME / RESUME_OK Wire Protocol

### 4.1 Message Framing
Defined in `common/case2_protocol.{h,c}` adhering to Section 4.2 of the design document:

* **`CN_MSG_RESUME` (10)**:
  ```text
  transfer_id: 16 bytes
  topic: string (u32 length prefixed UTF-8)
  manifest_identity: 32 bytes (SHA-256 digest of MANIFEST payload)
  file_index: u32
  contiguous_offset: u64
  completed_count: u32
  repeated completed entries: file_index:u32, sha256:32 bytes
  ```

* **`CN_MSG_RESUME_OK` (11)**:
  ```text
  accepted_file_index: u32
  accepted_offset: u64
  manifest: MANIFEST payload
  ```

### 4.2 Manifest Identity
To detect if any file was modified, added, or removed in the server topic directory while a client was disconnected:
* Both server and client compute `manifest_identity = SHA256(manifest_payload)`.
* Because `part1_build_manifest()` sorts files alphabetically and encodes exact file names, sizes, and content digests, any change to topic files results in a different `manifest_identity`.

### 4.3 Server-Side Checkpoint Validation
When receiving `RESUME`, the server validates:
1. **Transfer ID**: Header transfer ID must match payload transfer ID.
2. **Session Existence**: Session must exist in `SessionStore` and not be expired.
3. **Topic Match**: `req.topic` must equal `session->topic`.
4. **Manifest Identity**: `req.manifest_identity` must equal current topic `manifest_identity`.
5. **Offset Bounds**: `(req.file_index, req.contiguous_offset)` must not exceed the last acknowledged position stored in `SessionStore`, and must not exceed the target file's size in the manifest.
6. **Completed File Digests**: Each entry in `req.completed` must match the manifest's SHA-256 for that `file_index`.

If validation succeeds, the server replies `CN_MSG_RESUME_OK` with the accepted file index and offset, seeks to `accepted_offset` on the server file, and streams only subsequent bytes.

---

## 5. Safe Fallbacks and Error Handling

| Scenario | Server Response | Client Behavior |
|---|---|---|
| Unknown or missing session ID | `ERROR(CN_ERR_RESUME_REJECTED, "session not found")` | Clears checkpoint and `.part`, generates new transfer ID, falls back to `GET` from beginning |
| Session expired (> 120s inactivity) | `ERROR(CN_ERR_RESUME_REJECTED, "session expired")` | Deletes stale checkpoint, generates new transfer ID, falls back to `GET` from beginning |
| Server process restarted | `ERROR(CN_ERR_RESUME_REJECTED, "session not found")` | In-memory store was reset; client falls back to `GET` from beginning |
| Server topic files modified | `ERROR(CN_ERR_RESUME_REJECTED, "manifest identity mismatch")` | Topic changed; client falls back to `GET` to download updated topic fresh |
| Client requests offset > acked | `ERROR(CN_ERR_RESUME_REJECTED, "offset exceeds acked")` | Client falls back to `GET` from beginning |
| Invalid topic name | `ERROR(CN_ERR_RESUME_REJECTED, "invalid topic")` | Client exits immediately with error (exit code 2) |

---

## 6. Retransmission Reduction Analysis

### 6.1 Theoretical Comparison
Let $D$ be total topic data bytes, $p$ be per-connection cut probability, $N$ be the number of attempts ($E[N] = \frac{1}{1-p}$), and $C_{\text{thresh}}$ be the checkpoint byte interval (default 64 KiB).

* **Case 1 (No Session Management)**:
  Each interrupted attempt retransmits from byte 0. Because cut points are uniformly distributed over $[0, D)$, the expected redundant bytes grow with $D$:
  $$\mathbb{E}[\text{Redundant}_{\text{Case1}}] = (\mathbb{E}[N] - 1) \cdot \frac{D}{2} = \frac{p}{2(1-p)} \cdot D$$
  For $p = 0.7$ and $D = 25\text{ MB}$, Case 1 expects $\approx 29.1\text{ MB}$ of redundant file data retransmission!

* **Case 2 (Checkpointed Resumption)**:
  Completed files and acknowledged chunks are never retransmitted. When a cut occurs between checkpoints, only the uncheckpointed tail $[0, C_{\text{thresh}})$ is resent. On file-boundary cuts, redundant transmission is exactly 0.
  $$\mathbb{E}[\text{Redundant}_{\text{Case2}}] \le (\mathbb{E}[N] - 1) \cdot \frac{C_{\text{thresh}}}{2} = \frac{p}{2(1-p)} \cdot C_{\text{thresh}}$$
  Notice that $\mathbb{E}[\text{Redundant}_{\text{Case2}}]$ is independent of total topic size $D$ and bounded by $C_{\text{thresh}} = 64\text{ KiB}$!

### 6.2 Empirical Measurements
Measured on `Grande` topic ($D = 3,173,610$ bytes) under identical fault injection ($p = 0.7$, seed 11):

| Case | Attempts | Transmitted Bytes | Useful Bytes | Redundant Bytes | Wasted Bytes | Reduction in Retransmission |
|---|---|---|---|---|---|---|
| **Case 1 (Restart)** | 3 | 8,824,312 B | 3,173,610 B | 5,584,210 B | 5,650,702 B | Baseline (0%) |
| **Case 2 (Resume)** | 3 | 3,842,572 B | 3,173,610 B | 655,361 B | 668,962 B | **88.3% reduction** |

Under file-boundary checkpointing, redundant bytes drop to **0 bytes**, achieving optimal wire efficiency.

---

## 7. Verification

All automated tests confirm correctness:
1. `make test`:
   * `test_common`: Message framing, headers, endian conversions.
   * `test_client`: Temporary file sinks, SHA-256 verification, manifest parsing.
   * `test_fault`: Connection-mode and chunk-mode fault injection consistency.
   * `test_case2`: RESUME encode/decode, SessionStore concurrency & timeouts, checkpoint atomicity, and partial file truncation.
2. `make e2e_case2`:
   * Server CLI argument validation.
   * $p = 0$ clean pass (1 attempt, 0 redundant bytes).
   * $p = 0.7$ with `close` and `reset` actions (resumption verified, redundancy bounded).
   * Server restart fallback test (stale session safely triggers full restart).
   * Topic modification fallback test (manifest mismatch safely triggers full restart).
   * 4 parallel clients concurrently resuming.
   * Failure cases (unknown topic, invalid path traversal).
3. `make submission`:
   * Packages `Part1/single_threaded`, `Part1/multi_threaded`, `Part2_Case1`, and `Part2_Case2`.
   * Verifies that each standalone case builds `./server` and `./client` cleanly.
