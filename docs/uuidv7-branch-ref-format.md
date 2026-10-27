# UUIDv7 branch reference format (historical BRANCH-1 and current BRANCH-2)

**Current format: `AOWIS-BRANCH-2` only for default UUIDv7 projects,
backup, restore and verification.** The earlier `AOWIS-BRANCH-1` description
below is retained to document historical internal SHA fixtures; it is **not**
a supported backup input or migration format. A `BRANCH-2` record holds the
branch display name, canonical revision UUIDv7 and expected SHA-256 digest;
see the complete current record specification below.

**Breaking pre-release change, 2026-10-27. No legacy branch-ref compatibility.**

Branches are identified by a persistent UUIDv7, and the only published branch
files are located at `repository/refs/heads/<branch-uuidv7>`. The `<...>` part
is a **canonical, lowercase UUIDv7**, generated internally or supplied by a
verified identity-bearing replication bundle. No user-controlled branch name
or other user string is ever used as a branch-ref filename.

Each published file contains exactly:

```text
AOWIS-BRANCH-1
<validated-branch-name>
<lowercase-64-character-sha256-head>
```

The name is metadata, unique within the repository; the branch UUID is the
stable identity. User-supplied branch names remain restricted to 1–96 ASCII
alphanumeric/underscore/hyphen characters for now, though the filesystem
no longer relies on that policy for path safety. The SHA-256 is the current
revision identity until the separate UUIDv7 revision-record migration.

## Atomic operations

Creating a branch allocates a UUIDv7 and fsyncs a private staging file whose
filename contains only an internal UUID. It publishes the UUID-named record
with `link()` (no replace) and fsyncs the directory. On an existing branch,
HEAD advancement and renaming use a single fsynced replacement record
published using `rename()`. Thus its name and HEAD are never stored in
separate public files, and a rename **does not move/rename the UUID filename**.
The repository advisory lock serializes readers and mutations. An expected
HEAD is mandatory for a rename; conflicts do not mutate the branch.

A post-crash `.stage-<uuidv7>` file is unpublished and skipped by branch
enumeration. A published file must be a regular, non-symlink file with a
valid canonical UUIDv7 filename, valid record and nonzero HEAD. Duplicate
branch names, invalid refs and unrecognized path entries are errors, not
silently ignored or converted. Full verification and compaction use these
records directly; offline backup/restore snapshots their exact bytes.

## Compatibility boundary

This is an intentional development-only format break. Repositories using
`refs/heads/<user-name>` and `refs/branch-ids/<user-name>` cannot be opened.
They must be **deleted/recreated from trusted new-format data** or restored
from a new-format backup. There is **no fallback migration or automatic ID
assignment**. Old SHA-only replication bundles are rejected on import;
identity-bearing bundles preserve the existing branch UUID. New default
native exports use identity-bearing bundle headers.

Do not apply this patch to the sole copy of any repository whose data must
survive. This branch-ref change does not yet implement UUIDv7 revision IDs.

## Required pre-release stabilization work

Existing project, native replication, recovery, offline backup, bundle
limit, Merkle-integrity and current-format regression fixtures have been
converted to BRANCH-1 and identity-bearing transfers. Legacy fixtures are
retained strictly as rejection inputs, not as supported repositories. The
Qt-enabled HTTP suite and external Docker/qualification tests remain separate
acceptance gates until executed in the full developer environment.

## Phase 2c-2 native UUIDv7 revision HEAD checkpoint (not yet the service cutover)

New native-only `Repository::createBranchV2`, `headV2`, `commitV2` and
`renameBranchV2` operations publish `AOWIS-BRANCH-2` records in the **same
UUIDv7-named branch directory**. The only supported v2 branch record payload is:

```text
AOWIS-BRANCH-2
<validated-display-name>
<canonical-lowercase-revision-UUIDv7>
<64-lowercase-hex-SHA256-of-canonical-v2-revision-record>
```

The immutable v2 revision, all of its ancestors and the expected SHA-256 are
verified before a branch HEAD is read or updated. Commit writes its actual
Merkle data and canonical v2 revision to the pack, syncs the pack, publishes
the UUID→SHA resolver mapping, and **only then** atomically replaces the
UUID-named HEAD record. A crash before that last publication cannot move the
branch HEAD to an unsynced revision. Lost publication may leave an unreachable
immutable revision/mapping. V2-aware compaction now retains all published
UUID mappings and their reachable objects, including these unreferenced
records, until a deliberate UUID identity-retention policy is implemented.

A new project-local first branch has an empty v2 root revision, with a **fresh
UUIDv7**. A branch fork copies the same revision UUID and checksum; it does
not invent a revision. New raw commits allocate fresh revision UUIDv7 IDs and
use ordered UUID/SHA parent bindings. The caller may supply `expectedHead` to
reject stale commits. Renames change only the record's display name, not its
UUID-named file, HEAD UUID or expected SHA.

**Checkpoint limits:** this is a new native API, not a migration path for old
repositories. The old SHA-facing APIs are temporarily retained for the
remaining pre-release service/HTTP/replication tests; mixing SHA HEADs and v2
HEADs in one repository is explicitly rejected. A SHA-facing call on a v2
HEAD fails, rather than interpreting its checksum as a revision ID.
ProjectService's regular create/commit/history/graph, model-indexed commits,
merges, synchronization, verification, compaction, CLI, REST and GUI still
require a coordinated UUIDv7 API cutover. Do **not** use BRANCH-2 repositories
with those old interfaces. Old repositories are to be recreated, not migrated.
