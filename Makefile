CC ?= cc
CPPFLAGS ?= -Icommon -Iclient
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS ?=
LDLIBS ?= -pthread

# SHA-256 comes from OpenSSL's libcrypto; pkg-config also finds Homebrew installs.
CRYPTO_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)

# macOS hides O_NOFOLLOW and asprintf when _POSIX_C_SOURCE is defined.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif

BUILD_DIR := build
COMMON_SOURCES := common/protocol.c common/serialization.c common/socket_io.c common/cli.c
SERVER_SOURCES := server/server_common.c $(COMMON_SOURCES)
CLIENT_SOURCES := client/client.c $(COMMON_SOURCES)
HEADERS := $(wildcard common/*.h server/*.h client/*.h)

PROGRAMS := $(BUILD_DIR)/server_single $(BUILD_DIR)/server_multi $(BUILD_DIR)/client
TESTS := $(BUILD_DIR)/test_common $(BUILD_DIR)/test_client

LINK_WITH_CRYPTO = $(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
	$(filter %.c,$^) $(CRYPTO_LIBS) $(LDLIBS)

.PHONY: all test e2e clean

all: $(PROGRAMS) $(TESTS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/server_single: server/server_single.c $(SERVER_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/server_multi: server/server_multi.c $(SERVER_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/client: client/main.c $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/test_client: tests/test_client.c $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/test_common: tests/test_common.c $(COMMON_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

test: $(TESTS)
	./$(BUILD_DIR)/test_common
	./$(BUILD_DIR)/test_client

e2e: $(PROGRAMS)
	sh tests/e2e.sh

clean:
	rm -rf $(BUILD_DIR)
