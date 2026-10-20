# Complete hydraulic network persistence (native database)

The hydraulic persistence API is declared in `aowis/native/hydraulic_network_persistence.h`
with `saveHydraulicNetwork()`, `loadHydraulicNetwork()`, and `editHydraulicNetwork()` for the **complete**
`NetworkHydraulic` model. It is available when the **native-owned `ModelCodec<T>` serializer and hydraulic
validator** are enabled. Both are implemented in this repository.

## What is persisted

* The **network root** is one native `ModelRecord` (experimental kind `100`).
  Its payload is a versioned `ANX5` network membership manifest: canonically encoded
  network metadata with entity lists cleared, followed by ordered membership-
  chunk UUID lists for the 18 collections. Each chunk contains up to 256
  ordered entity UUIDs. Network options, map backdrop, solver settings and nested
  multi-species data remain in the metadata and therefore round-trip.
* Every top-level entity from the 18 lists is its **own** native `ModelRecord`,
  with the original `ModelCodec<T>::encode()` bytes and an experimental kind
  identifier. Existing Stage 4 kind IDs for junctions (`1`) and pipes (`4`)
  are unchanged.
* Outgoing model references are extracted by the native canonical validator
  and stored in the reverse-reference tree. For persistence,
  `validateHydraulicNetworkWithDependencies()` produces compact UUID pairs
  while performing the same validation pass; `hydraulicReferences()` retains
  the detailed, field-named API for diagnostics.
  References to nested MSX objects remain inside the network metadata, not as
  non-existent top-level native model entities.
* The root references membership chunks, and each chunk references its entities.
  This prevents a raw model deletion from leaving a declared member absent.
  A save updates the manifest, its chunks, entities, and deletions in **one native
  `commitModel()`** with one atomic published branch revision. The native commit also checks the
expected starting branch head under its repository lock, rejecting concurrent
branch advances instead of silently overwriting changes.

Lists retain their original ordering. Member UUIDs must be non-null and unique
across all 18 lists and distinct from the network UUID. Existing UUIDs may not
be claimed by another network or an unrelated object on the same branch.

The native `validateHydraulicNetwork()` performs typed relationship checks
before writes (pipes to nodes, pump/valve curves, patterns, demand-point
attachments, controls, network options and MSX). It permits nullable draft
references according to native rules. Loading additionally checks stored kinds,
UUIDs, complete membership, declared outgoing reference sets and validation.

## Compact whole-network reference validation

Complete saves and conservative edits now collect compact source/target UUID
pairs in the same traversal as the semantic validation checks. Sorted and
deduplicated pairs replace the per-entity Qt hash-of-sets temporary reference
graph. Whole-network loads compare sorted stored and canonical pairs.
Validation-only requests do not retain unused detailed reference strings,
and `hydraulicReferences()` remains available for diagnostics.

This only changes temporary memory use: the model payloads, persisted outgoing
references, reverse-reference index, Merkle roots, and formats are unchanged.
Measure import peak RSS using a fresh process to quantify improvement.

## Bounded commit payload retention

Full-network saves hand the encoded entity changes to the native store by
move. The native commit sorts the owned changes directly, validates all
outgoing/incoming references before writing, and releases each source
`ModelRecord` once its canonical bytes have been encoded. The final revision
tree also sorts borrowed pointers into the preflight buffers rather than
copying the complete payload batch again.

This is an **in-memory commit optimization**, not a change to the model codec,
Merkle hashes, object packs, membership format or atomic publication rules.
Existing callers may still pass a `const std::vector<ModelChange>&`; only an
explicit `std::move` transfers its payload ownership. The canonical model
validator still runs before whole-network persistence. Peak memory must be
measured with a fresh benchmark process and a new repository path: the
operation does **not** stream the application's `NetworkHydraulic` fixture.

## Usage

```cpp
#include <aowis/native/hydraulic_network_persistence.h>

// Create/choose a branch first, then:
const aowis::native::Revision saved =
    aowis::native::hydraulic::saveHydraulicNetwork(repository, "main", network);
QString error;
const std::optional<NetworkHydraulic> restored =
    aowis::native::hydraulic::loadHydraulicNetwork(repository, "main", network.uuid, &error);
```

Both Stage 7 packs and experimental block storage use this same model adapter.
The native database owns its canonical serializer and validator, and has no
SQLite, PostgreSQL, or sibling SQL-repository dependency.

## Scope and limitations

