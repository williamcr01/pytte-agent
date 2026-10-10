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
CORE_SRC := $(wildcard src/core/*.c)
LIB_SRC  := $(BASE_SRC) $(AI_SRC) $(CORE_SRC)
# Host parts.
DRV_SRC  := src/drv/sys_posix.c src/drv/http_faux.c src/drv/store_posix.c src/drv/store_memory.c
RT_SRC   := src/rt/rt.c
SYS_SRC  := $(DRV_SRC) $(RT_SRC)
CURL_SRC := src/drv/http_curl.c
TUI_SRC  := $(wildcard src/tui/*.c)

AI_OBJ   := $(BASE_SRC:%.c=$(BUILD)/obj/%.o) $(AI_SRC:%.c=$(BUILD)/obj/%.o)
LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/obj/%.o)
obj       = $(patsubst %.c,$(BUILD)/obj/%.o,$(1))
PYTTE_AI_OBJ := $(call obj,src/drv/sys_posix.c src/drv/http_faux.c $(CURL_SRC) src/cli/pytte_ai.c)
PYTTE_OBJ    := $(call obj,$(SYS_SRC) $(CURL_SRC) $(TUI_SRC) src/cli/main.c)

.PHONY: all test crash screen portable fuzz clean fixtures-update models width
all: $(BUILD)/pytte $(BUILD)/pytte-ai $(BUILD)/libpytte-ai.a $(BUILD)/libpytte.a

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libpytte-ai.a: $(AI_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/libpytte.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/pytte-ai: $(PYTTE_AI_OBJ) $(BUILD)/libpytte-ai.a
	$(CC) $(CFLAGS) $^ $(CURL_LIBS) -o $@

$(BUILD)/pytte: $(PYTTE_OBJ) $(BUILD)/libpytte.a
	$(CC) $(CFLAGS) $^ $(CURL_LIBS) -o $@

# ---- tests: built from source with ASan and UBSan ----
TEST_CC     ?= $(CC)
TEST_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TESTS       := $(patsubst tests/%.c,$(BUILD)/test/%,$(wildcard tests/test_*.c))

$(BUILD)/test/%: tests/%.c tests/t.h tests/vt.c $(LIB_SRC) $(SYS_SRC) $(TUI_SRC) $(wildcard include/*.h src/*/*.h src/*/*.inc)
	@mkdir -p $(dir $@)
	$(TEST_CC) $(CSTD) $(WARN) $(TEST_CFLAGS) $(CPPFLAGS) $< $(LIB_SRC) $(SYS_SRC) $(TUI_SRC) -o $@

test: $(TESTS) $(BUILD)/pytte-ai $(BUILD)/pytte
	@for t in $(TESTS); do ./$$t || exit 1; done
	@echo "cli:" && sh tests/cli.sh $(BUILD)/pytte-ai $(BUILD)/pytte
	@bash tests/crash.sh $(BUILD)/pytte 3

# The screen property test with many more random sessions (section 22.5).
screen: $(BUILD)/test/test_screen
	BAND_SEEDS=400 ./$(BUILD)/test/test_screen

# kill -9 pytte at random moments, many times (section 22.3).
crash: $(BUILD)/pytte
	bash tests/crash.sh $(BUILD)/pytte 100

# Regenerate the expected results of the stream fixtures (review the diff!).
fixtures-update: $(BUILD)/test/test_fixtures
	PT_UPDATE_FIXTURES=1 ./$(BUILD)/test/test_fixtures

# Refresh the model catalog from models.dev and OpenRouter (network).
models:
	python3 scripts/gen_models.py

# Regenerate the TUI's display-width table from the Unicode data (network).
width:
	python3 scripts/gen_width.py

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
FUZZ_TUI := src/tui/text.c src/tui/keys.c
$(BUILD)/fuzz/%: tests/fuzz/%.c $(LIB_SRC) $(FUZZ_TUI)
	@mkdir -p $(dir $@)
	clang -g -O1 -fsanitize=fuzzer,address,undefined $(CPPFLAGS) $< $(LIB_SRC) $(FUZZ_TUI) -o $@
fuzz: $(FUZZERS)
	@echo "run e.g.: ./$(BUILD)/fuzz/fuzz_json -max_total_time=60"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)
