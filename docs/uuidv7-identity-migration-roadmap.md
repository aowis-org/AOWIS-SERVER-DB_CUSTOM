> **Phase 2g-4b — native service and CLI canonical UUIDv7 cutover (2026-10-27).**
> `ProjectService::createProject()` now creates UUIDv7 initial revisions;
> unsuffixed project/branch/head/history/revision/graph/merge/model methods
> use typed UUIDv7 `RevisionId`, while SHA-based native test fixtures were
> isolated behind explicit `*ShaFixture` names. The public sync CLI now
> requires `PROJECT_UUID` for every command and exports/imports exclusively
> via `AOWREV21`/`AOWREV22`. Old SHA transfer and manifest commands are gone.
> This is a breaking pre-release API cutover, not a storage migration.
> **Still outstanding:** removal of native SHA fixture/storage primitives,
> Qt-enabled HTTP verification, and GUI integration. Earlier checkpoints
> below document historical stages, not the current default.

> **Phase 2g-4a — HTTP canonical UUIDv7 cutover (2026-10-27):**
> Project creation through `POST /projects` defaults unconditionally to UUIDv7
> revisions; no revision-format flag is accepted. Canonical branch, history,
> revision, graph, generic model-commit, generic merge, diff, hydraulic
> read/edit/merge, and bundle paths now use the existing UUIDv7 implementations.
> SHA-based HTTP route registrations and `-v2` public URL aliases are removed.
> AOWREV21/22 are the only HTTP transfer formats. SHA-256 remains a content
> integrity digest, not a revision identifier. This cutover is intentionally
> **HTTP-only**: native `ProjectService::createProject()` and older native/CLI
> consumers still need migration, followed by GUI integration. Existing
> SHA-based native test fixtures remain valid as tests of internal storage,
> but are no longer public HTTP API compatibility tests.
>
> **Phase 2g-2 update (2026-10-27):** Separate UUIDv7 REST routes now
> support model-indexed commits with mandatory expected HEAD, conservative
> three-way merges with both expected HEADs, and bounded immutable V2 revision
> diffs (`model-commits-v2`, `merges-v2`, `diff-v2`). The generic model-commit
> route transports opaque model bytes as Base64; hydraulic callers must supply
> ModelCodec-encoded payloads. It checks model references but is not yet a
> hydraulic field-level editing/validation API. The existing SHA-based
> hydraulic read/edit routes are not converted yet; the default V2 cutover
> remains unfinished. Native `diffRevisionsV2` resolves project-bound UUIDs
> and compares verified Merkle roots without using SHA hashes as revision IDs.

> **Phase 2e-2b update (2026-10-27):** Native UUIDv7 full and incremental
> bundle transfer is implemented (`AOWREV21`/`AOWREV22`). The exporter uses
> disk-backed visited/sort state; the importer reads from a disk-staged bundle
> using file-backed indexes and verifies omitted ancestry against the exact
> pinned recipient base. The 64 MiB whole-bundle limit is replaced by a
> bounded 64 GiB hard ceiling, with caller-configurable lower limits.
> **This is not an HTTP/CLI cutover** and network resumption/cancellation is
> still planned. See [v2 revision replication](uuidv7-revision-replication-v2.md).
>
> **Phase 2c-1 update (2026-10-27):** `Repository` now stores genuine
> canonical UUIDv7 `T/02` revisions in the existing SHA-addressed content
> pack and publishes durable project-bound UUID→SHA mappings only after
> pack fsync. Both merge parents and full ancestry are verified on read.
> This is a storage-stage interface; branch HEADs, ordinary commits/merges,
> bundle and HTTP/GUI identities have **not** switched yet. The old compactor
> and whole-repository verifier refuse v2-bound repositories until they can
> account for v2 reachability. There is no pre-release legacy migration.
>
> **Pre-release branch-ref breaking change underway:** `docs/uuidv7-branch-ref-format.md` supersedes all name-keyed branch-ref/sidecar and legacy migration plans below. Old repositories and bundles are not supported. The following material includes historical planning notes pending cleanup.

# UUIDv7 object identities and SHA-256 integrity — migration roadmap

**Migration roadmap, 2026-10-27. Phase 0 and Phase 1a implemented; Phase 1b-a locally persisted branch IDs implemented. Full revision/branch migration is NOT implemented.** This is the
next architectural milestone, to be completed **before P1 working-copy/checkout
and save work**. It does not replace the established [version-control
semantics](version-control-semantics.md): projects own repositories; immutable
revisions form a parent DAG; branches are named references to revisions; every
branch has its own HEAD. Creating a branch **does not** create a revision.

**Agreed product rule:** every new first-class AOWIS domain object (including
project, branch and revision) has a unique UUIDv7 **identity**. SHA-256 remains
the separate, verified **integrity/content address** for immutable records and
Merkle snapshot objects. UUIDv7 provides extremely strong practical uniqueness;
the store must still reject duplicate IDs and mismatched immutable content.

**Important boundary:** content-addressed blobs, Merkle tree nodes, indexes and
physical pack records are storage objects, **not** first-class domain objects.
Keep their SHA-256 content addressing. Do not allocate random UUIDs to every
identical chunk, or disable subtree deduplication. Existing hydraulic model
entity UUIDs and internally derived membership-chunk UUIDs need an explicit
platform-wide audit (section 7); they are not silently rewritten as part of
revision migration.

## 1. Why the change is needed; current verified baseline

