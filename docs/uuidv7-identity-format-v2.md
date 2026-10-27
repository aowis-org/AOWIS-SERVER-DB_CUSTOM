> **Current state after Phase 2g-4b:** the native `ProjectService` default and
> public synchronization CLI are UUIDv7-only. The historical staging text
> below describes earlier checkpoints; `createProject()` no longer creates
> SHA-addressed revisions. SHA storage fixture methods remain temporarily in
> native test-only paths, pending removal with old pack regressions.

# UUIDv7 identity and revision-record v2 — frozen design contract

**Phase 2c-3a native project/model checkpoint, 2026-10-27.** The canonical
`T/02` codec, UUIDv7 ancestry resolver, durable pack writer and UUIDv7
`AOWIS-BRANCH-2` HEAD records are implemented. Native `Repository` supports
v2 initial revisions, forks, raw commits, **model-indexed commits with reverse
reference validation**, revision-pinned model reads, and verified HEAD updates.
`ProjectService` exposes explicit `createProjectV2`, `createBranchV2`,
`headV2`, `commitModelV2` and `getModelAtRevisionV2` methods; they use the
same project UUID as the catalogue and enforce stale-HEAD checks. Shared
model-validation logic is used for both the temporary SHA commit path and the
v2 path, avoiding contradictory reference rules.

**This is not the service cutover:** `ProjectService::createProject()` is still
the SHA-based entry point used by existing REST and CLI workflows. V2 creation
must be explicitly requested while remaining SHA-facing APIs (merge, graph,
bundle transfers, normal HTTP routes, verification and compaction) are being
replaced. A repository cannot mix SHA and UUID branch HEAD records. There is
no compatibility/migration path for obsolete pre-release repositories: test
and development stores will be recreated for the coordinated cutover.

This is the binary/identity contract for the staged implementation. The
RFC 9562 generator and new-project UUIDv7 catalogue IDs may ship separately,
without changing legacy revision storage. See the
[migration roadmap](uuidv7-identity-migration-roadmap.md).

## Decision 1 — IDs versus hashes

- Domain identities: *typed* 16-byte RFC 9562 UUIDv7 for each newly created
  project, branch and revision. Validate version `0x7` and RFC 9562 variant
  `10xx`, not just canonical UUID punctuation. Use lowercase canonical
  `8-4-4-4-12` spelling for URLs/JSON. Native bytes are the 16 wire bytes in
  network order. Never reinterpret a legacy SHA-256 as a UUID.
- Domain identities persist when mirrored/cloned as the **same logical
  project**; a new project by copy is a **different** operation, with an
  explicit new namespace and verified ancestry/import policy. No automatic
  identity rewriting on ordinary bundle import.
- Cryptographic integrity: SHA-256 over **all canonical immutable revision
  bytes**, separate from the revision UUID. The model and reverse-reference
  roots remain SHA-256 Merkle roots; equal model snapshots may deduplicate
  without making revision UUIDs equal. SHA-256 is not an object UUID.
- A new commit is a new revision ID **even if snapshot roots are unchanged**.
  Branch creation just creates a new branch identity pointing to an existing
  revision; it **never** creates a revision.
- No mutable branch names, annotations or branch HEAD refs inside a revision
  digest. The two-parent ancestry order is immutable.

## Decision 2 — initial native v2 record bytes

A new immutable canonical **revision** record uses the following binary
layout; all counts and fields have fixed widths and no host-endian encoding:

| Offset | Bytes | Interpretation |
|---|---:|---|
| 0 | 1 | Record kind byte `0x54` (`T`, reserved for v2) |
| 1 | 1 | Revision-record version `0x02` |
| 2 | 16 | Revision UUIDv7 (wire byte order) |
| 18 | 16 | Logical project/repository UUIDv7 (provenance identity) |
| 34 | 32 | Complete snapshot Merkle root, SHA-256 bytes |
| 66 | 32 | Reverse-reference Merkle root, SHA-256 bytes |
| 98 | 1 | `modelIndexed`: `0x00` or `0x01`, no other values |
| 99 | 1 | Parent count `0x00`, `0x01` or `0x02` |
| 100 + 48*i | 16 + 32 | Parent `i`: UUIDv7, followed by its expected canonical record SHA-256 |

**Canonical size = 100 + 48 × parentCount bytes.** No optional or trailing
bytes. The exact two-byte prefix is the v2 domain separator. Unknown
record types/versions, noncanonical lengths, bad UUID versions/variants,
invalid flags, duplicate parent UUIDs, missing referenced parents, parent
content-hash mismatch, cycles and cross-project ancestry are rejected. SHA
of the record includes its project UUID, own revision UUID and all ordered
parent `{UUID, SHA}` pairs. A parent can only be reached via a validated
UUID→SHA resolver; an untrusted bare UUID never bypasses pack verification.

