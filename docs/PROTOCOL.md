# Wire Protocol Contract

This document is normative for every client and server in Part 1, Part 2
Case 1, Part 2 Case 2, and Part 2 Case 2 Enhanced.

## Transport and byte order

The transport is TCP. TCP is a byte stream, so receivers must read the complete
fixed-size header followed by exactly the declared payload length. A successful
partial `send()` or `recv()` does not complete a frame.

All multi-byte integers use unsigned network byte order. Strings are UTF-8 byte
sequences prefixed by an unsigned 32-bit byte length. Strings do not include a
terminating NUL on the wire. Binary data may contain zero bytes.

## Fixed 28-byte frame header

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 1 | version | unsigned byte; currently `1` |
| 1 | 1 | message type | value from `enum cn_message_type` |
| 2 | 2 | flags | unsigned network byte order |
| 4 | 8 | payload length | unsigned network byte order; payload only |
| 12 | 16 | transfer ID | opaque bytes; all zero before an ID exists |

Receivers reject unsupported versions, unknown message types, and payloads
larger than 64 MiB. A connection must be closed after a malformed or oversized
frame because its stream position can no longer be trusted.

## Stable message identifiers

| ID | Name | Purpose |
|---:|---|---|
| 1 | `GET` | Request one topic |
| 2 | `MANIFEST` | Describe the ordered files in a topic |
| 3 | `FILE_START` | Begin or identify one file |
| 4 | `DATA` | Carry file bytes at an explicit offset |
| 5 | `FILE_END` | Finish one file |
| 6 | `ACK` | Confirm a contiguous persisted offset |
| 7 | `ERROR` | Reject a request or report a protocol failure |
| 8 | `COMPLETE` | Finish the topic transfer |
| 9 | `CANCEL` | Optional application cancellation |
| 10 | `RESUME` | Request validated checkpoint resume |
| 11 | `RESUME_OK` | Accept a resume point |
| 12 | `ASSIGN_RANGE` | Assign a byte range in Enhanced Case 2 |
| 13 | `RANGE_READY` | Confirm a range assignment |
| 14 | `ACK_RANGE` | Confirm contiguous persisted range progress |
| 15 | `RANGE_END` | Finish one byte range |

IDs are append-only. Existing numeric values must never be reassigned.

## Sample design key terminology crosswalk

The instructor's sample design uses descriptive names rather than a required
wire-level enum. The implementation preserves the same semantics with the
stable messages above. This table makes the correspondence explicit for
evaluation and viva use.

| Sample key term | Implemented message(s) | Equivalent behavior |
|---|---|---|
| `GET` | `GET` | Client requests one topic. |
| `OK` | `MANIFEST` | Server accepts the topic and returns the ordered file list and metadata. |
| `START` | `GET` followed by `MANIFEST` | The accepted request starts transfer immediately; a redundant round trip is avoided. |
| `FILE_HDR` | `FILE_START` | Server identifies the next file, its size, and integrity digest. |
| `DATA` | `DATA` | Server sends a file chunk with file index, absolute offset, and length. |
| `ACK` | `ACK` | Client confirms the highest contiguous offset safely persisted. |
| `CLOSE` | `FILE_END` and `COMPLETE` | Server marks one file complete, then marks the whole topic complete. |
| `ERR` | `ERROR` | Either peer reports a rejected request or protocol/integrity failure. |
| Existing session ID in `GET` | `RESUME` and `RESUME_OK` | Client presents durable checkpoint state; server validates and accepts a bounded resume point. |

Enhanced Case 2 adds `ASSIGN_RANGE`, `RANGE_READY`, `ACK_RANGE`, and
`RANGE_END` to implement the sample key's permitted performance enhancement
using parallel, non-overlapping byte ranges.

## Payload formats

The field order below is part of the protocol.

```text
GET          topic:string
MANIFEST     entry_count:u32;
             repeated(file_index:u32, relative_name:string,
                      size:u64, sha256:32 bytes)
FILE_START   file_index:u32, relative_name:string, size:u64, sha256:32 bytes
DATA         file_index:u32, offset:u64, data_length:u32, data:data_length bytes
FILE_END     file_index:u32
ACK          file_index:u32, contiguous_offset:u64
ERROR        error_code:u32, message:string
COMPLETE     file_count:u32, total_file_bytes:u64
CANCEL       reason_code:u32
RESUME       transfer_id:16 bytes, topic:string, manifest_identity:32 bytes,
             file_index:u32, contiguous_offset:u64, completed_count:u32;
             repeated(file_index:u32, sha256:32 bytes)
RESUME_OK    accepted_file_index:u32, accepted_offset:u64,
             manifest:MANIFEST payload
ASSIGN_RANGE range_id:u32, file_index:u32, start_offset:u64, end_offset:u64
RANGE_READY  range_id:u32, file_index:u32, start_offset:u64, end_offset:u64
ACK_RANGE    range_id:u32, file_index:u32, contiguous_offset:u64
RANGE_END    range_id:u32, file_index:u32, end_offset:u64
```

The transfer ID in a `RESUME` payload must equal the transfer ID in that
frame's header. `DATA.data_length` must equal the number of remaining bytes in
the payload after its fixed fields.

## Limits

| Item | Limit |
|---|---:|
| Frame payload | 64 MiB |
| UTF-8 topic | 255 bytes |
| Relative filename | 4096 bytes |
| Error text | 4096 bytes |

Case implementations may enforce smaller contextual limits but must not accept
larger values than these shared limits.

## Disconnect and resume rules

- Case 1 discards partial progress and reconnects with a new `GET`.
- Case 2 resumes no further than the last contiguous offset written, flushed,
  durably checkpointed by the client, acknowledged to the server, and stored by
  the server.
- The client truncates any uncheckpointed tail before `RESUME`.
- The server validates the topic, manifest identity, file index, file size,
  offset bounds, and completed-file hashes before `RESUME_OK`.
- Enhanced ranges are half-open intervals `[start_offset, end_offset)`. Ranges
  for one file must not overlap.