The current native revision payload is `S | root(32) |
referencesRoot(32) | indexedFlag(1) | parentCount(1) | parentSHA256(32)*`, with
at most two parents. `Repository::readRevision()` derives `Revision::id` from
the SHA-256 of these bytes; the revision's Merkle root cryptographically
commits to its entire indexed snapshot. That is sound content integrity, but
the record does **not** include a distinct logical revision identity. Every
new empty repository can therefore share the same root revision hash
(`1615096adbe2…`); this is identical **content**, not an accidental SHA
collision. Additional branch names pointing at that one revision are expected
until they commit divergent histories.

Other implemented identity dependencies:

| Current place | Current representation | Impact |
|---|---|---|
| `ProjectService::randomId()` | 16-byte UUIDv4 project IDs (version bits `0100`) | Change generation; plan migration of durable legacy IDs and URLs. |
| `native_store.h::Revision` | `Hash id` and `vector<Hash> parents` | Needs distinct revision UUID, integrity hash and typed parents. |
| `Repository::readRevision`, commit/merge | SHA-keyed immutable `S` records | New **versioned** canonical record; preserve old decoder. |
| `refs/heads/<name>` | Name/path with 64-hex-character SHA-256 HEAD | Branch UUID + HEAD revision UUID, with verifiable SHA binding. |
| `Repository::head`, `revision`, `history`, `revisionGraph`, `revisionOnBranch`, diffs, historical reads, expected-HEAD commits | SHA IDs and SHA parent edges | Typed native UUID API and verified resolution; staged deprecation of legacy hash API. |
| `ProjectService` and catalogue | UUIDv4 project identity, branch names | Stable UUIDv7 project and branch identities; alias compatibility. |
| REST `/projects`, `/branches`, `/revisions/{hash}`, `/revision-graph`, mutations | Project UUID, branch name, 64-hex revision IDs, hash cursors | Explicit wire version transition; UUID revision IDs and separate hashes. |
| `.aowbundle`, incremental bases, import validation, sync CLI | Hash-based ancestry, HEADs, canonical content objects | Upgrade transport and validation while keeping physical content hashes. |
| Verifier, compactor, backups, old fixtures | Refs and reachable DAG keyed by SHA | Dual-format verification, migration/recovery and frozen-fixture gates. |
| AOWIS-SERVER-GUI | Project/revision graph keys, branch name + HEAD, inspector selection | UI model must use project UUID + revision UUID; hash is descriptive metadata. |

DVMDB already implements the **P0 full-DAG API** in native `Repository`,
`ProjectService` and HTTP. That read API must be upgraded with the identity
model, not discarded. The Model library provides `createUuidV7()` with a Qt
6.9 native and earlier-Qt fallback implementation; native-only DVMDB also
builds without Qt, so the new **native generator/validator must not depend on
Qt**. We need a common byte-level RFC 9562 UUIDv7 contract, cross-checked
against the Model implementation.

**Not a quick replacement:** changing the `Revision::id` type without a
migration would invalidate existing parent links, reference files, bundles,
HTTP clients and frozen fixtures. Changing a revision's canonical bytes also
changes its SHA-256, and therefore changes hashes of its descendant records
if the parent integrity hashes are part of those records.

## 2. Target identity and integrity model

### 2.1 Proposed logical schema

| Field | Semantics |
|---|---|
| `ProjectId` | Stable UUIDv7; project-scoped history container. |
| `BranchId` | Stable UUIDv7; independent of its user-editable name. |
| `RevisionId` | Fresh UUIDv7 **for every newly created revision**, including initial empty revisions and commits whose model snapshot is unchanged. |
| `Revision.contentHash` | SHA-256 digest of **all canonical immutable revision-record bytes**; must include the revision ID and project provenance and commit ancestry. Not a UUID and not an arbitrary generated ID. |
| `Revision.snapshotRoot` | Existing SHA-256 Merkle tree root for the complete model state. Identical snapshots may share this hash without sharing revision UUIDs. |
| `Revision.referencesRoot` | Existing SHA-256 reverse-reference Merkle root where applicable. |
| `Revision.parents` | Zero/one/two ordered **revision UUID references**; optionally carry the expected parent content hash in the canonical record to bind ancestry cryptographically. |
| `Branch.headRevisionId` | One revision UUID (with verified content-hash resolution); several branch IDs may share it. |
| `createdAt`, author, message | Optional **immutable** commit fields in a versioned schema; do not silently use wall-clock timestamp as the identity or a mutable field in the digest. |

The canonical revision hash must commit to the **whole immutable revision
record**, including version, UUID, project/provenance context, indexed flag,
Merkle roots, ordered parent IDs **and their expected hashes** (or an equally
strong, explicitly verified authenticated UUID→hash binding), and whichever
immutable metadata is defined for that format. Snapshot integrity remains
transitive through Merkle roots: **do not flatten/hash the entire hydraulic
database on each commit**. Mutable notes, display names, review flags and
mutable branch HEADs are outside the immutable revision-record digest.

A prospective vNext serialization, for design discussion only:

```text
RevisionRecordVNext {
  formatVersion
  revisionUuidV7
  projectUuidV7 / stable origin-provenance ID
  snapshotRootSha256
  referencesRootSha256
  modelIndexed
  parents[] = { parentRevisionUuid, expectedParentRecordSha256 }
  optional canonical immutable commit metadata
}
revisionContentHash = SHA256(canonicalEncode(RevisionRecordVNext))
```

**Do not implement this wire layout verbatim until Phase 0 approves the binary
schema**, byte ordering, length bounds and domain separation. It is an
architecture sketch, not an existing file format.

