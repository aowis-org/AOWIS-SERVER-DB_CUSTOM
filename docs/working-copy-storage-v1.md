# Native private working-copy records

DVMDB maintains **uncommitted working copies separately from immutable
revisions and branch HEADs**. A working-copy UUIDv7 is not a project,
branch, revision or remote-tracking identifier. Creating a working copy does
not write an immutable revision, change a branch HEAD or publish data.

## Persistent bootstrap record

A repository configured with the functional `private` role may create a
working copy for an existing UUIDv7 project/branch. `ProjectService` exposes
`createWorkingCopyV1`, `workingCopyV1` and `listWorkingCopiesV1`. Each newly
created record has its own UUIDv7, an originating session UUIDv7, the logical
project and branch UUIDv7s, an exact base committed revision UUIDv7 or null
for unborn `main`, generation zero, `dirty=false`, `unvalidated` validation
state and an empty candidate-change collection.

The native storage root contains one immutable bootstrap record per draft:

```text
working-copies/<draft-uuidv7>
```

Its initial on-disk representation uses `AOWIS-WORKING-COPY-1` followed by
newline-terminated tab-separated fields in this fixed order:

```text
AOWIS-WORKING-COPY-1
project\t<project-uuidv7>
branch\t<branch-uuidv7>
base\t<revision-uuidv7-or-dash>
session\t<session-uuidv7>
generation\t0
dirty\t0
validation\tunvalidated
changes\t0
```

The angle-bracketed entries above are placeholders, not literal stored
values. A dash denotes an unborn committed HEAD. Other fields use lowercase
UUIDv7 strings. Records larger than 512 bytes or with unknown fields,
additional lines, malformed UUIDs or invalid marker are rejected as corrupt.
Unpublished `.creating-<uuidv7>` staging files are never advertised as drafts.

An initial record is written to an exclusive staging file, synchronized,
atomically published under its UUID with a hard link, and the containing
directory is synchronized. The catalogue lifetime lock prevents other
`ProjectService` instances from writing the same storage root concurrently.
A restart or backup/restore retains the same draft identity and base. Offline
backup verifies the complete record contents and refuses malformed drafts.

## Lifecycle boundaries

This schema establishes the durable **checkout identity**, not yet a
writable draft. Initial creation is deliberately limited to the currently
observed local branch HEAD (or an unborn null HEAD). Historical committed
checkout, materialization and verification of a complete candidate model,
leases and fencing tokens, transactional multi-entity Save/autosave, validation,
discard, and native/HTTP mutation APIs are to be implemented under VC-04
before enabling editing in the GUI. The next schema version must preserve
these UUIDs and pinned bases across every migration. No Save action should
be allowed to modify immutable revision history or a published branch HEAD.

The private/shared role is a functional repository classification; it does
not implement user permissions or authentication. Those are deferred outside
the VC roadmap.