This introduces **complete network aggregate storage**, not yet equivalent
the full intended database server functionality. It does not implement permissions, event journaling,
merge conflict handling, remote replication, arbitrary historical-revision
loading by ID or a REST API. `saveHydraulicNetwork()` is a *complete network
replacement* and encodes all incoming entities. `editHydraulicNetwork()` uses
bounded indexed validation for a restricted set of changes, and full-network
validation otherwise. Membership changes touch bounded chunks; large full
replacements still process every entity. Physical format/kind IDs remain
**pre-release experimental**; no compatibility layer for obsolete prototypes.

Run `./compile_linux_tests.sh --both` on a Qt6-equipped machine. The optional
`aowis-native-hydraulic-network-tests` executable checks all 18 lists,
canonical payload equivalence, nested MSX metadata, updates/removals, branch
isolation, cross-network UUID collisions and reopening with both physical
backends.

## Incremental entity editing (Stage 18)

`editHydraulicNetwork(repository, branch, networkUuid, changes)` applies a
single atomic batch of upserts/deletions to an **existing** hydraulic network.
An upsert uses `HydraulicEntity` (`std::variant` of the 18 canonical top-level
Model types); `HydraulicEntityChange::uuid` must equal its payload UUID.
Deleting an object uses `std::nullopt` as its value. New members are appended to
the corresponding ordered list, whereas updates keep their existing position.
Changing an existing UUID's collection kind is rejected.

```cpp
HydraulicLinkPipe pipe = ...; // existing pipe, changed diameter or endpoints
const std::vector<aowis::native::hydraulic::HydraulicEntityChange> edits = {
    {pipe.uuid, aowis::native::hydraulic::HydraulicEntity{pipe}}
};
const aowis::native::Revision revision =
    aowis::native::hydraulic::editHydraulicNetwork(repository, "main", networkUuid, edits);
```

Only edited records are re-encoded. For an update that does not affect list
membership, the original network manifest and all unchanged members are left
untouched. Indexed inserts and deletions update affected membership chunks and
rewrite the root only if the list of chunk IDs changes. Full-validation
fallbacks may rebuild the chunk layout. Reverse-reference
indexes are maintained by the native model commit. Identical edits and empty
batches do not advance the branch. Branch-head changes concurrent with edit
preparation are rejected.

**Validation performance:** Stage 22 uses a bounded indexed path for existing
pipe updates limited to diameter, lengths, roughness values and minor loss;
existing junction updates limited to elevation fields; and existing map-label
text edits. It compares the entire canonical payload against the original
with only these whitelisted fields replaced, checks recorded references and
network ownership, and commits under an expected-head guard. These changes
cannot affect any currently defined canonical reference or consistency rule.
Stage 23 additionally covers rewiring existing pipes without inbound dependents,
using typed, same-network endpoint lookups and canonical reference checks.
Indexed insertion and deletion of selected entity types are described below;
other changes use complete-network loading and validation (O(network size)). The
fast path relies on an already valid persisted network; run the complete
loader/validator when importing externally modified or untrusted model data.

Pass `HydraulicEditDiagnostics*` as the final optional argument to
`editHydraulicNetwork()` to observe `indexedScalarFastPath`,
`indexedTopologyFastPath`, `indexedMembershipFastPath`, and `validatedEntities`
(only populated for successful indexed paths). The safe indexed
path does not rewrite the membership manifest. Network metadata changes still
require `saveHydraulicNetwork()`.

To measure the difference with canonical model records, run the dedicated
`aowis-native-hydraulic-edit-benchmark` on a fresh repository. It reports
mean indexed scalar edit time versus full topology validation time, along
with peak resident memory. Never compare across unequal validation workloads.

## Indexed entity queries (Stage 21)

For interactive inspectors and lists, callers need not reconstruct the full
`NetworkHydraulic` to read one object:

```cpp
QString error;
const std::optional<aowis::native::hydraulic::HydraulicEntity> result =
    aowis::native::hydraulic::loadHydraulicEntity(
        repository, "main", networkUuid, pipeUuid, &error);

const std::optional<QList<QUuid>> pipeIds =
    aowis::native::hydraulic::listHydraulicEntityIds(
        repository, "main", networkUuid,
        aowis::native::hydraulic::HydraulicEntityType::Pipe, &error);
```

* `loadHydraulicEntity()` tests membership through the persistent incoming-
  reference index and decodes **only the requested entity**. It validates
  its stored kind and embedded UUID and detects branch-head changes during
  the lookup. Missing or unowned UUIDs return `std::nullopt` with empty error.
  It does **not** perform complete-network semantic validation; use
  `loadHydraulicNetwork()` when the full invariant check is needed.
* `listHydraulicEntityIds()` returns the chosen collection's UUIDs in the
  original model order without loading entity payloads. It traverses only
  that collection's membership chunks, checking their stored reference sets.