Both a durable UUID→content-hash resolver and a hash→verified-bytes pack index
may be needed. Enforce: *one UUID maps to exactly one immutable record; same
UUID with different record/hash = conflict/corruption, not overwrite*.
Duplicate record content at **different** UUIDs is permitted, but their
revision record hashes differ because the UUID is committed in the record.
The existing packed value/Merkle objects continue to deduplicate by SHA.

### 2.2 Example: empty projects and shared branch tips

```text
Project A (uuidv7-A)               Project B (uuidv7-B)
    |                                   |
  R0-A (revision-uuidv7-A)            R0-B (revision-uuidv7-B)
    |                                   |
    +-- main (branch-uuidv7-A1)         +-- main (branch-uuidv7-B1)
    +-- feature (branch-uuidv7-A2)

R0-A.snapshotRoot == R0-B.snapshotRoot         (same empty content)
R0-A.revisionUuid != R0-B.revisionUuid         (different logical revisions)
R0-A.contentHash != R0-B.contentHash           (UUID/project in record)
main(A).HEAD == feature(A).HEAD == R0-A        (two refs, one revision)
```

**Never create a new revision merely to give a new branch a unique ID.** The
branch gets its own UUIDv7 and initially references the source's existing
revision UUID. This also applies to non-leaf HEADs, forks and merges.

### 2.3 Identity scope for replication and copies — must be explicit

Copying/mirroring the **same logical project** to another server must preserve
its project, branch and revision UUIDs, not regenerate them. Otherwise sync
and ancestry comparisons lose their common identity. Creating a **new logical
project by copying** is a distinct operation whose history/provenance and
namespace behavior need a documented, testable choice. Never remap UUIDs
implicitly on ordinary bundle import. Decide whether cross-project imports
remain possible only through an explicit *fork/import-as-new-project* workflow,
and how their original ancestor IDs/provenance are represented. The current
bundle behavior alone must not be treated as a solved project-identity policy.

## 3. Migration invariants and safety requirements

1. All existing project data, immutable snapshots, branch names, parent
   ordering, first/second merge parents and reachable history survive intact.
   Migration must **not** manufacture changes or branch divergence.
2. Every migrated revision has one stable UUID assigned **once** and durably
   recorded; importing the same migrated bundle on different machines must
   recover the **same** revision IDs. Independently generating random UUIDv7s
   at each replica is prohibited.
3. All new projects/branches/revisions use UUIDv7 and are validated for
   variant/version and collisions. Legacy UUIDv4 identifiers are recognized
   only in explicitly supported compatibility/migration contexts.
4. Preserve immutable SHA-256 integrity through all storage, bundle, fetch,
   import, restore, verification and compaction paths. Validate canonical
   revision bytes, roots, parent IDs+hash bindings, missing targets, cycles,
   duplicate IDs and cross-project identity conflicts.
5. A branch has exactly one HEAD UUID at a time; HEAD never rewinds through
   normal public APIs. Committing creates a new UUID/revision and advances
   only the target branch. P0 graph pagination pins the branch ID/name/HEAD
   snapshot; concurrent ref movement never alters a continuation page.
6. No silent dual meaning of a 64-character SHA string versus UUID; typed
   native APIs and versioned/unambiguous HTTP fields distinguish them. Stable
   legacy aliases, if supported, have explicitly bounded lifetime.
7. Crash-safe publication: durable vNext objects and UUID index are synced
   **before** switching refs/catalogue pointers; aborted migration never
   leaves a published reference to missing data. Retain verified backups and
   a well-defined rollback boundary.
8. Migration must preserve service identity across restart and replicas;
   readable old fixtures and idempotent reruns are non-negotiable gates.
9. All SQL introduced in the future must work **both with SQLite and
   PostgreSQL**, including concurrency, indexes and migrations. The current
   native pack implementation is not SQL-backed.

## 4. Implementation phases (deliver in independently testable patches)

**Implementation status:** the concrete [v2 identity/record contract](uuidv7-identity-format-v2.md)
fixes UUID wire representation, revision bytes and hashes, project-scoped
provenance, parent SHA binding and replica conversion policy. Its resolver
and branch-ref publication details remain implementation gates for Phase 2.
The first code slice (Phase 1a) adds RFC 9562 UUIDv7 infrastructure and
new-project generation while retaining SHA-based revision storage and
reading old project UUIDv4. Phase 1b-a persists UUIDv7 identity sidecars
for **new locally created** branches. Phase 1b-b-1 adds **opt-in identified
native full and incremental bundles** (`AOWSYNC3`/`AOWSYNC4`), imports the
preserved branch identity, and provides explicit HEAD-pinned authoritative
identity adoption for legacy refs. Old `AOWSYNC1`/`AOWSYNC2` exports remain the
default for existing native/REST callers; v1 imports remain ID-less. This is
intentionally **not full Phase 1b completion**: the HTTP transport must opt in,
legacy authoritative migration tooling/manifest must be implemented, and
rename plus final transactional ref mapping remain deferred to the v2 cutover.
No importer invents UUIDs for v1 bundles.


### Phase 0 — contract and format decision (design gate)

- Inventory SHA-as-ID uses in native storage, project catalogue, graph API,
  REST, CLI, model/hydraulic adapters, bundles, compaction, verifier and GUI;
  classify **identity** vs **integrity hash**. Keep an owner/checklist for each.