Immutable author/message/timestamp metadata is **not included in v2**. Those
require a separately versioned canonical record if adopted later; the UUIDv7
millisecond field is generation metadata and must not be treated as an
authoritative commit audit timestamp. Mutable annotations remain separate.
The existing `S` record decoder and frozen pack fixtures remain unchanged
only until coordinated replacement of ordinary branch/commit APIs.
This layout now has a strict native codec, verified-ancestry reader and a
real Repository pack writer. `T` records can be written by
`Repository::writeRevisionV2()` with an
explicit project UUIDv7. They can now be published through native BRANCH-2
operations; regular project/REST/GUI callers have **not** switched. In
particular, whole-repository validation and compaction deliberately refuse when published
v2 bindings are present until v2-aware reachability and retention ship.

### Deterministic specification vectors

The following fixtures use synthetic Merkle roots (not actual hydraulic
snapshots); they test **canonical serialization and hashing only**.

Common fields:

```text
project UUID = 018f84c0-0000-7000-8000-000000000002
root SHA256 = 2222222222222222222222222222222222222222222222222222222222222222
references SHA256 = 3333333333333333333333333333333333333333333333333333333333333333
modelIndexed = 01
```

Root revision, zero parents:

```text
revision UUID = 018f84c0-0000-7000-8000-000000000001
record hex =
5402018f84c0000070008000000000000001
018f84c0000070008000000000000002
2222222222222222222222222222222222222222222222222222222222222222
3333333333333333333333333333333333333333333333333333333333333333
0100
SHA256 = 8b1a70f56b56a30edf970a7c7bd0e5f3b89825249e1d070378cfbc2b75c61d7c
```

Child revision, one parent with the hash above:

```text
revision UUID = 018f84c0-0000-7000-8000-000000000003
record hex =
5402018f84c0000070008000000000000003
018f84c0000070008000000000000002
2222222222222222222222222222222222222222222222222222222222222222
3333333333333333333333333333333333333333333333333333333333333333
0101
018f84c0000070008000000000000001
8b1a70f56b56a30edf970a7c7bd0e5f3b89825249e1d070378cfbc2b75c61d7c
SHA256 = fddb8eb27f2f6193b1dcefe185b47b604e96f6814620ff916ab623273689dc6a
```

Negative vectors for the decoder/verifier: wrong kind or version; truncated
record at any field boundary; extra bytes; `modelIndexed=02`; parent count
`03`; UUIDv4 or non-RFC variant in any ID field; a repeated parent; flipped
parent-order byte; SHA mismatch; unknown parent UUID; parent UUID resolves to
another SHA; cross-project parent; a synthetic cycle in resolver records.
The v2 codec and verifier tests enforce the frozen vectors, zero/one/two
parent counts, truncation/extra-byte rejection, invalid UUIDs, invalid flags,
identical-snapshot distinct identity, missing or cross-project ancestors,
SHA mismatches, shared ancestors and a bounded traversal budget. The storage-level writer for real repository packs is now available; normal
branch commit/merge publication still uses the old revision API.

## Decision 3 — on-disk identities and branch refs

- For new project catalogues the existing `AOWIS-PROJECT-1` manifest carries a
  valid new UUIDv7; the `pack` repository bytes and layout are unchanged at
  this initial infrastructure stage. Existing project UUIDv4 manifests remain
  readable **without rewriting or renaming** their folders and URLs.
- Before writing any v2 revision, the *repository itself* needs a durable
  logical project identity, also when opened by standalone `Repository(path)`
  without `ProjectService`. For a ProjectService project it must be the same
  UUID as the project catalogue. A standalone repository must create/persist
  its own UUIDv7 **once**, never generate a new value on reopen; ordinary
  replica import must preserve it. A legacy UUIDv4 catalogue cannot be used
  in v2 provenance until explicitly migrated and its alias is persisted.
- Persist a collision-checked UUID→immutable record SHA resolver, project
  scoped. Once an ID has been published, rebinding to different bytes is
  corruption/conflict even if the new bytes are otherwise valid.
- Persist independent branch UUID→name→HEAD-revision-UUID bindings with
  durable rename semantics and optimistic expected HEAD validation.
  A branch name can change, its UUID cannot. One revision UUID may be the
  target of multiple named branches; a branch reference is **not** a DAG edge.
- UUID resolver/branch ref metadata must be fsynced and published atomically
  **after** immutable pack records. Crash/restart/compaction must never expose
  a published HEAD whose UUID or digest cannot be resolved and verified.

**Phase 2b resolver index (implemented, but not yet active):**

```
<repository>/revision-v2/project       "PRJ\x02" + 16-byte project UUIDv7 (20 bytes)
<repository>/revision-v2/lock          internal flock coordination file
<repository>/revision-v2/ids/<UUIDv7>  "RID\x02" + 16-byte revision UUIDv7
                                     + 16-byte project UUIDv7
                                     + 32-byte canonical record SHA-256 (68 bytes)
```

