# Remaining development and release qualification

This document records **unfinished work only**. Current features are described
in [architecture](architecture.md), [service](native-database-service.md),
[merging](native-branch-merging.md), [replication](offline-repository-synchronization.md)
and [operations](operations.md). Verified measurements and design tradeoffs
are preserved in [engineering findings](engineering-findings.md).

## Current baseline

AOWIS-SERVER-DVMDB is one native C++ versioned model database using a
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
and a whole-service offline restore drill. On October 26, 2026, after the
AOWIS-SERVER-DVMDB C++/HTTP naming cleanup, **51/51 CTest tests passed
in both Debug and Release** on an operator's clean Linux rebuild. The
standalone Release server also built successfully. Two 300-second Linux
HTTP resource soaks and the earlier uninstrumented
3,600-second run established initial resource evidence. Following removal
of full catalogue scans for single-branch lookups, an **instrumented
3,600-second** run completed **110,733 imports and 110,733 exports**,
**61.52 transfers/s** overall, zero admission 503s, a **28.0 MiB**
sampled RSS peak, 37 peak descriptors and 10 ms worst health latency.
The four 15-minute interval averages were **62.01, 58.70, 61.56 and
63.81 transfers/s**. The optimized run did not exhibit sustained throughput
loss as more than 110,000 branches accumulated. It used a different binary
from the previous one-hour run and does not establish a portable throughput
SLA or a representative model-workload envelope. Separately, an **actual
QEMU/KVM Debian 12 guest** passed four whole-VM cuts on ext4 and four on XFS, each with
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

- The canonical hydraulic workload has now passed three operator-run
  repetitions each at **10K, 50K and 100K pipes**. Reconstruction, validated
  merge, fresh-process verification and pack size grew roughly proportionally
  to pipe count in this generated chain fixture (see engineering findings).
  The remaining work is to compare the saved full evidence (same-binary
  verification, indexed edit/targeted-read/diff latencies, index size and
  peak RSS), define application latency budgets, and extend the workload with
  realistic network topology, deep revision histories and concurrent clients.
  The small CTest smoke and deterministic chain fixture are not
  production-scale evidence. The synthetic six-client/three-project bundle workload has passed
  an instrumented one-hour, >221K-transfer run with stable quarter-hour
  throughput after the branch-lookup fix; a further identical branch-growth
  soak is not the highest-priority evidence gap. Track actual pack/index
  size, scratch and disk I/O, per-project fairness, queue saturation and
  slow-client latency. Repeat the longer run on production storage stacks or
  other hardware if a deployment envelope requires it.
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
- Evaluate optional administrative project deletion/renaming and preview-only
  hydraulic merges when demanded by clients. Network enumeration is already
  tracked as needed for the future GUI checkout milestone below.

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

The P0 full-DAG graph reader is implemented in DVMDB. The next architectural
work **is** a versioned identity/storage migration, specified below; do not
treat it as a cosmetic graph/API change. Prefer reproducible evidence and
direct changes to the existing native engine.

## P-ID — UUIDv7 object identity and verified SHA-256 history (new prerequisite)

**Approved direction, implementation pending.** Give every new first-class
project, branch and revision a distinct UUIDv7 identity; use SHA-256 to verify
the **complete canonical immutable revision record** and its transitive Merkle
snapshot. Keep branch HEADs as references, not revision objects: new branch
creation allocates a branch UUID but **does not** make a revision. Existing
empty root SHA sharing is not cryptographic corruption; it reflects identical
content, while current revisions lack a separate logical identity.

This migration affects the native `S` revision record, SHA parent links,
branch files, catalogue (currently UUIDv4), P0 graph API, `expectedHead`,
replication bundles, native/HTTP clients, verifier, fixtures and the GUI.
Migrate on verified copies with an authoritative, replica-stable mapping of
legacy SHA IDs to new revision UUIDs; never generate divergent mappings on
independent replicas. Do not rekey immutable Merkle data unnecessarily. The
[UUIDv7 identity migration roadmap](uuidv7-identity-migration-roadmap.md)
defines six delivery phases, compatibility and rollback gates, and design
questions requiring sign-off. **Complete P-ID before introducing P1 checkout
and save.** Routine P0 graphical rendering can still be tested against the
existing backend while P-ID is designed.

## Version-control and GUI integration priorities (agreed 2026-10-27)

The core project/revision/branch/branch-HEAD semantics are now explicitly
recorded in [version-control semantics](version-control-semantics.md). The
revision **topology and branch-ref semantics** remain agreed; the separate
UUIDv7 milestone changes how their identities are persisted and transported.
A branch is a movable named revision ref, each branch has a HEAD, and the GUI
shows histories reachable from **all** HEADs. A HEAD is not necessarily a
leaf; the GUI's selected revision or eventual working-copy base is separate.
No branch creation-event or branch-parent object is currently persisted.

**P0 backend implemented: full-DAG graph discovery.** Native
`Repository`/`ProjectService` and REST now provide a coherent snapshot of all
branch HEADs and bounded, deduplicated all-parent revision enumeration,
without changing storage format. See [native revision history](native-revision-history.md)
and the [live OpenAPI route](dvmdb-http-api.yaml). The first implementation
uses stateless DFS replay and has documented traversal limits; indexed scalable
pagination is a later optimization. **Next P0 task:** wire the GUI's embedded
and REST readers to these pages and preserve partial/full graph state in the
renderer. The old `history` endpoint remains intentionally first-parent.
See the [graph acceptance contract](version-control-semantics.md#5-current-read-surface-and-missing-full-graph-contract)
and [GUI requirements](gui-integration-requirements.md).

**Deferred by product decision: working-copy checkout and saving.** Network
listing, revision-pinned checkout, dirty editor sessions and expected-HEAD
commits follow both the graph integration milestone and **P-ID identity
migration**. Project/branch rename-delete, richer
metadata/annotations, reviews and tags are separate follow-on work, **not
universal prerequisites** for a first trusted-development checkout/save
workflow. The AOWIS-wide TLS/authentication/authorization design remains a
hard prerequisite before untrusted network exposure; existing plain HTTP is
not secured by the provisional native access policy.

The remaining operational, durability and release qualification items above
remain open; this prioritization does not cancel them. Planned endpoint names
are **not** implemented HTTP API until code, tests and OpenAPI ship together.
