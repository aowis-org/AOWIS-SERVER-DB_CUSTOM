# Native AOWIS database — current capabilities and outstanding work

## Database architecture

AOWIS uses a native, content-addressed database. Immutable objects are stored
in one append-only pack format with a rebuildable index. Persistent Merkle
indexes represent model records and incoming references; atomic branch-head
publication commits revisions. Repositories support branches, immutable
history, semantic hydraulic merges, and full/incremental portable replication.
The same engine is usable directly from C++ and through the multi-project
HTTP/TCP service.

## Integrity and durability

Repository verification checks physical SHA-256 records, revision ancestry,
canonical Merkle paths, model-reference targets, and exact reverse-reference
index agreement. Bundle imports validate before publishing any branch head.
Revision-pinned hydraulic APIs supply consistent historical snapshots.
Subprocess tests cover injected crashes, partial writes, simulated ENOSPC,
index reconstruction, interrupted imports, and compaction publication. Actual
power-loss testing and operational backup/recovery procedures remain necessary.

## Storage backend

Pack storage is the sole supported physical format. Repository handles may
operate concurrently; offline compaction requires exclusive lifetime
ownership. In particular, do not run compaction while a server or another
process holds the repository open. The pack operational stress suite covers
concurrent readers and writers, model-reference updates, stale-write rejection,
merge histories, replication, compaction, reopening, and continued writes.

Pre-release experimental block repositories are **not** opened automatically.
Export portable bundles using an older build before upgrading, then import
the histories into a new pack repository. No historical format-migration
machinery is provided before the first public release.

## Replication scalability

Imported bundle indices, imported object visitation markers, Merkle validation
summaries, and large reference-edge sorts use private file-backed temporary
storage. Verification of later model snapshots compares changed immutable
paths against a verified parent without rebuilding all relationships. Local
incremental ancestors and some per-snapshot state still consume RAM.

HTTP downloads are file-backed, but Qt currently buffers complete incoming
upload bodies; genuine streamed upload ingestion with backpressure and
concurrency quotas remains unfinished. CPU/time budgets for untrusted large
requests and larger multi-server workload studies also remain outstanding.

## Build and release

Debug, Release, native-only and Qt-enabled tests should all pass. Keep
adversarial corruption, fault injection, concurrency, synchronization and
hydraulic semantic checks in CI. Complete the C++ naming/style cleanup,
review the HTTP operational limits, and freeze the physical format only when
preparing the first public release. No SQL dependency or separate database
adapter is planned.

## Review remediation (2026-10)

Findings from the October 2026 code review (torn pack tail recovery, typed
storage errors, request bounds, scratch storage location, incremental sync
cost, concurrent service, model value validation and cleanup) are scheduled
phase by phase in the [review remediation roadmap](review-remediation-roadmap.md).
