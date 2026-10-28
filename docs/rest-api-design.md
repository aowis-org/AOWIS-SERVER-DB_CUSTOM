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
- Commit with target branch UUID, exact `expectedHeadRevisionId` (nullable
  but **required**), canonical model delta/state and an idempotency key.
- Compare/reconcile stale working copies; merge branches using explicit
  expected source and target heads.

## Errors and concurrency

A concurrent advance of the target branch returns **409 Conflict** with
current committed HEAD details. The draft stays intact; no implicit retry,
rebase or overwrite occurs. Validation failures, missing identities,
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
