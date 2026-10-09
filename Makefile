# pytte build. Plain make, system compiler, libcurl via pkg-config.

CC       ?= cc
AR       ?= ar
CSTD     := -std=c99
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS   ?= -O2 -g
CPPFLAGS += -Iinclude -Isrc
BUILD    := build

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)

# Portable parts: no stdio, no malloc, no floating point.
BASE_SRC := $(wildcard src/base/*.c)
AI_SRC   := $(wildcard src/ai/*.c)
LIB_SRC  := $(BASE_SRC) $(AI_SRC)
# Host parts.
SYS_SRC  := src/drv/sys_posix.c src/drv/http_faux.c
CURL_SRC := src/drv/http_curl.c
CLI_SRC  := src/cli/pytte_ai.c

LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/obj/%.o)
HOST_OBJ := $(SYS_SRC:%.c=$(BUILD)/obj/%.o) $(CURL_SRC:%.c=$(BUILD)/obj/%.o) $(CLI_SRC:%.c=$(BUILD)/obj/%.o)

.PHONY: all test portable fuzz clean fixtures-update
all: $(BUILD)/pytte-ai $(BUILD)/libpytte-ai.a

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libpytte-ai.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/pytte-ai: $(HOST_OBJ) $(BUILD)/libpytte-ai.a
	$(CC) $(CFLAGS) $^ $(CURL_LIBS) -o $@

# ---- tests: built from source with ASan and UBSan ----
TEST_CC     ?= $(CC)
TEST_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TESTS       := $(patsubst tests/%.c,$(BUILD)/test/%,$(wildcard tests/test_*.c))

$(BUILD)/test/%: tests/%.c tests/t.h $(LIB_SRC) $(SYS_SRC) $(wildcard include/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(TEST_CC) $(CSTD) $(WARN) $(TEST_CFLAGS) $(CPPFLAGS) $< $(LIB_SRC) $(SYS_SRC) -o $@

test: $(TESTS) $(BUILD)/pytte-ai
	@for t in $(TESTS); do ./$$t || exit 1; done
	@echo "cli:" && sh tests/cli.sh $(BUILD)/pytte-ai

# Regenerate the expected results of the stream fixtures (review the diff!).
fixtures-update: $(BUILD)/test/test_fixtures
	PT_UPDATE_FIXTURES=1 ./$(BUILD)/test/test_fixtures

# The portable parts must build warning-free as strict C99 with gcc and clang,
# and must not use stdio or malloc.
portable:
	@for cc in gcc clang; do \
	  command -v $$cc >/dev/null || continue; \
	  echo "$$cc: $(LIB_SRC)"; \
	  $$cc $(CSTD) $(WARN) -Werror -fsyntax-only $(CPPFLAGS) $(LIB_SRC) || exit 1; \
	done
	@! grep -nE '#include <(stdio|stdlib|malloc|math|time)\.h>' $(LIB_SRC) include/*.h \
	  || (echo "portable code must not use these headers" && exit 1)
	@echo portable: ok

# ---- fuzzing (clang + libFuzzer) ----
FUZZERS := $(patsubst tests/fuzz/%.c,$(BUILD)/fuzz/%,$(wildcard tests/fuzz/fuzz_*.c))
$(BUILD)/fuzz/%: tests/fuzz/%.c $(LIB_SRC)
	@mkdir -p $(dir $@)
	clang -g -O1 -fsanitize=fuzzer,address,undefined $(CPPFLAGS) $< $(LIB_SRC) -o $@
fuzz: $(FUZZERS)
	@echo "run e.g.: ./$(BUILD)/fuzz/fuzz_json -max_total_time=60"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)
