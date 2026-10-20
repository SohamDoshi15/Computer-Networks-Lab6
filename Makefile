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
COMMON_SOURCES := common/protocol.c common/serialization.c common/socket_io.c common/cli.c common/case2_protocol.c
SERVER_SOURCES := server/server_common.c server/fault_injection.c server/session_store.c $(COMMON_SOURCES)
CLIENT_SOURCES := client/client.c client/checkpoint.c $(COMMON_SOURCES)
HEADERS := $(wildcard common/*.h server/*.h client/*.h)

PROGRAMS := $(BUILD_DIR)/server_single $(BUILD_DIR)/server_multi $(BUILD_DIR)/client \
	$(BUILD_DIR)/server_case1 $(BUILD_DIR)/client_case1 \
	$(BUILD_DIR)/server_case2 $(BUILD_DIR)/client_case2
TESTS := $(BUILD_DIR)/test_common $(BUILD_DIR)/test_client $(BUILD_DIR)/test_fault $(BUILD_DIR)/test_case2

LINK_WITH_CRYPTO = $(CC) $(CPPFLAGS) $(CRYPTO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
	$(filter %.c,$^) $(CRYPTO_LIBS) $(LDLIBS)

.PHONY: all test e2e e2e_case1 e2e_case2 submission clean

all: $(PROGRAMS) $(TESTS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/server_single: server/server_single.c $(SERVER_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/server_multi: server/server_multi.c $(SERVER_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

# Part 2 Case 1: thread-per-client server with fault injection, restarting client.
$(BUILD_DIR)/server_case1: server/server_case1.c $(SERVER_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/client_case1: client/main_case1.c $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

# Part 2 Case 2: thread-per-client server with session store and checkpointed resume.
$(BUILD_DIR)/server_case2: server/server_case2.c $(SERVER_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/client_case2: client/main_case2.c $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/client: client/main.c $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/test_client: tests/test_client.c $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/test_fault: tests/test_fault.c server/fault_injection.c $(COMMON_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/test_common: tests/test_common.c $(COMMON_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

$(BUILD_DIR)/test_case2: tests/test_case2.c $(SERVER_SOURCES) $(CLIENT_SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(LINK_WITH_CRYPTO)

test: $(TESTS)
	./$(BUILD_DIR)/test_common
	./$(BUILD_DIR)/test_client
	./$(BUILD_DIR)/test_fault
	./$(BUILD_DIR)/test_case2

e2e: $(PROGRAMS)
	sh tests/e2e.sh

e2e_case1: $(PROGRAMS)
	sh tests/e2e_case1.sh

e2e_case2: $(PROGRAMS)
	sh tests/e2e_case2.sh

# The graded layout (design document section 8), generated under build/.
submission:
	sh scripts/package_submission.sh $(BUILD_DIR)/FinalSubmission_Group_064_091

clean:
	rm -rf $(BUILD_DIR)
