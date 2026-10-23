# Native AOWIS database — remaining work

This document tracks work that is **not yet complete**. The implemented database
already uses a single append-only, content-addressed pack backend; persistent
Merkle model and reverse-reference indexes; durable revision/branch publication;
verification; hydraulic persistence and merge conflict handling; full and
incremental bundles; and a multi-project HTTP service with bounded streaming
bundle transfers and per-project operation coordination. Those capabilities
are not alternative backends or future proposals.

The [October 2026 review remediation plan](review-remediation-roadmap.md)
documents completed hardening phases. Do not reopen completed work without a
measured regression or a new requirement. No SQL adapter or authentication
system is planned in this project; AOWIS-wide security is a separate design.

## P0 — Semantic correctness before new database features

### 1. Validate hydraulic fast-forward merges — implemented

Native fast-forwards with `MergeOptions::validate` check the model-indexed
source snapshot under the publication lock and reject invalid candidates
without writing or moving refs. The hydraulic adapter supplies its full network
validator. `UpToDate` is explicitly a no-op, not a revalidation operation.
Native, hydraulic and HTTP regression cases cover valid and invalid sources,
stale head pins and equal-head merges. Keep this invariant in future changes.

### 2. Make export limits effective during traversal — implemented

`Repository::exportBundleLocked` now enforces `BundleLimits::maxObjects` as
unique object hashes enter the full or incremental export set, **before**
opening an output path or writing to a caller-owned descriptor. Shared Merkle
objects and revisions count once. Exceeding the limit reports `LimitExceeded`
without creating a partial output file or disturbing an existing destination.

`aowis-native-bundle-limit-tests` covers exact and over-limit exports,
repeated payload objects, two-parent revision histories, both export variants,
descriptor output safety, and successful imports of bounded bundles.

The limit caps objects **emitted**, not the pre-existing ancestry inspected to
establish a valid incremental base. Full or incremental exports that succeed
with an unlimited or very large object budget still construct the resulting
sorted hash set in memory. Bounded-memory successful export remains Phase 4.

## P1 — Complete distributed version control

### 3. Merge criss-cross revision histories

The ancestor algorithm deliberately rejects histories with multiple independent
lowest common ancestors. Implement recursive merge-base synthesis rather than
arbitrarily choosing one base.

- Design deterministic virtual-base consolidation, preserving UUID/model-field
  merge semantics and deterministic conflict reporting.
- Cover two and more incomparable bases, nested criss-cross histories, reversed
  branch selection, and repeated merges across independent replicas.
- Benchmark large shared histories and ensure ancestry traversal does not
  multiply work exponentially.

**Exit criterion:** valid multi-base DAGs merge or report actual semantic
conflicts without the current blanket rejection.

### 4. Bound full-history replication memory

Full export currently holds all reachable hashes in RAM; successful large
exports are not bounded by a fixed memory budget. Imported bundle verification
already has disk-backed techniques that can inform the export design.

- Prototype external-memory traversal/deduplication or a sorted disk-backed
  object index. Preserve deterministic bundle encoding and object verification.
- Use project-local, quota-aware scratch files, with correct cleanup on
  cancellation, simulated ENOSPC and process interruption.
- Keep incremental-transfer efficiency: do not regress to full-history walking
  for small deltas.

**Exit criterion:** export peak RSS grows slowly with history size and is
bounded under the configured operational budget; imported heads remain exact.

## P2 — Capacity and service operations

### 5. Measure, then optimize physical object lookup

The physical SHA-256-to-pack-location `unordered_map` remains RAM-resident,
including entries for unreachable objects until offline compaction.

- Benchmark 100k, 1m and 10m physical objects: fresh open/index-rebuild time,
  peak RSS, read latency, commit throughput, compaction time, and multiple
  concurrently open project repositories.
- If results miss agreed budgets, introduce a disk-backed searchable index plus
  bounded cache or a similarly measured alternative; retain the append-only
  pack as the **only** physical storage format.
- Exercise index rebuild after deletion/corruption, sparse and dense object
  distribution, cold cache and long-lived handles.

**Exit criterion:** recorded performance budgets and a reproducible benchmark;
any new lookup design must improve meaningful workloads without compromising
recovery or format compatibility.

### 6. Operational resource bounds and maintenance

- Test simultaneous large HTTP imports/exports across projects, slow clients,
  disconnects, timeouts and scratch disk exhaustion. Define and enforce
  request CPU/time budgets and global scratch/disk quotas in addition to the
  existing per-project pending-operation limits.
- Provide a coordinated backup/recovery procedure with explicit snapshot
  consistency, verification after restore, and managed offline compaction.
  Project delete/rename and online maintenance remain optional follow-on API
  work, subject to actual product needs.
- Write operator runbooks for full disks, recovery-tail evidence, unexpected
  corruption, partial publication ambiguity and service restart.

**Exit criterion:** adversarial service tests demonstrate bounded resource use
and actionable recovery steps.

## P3 — Release confidence and documentation

### 7. System-level correctness and compatibility

- Run Debug and Release, native-only and full Qt/model/HTTP suites in CI.
- Add long-running multireplica push/pull/merge tests, fuzzed corrupt bundles,
  independent integrity audits, process crash/ENOSPC injection and controlled
  device/filesystem power-loss tests where feasible.
- Define the supported Linux filesystems, resource assumptions and durability
  guarantees before freezing a public pack/bundle format.
- Recheck CMake/Ninja full rebuilds and ZIP snapshots after modifying submodules.

**Exit criterion:** documented, repeatable release gates; no open critical
integrity bugs; backup/restore and replication exercised end-to-end.

### 8. Keep documentation synchronized with the implementation

- Remove stale claims that HTTP only supports catalogue operations or does not
  exist in `docs/architecture.md`, `docs/native-branch-merging.md`, and
  `docs/native-revision-history.md`.
- Align service, OpenAPI, architecture, README and merge semantics; distinguish
  implemented functionality from future requirements.
- Retain concise explanations of actual behavior; do not turn historical patch
  mechanics into permanent architecture documentation.

**Exit criterion:** the documentation accurately describes the released APIs,
formats, limits and operational constraints.

## Repository snapshot packaging — addressed

`tools/zip_db.sh` includes the actual working-tree files of all initialized
Git submodules recursively, with non-ignored untracked and modified files.
Git metadata, ignored artifacts and configured private-file patterns are
excluded. Packaging fails for an uninitialized submodule and only replaces the
previous archive after a successful run. `aowis-native-zip-submodule-tests`
uses disposable nested Git repositories to guard this behavior.

For a first checkout, run `git submodule update --init --recursive` (or the
project's `./git_submodule_init.sh`) before creating the ZIP. The resulting
`db_custom.zip` is a self-contained **source snapshot**, not a Git clone: its
submodule trees contain files but no `.git` directories. Future packaging
changes must preserve that distinction.
