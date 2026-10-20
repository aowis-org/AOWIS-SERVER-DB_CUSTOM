# Complete hydraulic network persistence (native database)

The Qt/V5 adapter now includes `aowis/native/hydraulic_network_persistence.h`
with `saveHydraulicNetwork()`, `loadHydraulicNetwork()`, and `editHydraulicNetwork()` for the **complete**
`NetworkHydraulic` model. It is built only when the **native-owned canonical V5
ModelCodec and hydraulic validation** are available. The canonical V5 codec is implemented inside this native repository.

## What is persisted

* The **network root** is one native `ModelRecord` (experimental kind `100`).
  Its payload is a versioned `ANX5` network membership manifest: V5-encoded
  network metadata with the entity lists cleared, followed by the 18 ordered
  member UUID lists. Network options, map backdrop, solver settings and nested
  multi-species data remain in the metadata and therefore round-trip.
* Every top-level entity from the 18 lists is its **own** native `ModelRecord`,
  with the original `ModelCodec<T>::encode()` bytes and an experimental kind
  identifier. Existing Stage 4 kind IDs for junctions (`1`) and pipes (`4`)
  are unchanged.
* Outgoing model references are extracted by the native
  `hydraulicReferences()` validator and stored in the reverse-reference tree.
  References to nested MSX objects remain inside the network metadata, not as
  non-existent top-level native model entities.
* The root's dependency list includes every network member. This prevents a
  raw model deletion from leaving a manifest whose member is absent. A save
  updates the manifest, all altered members, and deletions in **one native
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

## Usage

```cpp
#include <aowis/native/hydraulic_network_persistence.h>

// Create/choose a branch first, then:
const aowis::native::Revision saved =
    aowis::native::v5::saveHydraulicNetwork(repository, "main", network);
QString error;
const std::optional<NetworkHydraulic> restored =
    aowis::native::v5::loadHydraulicNetwork(repository, "main", network.uuid, &error);
```

Both Stage 7 packs and experimental block storage use this same model adapter.
The native database owns its canonical serializer and validator, and has no
SQLite, PostgreSQL, or sibling SQL-repository dependency.

## Scope and limitations

This introduces **complete network aggregate storage**, not yet equivalent
the full intended database server functionality. It does not implement permissions, event journaling,
merge conflict handling, remote replication, arbitrary historical-revision
loading by ID or a REST API. The save operation is a *complete network
replacement*; it encodes all incoming entities, and its root tracks a flat
membership list. Large-network writes still need incremental network-membership
indexes and edit-oriented APIs to avoid processing every entity for a small
change. The root currently holds member UUIDs in both the ordered manifest and
reverse-reference dependencies; that is deliberate for deletion safety and
will have a measurable storage cost. Physical format/kind IDs remain
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
const std::vector<aowis::native::v5::HydraulicEntityChange> edits = {
    {pipe.uuid, aowis::native::v5::HydraulicEntity{pipe}}
};
const aowis::native::Revision revision =
    aowis::native::v5::editHydraulicNetwork(repository, "main", networkUuid, edits);
```

Only edited records are re-encoded. For an update that does not affect list
membership, the original network manifest and all unchanged members are left
untouched. Inserts and deletes update the ordered membership manifest and root
references atomically alongside their changed records. Reverse-reference
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
using typed, same-network endpoint lookups and canonical reference checks. Other
relationship changes, inserts, and deletes use complete-network loading and
native validation (O(network size)). The
fast path relies on an already valid persisted network; run the complete
loader/validator when importing externally modified or untrusted model data.

Pass `HydraulicEditDiagnostics*` as the final optional argument to
`editHydraulicNetwork()` to observe `indexedScalarFastPath`,
`indexedTopologyFastPath`, and `validatedEntities` (only populated for
successful indexed paths). The safe indexed
path does not rewrite the membership manifest. Network metadata changes still
require `saveHydraulicNetwork()`.

To measure the difference with canonical V5 records, run the dedicated
`aowis-native-hydraulic-edit-benchmark` on a fresh repository. It reports
mean indexed scalar edit time versus full topology validation time, along
with peak resident memory. Never compare across unequal validation workloads.

## Indexed entity queries (Stage 21)

For interactive inspectors and lists, callers need not reconstruct the full
`NetworkHydraulic` to read one object:

```cpp
QString error;
const std::optional<aowis::native::v5::HydraulicEntity> result =
    aowis::native::v5::loadHydraulicEntity(
        repository, "main", networkUuid, pipeUuid, &error);

const std::optional<QList<QUuid>> pipeIds =
    aowis::native::v5::listHydraulicEntityIds(
        repository, "main", networkUuid,
        aowis::native::v5::HydraulicEntityType::Pipe, &error);
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
* These are read-only APIs. They preserve existing canonical V5 bytes,
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
The canonical V5 *entity payload* format is unchanged. A full network import
or full-validation fallback still processes all entities. The manifest
contains only O(chunks) UUIDs instead of O(members) UUIDs.

`aowis-native-hydraulic-edit-benchmark` reports separate member insertion,
label deletion and `network_membership_chunk_count` values.

## Indexed membership changes (Stage 24)

The incremental editor can now validate **new junctions, new pipes, and new map
labels**, including a batch that adds a junction and a pipe referring to it,
without decoding the existing hydraulic entities. It checks UUID uniqueness,
network ownership and expected target types through the native object and
reverse-reference indexes. Only deletion of an **independent map label** is
currently eligible for an indexed removal. Existing scalar and eligible pipe
rewire fast paths are unchanged.

This is intentionally narrower than arbitrary indexed insert/delete. Deleting
nodes or pipes may invalidate nested network metadata, report selections,
MSX overrides or demand-point assignments. Such operations, and updates to
unsupported entity kinds, still use full canonical model validation.

Stage 25 replaces the flat root membership with bounded, persistent chunks.
The benchmark reports `indexed_membership_insert_batch_ms` and
`indexed_membership_delete_label_ms` for these operations, including durable
publication.
