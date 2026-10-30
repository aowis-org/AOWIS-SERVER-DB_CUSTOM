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
complete model from the pinned base still require integration with the editor.

## On-disk format

Each published draft has one record named `working-copies/<draft-uuidv7>`.
All new drafts use `AOWIS-WORKING-COPY-3`, including unborn drafts with no
lease. Earlier experimental formats are not supported.

The canonical fields, in strict order, use UTF-8/ASCII tab-separated records:

```text
AOWIS-WORKING-COPY-3
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
events\t<count>
event\t<transaction-uuidv7>\t<MODEL-binary-frame-as-hex>
...
last-save\t<last-batch-sha256-or-dash>
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
Offline backup/restore verifies the current record format and preserves its
exact bytes, candidate data, edit journal and lease generation.

## Private HTTP draft operations

The server exposes the native draft lifecycle through these routes (all under
`/dvmdb/v1`): `GET/POST /working-copies`, `GET /working-copies/{draftId}`,
`POST /working-copies/{draftId}/lease`, `POST /working-copies/{draftId}/save`
and `POST /working-copies/{draftId}/release`. Drafts are allowed only when the
DVMDB repository is configured with functional role `private`; this role is
not a user permission or a login.

The create request requires `projectId`, existing branch name, originating
`sessionId` UUIDv7 and explicit `baseRevision` (null for unborn). The Save
request requires session UUIDv7, current fencing UUIDv7, decimal-string
`expectedGeneration` and 1..128 changes. Generation and lease-expiration
values are represented as **decimal strings**, preserving the complete uint64
value without JSON floating-point rounding. Model changes use the same native
model-record representation as the existing model-commit endpoint. Read/list
responses include lease-holder status, **not** the active fencing token.
The list response omits candidates; `GET /working-copies/{draftId}` returns
the full saved candidate overlay. Only a successful lease acquisition returns a token.
Reopening after restart uses the same persisted draft UUID; stale generations
and stale tokens are rejected, never silently rebased. No draft operation
changes a committed HEAD or creates a revision.

## Integration boundary

The native and HTTP Save paths are durable and fenced, but do not yet implement
historical checkout, materialization/verification of the entire base model,
semantic validation of the assembled candidate, candidate discard, or the
GUI editor workflow. These are integration prerequisites before enabling
unrestricted GUI editing. A local Commit is a separate operation that has not
been accepted end to end.

## Provisioning a first-clone project for committed revisions

`POST /dvmdb/v1/projects/clone-target` accepts a source logical project
UUIDv7 (`projectId`) and display `name`. It publishes an empty catalogue
project with **that exact UUIDv7**; it creates no local branch or commit.
Repeated provisioning with the same identity and name is idempotent;
a different name for the same existing UUID is a conflict. The personal
repository's own root UUIDv7 remains independent of this logical project
UUIDv7. Following provisioning, a verified full revision bundle can create
the destination branch with its source branch UUIDv7 and immutable history.
The first-clone process does not alter source HEADs or create a draft.
Unborn branch transfer cannot use a nonempty revision bundle and requires
an explicit UUID-preserving unborn-branch operation.

## Atomic MODEL edit journal

`POST /dvmdb/v1/working-copies/{draftId}/save-events` accepts the active
writer session and fencing token, decimal-string `expectedGeneration`, one or
more base64-encoded canonical MODEL editing transactions in `events`, and the
corresponding typed `changes` array. The existing candidate and new journal
entries are written as **one fsynced working-copy update**. A successful
response acknowledges both; it does not create a revision or move HEAD.

Draft records use only `AOWIS-WORKING-COPY-3`; previous experimental V1/V2
records are intentionally unsupported. Existing development drafts must be
recreated; no pre-release migration or compatibility decoder is maintained.
The current format stores the combined journal and materialized
candidate. Each event preserves its MODEL UUIDv7 and original bytes, in user
operation order. `GET` returns both the candidate overlay and the journal;
`GET` list returns only metadata including `eventCount`. The last event-save
request is fingerprinted for an idempotent retry at its original generation.
Replaying an older batch or changing an existing event UUID is a conflict.
An unjournalled `/save` operation is rejected once a journal exists.

The native service verifies the MODEL frame size, SHA-256 and intrinsic UUIDv7;
the Qt-enabled HTTP route additionally runs MODEL's complete transaction
decoder. The native C++ caller must supply MODEL-validated event bytes and
model-record changes that correspond to those events. The membership-aware HTTP path validates canonical member record payloads
and event before/after bytes for insertion and deletion; this is **not** a
complete field-by-field reconciliation of all ordinary update events. The working-copy record remains
bounded to 8 MiB including the accumulated journal; segment/pack storage is
needed before long-running heavy edit histories are supported. This is the
first atomic acknowledgement boundary, not the complete undo/redo subsystem.

## Hydraulic membership in atomic event saves

When a MODEL transaction creates or deletes a hydraulic entity, the GUI adds
`membership` to the same `/save-events` request. Each item contains
`networkId`, `entityId` (canonical UUIDs), `kind` (native ModelRecord kind),
`collectionIndex` (0..17 in the established hydraulic collection order), and
`inserted` (boolean). The request includes the matching ordinary entity
creation/deletion in `changes` and the exact reversible MODEL before/after
image in `events`.

DVMDB checks the event image against the typed model payload or the pinned
candidate's existing entity. It computes the native membership-chunk and root
updates itself. Only affected collections are read, and only modified chunks
and necessary root metadata are placed into the resulting candidate overlay.
All generated records, original transactions and entity changes are published
in a **single generation-checked durable draft replacement**. A failed
materialization leaves the draft unchanged. The request fingerprint is derived
from canonical JSON of the original logical request, not the derived chunk
records, so the immediately preceding identical request can be acknowledged
after a lost response without reapplying its membership changes.

A request whose events create or remove any entity **without** a
`membership` array is rejected with 400 (`missing membership for created or
removed entity`) before the draft is read; only attribute updates may omit
it. Otherwise the candidate could hold an entity record that no membership
chunk lists, or a chunk that still lists a removed entity. This check needs
the MODEL decoder and therefore exists in the hydraulic-enabled HTTP build;
native C++ callers of `saveWorkingCopyTransactionsV1` must uphold it
themselves until a native membership-aware draft API exists.

This endpoint intentionally requires the owning hydraulic network to exist in
the pinned base or existing candidate. Creating a new network root, editing
network-wide metadata, and committing candidate overlays are separate tasks.
The HTTP request is JSON-framed for the existing API; canonical MODEL entity
and event payloads remain binary, encoded as base64 for transport. No ZIP
containers, compatibility reader, or new persisted format are introduced.