- Decide canonical vNext revision serialization: tagged/versioned record,
  UUID binary byte order, project provenance, parent UUID+hash commitment,
  domain separation, immutable commit metadata, field bounds and max parents.
  Write byte-accurate examples and negative-test vectors **before code**.
- Decide persistence for branch UUID/name/HEAD, project UUID catalogue, the
  durable UUID→content-hash resolver, duplicate/conflict policy, old SHA→new
  UUID aliases and whether such aliases need project scoping. The low-level
  `Repository(path)` can be used **without** `ProjectService`; define the
  persistent repository/project identity that must be supplied or created in
  this native-only mode so record provenance is never fabricated by callers.
- Decide upgrade support: legacy read-only vs dual-read, old bundle import,
  old API sunset, and whether fresh writes to old repositories are permitted.
  Design both offline conversion and clean creation paths; identify exact
  metadata needed to convert independently cloned legacy histories without
  divergent UUID assignments.
- Decide copy/fork/replication identity rules from section 2.3. Freeze the
  choices in this document and [format compatibility](format-compatibility.md).

**Design outcome:** the [v2 identity format contract](uuidv7-identity-format-v2.md)
records decisions, deterministic record examples, rejection cases and offline
migration/rollback policy. Exact resolver/ref journal bytes and versioned
API wire schema remain later gates and must be frozen before those respective
features ship. The independent Phase 1a project-ID infrastructure is safe
without any revision-pack format changes.

### Phase 1 — UUIDv7 infrastructure and typed native identifiers (1a, 1b-a and 1b-b-1 implemented)

- Implement a Qt-independent, cryptographically secure RFC 9562 UUIDv7
  generator/validator with collision rejection; share/fuzz cross-tests with
  `createUuidV7()` in AOWIS-SERVER-MODEL (Qt versions/fallback).
- Add strong `ProjectId`, `BranchId`, `RevisionId` types (or equally strict
  wrappers) separate from `Hash`, plus deterministic binary/hex parsers and
  canonical string formatting. Keep hash APIs distinguishable.
- Change **new** project creation from UUIDv4 to UUIDv7. Introduce durable
  branch UUIDs while preserving user-facing names; establish migration
  aliases for existing project UUIDv4/branch names as decided in Phase 0.
- Unit-test clock rollback, rapid same-millisecond generation, concurrent
  processes, malformed versions/variants and duplicate rejection.

**Phase 1b-a implemented:** new local `Repository::createBranch()` and
`createBranchAtRevision()` operations reserve durable UUIDv7 sidecars under
`refs/branch-ids/<name>`, and branch HEAD updates leave those IDs unchanged.
The native `Repository::branchId()` and `ProjectService::branchId()` readers
return an **optional typed BranchId**; `nullopt` on a valid existing branch
means *legacy/unconverted*, never synthesize an ID on read. The existing
`refs/heads` SHA text files and existing bundle header formats are unchanged.
An abandoned reservation is not advertised as a branch; retry of the same
name safely retains that reservation. Corrupt/duplicate sidecars are rejected
before creating another locally named branch.

**Phase 1b-b-1 implemented (native, opt-in):**
`exportIdentifiedBranchBundle` and
`exportIdentifiedIncrementalBranchBundle` add 16 UUIDv7 wire bytes to the
old bundle header, with **distinct** `AOWSYNC3` (full) and `AOWSYNC4`
(incremental) magic. Their branch identity comes only from the durable source
sidecar; a legacy ID-less branch cannot use these exporters. All existing
`exportBranchBundle`/`exportIncrementalBranchBundle` overloads still emit
`AOWSYNC1`/`AOWSYNC2`. The importer handles all four versions, checks the
actual UUIDv7 format, rejects different local identities and duplicate IDs
under multiple local names, and durably adopts the incoming UUID when a
locally valid legacy branch is otherwise compatible. An up-to-date import may
adopt the authoritative ID without moving HEAD. Objects remain SHA-256
verified; this feature **does not** convert SHA revision IDs.

An authorized operator or migration tool may also adopt an **externally
supplied** branch UUID via `Repository::adoptBranchId(name, identity,
expectedHead)` (or the ProjectService wrapper). It refuses stale-head, invalid,
duplicate and rebinding attempts; it never generates UUIDs. To migrate legacy
replicas, first assign and durably record one UUID at the chosen authoritative
source, distribute it and the HEAD pin to its replicas, and apply it only to
compatible local branches. Follow up with an identified bundle to verify
end-to-end identity. **Do not independently generate UUIDs at replicas**.
The typed method is a primitive, not an automated authoritative mapping or
full offline conversion tool.

**Phase 1b-b-2 implemented (HTTP/CLI opt-in and one-branch manifests):**
`GET .../bundle?identity=1` selects AOWSYNC3/4; omitted `identity` continues
exporting AOWSYNC1/2. POST auto-detects all four. Offline CLI supports
`export-identified`, `export-identified-incremental`, `branch-id`,
`seed-branch-id`, `manifest-export` and `manifest-apply`. The text manifest
`AOWIS-BRANCH-IDENTITY-1` contains one source name, authoritative UUIDv7 and
pinned SHA HEAD. It is published exclusively and durably, limited to a single
branch per operation, validated strictly, and applied through the existing
expected-HEAD/collision-checked `adoptBranchId`. Existing identities may be
reapplied idempotently; a stale HEAD or a conflicting identity is refused.
**Seed only on the chosen authoritative source**; distributing the manifest
alone does not establish who is authorized to speak for a project. Current
HTTP has NO transport authentication or identity provenance and must only be
used in controlled networks. A multi-branch atomic manifest is NOT available.

