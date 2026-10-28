# Model-record persistence, working copies and reference indexes

The generic model-record layer uses canonical `ModelCodec<T>` payloads and
UUID-keyed references without interpreting every hydraulic-domain field.
The authoritative commit/working-copy lifecycle is
[version-control semantics](version-control-semantics.md).

## Canonical entity records

A `ModelRecord` contains a nonzero kind, canonical payload and required
referenced entity UUIDs. A `ModelChange` addresses a record (or deletion) by
its stable UUID. `encodeModelRecord()` uses the canonical `AOMD` envelope,
including version, kind, sorted/deduplicated required references and
length-prefixed payload. The native store owns immutable object storage,
SHA-256 hashing and revision publication.

## Editing and publication

An editor changes a **private mutable working copy**, pinned to an immutable
base revision UUID or explicit null for an unborn branch. The server
prepares the candidate complete snapshot from its model changes. No write
modifies an already committed revision.

Before the user commits, validate every required reference in the resulting
snapshot, including entities created in the same batch; reject deletion of
still-referenced entities. Maintain the primary UUID-to-model index and
reverse-reference UUID-to-referrers index consistently.

The commit request must specify target branch UUIDv7, exact
`expectedHeadRevisionId` (including explicit null for first commit) and a
working-copy/idempotency identity. The repository compares the expected
HEAD under its publication lock, durably stores immutable candidate objects
and a new UUIDv7 revision record with canonical SHA-256 integrity, and
atomically publishes the new branch HEAD only if the pin is still current.
Stale commits are rejected without losing the private edits.

## Historical reads and imported data

Read APIs resolving more than one entity pin a single immutable revision.
Explicit historical reads use a committed revision UUIDv7 and never follow
moving branch HEADs. Reference-index queries return the incoming UUIDs
corresponding to the same pinned state.

Portable imports recalculate incoming references from canonical model
records and check the reverse index exactly before publication. Malformed,
missing, extra or dangling references fail verification. A full storage
verifier checks immutable Merkle closure, ancestry and reference indexes;
application-specific hydraulic semantics require the domain adapter.
See [hydraulic persistence](native-hydraulic-network.md) and
[repository verification](repository-verification.md).

## Validation boundaries

Reference existence does **not** prove a hydraulic topology is valid. All
complete-model semantic constraints must be verified before the commit or
merge becomes visible. Indexing and immutable-subtree reuse may optimize
preparation without weakening that requirement.
