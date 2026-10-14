# Part 1 server starter

This directory contains the initial Part 1 server implementation:
- `server_common.c/.h`: topic validation/discovery, sorted manifest, SHA-256, transfer logic.
- `server_single.c`: sequential server.
- `server_multi.c`: thread-per-client server.

Build from this directory:
```sh
make
```

Executables:
- `build/single/server`
- `build/multi/server`

Run with a dataset root whose immediate child directories are topics:
```sh
./build/single/server 9000 /path/to/topics_root_dir
./build/multi/server 9001 /path/to/topics_root_dir
```

This is a server-side starter and does not include a client. It expects the client to follow `../docs/PROTOCOL.md`, including sending `ACK` after each complete file. The manifest includes regular files directly inside a topic directory; symbolic links and subdirectories are not transferred.
