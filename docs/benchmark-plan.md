# Native database benchmarks

Benchmark the current append-only pack engine, not alternate physical
backends. Always use a fresh randomly named parent directory; tests must
never reuse an existing repository path.

## Canonical hydraulic models

Use the Release build of the real AOWIS hydraulic persistence benchmark:

```bash
run_root="$(mktemp -d /tmp/aowis-hydraulic-bench.XXXXXXXX)"
./build-linux-tests-release/aowis-native-hydraulic-edit-benchmark \
  "$run_root/network" 100000 20 3
```

Record initial import time, indexed entity edits, topology changes,
whole-network operations, peak RSS, branch/merge behavior, and disk usage.
Use representative networks and several repetitions. Compiler build mode,
cache state and hardware must be included with results.

## Replication

`tools/benchmark_replication_scaling.py` produces deterministic raw and model
histories for the pack format. Split source preparation from import runs to
avoid mixing fixture construction with importer latency:

```bash
fixture_root="$(mktemp -d /tmp/aowis-replication-fixture.XXXXXXXX)"
rmdir "$fixture_root"
python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux-tests-release/aowis-native-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --mode prepare \
  --fixture-dir "$fixture_root" --progress

python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux-tests-release/aowis-native-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --repeats 3 --mode imports \
  --fixture-dir "$fixture_root" --output "$fixture_root/results.csv"
```

The CSV includes full and incremental import timings, native verification
phases, independent full-integrity audit time, import peak RSS, transferred
object counts, pack size, and sampled temporary-file allocations. Results
are checked against the expected immutable revision heads. Each import
uses a new disposable repository; the fixture can be retained for reruns.

## Recovery, concurrency and maintenance

`aowis-native-pack-operational-stress-tests` covers multiple simultaneous
repository handles, readers/writers, stale edits, model merges, incremental
sync, compaction exclusivity and reopening. `aowis-native-recovery-tests`
adds deterministic crash and ENOSPC injection. Before public release also
run real power-failure and filesystem-specific tests where practical.

Report full repository verification and offline compaction separately.
Compaction is not supported while other handles are open. Measurements from
individual storage benchmarks are workload-specific and should not be used as
production guarantees.

## Physical object index

`aowis-native-index-scaling-benchmark` and `tools/benchmark_native_index.py`
measure the **existing** SHA-256-to-pack-location index. They do not
silently swap in a second storage engine. Use the optimized Release binary.
The driver **automatically rebuilds the exact Release target supplied in
`--binary`** before generating fixtures; it rejects Debug or foreign CMake
build trees. `./compile_linux.sh` builds `build-linux`, **not**
`build-linux-tests-release`. To do an explicit clean Release test build first,
run `./compile_linux_tests.sh --release`. The example below uses `build-linux`
so it also works after `./compile_linux.sh`:

```bash
run_root="$(mktemp -d /tmp/aowis-index-capacity.XXXXXXXX)"
python3 tools/benchmark_native_index.py \
  --binary ./build-linux/aowis-native-index-scaling-benchmark \
  --sizes 100000,1000000,10000000 \
  --keys 4096 --batch 1024 --reads 1000 --write-rounds 5 \
  --multi-projects 2 --work-root "$run_root" \
  --output "$run_root/index-capacity.csv"
```

For deliberately comparing already-built historical binaries, `--no-build`
skips the automatic rebuild; use it only when that is intentional. Smoke tests
run as part of CTest and do not invoke a nested CMake build.

For a quick validation of the benchmark itself, run
`ctest --test-dir build-linux-tests-release -R aowis-native-index-scaling-smoke
--output-on-failure`. The benchmark driver creates randomized disposable
subdirectories, refuses existing fixture paths, and removes fixtures after
sampling. **It never modifies an existing user repository.** Rebuild,
compaction and commit phases run on independent *copies* of the fixture.
Large runs require free space for these copies and temporary compacted packs.
Do not run benchmarks on a volume with production repositories or tight disk
budgets. Use `--skip-rebuild` and `--skip-compact` to measure indexing at 10M
objects first without paying for full-pack hashing or maintenance. Use
`--repeats 3` for independent repetitions. The CSV is atomically updated after each completed target/repetition;
partial results from completed targets survive later failures. An interrupted
target does not produce a misleading partial phase row. To assess sparsity, repeat with different `--keys` values,
for example 256 and 65536 at the same physical object targets.

Each row reports a separate process and phase:

- `prepare`: deterministic fixture creation until at least the requested
  **physical** object count. `physical_objects` includes old revision/tree
  objects, not just live model records. It reports durable pack and sidecar
  sizes, creation time, and current/peak RSS.
- `reopen`: fresh-process startup from the intact sidecar, subsequent checksummed
  reads of present UUIDs and misses, current memory before/after construction,
  and process peak RSS. Average read latency is
  `lookup_hit_ms / samples` or `lookup_miss_ms / samples`.
- `rebuild`: identical copy with **only the derived index sidecar removed**;
  measures full verified pack scanning and sidecar recreation. Asserts the
  object count and branch head are unchanged.
- `compact`, `post_compaction_reopen`, `writes`: separately copied fixtures;
  compaction preserves the complete published revision history. Writes report
  mean and p95 latency per fixed-size atomic commit. Phase peaks are not
  polluted by earlier phases.
- `multi_project_open`: multiple physically independent copies held open
  simultaneously in one process. The dynamic `rss_project_N_bytes` columns
  show resident growth as each additional repository is opened.

These measurements are **warm or uncontrolled OS-page-cache** timings,
not guaranteed cold-cache disk latency. For a meaningful 100k/1m/10m
comparison, record CPU, memory capacity, filesystem, storage media,
Linux kernel, build flags, available disk space, run repetitions and whether
cache was cold. `VmRSS` and `VmHWM` come from `/proc/self/status` and are
subject to page granularity, allocator retention and process overhead;
`rss_after_open_bytes - rss_before_bytes` estimates map-related resident
memory but is not an isolated allocator measurement. Record the actual
`physical_objects` value because batches can overshoot the requested target.

**Retain the compact sidecar-backed index** unless reproducible benchmarks establish a regression or stronger alternative. The before/after evidence and assumptions are recorded in [engineering findings](engineering-findings.md). Any future changes must preserve pack integrity and publication guarantees. This benchmark does not yet qualify cold-cache reads, sustained
production mixed workloads, or network-service contention.

## Sustained HTTP resource envelope

For repeatable Linux process measurements, run the existing real HTTP bundle
soak through `tools/qualify_http_resources.py` rather than scraping PID values
from a separately launched server. It measures the same six-client/three-project
workload, saves a raw observation CSV, an immutable-per-run JSON summary and
the original soak log under a fresh `build-qualification/http-resources-*`
directory. Run several durations, e.g. 300, 3600 and 21600 seconds, with
comparable machine, kernel and fixture conditions. See
[durability qualification](durability-qualification.md) for the command line
and interpretation limits.

The existing fixture exercises multi-MiB bundle transfers and increasing
branch counts. It is **not** a replacement for representative hydraulic
model reads/writes, large object deltas, high-divergence merges, disk I/O
profiling or real-world client latency. Record those separately before
publishing a deployment resource envelope.
