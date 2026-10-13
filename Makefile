CC ?= cc
CPPFLAGS ?= -Icommon
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS ?=
LDLIBS ?= -pthread

BUILD_DIR := build
COMMON_SOURCES := common/protocol.c common/serialization.c common/socket_io.c common/cli.c
TEST_SOURCES := tests/test_common.c
TEST_BINARY := $(BUILD_DIR)/test_common

.PHONY: all test clean

all: $(TEST_BINARY)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TEST_BINARY): $(COMMON_SOURCES) $(TEST_SOURCES) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(COMMON_SOURCES) $(TEST_SOURCES) $(LDLIBS)

test: $(TEST_BINARY)
	./$(TEST_BINARY)

clean:
	rm -rf $(BUILD_DIR)