**Phase 1b-b-3 implemented (integrity and backup gate):** full repository
verification checks UUIDv7 sidecar names, file types, values and uniqueness;
verified offline backup/restore rejects damaged/duplicate identity metadata
and tests exact UUID preservation across copies, restores and compaction.
Legacy HEAD refs without IDs and durable reservations without published HEADs
remain valid. No branch ID is silently generated or reassigned.

**Phase 1b-b remains open:** multi-branch/fleet transaction planning,
crash-safe rename semantics and eventual atomic ID/name/HEAD ref format. The current simple sidecar is transitional:
there is no crash-atomic multi-file rename transaction. Do not implement rename
by independently renaming HEAD and ID files until a transactional solution is
designed. Backup verification protects the existing state but cannot make a
multi-file rename atomic. Include project provenance and signed/authenticated transport policy
in the v2 contract; an unsigned UUID in a current bundle establishes a claim,
not remote authority. Existing v1 bundles have no ID and cannot silently
create a supposedly replica-stable UUID.

**Exit (full Phase 1):** new logical IDs are UUIDv7, native-only builds stay
Qt-free, and existing old repositories still open unchanged through the
supported reader. This exit is **not yet met** while legacy and bundle-imported
refs have no authoritative identity.

### Phase 2 — versioned revision records and UUID-indexed ancestry

**Implementation checkpoints (2026-10-27):** Phase 2a's canonical v2 codec
and DAG verifier and Phase 2b's standalone, project-bound UUID→SHA disk
resolver are complete with native tests. Phase 2b does **not** yet publish
branch HEADs or insert revision objects into the live pack. The next step is
atomically connecting object-pack writes, resolver publication, repository
identity and UUID-valued branch HEADs. No old-format compatibility path is
planned for the development repository cutover.

- Implement new immutable revision record reader/writer, keeping old `S`
  decoding as decided; separate revision UUID from content hash and Merkle
  roots throughout commit, merge, raw/model edits and HEAD publication.
- Implement durable, verified UUID→SHA mapping and branch ID→HEAD revision
  mapping. Persist the repository's own stable logical identity for direct
  native-only usage as well as projects opened by `ProjectService`; ensure it
  is identical across exported replicas. Preserve SHA-addressed pack/value/
  trie deduplication.
- Convert all native queries, historical reads, diffs, branch reachability,
  optimistic `expectedHead`, merge-base resolution, both parent edges and
  compaction/verification traversal to UUID identity with hash validation.
- Preserve the initial **empty** revision for each new project but allocate
  it its **own** UUIDv7; multiple named branches may still share that ID.
- Enforce publication order and detect UUID collisions/conflicting imports;
  refuse corrupt/missing/mismatched parent bindings.

**Exit:** linear history, forks, two-parent merges, shared HEAD, non-leaf
HEAD, identical-content commits, reopen and crash recovery pass using UUIDs.
No public REST/bundle schema switches until native reliability is proven.

### Phase 3 — upgrade existing repositories and replicas safely

- Build a **read-only planning/dry-run tool** reporting legacy projects,
  branches, reachable revisions, merge DAG size, estimated disk/scratch cost,
  namespace conflicts and whether replica mappings are already available.
- Convert on a disposable **copy**, preserving original `.aowbundle` and
  pack/refs and recording a durable per-project map:
  `old SHA revision → new revision UUID + new content hash`, and
  `old branch name → branch UUID`. For legacy UUIDv4 projects, specify a
  stable `oldProjectUuid → newProjectUuid` alias if product policy requires
  migrating the project identity; never strand external references silently.
- Migrate ancestry in topological order so both parents resolve; recompute
  new record hashes, verify roots, branch HEADs, every reachable snapshot and
  imported refs. The UUID map must be **exportable/preserved across replicas**
  or assigned once at an authoritative origin before distributing converted
  bundles. Standalone cloned legacy histories cannot independently randomize
  a compatible UUID mapping.
- Make conversion idempotent and restartable via a journal/manifest, with
  checksummed completion marker and atomic cutover. On any failure keep the
  source repository untouched and recover from the verified backup. Document
  how to handle partial/different-version upgrades and when rollback to old
  binaries is no longer possible.
- Test frozen `tests/fixtures/pack-20261024`, historical bundles, multiple
  projects, branched/merged/deep histories, mixed versions and two replicas;
  never rewrite/re-record the original test fixtures.

**Exit:** lossless conversion, deterministic replica identity, idempotent
rerun and crash/rollback tests pass; operator migration guide and legacy
compatibility matrix exist.

### Phase 4 — portable bundles, sync tools and verifier

- Version the bundle manifest/record types, preserve both UUID and content
  hash, parent binding and immutable record integrity; maintain previously
  agreed old-import behavior without silently changing old hashes.
- Update full/incremental export base IDs, import validation, ref publication,
  clone/mirror identity and conflict handling. Verify that identical
  UUID+digest reimports are idempotent, while equal UUIDs with different
  hashes are rejected; ensure a separate project with equal snapshot content
  remains distinct.
- Update CLI, compaction, `aowis-dvmdb-verify`, backup/restore and cold
  upgrade tests to traverse UUID-based history and validate SHA all the way
  to Merkle leaves. Re-measure scratch, memory, transfer and traversal costs.

**Exit:** native/CLI sync between two installations preserves exactly the
same IDs/HEADs/history and identical snapshot integrity; historical bundles
still pass documented compatibility tests.

### Phase 5 — ProjectService, REST and P0 graph contract

