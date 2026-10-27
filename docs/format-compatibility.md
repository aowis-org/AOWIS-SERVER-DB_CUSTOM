# Repository and bundle format policy (pre-release)

The native store is still pre-release. **Backward compatibility for the
previous experimental name-keyed branch layout and SHA-only bundles has been
explicitly dropped.** This is not a stable-format upgrade path. Recreate old
development repositories or start from a backup written by the current format;
do not attempt an automatic conversion. See the
[BRANCH-1 record contract](uuidv7-branch-ref-format.md).

## Current format

- Immutable objects, revisions, and Merkle trees remain SHA-256
  content-addressed; `objects.idx` is a rebuildable derived index.
- Every project newly created by DVMDB has a UUIDv7 identity.
- Every branch is a single atomic `AOWIS-BRANCH-1` record at
  `repository/refs/heads/<canonical-uuidv7>`; the name and SHA-256 HEAD
  are **inside** the record, not filenames. Branch renaming retains UUID and
  HEAD. Duplicate IDs/names and malformed records are rejected.
- Native, HTTP, and CLI **full** exports use `AOWSYNC3`, a 64-byte header
  containing the stable 16-byte branch UUIDv7. **Incremental** exports use
  `AOWSYNC4`, a 96-byte header. The importer rejects `AOWSYNC1/2`.
  The `identity=1` HTTP query and explicit `export-identified*` native/CLI
  forms are accepted aliases, not an opt-in to a newer format.
- Revision identities and their parent links remain SHA-256 until the
  independently specified UUIDv7 revision-v2 milestone.

## Acceptance tests and deliberately rejected fixtures

`tests/fixtures/pack-20261024/` contains an obsolete name-keyed repository
and `AOWSYNC1/2` bundles. The immutable fixture bytes are retained as
**negative tests only**. `native_format_compatibility_tests.cpp` asserts
that old repositories and old bundles are refused, and then constructs fresh
UUIDv7-only repositories to verify full/incremental export and import,
identity consistency, ancestry, restart, and revision integrity.

`native_upgrade_restore_tests.cpp` creates a new-format four-project root
including divergent histories and large model values. It exercises the real
offline backup/restore flow, missing derived index reconstruction, stable
branch UUIDv7s, historical reads, and successful post-restore writes. It
**does not** claim an older name-keyed repository can be upgraded.

The current native-only suite covers additional corruption, recovery,
compaction and sync cases. Full HTTP/Qt, physical failure and qualification
coverage must be executed separately before release.

## Future revision-ID cutover

[UUIDv7 revision v2](uuidv7-identity-format-v2.md) specifies a separate
UUIDv7 logical identity with SHA-256 immutable-record verification. Its
original migration-plan notes still discuss legacy conversion and
compatibility; **those notes are superseded for this pre-release product**.
No compatibility mapping or dual-format decoder should be implemented just
for those old development repositories. New revision-format tests must
assert the new canonical bytes, stable IDs, both merge parents, replica
consistency, corruption rejection, backup/restore and transport consistency.

Nothing in this decision weakens SHA-256 integrity checks, structured
validation, or the need to back up irreplaceable data before format changes.
The first stable release will need an explicit, published format-support
policy. See [identity roadmap](uuidv7-identity-migration-roadmap.md) and
[operations](operations.md).
