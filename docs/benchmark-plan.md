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

## Merge ancestry and conflict processing

Build optimized binaries with `./compile_linux.sh`, then run the dedicated
conflict benchmark using a **fresh, nonexistent repository path for every run**:

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-history.XXXXXXXX)"
./build-linux/aowis-native-merge-history-benchmark \
  "$run_root/pack" 10000 100 pack
./build-linux/aowis-native-merge-history-benchmark \
  "$run_root/blocks" 10000 100 block
```

The arguments are repository path, shared-commit count, divergent commits **per
branch**, and physical backend. The benchmark constructs a common history,
creates diverging branches with distinct UUIDs, then deliberately introduces
one same-UUID conflict. It reports `merge_conflict_ms` for the failed merge,
which includes ancestor lookup and Merkle difference/conflict computation but
**not** merge revision writing or fsync. The benchmark asserts the correct
base, one conflict, and unchanged heads. `process_peak_rss_kib` measures the
**whole process**, including fixture construction, not the merge operation's
incremental memory usage; don't use it to claim a merge-only RSS reduction.

Measure repeated Release runs and report median, sample count and hardware.
Use larger histories and varied branch DAGs to validate scaling; the separate
ancestry correctness suite checks many non-linear histories against a deliberately
exhaustive oracle. This microbenchmark does not substitute for hydraulic semantic
merge, disk recovery or HTTP workloads.

## Replication memory and temporary-disk benchmark

This benchmark exercises the native bundle importer in **separate child processes**
so creating the source fixture cannot inflate the import process's RSS. It
checks an independently imported full history against a baseline+incremental
transfer, performs full native integrity verification, and measures all four
pack/block source/destination combinations with both raw and model-indexed
histories. Model fixtures contain unique payloads, scattered deterministic
UUIDs and a chain of valid model references; they do **not** instantiate
hydraulic-domain ModelCodec objects.

Build the Release executable, then use a unique temporary run root:

```bash
./compile_linux.sh
run_root="$(mktemp -d /tmp/aowis-db-custom-replication.XXXXXXXX)"
python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux/aowis-native-replication-scaling-benchmark \
  --objects 10000 --commits 20 --payload-bytes 96 \
  --kinds both --backends both --repeats 3 \
  --work-root "$run_root" --output "$run_root/results.csv"
```

Repeat with 1,000 / 10,000 / 100,000 objects (and, if practical, 1M) and
vary the number of revisions independently. Full and incremental imports are
recorded separately; each uses a **fresh recipient repository**. Baseline
seeding for an incremental transfer is performed first, in a different child
process, and is not counted as part of the delta import. The test suite also
runs `--smoke`, using small raw/model graphs and validating identical bundle
bytes from the pack/block exporters, imported revision heads, verification
counts, and full-versus-delta object totals.

CSV fields include:

- `import_ms`: duration of the native importer only, including its required
  pre-publication structural and model-reference verification and publication.
- `verify_ms`: **additional**, independent post-import full-repository audit.
- `import_peak_rss_kib`: peak child-process RSS read from Linux
  `/proc/self/status` (`VmHWM`) immediately after import. It includes runtime
  and repository setup, but excludes subsequent audit allocation. Using
  `getrusage(RUSAGE_SELF).ru_maxrss` here would be incorrect: Linux may retain
  the Python launcher's pre-`exec` high-water mark, yielding a misleading
  constant memory baseline for every child.
- `process_peak_rss_kib`: the same child-local Linux `VmHWM` measurement
  after import **and** post-import audit. It must be at least as large as the
  post-import high-water mark.
- `observed_peak_scratch_logical_bytes` and
  `observed_peak_scratch_allocated_bytes`: sampled open scratch-file sizes
  (`fstat` via Linux `/proc/<pid>/fd`), including unlinked scratch files.
  This is a **sampled lower bound**, not a guaranteed high-water mark: files
  created and closed between 10 ms samples may be missed. Logical size and
  filesystem allocated blocks are reported independently.
- `profile_total_ms`: importer's own elapsed time (excluding the independent
  audit); it should closely agree with `import_ms`. The `profile_*_ms` fields
  break this down into nonoverlapping scan/checksum/index, structural graph
  traversal, semantic model-reference verification, reachability/ancestry,
  branch fast-forward decision, object copying and fsync/ref publication.
  `profile_graph_ms` includes scratch-table preparation, while
  `profile_model_references_ms` covers the complete semantic validation of
  distinct model snapshots. These are **wall-clock phase timings**: nested
  individual operations such as I/O are not measured separately.
- `profile_bundle_reads`, `profile_bundle_read_bytes`, `profile_local_reads`,
  and `profile_local_read_bytes`: object fetch operations by the importer after
  the initial checksum pass. These count **repeated reads** during verification
  and copying, not unique physical objects or disk-sector reads. A repeated
  local fetch can hit the operating-system page cache.
- `process_wall_ms`: process-launch-to-exit wall time, including startup,
  import, separate full verification and benchmark-report generation.
- `bundle_bytes`, `imported_objects`, `verified_revisions`,
  `verified_reachable_objects`, and `storage_pack_bytes`: context for
  interpreting each result. Storage bytes are pack bytes, **not** the total
  index/manifest footprint.

The script requires Linux `/proc`, reports CSV, and cleans all intermediate
repositories and scratch directories. Each invocation creates a random,
previously nonexistent working directory. A smoke check is also available:

```bash
python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux/aowis-native-replication-scaling-benchmark --smoke
```

**Interpretation:** A file-backed mapping is not necessarily zero-RSS: accessed
pages still count toward resident memory, even when reclaimable. Check absolute
peak RSS and how it scales with objects/revisions, rather than claiming constant
memory from the use of temporary files. CPU/page-cache effects, disk latency,
filesystem allocation and OS caching can dominate individual measurements;
repeat Release runs on representative hardware, preferably with both warm and
cold-cache regimes. This benchmark measures the native library, **not** Qt HTTP
upload buffering, concurrent transfers, physical power loss or hydraulic
semantic parsing. It establishes a baseline for identifying the next memory
bottleneck without altering the database engine.

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

### Large-repository replication measurements (persistent fixture mode)

Preparing a large source repository may take considerably longer than a single
import. For large model-indexed histories, split preparation from measurements
so a timeout or an interrupted import cannot force fixture reconstruction.
The runner refuses to overwrite an existing fixture directory and records SHA-256
checksums of all bundles in `manifest.json`. Import-only runs validate the
manifest, requested fixture parameters, source heads and bundle checksums before
measuring transfers. The preparation phase is **not** included in import timing.

From the project root, after a Release build:

```bash
# Unique, previously nonexistent directory for persistent fixtures.
fixture_root="$(mktemp -d /tmp/aowis-db-custom-100k-fixture.XXXXXXXX)"
rmdir "$fixture_root"

