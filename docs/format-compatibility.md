# Repository format and compatibility contract

The native on-disk and portable transfer formats must preserve the AOWIS
[version-control lifecycle](version-control-semantics.md). UUIDv7 identifies
logical projects, branches, immutable revisions and private working copies;
SHA-256 verifies canonical committed records and Merkle snapshots.

## Storage invariants

- A project includes a UUIDv7 identity and a `main` branch UUIDv7 even
  before its first commit. An unborn branch has **null committed HEAD** and
  zero committed revisions; no empty-root revision is manufactured.
- Branch records are addressed by UUIDv7, not human-readable names, and
  bind nullable committed HEAD and immutable origin-branch/fork-revision UUIDs.
- Committed revisions have fresh UUIDv7 IDs, canonical versioned records,
  ordered parent IDs and SHA-256 integrity hashes. Immutable model records
  and Merkle subtrees remain content-addressed and deduplicable.
- Working copies are mutable, session-owned data and are never silently
  serialized as immutable committed revisions.
- Update publication is atomic and uses exact expected-HEAD checks;
  cross-process readers must not see partially published branch records.

## Export and import

Native, CLI and HTTP bundle transfer must preserve logical UUIDv7 IDs,
nullable unborn HEADs, branch names and origin UUIDs, fork point UUIDs,
ordered revision parent IDs and complete canonical hash bindings. All
transport implementations must reject missing, duplicate, noncanonical,
corrupt or cross-project identity references.

Incremental transfers name their exact immutable base UUID and integrity
hash. Import refuses divergent HEADs or asks for an explicit validated
merge; it never silently resets history or assigns new IDs to make a bundle
appear compatible. See [replication](uuidv7-revision-replication-v2.md).

## Versioning and recovery

On-disk record and bundle schemas carry explicit format versions. Any format
upgrade must have one documented atomic/recoverable transition, or fail
closed when it encounters an unsupported schema. Integrity verification,
backup, restore and compaction are required to preserve UUIDv7 identity,
Merkle closure and committed parent ancestry.

Validation includes an empty project, initial commit, shared committed
history, divergent/two-parent merge, branch rename, fork provenance,
concurrent HEAD updates and complete restore. A storage-format release is
not complete until these invariants pass on native and REST paths.
