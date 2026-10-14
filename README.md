# Computer-Networks-Lab6

IT305 socket-programming project for Groups 064 and 091.

This repository contains the implementation of the fault-tolerant topic-based
file distribution service described in the project brief.

The first implemented milestone is the shared protocol foundation under
`common/`. It defines the on-wire frame header, message identifiers,
serialization rules, reliable socket I/O, and strict command-line parsers that
all four required cases will use.

## Layout

```text
common/        Shared protocol, serialization, socket I/O, and CLI helpers
docs/          Protocol contract and integration rules
tests/         Focused tests for the shared foundation
sources/       Read-only assignment and design references (not edited)
```

## Build and test

The project targets a POSIX environment with a C11 compiler and pthreads:

```sh
make test
```

The test binary is created under `build/` and exercises header encoding,
payload serialization, fragmented socket traffic, malformed frames, and CLI
argument parsing.
