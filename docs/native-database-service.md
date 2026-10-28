# DVMDB native project service and HTTP daemon

The DVMDB service hosts independent project repositories and exposes the
same logical contract through native C++ and REST. See
[version-control semantics](version-control-semantics.md),
[REST contract](rest-api-design.md) and
[repository architecture](architecture.md). The native core remains Qt-free;
HTTP transport does not invent different project/branch/revision identities.

## Service topology

- One database root contains a durable project catalogue; each UUIDv7
  project has its own versioned repository and per-project operation control.
- Each repository stores immutable content-addressed objects, canonical
  revision records and UUIDv7 branch references. User-visible names are
  metadata, not filesystem identifiers.
- Project creation is staged and published atomically. A new project has
  only its **unborn** `main` branch and no committed revision.
- The service distinguishes read-only history and snapshot queries from
  private working-copy editing sessions and conditional writes.
- The daemon must enforce bounded operations, safe shutdown, restart
  recovery, durable object/ref publication and explicit error classes.

## Lifecycle entry points

| Operation | Contract |
|---|---|
| Create project | Return project UUIDv7, `main` branch UUIDv7, `headRevisionId: null` |
| List projects/branches | Return UUIDv7 identities, UTF-8 names, nullable committed HEADs and immutable branch fork provenance |
| Read revision | Return immutable revision UUIDv7, ordered parent UUIDs, canonical record SHA-256 and snapshot roots |
| Full revision graph | Return every branch HEAD and reachable revision, with consistent pagination and both merge parents |
| Checkout | Return private working-copy UUIDv7, owner/session, target branch and pinned base revision/null |
| Edit working copy | Change only owned mutable draft data; do not move a committed HEAD |
| Commit | Require exact expected branch HEAD/null, complete model validation, idempotency; publish immutable revision and HEAD atomically |
| Create branch | Persist source branch and exact immutable fork revision, create no revision |
| Merge | Expected source/target HEADs, genuine one-/two-parent history, domain-safe resolution |
| Replicate | Preserve UUIDs, nullable refs, origin/fork metadata, SHA-256 and complete ancestry |

A working copy may remain stale when someone else commits to its target
branch; the server must reject its stale commit with a conflict and preserve
its uncommitted changes. HEAD is committed/immutable, not the working state.

## HTTP behavior

The `/dvmdb/v1` service exposes health/readiness, project and branch
catalogue, immutable revision lookup, full-history pagination, hydraulic
snapshot reads, working-copy lifecycle, conditional commit, merge and
identity-preserving replication. The concrete route/schema declarations in
[OpenAPI](dvmdb-http-api.yaml) must be synchronized with the operations
specified here as they are implemented. No endpoint is presumed implemented
solely because it is part of this normative contract.

For HTTP errors, distinguish bad input, missing IDs, permission refusal,
`409 Conflict` for a stale expected HEAD, semantic validation errors,
resource exhaustion, service unavailability and storage corruption. An
omitted expected HEAD must be rejected; explicit `null` is valid for an
unborn first commit. A write response lost after publication is resolved
with its idempotency key or authoritative branch status, not blindly retried.

## Hydraulic persistence

The DVMDB hydraulic adapter validates canonical model/entity encoding,
UUID references, membership and topology. Immutable revision reads pin one
committed snapshot. A full-network save or indexed edit is prepared in a
**working copy**, not by changing the committed HEAD's record. All domain
invariants must be checked on the complete candidate network before a
conditional commit or merge publication.

## Operations and deployment

Run native repository/service operations against a known, explicitly
configured storage root. Avoid starting the daemon against an accidental
empty directory after changing the working directory. Protect the root and
service logs with operating-system permissions; do not expose an unauthenticated
plain-HTTP service to untrusted networks. Repository compaction and offline
backup require coordinated access so that snapshots, refs and published
objects are mutually consistent. See [operations](operations.md),
[verification](repository-verification.md) and
[durability qualification](durability-qualification.md).
