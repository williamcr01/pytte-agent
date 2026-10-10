# Performance measurements

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
directory, copy `tests/bench_perf.c` into it, and compile that harness with
`-DPT_BENCH_LEGACY`. The current Makefile's benchmark rule can be used from that
directory with:

```sh
make -f /absolute/path/to/current/Makefile BUILD=build/before \
  CPPFLAGS='-Iinclude -Isrc -DPT_BENCH_LEGACY' build/before/bench/perf
```

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
