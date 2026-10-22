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
