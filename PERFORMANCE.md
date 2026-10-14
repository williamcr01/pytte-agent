# Performance measurements

<!-- TODO: Investigate a smaller HTTP client to reduce runtime memory use. -->

### Implementation

OpenSSL loads every certificate in a CA bundle into a store. Its hashed
[CA directory](https://docs.openssl.org/3.6/man3/SSL_CTX_load_verify_locations/)
loads certificates when needed. The system bundle and `/etc/ssl/certs` on
this machine contain the same 121 certificate fingerprints. Pytte now
prefers curl's configured directory, or the native `/etc/ssl/certs` directory
for the standard Unix bundle locations documented in README.md. This is a
change in the selected system trust source; distributions with different
store contents should use `SSL_CERT_FILE` to select their required bundle.

Curl 8.22 restores its compiled-in CA file when CAINFO is NULL. Set CAINFO
to one readable hashed certificate from the selected directory and CAPATH
to the directory. All directory roots remain available. If verification or
CA-file loading fails, restore the original bundle and original CAPATH once.
Only retry when no request headers or body bytes were sent. Subsequent turns
use the bundle to retain connection reuse. Proxy configurations and explicit
CA files use the normal bundle path; Apple and other TLS backends retain their
native behavior. Verification and crypto defaults, including HTTP/2 and
post-quantum key exchange, are unchanged.

Trust paths occupy one owned `pt_buf`, reserved before pointers are taken.
The directory is scanned once per driver. A probe easy handle supplies
CAINFO, CAPATH and TLS-backend information, then becomes the first transfer's
handle. This avoids an extra easy-handle allocation and avoids
`curl_version_info`, which calls optional dependency version functions and
added roughly 400 KiB of resident pages in the initial implementation.
The module remains loaded for resolver-thread safety; the multi handle
continues to reuse connections. No heap trimming or page-discard tricks are
used. Experiments with lazy binding, smaller upload buffers and heap trimming
gave little useful improvement on this system.

### Measurements

The standard build remains 64-bit. The stripped release is **375,848 bytes**
for `pytte` and 178,688 bytes for `pytte-ai`; neither contains libcurl or TLS.
Measured with installed libcurl 8.22.0, OpenSSL 3.6.4 and glibc 2.44 on an
Intel i5-1035G7 running Linux x86-64. GCC 16.2.1 built both comparison versions
with `-O2 -g`; the release uses `-Os -flto` and section garbage collection.

| Actual TUI, median RSS | Before, same compiler flags | After, same compiler flags | Final 64-bit release |
|---|---:|---:|---:|
| After first 512-byte HTTPS reply | 13.21 MiB | 12.46 MiB | 12.45 MiB |
| After 100 short HTTPS turns | 13.73 MiB | 13.07 MiB | 12.99 MiB |
| After first HTTP/2 reply | 13.26 MiB | 12.58 MiB | 12.57 MiB |

RSS includes resident shared-library pages. Release PSS was 7.46 MiB after
the first reply and 8.25 MiB after 100 turns in this run; sharing with other
processes affects those numbers. The terminal guardian's memory is recorded
separately. The release still exceeds **4–8 MiB RSS**. This experiment saves
roughly 0.7 MiB with system curl; it does not establish a universal minimum.

DHAT measures the whole agent harness, including system curl/TLS allocations:

| Optimized workload | Allocated bytes, before → after | Backing allocation blocks, before → after | Peak live bytes, before → after |
|---|---:|---:|---:|
| Three short HTTPS turns | 4,550,470 → 1,980,945 | 46,538 → 15,262 | 1,569,001 → 1,000,348 |
| 100 short turns | 34,660,490 → 32,095,913 | 56,382 → 25,203 | 2,178,313 → 1,609,610 |
| Three 64-KiB replies | 7,478,067 → 4,793,857 | 47,704 → 16,425 | 2,948,866 → 2,265,528 |

The short workload has 67.2% fewer backing allocations and a 36.2% lower
requested-byte peak. Massif independently measured 1,568,360 → 999,709 peak
requested heap bytes, plus 269,544 → 127,227 allocator-overhead bytes at those
snapshots. Both builds retain the same 39,660 bytes in 96 loader/library blocks
at process exit. Core allocator live bytes reach zero after teardown.

Portable-core counters are unchanged by the driver optimization. At the first
short reply: 38 allocations, 18 reallocations, 87,952 peak bytes, 68,384 live
bytes, 47,296 arena-capacity bytes (19,760 used), and 4,608 buffer-capacity
bytes. At turn 100: 1,924 allocations, 1,538 reallocations, 786,024 peak bytes,
471,072 live bytes, 355,072 arena-capacity bytes and 8,448 buffer-capacity
bytes. With three large replies: 86 allocations, 67 reallocations, 987,048
peak bytes, 783,632 live bytes, 634,640 arena-capacity bytes and 132,096
buffer-capacity bytes. These are backing allocations, not arena object counts.

| Transport only, median latency | Before | After |
|---|---:|---:|
| First short HTTPS reply, after curl initialization | 20.584 ms | 8.547 ms |
| Third short reply, reused connection | 0.774 ms | 0.965 ms |
| Third short reply, reconnect | 4.850 ms | 4.787 ms |
| Third 64-KiB reply | 16.618 ms | 16.704 ms |

The large fixture carries 185,541 wire bytes: about 10.6 MiB/s in both builds.
The optimization reduces first-handshake work; reused transfers do not show a
throughput improvement. Full TUI durable completion for the first reply was
33.215 → 22.161 ms with the same flags. Rendered first text stayed around
33–34 ms because of the frame schedule. Filesystem commits and scheduling
contribute timing noise; these local results do not predict provider latency.
Plain HTTP RSS was essentially unchanged (9,760 → 9,784 KiB in the harness).

The 32-bit experiment reached 10.42 MiB RSS after one reply and 10.94 MiB
after 100 turns, but its PSS was higher than the 64-bit build on this desktop,
where other processes share the 64-bit libraries. It is an experiment only;
64-bit remains the standard.

Validation: strict C99 with GCC and Clang; the complete ASan/UBSan test suite;
release CLI and PTY checks; and 38 local TLS/trust checks covering TLS 1.2/1.3,
HTTP/2, certificate/hostname rejection, explicit overrides, fallback, proxies
and preventing request replay. The trust checks also passed with an
ASan/UBSan app and harness and in the separate 32-bit build. System libraries
themselves are not sanitizer-instrumented. No Memcheck result is claimed.

### Reproduction

All native measurements use optimized builds without sanitizers, nine
samples in three alternating batches, and an excluded warm-up per build and
batch. Each fixture includes the complete system root set plus a temporary
local root, as a bundle for the baseline and an equivalent hashed directory
for the optimized builds. This substitutes the local fixture for the normal
system store without changing the anchor set between builds. SHA-256 checks
validate every response and all counted core bytes must be freed at exit.
No real provider or credential is used.

The TUI measurement runs the actual executable in an 80x24 PTY. A test-only
DNS shim maps only `api.anthropic.com` to the local TLS server and rejects
all other named destinations. TLS still verifies that hostname. The shim's
ELF class must match the app, and it is excluded from distributed binaries.
The existing terminal-restoration guardian is measured separately; it never
loads curl. TUI completion latency means the durable run-completion record;
first-text latency measures rendered output and includes the existing frame
schedule. Timing runs do not overlap builds, regression tests or profilers.

The baseline HTTP driver is included verbatim in the checked-in benchmark
record. It matches `ee4225b` plus CA environment overrides solely to trust the
local fixture. Extract it to reproduce the baseline:

```sh
python3 - <<'PYBASE'
import json
from pathlib import Path
record = json.loads(Path('docs/benchmarks/system-curl.json').read_text())
Path('build/system-curl').mkdir(parents=True, exist_ok=True)
Path('build/system-curl/baseline_http_curl.c').write_text(record['baseline_http_source'])
PYBASE
make -j4 BUILD=build/system-curl/before CURL_SRC=build/system-curl/baseline_http_curl.c all build/system-curl/before/bench/http
make -j4 BUILD=build/system-curl/after all build/system-curl/after/bench/http build/system-curl/after/bench/http-defaults build/system-curl/after/bench/tui-dns.so
make release
python3 scripts/bench_http.py --bin before=build/system-curl/before/bench/http after=build/system-curl/after/bench/http --directory-build after --runs 9 --output build/system-curl/http-final.json
python3 scripts/bench_http.py --bin before=build/system-curl/before/bench/http after=build/system-curl/after/bench/http --directory-build after --transport-only --runs 9 --cases https reconnect large --output build/system-curl/transport-final.json
python3 scripts/bench_tui_http.py --bin before=build/system-curl/before/pytte after=build/system-curl/after/pytte --directory-build after --shim build/system-curl/after/bench/tui-dns.so --runs 9 --output build/system-curl/tui-final.json
python3 scripts/bench_http.py --bin current=build/system-curl/after/bench/http --ai-bin current=build/system-curl/after/pytte-ai --defaults-bin build/system-curl/after/bench/http-defaults --verify-only --output build/system-curl/trust-checks.json
python3 scripts/bench_http.py --bin before=build/system-curl/before/bench/http after=build/system-curl/after/bench/http --directory-build after --profile dhat --runs 1 --cases https conversation large --output build/system-curl/dhat.json
python3 scripts/bench_http.py --bin before=build/system-curl/before/bench/http after=build/system-curl/after/bench/http --directory-build after --profile massif --runs 1 --cases https --output build/system-curl/massif.json
make portable
make test
```

A separate 32-bit experiment uses existing multilib system packages. The standard
build remains native 64-bit; there is no 32-bit release preset. Reproduce the
experiment and its matching test-only DNS shim:

```sh
make -j4 BUILD=build/system-curl/i686 CFLAGS='-m32 -O2 -g -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64' BENCH_CFLAGS='-m32 -O2 -g -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64' LDFLAGS='-m32' all build/system-curl/i686/bench/http build/system-curl/i686/bench/http-defaults build/system-curl/i686/bench/tui-dns.so
make -j4 BUILD=build/release32 CFLAGS='-m32 -Os -flto -ffunction-sections -fdata-sections -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64' LDFLAGS='-m32 -Wl,--gc-sections' all
strip build/release32/pytte build/release32/pytte-ai
python3 scripts/bench_tui_http.py --bin before=build/system-curl/before/pytte native=build/release/pytte i686=build/release32/pytte --directory-build native i686 --shim build/system-curl/after/bench/tui-dns.so --shim32 build/system-curl/i686/bench/tui-dns.so --runs 9 --output build/system-curl/tui-release.json
PYTTE_BIN="$PWD/build/system-curl/i686/pytte" make -j4 BUILD=build/system-curl/i686 CFLAGS='-m32 -O2 -g -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64' LDFLAGS='-m32' TEST_CFLAGS='-m32 -O1 -g -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' test
```

Full raw outputs and profiles live under ignored `build/system-curl/`.
Selected samples, source/binary hashes, build commands, trust inventories and
profiler totals are in `docs/benchmarks/system-curl.json`. The final warning
cleanup names the checked `curl_off_t` conversion; the native executable and
harness `.text` sections are byte-identical before/after that refactor, so the
recorded native timing/memory runs remain applicable. No production stream or
JSON parser sources changed.

## Earlier core and TUI measurements (2026-10-10)

Measured on 2026-10-10 against commit `0ae035f90c2f6fb83acaa108657d1734bff6adb8`,
using the same offline inputs and GCC 16.2.1 `-O2 -g` builds without sanitizers.
Machine: Intel i5-1035G7, 4 cores / 8 threads, Linux x86-64, powersave governor,
repository and temporary state on btrfs. Timing runs ran without a concurrent
build, profiler or fuzzer. These measurements cover warm cache on this machine.

The first interactive frame now appears in about 1.85 ms, with input echoed in
about 0.10 ms. The 250 ms terminal-query gate and 60 ms resize debounce are gone.
Session opening/recovery, durable commits, session switches, session listing and
settings persistence run on bounded workers. Agent callbacks and UI mutation
remain on the event-loop thread. Existing terminal protocols, Unicode editing,
history, drafts, settings, recovery, locking and write-ahead durability remain.

## Launch and input

The PTY harness uses an isolated configuration/state directory, an 80x24 terminal
and the faux provider. Thirty samples per case, in three batches with an excluded
warm-up per case/batch. Launch is measured from native process spawn to a visible
prompt/model; continuing a session additionally waits for its transcript tail.
Input is measured from writing a unique marker to seeing it in terminal output.
Commit cases wait for initial durable claims before submitting work.

| Case | Launch before, median ms | Launch after, median / p95 ms | Input before, median ms | Input after, median / p95 ms |
|---|---:|---:|---:|---:|
| Terminal answers queries | 18.441 | 1.851 / 2.312 | 9.584 | 0.101 / 0.244 |
| Terminal never answers | 255.064 | 1.871 / 2.087 | 10.136 | 0.088 / 0.171 |
| Input during commits | 19.286 | 1.837 / 2.101 | 2.648 | 0.135 / 1.223 |
| Input during streaming | 18.083 | 1.611 / 2.345 | 0.186 | 0.183 / 0.422 |
| Input during `/new` | 19.587 | 1.724 / 2.359 | 19.649 | 0.117 / 0.222 |
| Input during a settings save | 18.925 | 1.875 / 2.243 | 8.078 | 0.143 / 0.300 |
| `-c`, with restored transcript | 9.782 | 2.943 / 3.305 | 5.353 | 0.222 / 0.309 |
| 1,000 saved history entries | 23.865 | 1.809 / 2.109 | 9.913 | 0.076 / 0.137 |

The release build was also measured separately: nine samples per case, three
batches. Normal launch was 1.924 ms median, 2.383 ms p95; input was 0.096 ms
median, 0.140 ms p95. All eight release cases stayed below 10 ms for launch and
input in this sample. Raw samples include maximums and memory snapshots.

Ordinary input bypasses the streaming paint rate limit. Legacy lone-Esc
disambiguation remains 30 ms and does not pause other input. Provider retry
timers and durable-write barriers remain; the event loop can accept input while
they are pending. Exit waits for accepted inputs and settings to be persisted.

## Size and resident memory

| Artifact | Before, bytes | After, bytes |
|---|---:|---:|
| Normal optimized build with debug information | 1,045,160 | 1,100,056 |
| That build, stripped | 338,912 | 355,304 |
| `-Os -flto`, section collection, stripped | 277,456 | 289,752 |

`make release` produces the last variant in `build/release/`. Its 289,752 bytes
(283.0 KiB) are 14.5% smaller than the original stripped default build. The
workers and supporting behavior add code: the size reduction in the release
artifact comes from its compiler/linker settings. The normal build stays
debuggable, and the table includes comparisons with matching flags.

| Settled, idle session | Before, KiB | After, KiB |
|---|---:|---:|
| Main process RSS | 6,290 | 2,480 |
| Main process PSS | 2,790 | 451 |
| Main process private memory (USS) | 2,040 | 260 |
| Main process + watchdog PSS | 3,497 | 678 |

Snapshots come from `/proc/PID/smaps_rollup`, after setup settles for 100 ms.
PSS apportions shared pages, so adding parent/watchdog PSS avoids double-counting
shared pages. These are settled measurements, rather than peak RSS. PSS also
depends on sharing with other processes on the machine.

The default binary eagerly needs only libc on this Linux system; libcurl and its
TLS dependency tree load on the first real HTTP request. Real HTTP therefore has
additional initialization and resident-memory costs. A successfully loaded curl
module stays loaded until process exit: global cleanup does not wait for resolver
threads, so unloading the module after cancellation could invalidate their code.
See [libcurl's cleanup documentation](https://curl.se/libcurl/c/curl_global_cleanup.html).
`CURL_LINK=direct` remains available for custom/static curl builds.

## Offline work and allocations

The harness counts backing allocations, reallocations, requested bytes, peak
live bytes and retained capacity through `pt_allocator`. Arena suballocations
are distinct from backing allocations. Each workload frees all counted storage
and asserts zero counted bytes remain live. Latencies below are the median of
three interleaved batches, ten iterations per batch; one iteration is the whole
workload described in the first column.

| Workload | Time before / after, ms | Allocations before / after | Reallocations before / after | Peak live bytes before / after |
|---|---:|---:|---:|---:|
| One offline turn, 512-byte reply | 0.210 / 0.199 | 41 / 37 | 24 / 24 | 249,216 / 69,208 |
| 100 turns, 512-byte replies | 35.044 / 34.400 | 2,439 / 2,131 | 2,056 / 2,056 | 1,080,488 / 977,552 |
| 64 KiB reply, 16-byte stream chunks | 19.059 / 18.828 | 45 / 41 | 58 / 58 | 1,315,296 / 1,172,264 |
| Load 1,000 history entries | 0.823 / 0.763 | 2,003 / 5 | 14 / 10 | 182,880 / 151,648 |
| 52 KiB multiline draft, 500 inserts and two layouts/edit | 1,655.882 / 3.181 | 2 / 2 | 17 / 19 | 114,688 / 114,688 |
| 60 KiB logical line, 500 backspaces and layouts | 1,511.474 / 3.143 | 2 / 2 | 16 / 18 | 90,112 / 90,112 |

The large draft workloads are about 500 times faster. Input wrapping reuses
unchanged rows and rebuilds only the affected suffix; width/ambiguous-width
changes still rebuild the full layout. ASCII backward movement has a constant
time shortcut, with the existing Unicode cluster walk retained for mixed text.
Smaller initial row capacity adds two growth operations to the large workloads.

| Retained storage | Before, bytes | After, bytes |
|---|---:|---:|
| One turn: arena capacity / used | 139,008 / 2,208 | 20,672 / 2,496 |
| 100 turns: arena capacity / used | 346,112 / 182,784 | 287,488 / 211,584 |
| 64 KiB stream: arena capacity / used | 270,416 / 132,256 | 152,080 / 132,544 |
| History: text + entry-array capacity | 113,152 | 81,920 |

Task/input structs now live in a shared arena, so the new arena-used figure
includes structs that were formerly separate allocations. Small arenas start
smaller and grow chunks geometrically, with reset retaining the initial chunk.
Input text is retained across status changes, task string storage is reused,
and internal request building borrows unchanged contexts. The public context
view still owns its message/block arrays in the supplied arena. Curl borrows
request-body bytes through completion/cancellation, and reuses header scratch.

DHAT confirms the 100-turn harness peak: 1,080,488 to 977,552 bytes; cumulative
backing allocation traffic fell from 29,494,368 to 27,315,672 bytes. DHAT counts
reallocations as backing blocks: 4,496 to 4,188. Both profiles end with zero live
blocks. The separate 60-fixture profile peaks at 132,083 bytes and also ends with
zero live blocks. Profiling used optimized debug builds without sanitizers;
none of its instrumented timings are used above.

## Reading long logs

Measured on 2026-10-11 (commit `ece1569` against `fbe56f4`), same machine and
`-O2 -g` builds. Recovery (`pytte -p -c`) and `pytte log` used to read the
whole log into one buffer before parsing; they now read 64 KB chunks and
carry only a line that spans two chunks. Workload: one offline session of
300 `bash` calls that each print 48,894 bytes (`seq 1 10000`), so each
result keeps 16 KB in the log and the rest in a blob. The log is 8,411,728
bytes in a single epoch. The table shows medians of 7 runs per command,
alternating builds, and two rounds:

| Command | Peak RSS before | Peak RSS after | Wall time before | Wall time after |
| --- | ---: | ---: | ---: | ---: |
| `pytte log` | 25.9 MB | 17.7 MB | 150–155 ms | 140–144 ms |
| `pytte log --json` | 25.9 MB | 17.7 MB | 145–148 ms | 136–139 ms |
| `pytte -p -c` (recover, then close) | 26.0 MB | 18.0 MB | 162 ms | 157–171 ms |

The saving is the size of the log. The rest is the current epoch's entries,
which the agent keeps in memory. Wall time stays within run-to-run noise.
`pytte ls` reads at most 64 KB from the head and 64 KB from the tail of each
log, whatever its size. The workload script (300 turns of
`{"type":"tool_call","name":"bash","args":{"command":"seq 1 10000"}}` and a
final text turn) runs with `PYTTE_PERMISSION_MODE=auto pytte -p -m faux/faux
--context-window 100000000 --script many.json go`. Peak RSS is `ru_maxrss`
from `wait4`, measured with stdin, stdout and stderr on `/dev/null`.

## Reproduction and verification

```sh
make
make release
make portable
make test
make pty-asan
PYTTE_BIN=build/release/pytte ./build/test/test_pty
make bench
python3 scripts/bench_startup.py --bin build/pytte --runs 30 --output build/opt/startup.json
make BUILD=build/profile TEST_CFLAGS='-O2 -g' build/profile/test/test_fixtures
valgrind --vgdb=no --tool=dhat --dhat-out-file=build/opt/dhat-fixtures.json ./build/profile/test/test_fixtures
valgrind --vgdb=no --tool=dhat --dhat-out-file=build/opt/dhat-conversation.json ./build/bench/perf conversation 1
make BUILD=build/direct CURL_LINK=direct build/direct/test/test_http
./build/direct/test/test_http
```

For matching before measurements, archive the baseline commit into a separate
directory, copy `tests/bench_perf.c` into it, and compile that harness with the
switch for the baseline's `pt_agent` layout: `-DPT_BENCH_FLAT_AGENT` for a
baseline from before its state was grouped into sub-structs (`main@f1d8c5c` and
older), or `-DPT_BENCH_LEGACY` for one that also predates the `objects` arena
(it implies the first). The current Makefile's benchmark rule can be used from
that directory with:

```sh
make -f /absolute/path/to/current/Makefile BUILD=build/before \
  CPPFLAGS='-Iinclude -Isrc -DPT_BENCH_FLAT_AGENT' build/before/bench/perf
```

`tests/bench_http.c` takes the same switch; build it with the baseline's own
Makefile, whose source lists match its tree.

Alternate before/after workloads on the same machine and filesystem. The
benchmark uses synthetic data and no credentials; its temporary state lives
under the chosen `--work` directory.

All required portable, unit, fixture, CLI and crash checks passed, with no golden
fixture changes. The PTY suite passed 170 checks against normal, release and
ASan/UBSan binaries, including blocked setup/settings writes, late/missing query
replies, resize, interrupt/queue, suspend, history, pager and draft recovery.
Worker tests passed 1,968 checks, including a held write, host-thread callbacks,
write failure and saving more than 1,000 queued inputs on immediate close.
Dynamic and direct curl transports passed the loopback HTTP test with leak
checks. Input, log and stream fuzz runs completed without failures (14,196,
275,375 and 301,107 cases in their final recorded runs).

Raw results are in ignored `build/opt/`: `startup-before-final.json`,
`startup-after-final.json`, `startup-release-final.json`,
`core-before-final.jsonl`, `core-after-final.jsonl`, `dhat-*-before.json`,
`dhat-*-after.json` and the build/test logs. Immutable binary snapshots accompany
them. Before SHA-256: `002039e4268f77c2ba7b61ba5d9f56161e4f8d51b4de2909dbdf35a5b824e2bc`;
after: `82abf5accf6d110e11c6528a90cd5744bb8c966ea454d0c6dfdc93bdd38d872d`;
release: `b655a98dc0e3c715580f0af98b9c5742b1ccc9caa0bf53ca787b791483e9847a`.

Cold storage, real network/TLS setup, unusually large logs or pastes, terminal
write backpressure and machine scheduling have separate costs. These results
establish the measured offline workloads; arbitrary workloads have no fixed
10 ms worst-case guarantee.