python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux/aowis-native-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --repeats 1 \
  --mode prepare --fixture-dir "$fixture_root" --progress

python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux/aowis-native-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --repeats 3 \
  --mode imports --fixture-dir "$fixture_root" \
  --output "$fixture_root/import-results.csv"
```

The fixture directory is **retained** for import-only reruns; delete it manually
when no longer needed. Import recipients and scratch data are created in fresh
random temporary directories for each run, not inside the retained fixture.
Progress reports each fixture commit and identifies base/full/incremental bundle
exports and the final independent source verification; this makes long preparation
stalls distinguishable from import bottlenecks. `--mode all` retains the original
one-command behavior with automatically deleted fixtures, and `--smoke` still
exercises both physical backends and both logical record kinds. To compare the
physical stores, prepare another fixture using `--backends both`; that takes
longer but also confirms identical exported bundles across the two backends.

Do not interpret the 100k **logical model-record** fixture as 100k physical
content-addressed objects: each commit creates additional immutable Merkle nodes
and reverse-reference data. The CSV reports transferred physical object counts.
Source fixture preparation, bundle generation and source verification are timed
together and must not be confused with `import_ms`, which only measures native
import and its prepublication integrity checks. The benchmark still does not
measure streaming HTTP upload memory or production hydraulic ModelCodec parsing.

### Investigating import latency (Stage 9.11)

The bundle importer accepts an optional `BundleImportProfile*` for lightweight
per-phase instrumentation. Normal C++ and HTTP callers may omit it; integrity
checks and publication semantics are identical. A successful profile is reset
on entry and populated before return, including idempotent up-to-date imports;
partial fields after exceptions are **not** a valid completed profile.

The 100k model benchmark on the development machine (pack → pack) previously
showed incremental transfer taking longer than a larger full transfer. Rather
than assuming the smaller transfer should necessarily be faster, compare the
new phase columns on the *same* persistent fixture:

```bash
python3 tools/benchmark_replication_scaling.py \
  --binary ./build-linux-tests-release/aowis-native-replication-scaling-benchmark \
  --objects 100000 --commits 20 --payload-bytes 96 \
  --kinds model --backends pack --repeats 3 \
  --mode imports --fixture-dir "$fixture_root" \
  --output "$fixture_root/results-profile.csv"
```

A local Release run with 10k logical model records (20 revisions, two
repetitions, pack → pack) found `profile_model_references_ms` around 2.1–2.6 s
out of 3.2–3.6 s import time. The incremental case fetched roughly 576k
locally resident objects (114 MB of *logical bytes reread*, not necessarily
physical disk I/O). Those observations identify a verification hotspot, but
larger and cold-cache measurements must establish how much can safely be saved
before changing integrity rules. The independent `verify_ms` audit is always
separate and is **not** included in any of the `profile_*` phases.

### Model-reference verification and incremental history

Imported histories are checked for canonical model records, existing dependency
targets and exact consistency of the reverse-reference index. The importer
validates the first model-indexed snapshot in full and thereafter compares
immutable Merkle subtrees against an already validated model-indexed parent.
Changed outgoing and incoming edges must agree exactly, and deleting any
still-referenced target is invalid even when its referrers were not edited.
Merge parents are each checked before the merged revision.

For performance comparisons, benchmark the same deterministic fixture and
physical backend with identical import/verification settings in separately
built revisions of the code. Compare `profile_model_references_ms`, full and
incremental `import_ms`, `profile_local_reads`, peak RSS and scratch space.
The verification time reported in `verify_ms` is a **separate comprehensive
post-import audit**, not part of `import_ms`. Larger record updates can fall
back to merging all leaves of a structurally rewritten subtree; measure those
cases separately from narrow incremental edits.
