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
establish a valid incremental base. The successful full-export hash set has
since moved to disk-backed scratch storage (Phase 4); large incremental
deltas continue to use an in-memory sorted set.

## P1 — Complete distributed version control

### 3. Merge criss-cross revision histories — implemented

Multiple independent lowest common ancestors are combined into deterministic
virtual merge bases without writing temporary revisions or pack objects. The
ancestry tests exercise two and three bases, nested criss-cross histories,
reversed merges, and genuinely conflicting virtual bases. Keep the recursive
merge result deterministic and benchmark very large/shared DAGs as part of
release qualification.

### 4. Bound full-history replication memory — disk-backed core implemented

Full-history export now discovers reachable hashes in an open-addressed table
held in private, repository-local scratch files. The table grows on disk and
uses `pread`/`pwrite` rather than writable file mappings, so scratch ENOSPC
raises an error rather than SIGBUS. A bounded-chunk external merge sort emits
objects in unchanged SHA-256 order. Neither an in-memory `std::set` of all
reachable hashes nor a complete in-memory sorted output is required. Both
path and descriptor exports retain the existing wire format and error behavior.

- Measure peak RSS, scratch bytes and throughput at 100k, 1m and 10m objects.
- Define and enforce operational scratch-disk and transfer-time budgets;
  exercise ENOSPC, cancellations and interrupted exports on deployed filesystems.
- Incremental export still collects a sorted in-memory set of emitted delta
  hashes to preserve efficient changed-trie traversal. Consider externalizing
  large deltas only if profiling demonstrates a need.

**Remaining exit criterion:** verify large-export RSS under a documented budget,
quota enforcement, and failure recovery on the supported production filesystems.

## P2 — Capacity and service operations

### 5. Compact physical object lookup — implemented, scale verification pending

The Release baseline measured on the developer machine (October 23, 2026)
showed 101,387 / 1,001,959 / 10,001,723 physical objects using approximately
8.4 / 72.8 / 703.5 MiB extra RSS on open and 49.5 / 690 / 8,612 ms to
reconstruct the old in-memory `unordered_map`. Two 10m-object repositories
reached approximately 1.38 GiB RSS (1.48 GB). See [benchmark-plan.md](benchmark-plan.md)
for the repeatable benchmarking procedure.

The physical lookup accelerator now stores 8-byte open-addressed slots
(32-bit FNV-derived hash fingerprint plus a 32-bit ordinal referencing the
existing checksummed `objects.idx`). A fingerprint match **must** compare the
full SHA-256 hash in the sidecar before using its pack offset. A small 4096-slot
direct-mapped hot-object cache reduces repeated sidecar reads. The append-only
pack, sidecar encoding, revision IDs, and replication format are unchanged.
`pread` is used for random sidecar reads, so the index does not mmap the full
sidecar into process RSS. Rebuilding a lost/corrupt derived sidecar still
verifies the pack. This accelerator currently supports at most 2^32-1
physical sidecar entries per generation; that bound must be revisited before
exceeding it.

- Repeat the identical 100k/1m/10m **Release** matrix with the compact index
  and publish before/after RSS, reopen, lookup, commit, compaction, sidecar
  rebuild, and simultaneous-project figures.
- Test cold-cache/random lookups, hash fingerprint collisions, corrupt and
  truncated sidecars, crashes during partial publication, and long-lived
  cross-process repository handles.
- Profile the optional bounded cache; increase it only with evidence, not
  by making the physical index memory proportional to pack size again.

**Remaining exit criterion:** establish and meet a documented large-scale
memory/latency budget on the deployed storage hardware, with operational and
corruption recovery verified. This remains open until the Release rerun and
fault matrix have completed.

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
