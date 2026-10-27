# UUIDv7 object identities and SHA-256 integrity — migration roadmap

**Proposed implementation roadmap, 2026-10-27. Not implemented.** This is the
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

**Exit:** agreed data diagrams and binary/wire contracts; frozen vNext
specimen, error taxonomy and a reviewed migration/rollback design. No code
change until these decisions are explicit.

### Phase 1 — UUIDv7 infrastructure and typed native identifiers

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

**Exit:** new logical IDs are UUIDv7, native-only builds stay Qt-free, and
existing old repositories still open unchanged through the supported reader.

### Phase 2 — versioned revision records and UUID-indexed ancestry

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
