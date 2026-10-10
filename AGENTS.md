# Development rules

## Code quality

- Never add backward compatibility unless the user explicitly asks for it.

- Keep portable code (`src/base`, `src/ai`, `src/core`) strict C99.
  Route dynamic memory through `pt_allocator`; do not call libc
  allocation functions directly in these layers.

- Avoid heap allocations in per-byte, per-token, and per-event loops.
  Reuse scratch storage and buffers; explain unavoidable allocation
  in frequently executed paths.

- Use `pt_arena` for objects that share a lifetime. Separate temporary
  data from persistent data, and reset temporary arenas promptly.

- Reuse `pt_buf` capacity. Reserve once when the required size is known;
  otherwise use geometric growth. Do not resize for every append.

- Prefer borrowed `pt_str` slices when the backing storage lives long
  enough. Copy only when ownership or lifetime requires it. Document
  ownership, lifetime, and pointer invalidation in APIs.

- Prefer compact, contiguous storage when it fits the access pattern.
  Avoid separately allocating every small object without a reason.

- Keep stack allocations small and bounded. Do not use input-sized
  VLAs, `alloca`, or unbounded recursion.

- Process streams incrementally. Avoid repeatedly scanning, copying,
  parsing, or serializing the entire accumulated input on each update.
  Watch for accidentally quadratic work.

- Check allocation-size arithmetic for overflow and handle allocation
  failure consistently. Preserve existing storage on resize failure.
  Never silently return success with truncated or incomplete output.

- Preserve correctness, input validation, and durability guarantees
  when optimizing. Do not rely on undefined behavior for speed.

- Support performance claims with reproducible before/after measurements
  using optimized builds and representative offline workloads. Track
  latency or throughput, allocation/reallocation counts, peak live
  bytes, and retained capacity—not just allocation count.

- Keep compiler warnings clean. Run `make portable` and `make test`
  for C changes; exercise relevant fuzz targets for parser changes.

## Performance and memory profiling

- Profile when optimizing performance or changing allocation behavior.
  Choose a workload that exercises the affected path; unrelated edits
  do not require a profiler run.

- Use counters around `pt_allocator` for focused allocation checks.
  Distinguish backing allocations from arena suballocations, and track
  retained arena and buffer capacity. Without custom allocator
  annotations, heap profilers see arena chunks rather than each object
  allocated inside them.

- Use [Valgrind DHAT](https://valgrind.org/docs/manual/dh-manual.html)
  for allocation churn and underused storage,
  [Massif](https://valgrind.org/docs/manual/ms-manual.html) for peak heap
  usage, and [Memcheck](https://valgrind.org/docs/manual/mc-manual.html)
  for suspected leaks or invalid memory access. Equivalent tools are
  acceptable when they measure the relevant behavior.

- Run Valgrind on a separate optimized build with debug symbols and
  without sanitizers. The normal `make test` binaries use ASan/UBSan;
  do not run those binaries under Valgrind. For example, to profile the
  existing offline stream fixtures:

  ```sh
  make BUILD=build/profile TEST_CFLAGS='-O2 -g' build/profile/test/test_fixtures
  valgrind --tool=dhat --dhat-out-file=/tmp/pytte-dhat.json ./build/profile/test/test_fixtures
  ```

- Measure latency and throughput separately, without Valgrind or
  sanitizer instrumentation. Compare the same inputs, compiler flags,
  and machine before and after; repeat timing runs to check noise.
  Report the commands, results, and any memory/performance tradeoff.
