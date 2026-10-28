# DVMDB engineering and delivery plan

The [version-control contract](version-control-semantics.md) is the
normative definition of project, branch, revision, working-copy and HEAD
semantics. No feature is complete if it violates those invariants. This
roadmap describes implementation and qualification work; it does not alter
the agreed model.

## Version-control lifecycle

1. **Unborn projects:** project creation generates project UUIDv7 and an
   empty `main` branch UUIDv7 with **null committed HEAD** and no fabricated
   revision. Verify atomic creation and recovery.
2. **Personal working repositories:** Standalone uses built-in DVMDB;
   basic/web GUI uses a configured private HTTP DVMDB. Implement durable
   drafts, local immutable commits, repository-qualified local branch refs
   and remote-tracking state. See
   [private working repositories](private-working-repositories.md).
3. **Local Commit, remote Publish:** create validated immutable revisions
   and atomically advance only local branch HEADs on Commit. Publish verified
   history using a separate expected **shared** HEAD/null and safe
   idempotent update; failed Publish keeps all local commits and drafts.
4. **Branch provenance:** record immutable source branch UUIDv7 and exact
   committed fork revision UUIDv7. Creating a branch from a committed
   snapshot never creates a revision. Creating it from uncommitted edits
   requires an explicit user choice.
5. **Merge and reconcile:** compare stale working-copy bases with the current
   committed tip, resolve field/object and hydraulic-domain conflicts, then
   publish only validated one-/two-parent immutable revisions.
6. **Full history and replication:** expose UUIDv7 identities and distinct
   SHA-256 hashes, nullable HEADs, immutable branch provenance, both merge
   parents, and consistent paginated graph snapshots on native and REST.
7. **GUI integration:** Git-style committed revision ancestry, immutable revision cards,
   separate private working copies, clear conflict recovery and no fake
   branch-origin ancestry edges. See [GUI contract](gui-integration-requirements.md).

## Integrity and fault tolerance

- Ensure snapshot Merkle closure, canonical revision hashes, cross-reference
  indexes, branch-origin metadata and null HEADs survive crash/reopen,
  online service operations, export/import and compaction.
- A branch HEAD must never reference an incompletely persisted revision.
  Ambiguous write results are resolved through idempotency/authoritative
  state, not blind replay.
- Exercise fault injection at immutable object append, revision publication,
  branch ref compare-and-swap, bundle import and compaction.
- Continue qualified ext4/XFS/POSIX storage fault testing, ENOSPC and
  quota drills, long-running resource tests and full backup/restore.

## Scalability and security

- Validate deep/multi-branch DAGs, numerous concurrent private working
  copies, multi-project service load, large hydraulic networks and bounded
  graph/bundle pagination with measured CPU, memory, I/O and latency.
- Record access-control findings and future qualification in the dedicated
  [security/permissions backlog](security-permissions-design.md). Private
  HTTP repos require deployment isolation until this work is implemented.
- Keep native C++ core independent from Qt and SQL backends; native and
  REST entry points must expose consistent commit and conflict rules.

## Release acceptance

A successful release must pass the scenario set in
[version-control semantics](version-control-semantics.md#10-required-acceptance-scenarios):
empty project, first commit, exact historical branching, two-editor conflict,
safe reconciliation, true divergent commits, two-parent merge,
identity/provenance survival across restart/replication, and GUI/native/REST
parity. Functional UI tests must exercise the real data lifecycle, not only
static graph layout.
