> Historical benchmark plan retained for reference. The current AOWIS database
> direction is the native engine; no SQL repository is needed for build or use.

# Historical SQL/native experiment: benchmark contract

This historical plan required **complete equivalent model operations**, not SQL
queries against raw filesystem operations. Native v0 is only an initial storage
microbenchmark and is **not yet comparable** to the SQL engine.

## Datasets

- 1,000; 10,000; 100,000; 1,000,000 pipe-scale models.
- Fixed deterministic UUIDs, identical canonical AOWIS model records,
  topology, hydraulic references, attributes and validation rules.
- Distinct deterministic payloads by default; avoid inadvertent duplication
  (the original native-v0 synthetic fixture repeated every 256 objects).
  Keep a `legacy` fixture solely for reproducing the original baseline.
- Revision histories with 1, 10, 1,000 and 10,000 changed entities per edit,
  both clustered and scattered UUID distributions.
- Branches with divergent edits, disjoint changes, conflicts and deletions.

## Operations to measure

- Cold reopen/startup and integrity verification.
- Initial import (including all required indexes/constraints).
- Random single-model lookup, selected network lookup and complete scan.
- Single-object edit and multi-object transactional batch edit.
- Revision finalization, create branch, compare branches and three-way merge.
- Multiple editors, recovery following forced interruption and disk-full.
- Historical storage growth and garbage-collection/compaction cost.

Measure wall-clock latency (median and p95 after repeat runs), operations per
second, RAM/high-water memory, bytes persisted and read/write amplification.
Separate warm and cold cache tests, use identical hardware and compiler flags,
and ensure successful commits are durable on every engine. Clearly mark any
unsupported operation `N/A`, never zero milliseconds.

## Comparison candidates

1. Existing SQL baseline on SQLite.
2. Existing SQL baseline on PostgreSQL.
3. Optimized SQL model (Merkle diff, no full working-state copy, batched tree
   transformations) on both databases.
4. Native prototype with equivalent model validation, durability and history.
5. Optional hybrid where SQL metadata coordinates native content packs.

The **correctness gate precedes performance comparison**: serialized state,
revision roots, branch behavior, conflicts, model references, crash recovery
and audit semantics must be equivalent or deviations must be stated.

## Native model-record microbenchmark (stage 3)

`aowis-native-model-benchmark` uses two hydraulic-shaped entity categories
(junction-shaped records and pipe-shaped records with endpoint UUIDs), but the
payload bytes remain synthetic placeholders. It measures atomic reference
validation and a typed binary envelope, NOT the full ModelCodec or hydraulic
constraint validation. Results are not SQL model-performance comparisons.
See [model-records.md](model-records.md) for limitations.

## Canonical model-codec integration (optional stage 4)

`aowis-native-hydraulic-model-benchmark` now uses the native-owned canonical model codec; it measures real AOWIS
`HydraulicNodeJunction` and `HydraulicLinkPipe` payloads and decodes them on
scan. It separates serialization from native storage time. This closes the
placeholder-payload gap **only for these two entity categories**. Complete
model validation, full hydraulic indexes, audit semantics and SQL-equivalent
fixtures remain future milestones. Stage 5 measures a persistent, incremental
UUID reverse-reference index separately; its import and storage overhead are
part of the indexed benchmark, not a free optimization. See
[canonical-model-codec.md](canonical-model-codec.md).

## Persistent index measurements (Stage 6)

Compare against a Stage 5 build **on the same machine** and identical fixture.
Stage 5 `reopen_ms` includes full payload hashing; Stage 6 `reopen_ms` reads
and validates sidecar entries, while its separate `verify_ms` performs full
pack hashing. Report both for a fair integrity-cost accounting. Also measure
`index_bytes` because faster opening consumes additional persistent storage.
Benchmark commands must create fresh random temporary directories, e.g.
`mktemp -d /tmp/aowis-db-custom-stage6.XXXXXXXX` and `$run_root/repository`.
For a production-grade decision, include repeated warm/cold-cache runs,
corrupted sidecar fallback, explicit verification and process-crash tests.
