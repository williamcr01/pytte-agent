# pytte build. Plain make, system compiler, libcurl via pkg-config.

CC       ?= cc
AR       ?= ar
CSTD     := -std=c99
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS   ?= -O2 -g
LDFLAGS  ?=
STRIP    ?= strip
ifeq ($(shell uname -s),Darwin)
GC_LDFLAGS := -Wl,-dead_strip
NOW_LDFLAGS :=
else
GC_LDFLAGS := -Wl,--gc-sections
# Bind every symbol at startup: the TUI's watchdog, a fork, then never runs
# the lazy resolver, which would fault the dynamic linker's pages into it.
NOW_LDFLAGS := -Wl,-z,now
endif
CPPFLAGS += -Iinclude -Isrc
BUILD    := build

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
# Keep libcurl's shared-library tree out of startup; direct linkage remains
# available for static/custom curl builds: make CURL_LINK=direct.
CURL_LINK ?= dynamic
ifeq ($(CURL_LINK),direct)
CPPFLAGS += -DPT_CURL_LINKED
CURL_LDLIBS := $(CURL_LIBS)
else
CURL_LDLIBS := $(if $(filter Darwin,$(shell uname -s)),,-ldl)
endif

# Portable parts: no stdio, no malloc, no floating point.
BASE_SRC := $(wildcard src/base/*.c)
AI_SRC   := $(wildcard src/ai/*.c)
CORE_SRC := $(wildcard src/core/*.c)
LIB_SRC  := $(BASE_SRC) $(AI_SRC) $(CORE_SRC)
# Host parts.
DRV_SRC  := src/drv/sys_posix.c src/drv/http_faux.c src/drv/store_posix.c src/drv/store_memory.c src/drv/worker_posix.c src/drv/proc_posix.c
RT_SRC   := src/rt/rt.c src/rt/tools.c src/rt/tools_builtin.c src/rt/tools_bash.c src/rt/tools_search.c src/rt/tools_web.c src/rt/tools_websearch.c src/rt/prompt.c src/rt/mcp.c src/rt/mcp_io.c src/rt/mcp_proto.c src/rt/mcp_oauth.c
SYS_SRC  := $(DRV_SRC) $(RT_SRC)
CURL_SRC := src/drv/http_curl.c
# The TUI's parts; app*.c (the app and its loop) only go into pytte.
TUI_SRC  := $(filter-out src/tui/app%,$(wildcard src/tui/*.c))
CLI_SRC  := src/cli/config.c src/cli/providers.c src/cli/mcp_cfg.c
APP_SRC  := $(wildcard src/tui/app*.c) src/cli/session.c src/cli/oauth.c src/cli/main.c src/cli/print.c src/cli/commands.c

AI_OBJ   := $(BASE_SRC:%.c=$(BUILD)/obj/%.o) $(AI_SRC:%.c=$(BUILD)/obj/%.o)
LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/obj/%.o)
obj       = $(patsubst %.c,$(BUILD)/obj/%.o,$(1))
PYTTE_AI_OBJ := $(call obj,src/drv/sys_posix.c src/drv/http_faux.c $(CURL_SRC) src/cli/pytte_ai.c)
PYTTE_OBJ    := $(call obj,$(SYS_SRC) $(CURL_SRC) $(TUI_SRC) $(CLI_SRC) $(APP_SRC))

.PHONY: all release test crash screen pty-asan portable fuzz clean fixtures-update models width bench bench-http test-https
all: $(BUILD)/pytte $(BUILD)/pytte-ai $(BUILD)/libpytte-ai.a $(BUILD)/libpytte.a $(BUILD)/replay

# Keep normal builds debuggable. The distributable binaries use the same
# features, with cross-file optimization and unused code/symbols removed.
release:
	$(MAKE) BUILD=$(BUILD)/release CFLAGS='-Os -flto -ffunction-sections -fdata-sections' LDFLAGS='$(LDFLAGS) $(GC_LDFLAGS)' all
	$(STRIP) $(BUILD)/release/pytte $(BUILD)/release/pytte-ai

# Offline optimized workloads; counters are in the harness, not the core.
BENCH_CFLAGS ?= -O2 -g
$(BUILD)/bench/perf: tests/bench_perf.c $(LIB_SRC) src/tui/input.c src/tui/text.c $(wildcard include/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) -O2 -g $(CPPFLAGS) $< $(filter-out src/base/arena.c,$(LIB_SRC)) src/tui/input.c src/tui/text.c -o $@

bench: $(BUILD)/bench/perf $(BUILD)/pytte
	@for w in core conversation stream history layout editing; do ./$(BUILD)/bench/perf $$w 10 || exit 1; done

$(BUILD)/bench/http: tests/bench_http.c $(LIB_SRC) $(SYS_SRC) $(CURL_SRC) src/cli/session.c src/cli/oauth.c $(CLI_SRC) $(wildcard include/*.h src/*/*.h) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(BENCH_CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) $< $(filter-out src/base/arena.c,$(LIB_SRC)) $(SYS_SRC) $(CURL_SRC) src/cli/session.c src/cli/oauth.c $(CLI_SRC) $(CURL_LDLIBS) -pthread $(LDFLAGS) -o $@

bench-http: $(BUILD)/bench/http
	python3 scripts/bench_http.py --bin current=$(BUILD)/bench/http --directory-build current --output $(BUILD)/bench/http.json

