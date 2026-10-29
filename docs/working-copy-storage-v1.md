# Durable private working copies

A working copy is a private, uncommitted candidate tied to an exact logical
project UUIDv7, branch UUIDv7 and base revision UUIDv7 (or null for an unborn
branch). Creating or saving it **never moves committed HEADs or creates a
revision**. Each draft has its own UUIDv7 and originating session UUIDv7.

Only repositories explicitly configured with functional role `private` may
create or save drafts. This role is deployment metadata, not authentication
or permissions.

## Native lifecycle

`ProjectService` provides `createWorkingCopyV1`, `workingCopyV1`,
`listWorkingCopiesV1`, `acquireWorkingCopyLeaseV1`,
`saveWorkingCopyChangesV1` and `releaseWorkingCopyLeaseV1`.

An editor must explicitly acquire an exclusive writer lease, providing its
session UUIDv7 and a duration of 1–120 seconds. The result contains a fresh
fencing token UUIDv7, expiration as Unix epoch milliseconds, and the current
draft generation. Only the active session, with the exact token and expected
generation, may save a batch. Reacquiring a lease rotates the token. Another
session may take over **only after expiry or explicit release**. Expiration
never deletes an unsaved or saved working copy. A stale token, expired lease
or old generation is rejected with Conflict before any candidate is modified.

`saveWorkingCopyChangesV1` accepts a batch of typed `ModelChange` objects:
entity UUID key, optional `ModelRecord` for create/update, or null for delete.
The batch must not contain duplicate entity keys. A Save merges the entire
batch into the accumulated candidate overlay, stores the new generation and
dirty flag durably, and acknowledges only after publishing the complete new
record and syncing its directory. Reading the draft yields the full overlay.
An empty batch still increments the generation but leaves the overlay intact.
The validation state remains `unvalidated`: domain validation and loading a
complete model from the pinned base are separate VC-04 work.

## On-disk format

Each published draft has one record named `working-copies/<draft-uuidv7>`.
The initial creation uses `AOWIS-WORKING-COPY-1`; the first lease acquisition
upgrades the draft to `AOWIS-WORKING-COPY-2`. Both remain readable.

Version 2 fields, in strict order, use UTF-8/ASCII tab-separated records:

```text
AOWIS-WORKING-COPY-2
project\t<project-uuidv7>
branch\t<branch-uuidv7>
base\t<revision-uuidv7-or-dash>
session\t<originating-session-uuidv7>
generation\t<decimal-uint64>
dirty\t<0-or-1>
validation\tunvalidated
lease-session\t<session-uuidv7-or-dash>
lease-token\t<token-uuidv7-or-dash>
lease-expires\t<decimal-unix-epoch-ms-or-zero>
changes\t<count>
change\t<16-byte-key-as-32-lowercase-hex>\t<model-envelope-hex-or-dash>
...
```

The change records are strictly sorted by entity UUID key. The model envelope
uses the existing native typed-record encoding (`AOMD` version 1), not a
new codec. Draft records are bounded to 8 MiB and 20,000 changed keys.
Deleted values remain explicit in the overlay. The original pinned base is
immutable, independent of later branch HEAD movement.

Updates are written to a fresh exclusive `.updating-<uuidv7>` staging file,
fsynced, renamed atomically over the previous record, then the working-copy
directory is fsynced. On crash, readers see either the old complete record
or the new complete record. Any unpublished `.creating-` or `.updating-`
staging file is ignored but checked for safe type/name. If publication is
uncertain, the service refuses further writes until restart and inspection.
Offline backup/restore verifies both record versions and preserves their
exact bytes, candidate data and lease generation.

## Integration boundary

The native Save path is durable and fenced, but it does not yet implement
historical checkout, materialization/verification of the entire base model,
semantic validation of the assembled candidate, candidate discard, or the
private HTTP/UI editor workflow. These are VC-04 tasks before enabling GUI
editing. A local Commit is a separate VC-05 operation.