- Expose explicit project/branch/revision UUIDs and separate SHA digests in
  native `ProjectService` and the HTTP schemas. Update `GET /revision-graph`
  to emit revision UUID edges, UUID-backed HEADs, SHA digests and consistent
  typed pagination. Keep all-parent traversal and HEAD pinning semantics.
- Update `GET /revisions/{id}`, `history`, compare/diff, historical network
  reads, commits, merges, `expectedHead` and bundle endpoints. Define an
  unambiguous compatibility/version policy for old `{hash}` routes and
  clients, with deprecation errors rather than accidental hash parsing.
- Update signed graph cursors with explicit schema/version and project
  identity; reject mixed-version continuation tokens, stale head-set IDs,
  mismatched project and manipulated UUID/hash bindings.
- Extend native+HTTP loopback tests for branch rename, shared/non-leaf HEAD,
  merges, pagination during commits, duplicate/cross-project IDs, concurrency,
  old-format clients and old bundles.

**Exit:** native and REST APIs agree on identities and cannot alias two
logical revisions because their model snapshots match. Published OpenAPI is
updated **together with working code**.

### Phase 6 — GUI migration and P1 readiness

- Switch AOWIS-SERVER-GUI models, selection keys, graph nodes, inspector
  detail and historical branch creation to revision UUID and branch UUID;
  keep SHA-256 as an explicitly labelled integrity/snapshot field.
- Preserve source+project scoping for GUI display and support multiple
  branches referencing one revision. An inspector must show the UUID in full
  and the digest separately; avoid displaying an abbreviated hash as the only
  identity. Graph itself stays chronological and HEAD-labelled.
- Verify embedded and REST readers render the **same** pinned P0 graph, with
  stable selection across refresh and paging. Decide user-visible legacy
  mapping/upgrade status before offering writes to old repositories.
- Only then resume P1 working copies and revision saving on the new ID
  contract, including optimistic `expectedHead` and conflict recovery.

**Exit:** GUI and backend agree on project/branch/revision identities,
controlled forks/merges render correctly, and P1 saves are safe to start.

## 5. Mandatory acceptance matrix

| Scenario | Expected result |
|---|---|
| Two fresh empty projects | Distinct project UUIDs, revision UUIDs and revision record SHA; identical empty **snapshot** roots permitted. |
| Branch `feature` from `main` without commit | New **branch** UUID, exactly the same HEAD **revision** UUID; no extra revision. |
| Two successive commits with unchanged snapshot | Distinct revision UUIDs and hashes, identical snapshot roots; correct parent chain. |
| Fork from old revision | New branch HEAD equals selected existing revision UUID; original branch never rewinds. |
| Divergence then two-parent merge | New merge revision UUID, both ordered parents, both branches intact. |
| `main` points at an ancestor of `feature` | Both HEADs shown even when `main` HEAD is a non-leaf. |
| Two machines import same converted history | Identical UUID mapping and SHA verification; no second identity generation. |
| UUID collision or same UUID/different record | Hard conflict, no branch HEAD movement or silent overwrite. |
| Corrupt parent/hash/roots, missing target, cycle | Rejected before publishing refs. |
| Kill during conversion, commit or bundle publication | Old or new **verified complete** state; no broken published HEAD. |
| Old frozen repository + incremental bundle | Support or explicit offline migration per version matrix; no silent loss or rewritten fixtures. |
| P0 graph native/REST paging with moving branches | Stable pinned UUID HEAD set; consistent closure and deterministic parents. |
| Concurrent editors, stale expected HEAD | Conflict without losing edits, no branch rewind. |
| Compaction, verifier, backup/recovery | Follow UUID ancestry and verify canonical SHA+Merkle object integrity. |

## 6. Deployment, compatibility and rollback gate

A forced in-place reinterpretation of a 32-byte SHA as a 16-byte UUID is
**forbidden**. The safe default is a **versioned reader plus explicit offline
migration with backup/verification and an atomic activation point**. Decide
whether rolling mixed-version servers or clients are supported; if not,
state that all writers must be stopped, backups made, replicas upgraded and
published together. A legacy repository reopened by old binaries after vNext
writes is *not* automatically reversible. A downgrade path must either be
proven or explicitly unsupported with restore-from-backup instructions.

The pre-release storage/bundle bytes are protected by fixed-format test
fixtures. Keep old SHA addresses and fixtures readable/importable according to
the approved compatibility matrix, and add **new** vNext fixtures; never
silently mutate the existing golden inputs. Include multi-machine replication
before/after upgrade, crash injection around all durable pointers, and a
representative full-service cold-restore rehearsal. See
[format compatibility](format-compatibility.md), [operations](operations.md)
and [offline replication](offline-repository-synchronization.md).

## 7. Decisions that require explicit sign-off before code

1. **Project scope across replicas:** same logical project preserves its UUID;
   what precisely counts as import-as-new-project or an intentional fork?
2. **Legacy UUIDs:** must old UUIDv4 project IDs be replaced with UUIDv7
   immediately, or may a versioned read-only alias survive for external
   references? Decide alias retention and collision protection.
3. **Old SHA→UUID conversion:** authoritative mapping origin, cross-replica
   publication and import-time rules for legacy bundles that have no map.
4. **Revision record format:** whether parent `{UUID, SHA}` pairs and immutable
   `createdAt`/message/actor are mandatory in vNext; canonical encoding,
   domain tag/version and maximum parent count. Metadata choice must not
   accidentally turn mutable descriptions into hashed identity.
