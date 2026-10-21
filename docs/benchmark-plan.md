# Native database benchmarking

Benchmarks measure the **AOWIS native storage engine** and its model workflows, including persistence, editing, merging, and replication.

## Workloads

- Synthetic object-store microbenchmarks (isolate packing, Merkle updates, indexes, reopening and verification).
- Canonical junction/pipe model serialization and model-indexed storage.
- **Complete hydraulic network import and indexed editing**, including rewiring, membership insert/delete, catalogues, dependent controls and physically parameterized entities.
- History operations, branch diffs, multi-parent merges and conflict handling.
- Full and incremental branch transfer across independent repositories and both physical formats.
- Maintenance: explicit verification, offline compaction, crash and recovery paths.

## Running a hydraulic edit benchmark

```bash
./compile_linux.sh
run_root="$(mktemp -d /tmp/aowis-db-custom-bench.XXXXXXXX)"
./build-linux/aowis-native-hydraulic-edit-benchmark \
  "$run_root/repository" 100000 20 3 block
```

`block` chooses the block backend; omit it to use the default object-pack backend. The benchmark target must not already exist. **Create a fresh random parent for every run.** Report the physical backend, entity count, edit counts, compiler mode, fixture type and actual metric names with the measurements.

The canonical hydraulic fixture reports `import_ms`, `peak_rss_kib`, `network_membership_chunk_count`, and indexed edit/structural operation times. It constructs a complete `NetworkHydraulic` fixture in memory; its peak RSS is not a measure of a future streaming import implementation. Lower-level benchmarks are deliberately not equivalent to whole-network validation.

## Comparing storage backends

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-backends.XXXXXXXX)"
./build-linux/aowis-native-hydraulic-model-benchmark \
  "$run_root/pack" 100000 20 10
./build-linux/aowis-native-hydraulic-model-benchmark \
  "$run_root/blocks" 100000 20 10 block
```

The logical model and SHA-256 objects are independent of the physical backend. Benchmark total pack **plus index** bytes, reopen, verified reads, whole-pack verification and compaction—not just one storage file's size.

## Measurement standards

- Use repeated **Release** runs, medians and p95 when enough samples exist; separate cold-cache and warm-cache measurements.
- Include wall-clock latency, maximum RSS, storage growth, object count, fsync cost and recovery/verification overhead.
- Compare equal operations and equal validation work; a bounded indexed edit and a full-network validation run are **different workloads**.
- Distinguish successful, durable commits from staged/unpublished object writes.
- Exercise model-reference integrity, revisions, branch isolation and errors as a correctness gate before interpreting speed.
- Do not equate benchmark passes with production durability. Still needed: power-loss testing, disk-full tests, backup/restore validation and sustained concurrent HTTP workloads.

## Planned service benchmarks

When the REST server exists, add separate measurements for project catalogue lookup, HTTP JSON/binary overhead, indexed entity GET, atomic edit POST, paginated history, large binary synchronization and concurrent project access. Compare library-direct and HTTP paths without modifying persistence semantics.