* These are read-only APIs. They preserve existing canonical model bytes,
  branch/revision identities and both native physical storage formats.
  Stage 22 adds the conservative indexed scalar-edit path described above.

## Bounded validation of independent pipe rewires (Stage 23)

A batch containing only updates to existing pipes can now use the indexed
validation path when **no other entity references any edited pipe** beyond its
network membership. Endpoint UUIDs are checked against existing, same-network
junction/reservoir/tank records, material UUIDs against same-network pipe
materials, and stored outgoing edges against the canonical old payload.
Only endpoint changes and the previously whitelisted pipe hydraulic fields
are eligible; the revision is published with the expected-head guard.

If any edited pipe has an incoming control, demand point or other dependency,
or if a proposed edit changes other fields, adds/deletes objects, refers to a
missing/wrong-type target, or otherwise fails the eligibility proof, editing
falls back to the full canonical validator. In particular, assigned-junction
demand-point attachments are never bypassed. This is a deliberately narrow
incremental validator, **not** general graph-wide incremental validation.

The hydraulic edit benchmark's `indexed_topology_mean_ms` measures the safe
independent pipe rewire case. It is not directly comparable to the old
`topology_full_validation_mean_ms` as an equivalent mix of all topology edits.

## Chunked persistent membership (Stage 25)

The authoritative network root now stores metadata and ordered chunk UUIDs
instead of a flat list of every entity. Each bounded chunk contains up to
256 ordered member UUIDs. Two-level, committed reference edges maintain
network -> chunk -> member ownership. All 18 collection orders are retained.
Indexed entity reads resolve ownership through this chain, and only the
changed chunk needs rewriting for append/delete operations. The root changes
only when a chunk is added or removed. The reverse-reference index and both
tree roots are committed atomically using the existing expected-head guard.

The low-level `Repository::diff()` operates on physical model records.
Consequently, adding or removing members can report changed membership chunks
in addition to the network root and user-visible entities. Callers producing
user-facing history should distinguish internal chunk records from model edits;
raw diff counts are not semantic entity-change counts.

This is a new **pre-release manifest v2** layout; repositories created by
Stage 24 must be recreated (no obsolete-format migrations are maintained).
The canonical model *entity payload* format is unchanged. A full network import
or full-validation fallback still processes all entities. The manifest
contains only O(chunks) UUIDs instead of O(members) UUIDs.

`aowis-native-hydraulic-edit-benchmark` reports separate member insertion,
label deletion, pipe/junction batch deletion and
`network_membership_chunk_count` values.

## Indexed membership changes (Stage 24)

The incremental editor can now validate **new junctions, new pipes, and new map
labels**, including a batch that adds a junction and a pipe referring to it,
without decoding the existing hydraulic entities. It checks UUID uniqueness,
network ownership and expected target types through the native object and
reverse-reference indexes. Only deletion of an **independent map label** is
initially eligible for an indexed removal. Existing scalar and eligible pipe
rewire fast paths are unchanged.

The deletion path also supports **junctions and pipes** when every incoming
reference is their membership chunk or another deleted entity in the *same*
atomic batch. Network metadata references are checked with the canonical
hydraulic reference walker, protecting report selections and nested MSX
relationships that have no stored entity-to-entity reverse edge. Thus an
independent pipe may be deleted, or an otherwise unreferenced junction and its
only incident pipe removed together. Surviving controls, demand points,
other links, labels, or metadata references force full-network validation;
unsupported kinds and mutations still take the full path.

Stage 25 replaces the flat root membership with bounded, persistent chunks.
The benchmark reports `indexed_membership_insert_batch_ms` and
`indexed_membership_delete_label_ms` for these operations, including durable
publication.

## Indexed catalogue edits

The native editor can update, insert and delete **all eight reference-free
catalogue entity collections** (pipe materials, time patterns and the six curve
kinds) using the persistent ownership/reverse-reference indexes. Existing
catalogue payload updates preserve UUID and entity kind, and cannot change
outgoing references (the canonical validator defines none for these types).
They do not rewrite network membership. New/deleted entries use the same
bounded membership chunks as junction/pipe edits, in a single atomic revision.

Deletion refuses fast validation when a surviving model record references the
catalogue UUID or when network-level metadata refers to it. For example, a
material in use by a pipe or a time pattern selected by a pump cannot be
removed. Unsupported edit combinations still use full canonical validation;
there is no general-purpose bypass of cross-entity checks. The diagnostics
flag `indexedCatalogueFastPath` distinguishes catalogue payload updates from
`indexedMembershipFastPath` for member insertion/deletion.

## Indexed physical/operating-parameter edits (Stage 28)