$(BUILD)/bench/http-defaults: tests/bench_http.c tests/http_curl_defaults.c $(LIB_SRC) $(SYS_SRC) $(CURL_SRC) src/cli/session.c src/cli/oauth.c $(CLI_SRC) $(wildcard include/*.h src/*/*.h) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(BENCH_CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) $< $(filter-out src/base/arena.c,$(LIB_SRC)) $(SYS_SRC) tests/http_curl_defaults.c src/cli/session.c src/cli/oauth.c $(CLI_SRC) $(CURL_LIBS) -pthread $(LDFLAGS) -o $@

$(BUILD)/bench/tui-dns.so: tests/http_tui_dns.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(BENCH_CFLAGS) -fPIC -shared $< -ldl -pthread -o $@

test-https: $(BUILD)/bench/http $(BUILD)/bench/http-defaults $(BUILD)/pytte-ai
	python3 scripts/bench_http.py --bin current=$(BUILD)/bench/http --ai-bin current=$(BUILD)/pytte-ai --defaults-bin $(BUILD)/bench/http-defaults --verify-only --output $(BUILD)/bench/https-checks.json

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) -pthread -MMD -MP -c $< -o $@

$(BUILD)/libpytte-ai.a: $(AI_OBJ)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/libpytte.a: $(LIB_OBJ)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/pytte-ai: $(PYTTE_AI_OBJ) $(BUILD)/libpytte-ai.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(CURL_LDLIBS) -pthread -o $@

$(BUILD)/pytte: $(PYTTE_OBJ) $(BUILD)/libpytte.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(NOW_LDFLAGS) $^ $(CURL_LDLIBS) -pthread -o $@

# ---- tests: built from source with ASan and UBSan ----
TEST_CC     ?= $(CC)
TEST_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TESTS       := $(patsubst tests/%.c,$(BUILD)/test/%,$(wildcard tests/test_*.c))

$(BUILD)/test/test_http: tests/test_http.c tests/t.h $(BASE_SRC) src/drv/sys_posix.c $(CURL_SRC) $(wildcard include/*.h src/drv/*.h)
	@mkdir -p $(dir $@)
	$(TEST_CC) $(CSTD) $(WARN) $(TEST_CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) $< $(BASE_SRC) src/drv/sys_posix.c $(CURL_SRC) $(CURL_LDLIBS) -pthread -o $@

$(BUILD)/test/test_http_trust: tests/test_http_trust.c tests/t.h $(BASE_SRC) src/drv/sys_posix.c $(CURL_SRC) $(wildcard include/*.h src/drv/*.h)
	@mkdir -p $(dir $@)
	$(TEST_CC) $(CSTD) $(WARN) $(TEST_CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) $< $(BASE_SRC) src/drv/sys_posix.c $(CURL_LDLIBS) -pthread -o $@

$(BUILD)/test/%: tests/%.c tests/t.h tests/vt.c $(LIB_SRC) $(SYS_SRC) $(TUI_SRC) $(CLI_SRC) $(wildcard include/*.h src/*/*.h src/*/*.inc)
	@mkdir -p $(dir $@)
	$(TEST_CC) $(CSTD) $(WARN) $(TEST_CFLAGS) $(CPPFLAGS) $< $(LIB_SRC) $(SYS_SRC) $(TUI_SRC) $(CLI_SRC) -pthread -o $@

test: $(TESTS) $(BUILD)/pytte-ai $(BUILD)/pytte
	@for t in $(TESTS); do ./$$t || exit 1; done
	@echo "cli:" && sh tests/cli.sh $(BUILD)/pytte-ai $(BUILD)/pytte
	@echo "tools:" && sh tests/tools.sh $(BUILD)/pytte
	@bash tests/crash.sh $(BUILD)/pytte 3

# Replay a PYTTE_TUI_TRACE file into the test emulator: build/replay TRACE
$(BUILD)/replay: tests/replay.c tests/t.h tests/vt.c $(BASE_SRC) src/drv/sys_posix.c src/tui/text.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(LDFLAGS) $(CPPFLAGS) $< $(BASE_SRC) src/drv/sys_posix.c src/tui/text.c -o $@

# The screen property test with many more random sessions (section 22.5).
screen: $(BUILD)/test/test_screen
	BAND_SEEDS=400 ./$(BUILD)/test/test_screen

# The pty test against pytte built with ASan and UBSan: app*.c and term.c
# only run inside the real binary. A report fails the test (exit code).
pty-asan: $(BUILD)/test/test_pty
	$(MAKE) BUILD=$(BUILD)/asan CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' $(BUILD)/asan/pytte
	PYTTE_BIN=$(BUILD)/asan/pytte ./$(BUILD)/test/test_pty

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
FUZZ_TUI := src/tui/text.c src/tui/keys.c src/tui/input.c
$(BUILD)/fuzz/%: tests/fuzz/%.c $(LIB_SRC) $(FUZZ_TUI)
	@mkdir -p $(dir $@)
	clang -g -O1 -fsanitize=fuzzer,address,undefined $(CPPFLAGS) $< $(LIB_SRC) $(FUZZ_TUI) -o $@
fuzz: $(FUZZERS)
	@echo "run e.g.: ./$(BUILD)/fuzz/fuzz_json -max_total_time=60"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)
