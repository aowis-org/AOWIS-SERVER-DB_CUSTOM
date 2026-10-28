# UUIDv7 native and REST history contract

Both native and HTTP interfaces implement the same logical semantics in
[version-control semantics](version-control-semantics.md). UUIDv7 identifies
projects, branches, committed revisions and private working copies.
SHA-256 hashes are independent integrity fields.

## Catalogue and history

- Creating a project returns its project UUIDv7 and unborn `main` branch
  UUIDv7, with `headRevisionId: null`. No revision is committed on creation.
- Listing branches includes stable branch UUID, UTF-8 name, nullable
  committed HEAD UUID, `originBranchId` and `forkRevisionId`.
- Creating a branch from a finalized revision records the exact fork UUID
  and optional source branch UUID, without generating a new revision.
- Immutable revision reads return ordered committed parent UUIDv7s,
  SHA-256 snapshot roots, canonical record hash and parent hash bindings.
- Paginated history captures all branch HEADs consistently, includes both
  parents of merges and validates closure across pages.

## Editing and publication

Checkout/start-session returns a separate working-copy UUIDv7, an exact
committed base UUID or null and the associated target branch UUID. Editing
modifies only this private working copy.

Commit supplies `expectedHeadRevisionId` (explicit null for a first commit),
branch UUIDv7, working-copy identity, model content/delta and idempotency
key. The server validates the whole model and advances HEAD atomically
only when its current value equals the expected value. A stale expected
HEAD returns **409 Conflict**, preserving the editor's uncommitted changes.
Any retry after an ambiguous result must consult authoritative operation
status, not duplicate an uncertain commit.

Merge uses pinned source and target HEADs. Native and REST responses must
agree on revision UUIDs, parent bindings, outcome codes and conflict details.
Never accept a SHA digest in place of a revision UUID or manufacture a
committed revision for an unborn branch.