The editor can update **existing reservoirs, tanks, pumps and valves** using
bounded indexed validation, including atomic batches that mix these four entity
types. It decodes each old entity, checks ownership in the persistent
root -> membership chunk -> entity chain, and compares canonical codec bytes
after copying a strict whitelist of physical parameters into the old value.
This ensures the change cannot modify UUID, endpoints, head/efficiency curves,
patterns, quality-source links, metadata or any newly added unreviewed field.
Stored outgoing-reference edges are also compared against the corresponding
canonical reference extraction before reusing them.

The fast path includes reservoir hydraulic head/elevations, tank water levels
and geometric measurements, pump power/speed/efficiency/energy price, and valve
settings/diameter/loss parameters. It does **not** make new typed references
or change the structure of the network. Invalid or unsupported operations use
full canonical validation and cannot cause a partial commit; unchanged batches
return their existing revision. The diagnostic `indexedPhysicalFastPath`
identifies this operation separately from scalar-pipe or catalogue updates.

The dedicated benchmark reports `indexed_physical_update_batch_mean_ms` for a
single atomic batch updating one reservoir, tank, pump and valve on each
iteration. This measurement includes durable commit publication and should be
compared with other indexed batch edits, not individual scalar operations.

## Indexed reservoir/tank/pump/valve membership (Stage 29)

Creating reservoirs, tanks, pumps and valves uses the same bounded membership
chunks and native reverse-reference index as pipe/junction creation. Validation
checks *each* canonical outgoing UUID against the intended entity kind: node
endpoints may be junctions, tanks or reservoirs; tank-volume curves, pump-head
and pump-efficiency curves, valve-headloss and valve-characteristic curves, and
time patterns must have their respective kinds. Targets may be pre-existing
members or inserted in the same atomic edit, regardless of change ordering.
The canonical validator's null-reference policy is retained.

Deletion is indexed only when all incoming edges originate from membership
chunks or entities deleted in that same batch. Network-level report selections
and embedded multi-species references are checked separately using the native
validator's metadata reference extraction. In-place physical edits are still
restricted to the Stage 28 whitelist; arbitrary structural updates continue
through the complete-network validator.

`indexed_structural_insert_batch_ms` and `indexed_structural_delete_batch_ms`
measure a four-entity mixed reservoir/tank/pump/valve membership operation.

## Indexed demand points and hydraulic controls (Stage 30)

The native incremental editor also supports **inserting and deleting demand
points, simple controls and rule controls** through bounded indexed membership
edits. The canonical validator's outgoing reference sets are maintained on
these records and in the persistent reverse-reference index. New controls may
refer to existing or same-batch-created node and link objects, and rule
premises are checked using their node/link object categories. Null UUIDs retain
the canonical validator's existing behavior.

Pipe-attached demand points require a finite normalized attachment position in
`[0, 1]`, a pipe-kind target, and, for `AssignedJunction` allocation, a
junction-kind target that is **one of that pipe's actual endpoints**. The
indexed validator checks either a stored pipe record or a pipe inserted in
the same atomic batch. Demand pattern UUIDs must refer to time patterns, and
junction attachments must refer specifically to junctions, not reservoirs or
tanks. Every inserted dependent entity is validated before any commit occurs.

Deletion is authorized only if no surviving record or network metadata depends
on the removed UUID. Removing controls/demand points together with the links
and nodes they depend on is supported when all incoming dependencies are
included in the same atomic deletion. Existing reference-changing edits to
these entities still use full-network canonical validation; this fast path does
not authorize arbitrary in-place mutation of rules or demand attachments.

The dedicated edit benchmark reports `indexed_dependent_insert_batch_ms` and
`indexed_dependent_delete_batch_ms` for an atomic three-entity demand-point /
simple-control / rule-control batch.

## Indexed existing demand and control updates (Stage 31)

Existing demand points, simple controls and rule controls can update their
canonical model payloads and outgoing references atomically without touching
network membership chunks. All 3 types are supported in one batch. The editor
checks ownership, UUID and kind, verifies the previous persisted reference set
against the decoded canonical payload, and checks every new reference against
its intended entity type and same-network ownership. Pipe-attached demands must
have finite positions in `[0, 1]`; assigned junctions must be actual endpoints
of the referenced pipe (read and checked from the indexed store).

Reference-list changes update the reverse-reference index through the normal
atomic model commit. Identical updates create no revision. Invalid edits and
unsupported mixed batches continue to the full-network canonical validator;
no subset of a failed batch is committed. Full network validation remains the
authoritative fallback, and the fast path does not combine arbitrary insertions
or deletions with reference-changing updates.

`indexed_dependent_update_batch_ms` times one three-object reference-update
batch in the dedicated hydraulic edit benchmark.
