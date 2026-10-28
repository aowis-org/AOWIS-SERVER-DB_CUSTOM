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
2. **Private workspaces:** support independent UUIDv7 working copies per
   editor/session, pinned to a committed base UUID or explicit null, with
   draft recovery and ownership.
3. **Conditional commits:** validate the complete candidate model, generate
   one immutable committed revision UUIDv7 and canonical SHA-256, then
   atomically advance the chosen branch only when its expected committed
   HEAD/null matches. Provide idempotent operation status.
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
7. **GUI integration:** branch-oriented lanes, immutable revision cards,
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
- Define owner/session access to working copies, project/branch write
  permissions, API authentication, transport encryption and audit events.
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
