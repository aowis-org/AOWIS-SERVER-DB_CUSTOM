# Revision history and branch-local committed HEADs

This document specifies the native history contract. The authoritative
behavior is [version-control semantics](version-control-semantics.md).

## Revisions and identity

A revision is a finalized, immutable whole-model snapshot. Each committed
revision has a UUIDv7, project UUIDv7, zero/one/two ordered parent revision
UUIDv7s, immutable metadata, snapshot Merkle roots and an independently
verified SHA-256 canonical record digest. UUIDv7 is the object's identity;
SHA-256 proves content integrity. There is no mutable revision state.

Parent references form a DAG. Commits from multiple branches can share
ancestors without copying immutable revision objects. A two-parent merge
revision preserves both ancestry links; first-parent history is only a
convenient projection, never the entire project history.

## Branches and unborn HEADs

Each named branch has its own UUIDv7, an optional `headRevisionId` and
immutable creation provenance (`originBranchId`, `forkRevisionId`). A project
is created with an unborn `main` branch and **zero committed revisions**.
Only the first successful commit supplies its first HEAD UUIDv7.

The branch HEAD is the latest **committed** revision, not the user's
working copy. Several branches can point at the same committed revision.
`head(branch)` is independent of other branch HEADs and of any GUI selection.
No repository-global checkout HEAD exists in the multi-user service.

## Private working copies

Checkout creates an independent, editable working copy with UUIDv7, project
UUID, branch UUID, owner/session, exact committed base UUID (or null), and
private model changes. Multiple people may check out the same branch at the
same base. The committed revision remains immutable. Opening a historical
revision for inspection does not make it writable or move any branch HEAD.

## Commit and conflict behavior

A commit must pin its target branch and `expectedHeadRevisionId` (including
explicit null for an unborn HEAD), validate the complete proposed model,
durably create one fresh immutable revision with the correct parents, and
atomically advance that branch ref only when the expected HEAD matches.
Stale HEADs fail without publishing changes. A stale HEAD requires an explicit
reconcile/rebase/merge workflow; a stale base is not automatically a content
conflict. Successful writes remain discoverable after ambiguous timeouts
through idempotency and authoritative reads.

## Graph and read APIs

Native history, immutable revision lookup, diffs and full revision graph
queries must return the same UUIDv7 identities and SHA-256 bindings as REST.
Paginated full-graph queries capture a consistent set of all branch HEADs,
include all reachable committed revisions and both merge parents, and
validate ancestry closure. Unborn branches appear in the branch list but
contribute no revision vertex.

Read-only operations must never change HEADs, working-copy bases or owner
session state. See [merge semantics](native-branch-merging.md) and
[GUI integration](gui-integration-requirements.md).
