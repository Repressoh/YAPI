TEST_DIR ?= $(CURDIR)/build_tests
TEST_BIN := $(TEST_DIR)/proxy_stream_test
CONTROL_TEST_BIN := $(TEST_DIR)/proxy_control_test

HOSTCC ?= clang-18
TARGET ?= ps5-package-proxy.elf
DEBUG ?= 0
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -pthread -DPS5_PROXY_DEBUG_BUILD=$(DEBUG)
LDADD := -lSceIpmi -lSceAppInstUtil -lSceHttp -lSceSsl

ifeq ($(DEBUG),1)
CFLAGS += -g
else ifeq ($(DEBUG),0)
LDFLAGS += -Wl,--strip-all
else
$(error DEBUG must be 0 or 1)
endif

# The repository build skips this payload when the PS5 payload SDK is not installed.
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
# The SDK's bin/ wrappers are bash scripts that need llvm-config; Windows uses its win/ wrappers instead.
ifeq ($(OS),Windows_NT)
CC := $(PS5_PAYLOAD_SDK)/win/prospero-clang.cmd
endif
all: $(TARGET)
else
all:
	@echo "[!] PS5_PAYLOAD_SDK is not set; skipping $(TARGET)"
endif

$(TARGET): main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDADD)

test-host: test_stream.c test_control.c main.c
	mkdir -p $(TEST_DIR)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -pthread -DPS5_PROXY_HOST_TEST=1 -DPS5_PROXY_DEBUG_BUILD=$(DEBUG) -ffunction-sections -fdata-sections -Wl,--gc-sections test_stream.c -o $(TEST_BIN)
	$(TEST_BIN)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -pthread -DPS5_PROXY_HOST_TEST=1 -DPS5_PROXY_DEBUG_BUILD=$(DEBUG) -ffunction-sections -fdata-sections -Wl,--gc-sections test_control.c -o $(CONTROL_TEST_BIN)
	$(CONTROL_TEST_BIN)

clean:
	rm -f $(TARGET) $(TEST_BIN) $(CONTROL_TEST_BIN)

.PHONY: all clean test-host
