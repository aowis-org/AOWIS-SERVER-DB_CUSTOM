# Remaining development and release qualification

This document records **unfinished work only**. Current features are described
in [architecture](architecture.md), [service](native-database-service.md),
[merging](native-branch-merging.md), [replication](offline-repository-synchronization.md)
and [operations](operations.md). Verified measurements and design tradeoffs
are preserved in [engineering findings](engineering-findings.md).

## Current baseline

AOWIS-SERVER-DB_CUSTOM is one native C++ versioned model database using a
content-addressed pack, durable branch references, Merkle indexes, distributed
full/incremental bundles and recursive merge bases. Canonical hydraulic
persistence/validation and the multi-project HTTP service are implemented.
The server has separate aggregate HTTP-staging and internal-scratch budgets,
per-project operation control, atomic named-bundle exports, full repository
verification and offline backup/restore. The executable can also be embedded
as native libraries. Authentication and TLS are reserved for the future
AOWIS-wide security design, not to be improvised here.

Existing tests include process crash/restart, native fault injection, a
private-tmpfs physical ENOSPC drill, real-process concurrent HTTP transfers,
and a whole-service offline restore drill. On October 24, 2026, **43/43 CTest
suites passed in both Debug and Release**, and an opt-in 300-second HTTP
soak passed (7,516 imports and 7,516 exports). Separately, an **actual QEMU/KVM
Debian 12 guest** passed four whole-VM cuts on ext4 and four on XFS, each with
full repository verification and no acknowledged commit lost. These are
operator-reported measurements, not automatic CI or hardware-level durability
certification; see [engineering findings](engineering-findings.md).

## Durability and failure qualification

- Repeat the now-proven QEMU ext4/XFS guest-cache-loss drill with many more
  rounds, wider commit timings, new guest/kernel/QEMU versions, and automated
  retention of machine, disk-cache and JSON evidence. Four rounds per
  filesystem establish a useful initial result, not exhaustive coverage.
- Repeat on deployment filesystems, hardware and storage stacks. VM-kill
  tests do not simulate loss of the host/controller's volatile cache; document
  exactly which guarantees are supported by the target filesystem and device.
- Repeat kernel ENOSPC tests for deployed ext4/XFS and the HTTP staging path;
  exercise filesystem quota, inode exhaustion, and crash during compaction.
- Confirm recovery of branch heads, pack generations and export publication
  after abnormal process shutdowns and across version upgrades.

## Scalability and operations

- Run longer, repeated mixed-read/write/merge/replication soaks on realistic
  project sizes. The Linux HTTP resource sampler records RSS, CPU and open
  descriptors during the existing three-project soak; it still needs measured
  target-machine runs. Also monitor pack/index growth, scratch usage,
  per-project fairness, queue saturation and latency under slow clients.
- Record 100K/1M/10M cold-cache and repeated cache-warm index measurements,
  including sidecar regeneration and compaction. Re-evaluate the compact
  lookup only if representative regressions justify it.
- Test much larger full-history exports and incremental deltas under scratch
  limits; profile disk space, wall time and cancellation behavior. The
  incremental path still collects a sorted in-memory set for emitted delta
  hashes; bound it if measurements show an actual need.
- Extend the existing historical-format, four-project cold-upgrade/restore
  drill to larger representative service roots and actual cross-host/storage
  migrations. Online snapshots and online compaction are not implemented.
  Define retention, restore and orphan-scratch cleanup policy.
- Evaluate optional administrative project deletion/renaming, preview-only
  hydraulic merges and network enumeration only when demanded by clients.

## Release gates

- Run native-only, Qt/model, HTTP, Debug and Release CI on supported targets;
  add fuzz/property testing for malformed bundles, merge DAGs and failure
  interleavings. No known critical integrity regression may remain open.
- Define supported platforms/filesystems and explicitly freeze the pack,
  sidecar and portable bundle compatibility policy before a stable release.
  Frozen pre-release pack/bundle fixtures and a multi-project restore/upgrade
  drill protect historical read/import and new-write compatibility; see
  [format compatibility](format-compatibility.md).
- Publish a measurable resource envelope, backup/restore procedure, version
  migration guide and operational troubleshooting guide. Validate field
  deployment/upgrade on disposable representative datasets.
- Specify and implement the cross-AOWIS security system before exposure to
  untrusted networks; current plain HTTP does **not** enforce access rules.

A new feature or major storage rewrite is **not** implied by this roadmap.
Prefer reproducible evidence and direct changes to the single native engine.

## GUI integration contract (2026-10-25)

See [GUI integration requirements](gui-integration-requirements.md) for the
proposed project/branch CRUD, network discovery, immutable commit metadata,
mutable annotations and labels, graph paging, dry-run merges, security and
concurrency work. This is a design backlog, not implemented functionality.