5. **Branch identity persistence:** stable UUID keyed independently of
   mutable names, branch-rename compatibility, how refs are encoded, and what
   is copied/mapped by bundles.
6. **AOWIS-wide UUIDv7 policy:** inventory existing `QUuid::createUuid()`
   callers, imported legacy model records and deterministic UUIDs such as
   `hydraulic_network_persistence.cpp::chunkUuid`. Deterministic internal
   membership chunks and immutable value keys have distinct semantics from
   user-visible objects; decide their treatment deliberately. Do not silently
   rekey hydraulic entities while migrating revision ancestry.
7. **Compat rollout:** whether dual-read and offline-convert suffice or an
   online dual-format transition is required, and the explicit minimum client,
   server and bundle versions at cutover.

**Definition of done:** the code, tests, on-disk/bundle schemas, native/HTTP
contracts, migration tooling, CLI/GUI clients and operational documentation
all follow the same ID-vs-hash invariant. A UI-only replacement of abbreviated
hash labels with UUID-like text is **not** completion of this roadmap.

## Phase 2c-2 checkpoint (2026-10-27)

The repository-native BRANCH-2 record and raw-commit operations now provide a
first end-to-end UUIDv7 branch HEAD: initial revision, fork, optimistic raw
commit, durable SHA binding, atomic HEAD advance, stable branch rename and
recovery. The v2 record format is specified in
[the branch reference contract](uuidv7-branch-ref-format.md). This is not the
project-service/REST/GUI cutover; v2 model-indexed writes, merges, complete
graph reads, bundle transport, verification and safe compaction remain open.
SHA and UUID branch records cannot coexist in one repository. Legacy
repository migration is **not** supported; recreate development stores after
the coordinated cutover. Do not report ordinary `createProject()` as UUIDv7
revision-ready until the service has been changed and tested.

## Phase 2c-3a checkpoint (2026-10-27)

`Repository::commitModelV2()` now writes immutable model-indexed UUIDv7
revisions through the real Merkle roots and v2 resolver. It reuses the exact
model UUID/reference validation and reverse-reference index construction of
the SHA writer; both writes are atomic with respect to branch HEAD publication
and accept optimistic expected UUID HEAD checks. Pinned v2 model reads are
available through `getModelAtRevisionV2()`.

The `ProjectService` adapter has explicit opt-in v2 project creation,
branch forks, HEAD reads, model commits and revision-pinned model reads. A
fresh v2 project receives its **own unique root revision UUIDv7**, even if its
snapshot hashes match another empty project. One revision can still be the
HEAD of several branches. The native regression tests cover independent
forked model commits, reference-index enforcement, restart recovery and
stable branch UUIDs.

**Next required cutover work:** UUIDv7 model merges and ancestor traversal;
UUIDv7 graph/first-parent history with pinned pagination; bundle and restore
support; full verifier and safe compaction; then replace the SHA-based default
ProjectService methods and expose the revised REST/CLI contracts together.
Until then do not present `createProject()` or current HTTP project creation
as having UUIDv7 revision HEADs. No old-repository compatibility is planned.

## Phase 2c-3b checkpoint (2026-10-27)

The **explicit** UUIDv7 `Repository` and `ProjectService` interfaces now
provide `createBranchAtRevisionV2()` and `mergeBranchesV2()`; native storage
also exposes `revisionReachableFromV2()`. Every ancestry check traverses **both**
ordered parent UUIDv7 references using the verified UUID-to-SHA resolver, not
SHA hashes as revision identities. Creating a branch from a historical revision
publishes a fresh branch UUIDv7 with the *same* referenced revision UUID; the
source branch does not move.

The v2 merge operation validates optional expected HEAD UUIDs while holding
both repository locks and supports:

- Already incorporated history (`UpToDate`), including through a second merge
  parent; neither HEAD moves.
- Fast-forward (`FastForward`) to the source's verified revision UUID, keeping
  the source branch and both branch UUIDs unchanged.
- Divergent histories with exactly one lowest common ancestor (`Merged`):
  non-overlapping changes produce a new UUIDv7 revision with ordered parents
  `[previousTargetHEAD, sourceHEAD]`; source HEAD stays unchanged.
- Conflicting simultaneous edits and invalid merged model references
  (`Conflicts`) without publishing a HEAD. Model merges reuse the existing
  reverse-reference validation/preflight path. The result reports the selected
  common base, both captured HEADs, and the resulting target revision UUID.
- Ambiguous/criss-cross multiple lowest merge bases: an explicit `Conflict`
  error, not arbitrary ancestor selection.

**Boundary:** this is a conservative, noninteractive three-way merge API. The
SHA-identity merge API has richer caller-supplied conflict resolution,
reconciliation, and domain-validation callbacks; those are **not** available
in `mergeBranchesV2()` yet. V2 callers must not assume support for merging
conflicting model records or semantic domain validation merely because the
old API can do it. For large ancestry graphs, the present resolver authenticates
full ancestry on lookup and may repeat work; bounded traversal is enforced,
but efficient batch DAG resolution is still pending performance work.

Neither default `ProjectService` operations nor REST/CLI/GUI are switched by
this checkpoint. Next work: v2 all-parent graph and first-parent history APIs
with pinned pagination, then identity-aware replication, verification and
compaction, followed by the coordinated default API cutover. No pre-release
legacy-repository migration is provided.

## Phase 2d native history and graph checkpoint (2026-10-27)