Only UUIDv7 identities and fixed internal tokens appear in filenames. The
project manifest is immutable and pins provenance across reopening; a missing
manifest in an existing index fails closed. Publication checks that the v2
record is already in a durable SHA-addressed object store, validates all
bound parent records, fsyncs the staging record, links it with no replacement,
then fsyncs its directory. Incomplete `.stage-<UUIDv7>` entries are ignored;
rebindings, changed objects, symlinks and malformed records are rejected.
`RevisionResolverV2` is used by the real Repository pack writer and reader.
The new writing order is: validate supplied tree roots and all parent
identities under the repository lock → encode canonical `T/02` → append to
pack → fsync pack → resolver verifies all ancestry → publish UUID binding
atomically. No branch HEAD publication, compaction retention, or HTTP read
contract has yet switched. The old verifier and compactor now **fail closed**
if UUIDv7 revision bindings are present; `verifyPack()` continues checking
physical SHA integrity. This protects unpublished v2 objects from accidental
pruning but does not constitute complete logical-repository verification. The immutable pack reader supplied by the
caller must guarantee objects are synced before resolver publication; the
resolver cannot establish that durability by inspecting a callback.

The mutable branch HEAD/UUID record schema and repository-level journal are
still pending the later Phase 2 cutover. The current pre-release `AOWIS-BRANCH-1` UUIDv7-filename records still hold
SHA-256 revision HEAD values until the v2 cutover; name-keyed ref files and
SHA-only portable bundles were already removed and are **not supported**.

## Decision 4 — pre-release cutover and replication

The product is **not yet released**, and the user explicitly decided **not to
support old SHA-only revision repositories** through a compatibility reader or
live migration. The v2 transition will require recreation of development
repositories. Do not quietly accept old `S` revision records in the eventual
UUID-only storage reader, and do not build a dual-writer or transparent remap.
Retain frozen obsolete fixtures as **negative tests** only.

- A clean v2 repository must durably pin its logical project UUIDv7 before
  publishing any revision. Standalone repository instances need their own
  persistent logical identity; a ProjectService repository uses its owning
  project UUID. Replica copies of the **same** project preserve that identity.
- The UUID→SHA index must reject rebinding and verify the entire immutable
  record plus parent binding before exposing a HEAD. Physical Merkle objects
  remain SHA-addressed and can deduplicate without revision identity sharing.
- Full/incremental bundles, HEAD publication, snapshot cursors and native/HTTP
  calls must switch together to the new UUIDv7 contract. No silent import of
  SHA-only bundles or name-keyed branch files.
- Backups of development data must be taken before the breaking cutover. They
  are not in-place migration inputs, and the completed v2 implementation is
  not required to open obsolete pre-release repositories.

## Decision 5 — change order and acceptance gates

1. **Implemented in Phase 1a:** Qt-free cryptographically random and locally
   monotonic UUIDv7 generator; strongly typed `ProjectId`, `BranchId`,
   `RevisionId` wrappers; new project UUIDv7 IDs; strict parse with legacy
   project UUIDv4 catalogue compatibility. Old SHA revisions untouched.
2. **Partially implemented, Phase 1b-a:** locally created branches persist
   UUIDv7 sidecars (`refs/branch-ids/<name>`) independently of SHA-only HEAD
   files. No new revision is created; old named refs and v1 bundle-imported
   refs remain without a UUID. The typed native reader returns `nullopt` for
   those refs; it does not invent a replica-inconsistent UUID.
   **Phase 1b-b in progress:** identified native/HTTP/CLI bundle transport,
   HEAD-checked one-branch authoritative migration manifests and seeded legacy
   IDs are implemented. Multi-branch conversion, branch renaming, stronger
   provenance/authentication and the final replica-stable ref schema remain
   open. These sidecars are an intermediate compatibility mechanism, **not
   the final UUID→name→HEAD-revision-UUID v2 schema**.
3. **Phase 2:** versioned v2 serializer/decoder and verified resolver,
   repository identity, refs, revision creation, ancestry, compaction and
   crash tests; v2 vectors above become executable golden tests.
4. **Phase 3–4:** offline copy conversion, authoritative replica mappings,
   bundles, sync tools and verifier.
5. **Phase 5–6:** native+REST dual-version contracts and GUI migration to
   revision and branch UUIDs. Only afterwards resume P1 working-copy edits.

**Current runtime boundary:** Phase 2a/2b components are compiled and tested,
but live commits still use SHA-keyed `S` revisions and branch HEADs. The
pre-release UUIDv7 branch storage and identified AOWSYNC3/4 transfer formats
remain as implemented. The eventual UUID-only revision cutover deliberately
will not support opening old SHA-only development repositories.

## Phase 2e-2a native full revision transport

DVMDB now has a distinct `AOWREV21` full-revision bundle protocol that
preserves project/branch/revision UUIDv7 identities and transfers the complete
immutable two-parent ancestry. The import verifies both revision identity and
SHA-256/Merkle content **before** publishing any branch ref. It is not a
replacement for the existing SHA-facing AOWSYNC3/4 APIs yet, and has no
legacy reader. See [v2 replication contract](uuidv7-revision-replication-v2.md).
