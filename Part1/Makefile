CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS ?=
LDLIBS += -pthread -lcrypto

COMMON = ../common/protocol.c ../common/serialization.c ../common/socket_io.c ../common/cli.c
SERVER_COMMON = server_common.c
INCLUDES = -I../common

.PHONY: all clean
all: build/single/server build/multi/server

build/single/server: server_single.c $(SERVER_COMMON) server_common.h $(COMMON)
	mkdir -p build/single
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -o $@ server_single.c $(SERVER_COMMON) $(COMMON) $(LDLIBS)

build/multi/server: server_multi.c $(SERVER_COMMON) server_common.h $(COMMON)
	mkdir -p build/multi
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -o $@ server_multi.c $(SERVER_COMMON) $(COMMON) $(LDLIBS)

clean:
	rm -rf build