The explicit UUIDv7 native reader now exposes first-parent `historyV2` and
all-parent `revisionGraphV2` through `Repository` and `ProjectService`.
Graph pages carry branch UUID/name/HEAD/revision-SHA snapshots, authenticated
revision UUID→SHA bindings, both ordered merge-parent UUID/SHA pairs, and
stable replay-based cursors. One revision is returned only once per project
regardless of how many branch HEADs reach it. A moving branch does not change
an existing pinned graph snapshot. Tests cover merge ancestry, non-leaf and
shared HEADs, page boundaries, forged cursors, historical first-parent cursors,
restart and project scope.

This is **native API only**. Default SHA-facing service calls, REST schemas,
bundles, verifier, compaction and GUI remain to be cut over together. Old
pre-release repositories are not supported after the eventual format cutover.
Next stage: UUIDv7-aware replication and verification/compaction, then a
coordinated native/REST/CLI/GUI transition (including authenticated HTTP
continuation tokens). Do not report the HTTP full-graph contract as v2-ready.

## Phase 2e-1 checkpoint — v2 repository verification and safe compaction (2026-10-27)

Implemented: full native `Repository::verifyRepository()` authenticates the
published v2 UUIDv7→SHA resolver entries, complete two-parent ancestry and
Merkle/model/reverse-index state. SHA-based and v2 revisions share the same
logical Merkle/model verifier; the v2 view is synthesized **only after**
checking the canonical packed v2 bytes and immutable parent UUID/hash bindings.

`Repository::compact()` now includes all published v2 UUID→SHA bindings in
its retention roots; every referenced parent, snapshot trie, and leaf is kept.
Records published without a corresponding branch HEAD are retained on purpose
so no stable UUID becomes a dangling pointer after compaction. This is a
conservative pre-release retention policy, **not** v2 revision garbage
collection. Tests cover an unreferenced two-parent DAG, independent branch
heads, an indexed model merge, re-verification after compaction, and corrupt
resolver entries.

**Remaining:** v2 identity-preserving replication format and import protocol,
including project UUID ownership, ordered parents, stable UUID mappings, and
atomic publication of imported pack objects/resolver entries/branch HEADs;
then migrate the default native/REST/CLI/GUI surfaces. No SHA-only bundle may
be used to claim UUIDv7 history was transported.

## Phase 2g-a — explicit UUIDv7 REST project/branch/history surface (2026-10-27)

The REST adapter now exposes **opt-in** coherent UUIDv7 project, branch and
revision reads/writes without reinterpreting SHA-era API inputs:

- `POST /dvmdb/v1/projects` with `revisionFormat: "uuidv7"` creates a v2
  initial revision. Omitting that field still uses the previous service path
  while remaining model-edit and diff endpoints await conversion. This is a
  **temporary rollout boundary**, not promised long-term compatibility.
- `POST /dvmdb/v1/projects/{id}/branches-v2` and
  `PATCH /dvmdb/v1/projects/{id}/branches/{branch}/rename-v2` manage UUIDv7
  branch refs. Historical forks require an explicit source branch and revision
  UUIDv7, and names are stored only as branch-record metadata, never as
  filesystem filenames.
- `GET /dvmdb/v1/projects/{id}/branches/{branch}/head-v2`, `.../history-v2`,
  `GET /dvmdb/v1/projects/{id}/revisions-v2/{revision}` and
  `GET /dvmdb/v1/projects/{id}/revision-graph-v2` expose UUIDv7 IDs and
  record SHA-256 separately. The graph has authenticated, project-scoped
  pinned HEAD cursor snapshots; both merge parents are returned.

**Not a default cutover yet:** SHA-era model mutation/diff/merge and hydraulic
HTTP routes are not UUIDv7-aware; changing default project creation before
those operations switch would strand newly created projects. Next phase must
convert the missing service and REST model/merge/diff operations, run the full
HTTP suite, then flip/remove the SHA-era default endpoints and update the GUI
atomically. No old-storage migration is provided.

## Phase 2g-3b — UUIDv7 hydraulic merge and conflict resolution (2026-10-27)

The native `Repository::mergeBranchesV2` now accepts optional *pure*
`MergeOptions` domain callbacks while retaining exact UUIDv7 expected-HEAD
pins, authenticated two-parent ancestry, and atomic HEAD publication. It
supports same-UUID whole-object selection (including deletion), canonical
field-level resolution of existing typed model records, concurrent hydraulic
membership-chunk additions, candidate membership repair, complete candidate
semantic validation, and mandatory validation of fast-forward source tips.
Selections not consumed by a genuine conflict are rejected before publication.
The SHA and UUIDv7 implementations share the **same hydraulic option builder**;
there is no separately weakened V2 hydraulic resolution logic.

The domain API `hydraulic::mergeHydraulicBranchesV2` takes a project UUID and
mandatory pair of expected revision UUIDs, with optional field/object
selections. `POST /dvmdb/v1/projects/{id}/hydraulic-merges-v2` exposes it via a
separate route so the generic `merges-v2` operation is never mistaken for a
hydraulically validated merge. Unknown, duplicate, stale, unused, or
contradictory selections fail; invalid complete-network candidates return
conflicts without moving branch HEADs. A successful divergent merge creates a
new UUIDv7 revision whose ordered parents are the two original branch tips.

This is still an **explicit V2** API. The default SHA-facing REST routes,
indexed hydraulic V2 edit optimizations, and GUI cutover remain outstanding.
The Qt hydraulic/HTTP regression tests must run in an environment with Qt and
the pinned model submodule; native-only validation cannot prove those paths.
