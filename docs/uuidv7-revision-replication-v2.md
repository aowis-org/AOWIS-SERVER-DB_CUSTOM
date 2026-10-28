# Revision and branch replication contract

Full and incremental transfers preserve the logical objects defined by
[version-control semantics](version-control-semantics.md). A transfer is a
verifiable collection of **committed** snapshots, revision ancestry and
branch refs, not a copy of another editor's private mutable working copy.

## Transfer content

- Project UUIDv7 and required canonical format/version information.
- Branch UUIDv7, UTF-8 label, nullable committed HEAD, immutable
  `originBranchId` and exact `forkRevisionId`.
- Committed revision UUIDv7, ordered parent UUIDv7s, parent SHA-256 bindings,
  canonical immutable metadata, record SHA-256 and snapshot Merkle roots.
- Content-addressed immutable objects/subtrees needed to verify all included
  snapshots, including reference-index data and hydraulic model records.
- Explicit shared-base and completeness information for incremental bundles.

## Import correctness

Before publishing imported refs, validate canonical encoding, UUID
uniqueness, all required parent and object closure, SHA-256 integrity, branch
provenance, project ownership, and applicable domain constraints. Branch
HEAD conflicts cannot be resolved by overwriting unrelated local history.
Compare expected current HEADs and move refs atomically only after imported
objects are durable. Preserve UUIDs as-is across all transports.

An unborn branch is represented as a **branch with null HEAD**—not as a
fabricated empty revision. Private working copies are not committed history;
backing up an editor's uncommitted changes requires a different, explicitly
authorized session mechanism.
