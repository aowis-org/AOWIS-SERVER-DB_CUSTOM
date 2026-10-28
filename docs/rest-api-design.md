# DVMDB REST API design contract

The HTTP service is a transport for the same project/history semantics as
the embedded native API. See
[version-control semantics](version-control-semantics.md),
[branch records](uuidv7-branch-ref-format.md) and
[revision integrity](uuidv7-identity-format-v2.md).

## Identity and representation

Project, branch, committed revision and private working-copy resource IDs
are UUIDv7. Human names are UTF-8 metadata; hashes are SHA-256 integrity
fields. REST must not use branch names as permanent IDs or hashes as
revision IDs. Branch responses include `headRevisionId` (nullable),
`originBranchId` and `forkRevisionId`. Commit responses include immutable
revision UUID, parent UUIDs, record hash and committed snapshot identity.

## Personal repository versus shared source

The same REST transport supports different *repository roles*: private
working DVMDB (durable drafts and **local** commits) and shared project
DVMDB (published branch refs). This is an explicit connection/authorization
choice, not a silently inferred role. See
[private working repositories](private-working-repositories.md).

## Operations

- List/create projects. New project creates an unborn `main` branch and
  zero committed revisions.
- List/get branches, creation provenance and committed HEADs, including
  explicit null HEADs.
- Create branch from a finalized revision with exact source branch/fork
  UUIDs. Creation itself must not create a revision.
- Read immutable revisions, snapshots, diffs and a pinned paginated full
  project history including both parents of merge revisions.
- Create/open a private working copy for a user/session, pinned to a
  committed base UUID or explicit null for the unborn initial state.
- Edit/validate that private working state without changing a committed HEAD.
- Commit **locally** in a personal DVMDB using required exact expected local
  HEAD/null, validated model state and an idempotency key.
- Fetch immutable objects and source remote-tracking state; Publish/Push
  with required exact expected shared HEAD/null, fast-forward validation,
  idempotency and durable remote publication.
- Compare/reconcile stale working copies; merge branches using explicit
  expected source and target heads.

## Errors and concurrency

A concurrent local HEAD advance or a stale shared Publish returns **409
Conflict** with the current affected HEAD. Draft state and already-successful
local commits remain intact; no implicit retry, rebase or overwrite occurs. Validation failures, missing identities,
permission failures, unavailable source state and ambiguous transport
failures have distinct machine-readable outcomes. An omitted expected
HEAD field is an invalid request, not a synonym for a first commit with
expected null. A timed-out commit must be safely queryable by operation ID
or idempotency key before retry.

The server validates canonical UUIDs, hashes, complete model references,
all ancestry edges and branch fork metadata before durable ref publication.
Native and REST publish equivalent outcomes and verification fields.

## Replication and security

Full/incremental transfers preserve UUIDv7 identity, nullable HEADs,
immutable branch provenance, parent bindings and SHA-256 hashes. Private
uncommitted working copies are not automatically exported as history.
Authentication/authorization must restrict draft access by owner/session
and protect commit/branch mutations; transport security is separate from
revision integrity. Neither UUIDv7 nor SHA-256 is a permission mechanism.

The machine-readable OpenAPI file must be kept synchronized with this
contract when implementing the corresponding endpoints; never claim an
unimplemented route exists solely because it is specified here.
