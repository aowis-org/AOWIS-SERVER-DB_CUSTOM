# Hydraulic network persistence and indexed editing

`aowis::native::hydraulic` stores complete `NetworkHydraulic` objects using this repository's canonical `ModelCodec<T>` and native reference validator. It can be called directly from C++ without a database service or HTTP.

Public API: `include/aowis/native/hydraulic_network_persistence.h`.

## Model structure and storage

A network root is one versioned `ModelRecord` (current kind `100`), containing network metadata and a manifest of ordered membership-chunk UUIDs. Each membership chunk holds at most **256 ordered member UUIDs** and references the member objects. Every top-level member is a separate canonical model record, rather than a copy of the full network aggregate.

The **18 top-level collections** are: pipe materials; time patterns; tank volume, pump head, pump efficiency, valve headloss, valve characteristic and generic curves; reservoirs, junctions and tanks; demand points; pipes, pumps and valves; simple controls, rule controls and map labels.

- Root -> membership chunks -> entity records preserves membership and reference ownership.
- Individual entity UUIDs must be non-null and unique on the branch, across networks and entity types.
- Collection order is preserved. Existing UUIDs cannot silently change collection kind.
- Network metadata (solver/options, map backdrop and nested MSX data) remains in the root; MSX-internal data is **not** represented as fictional top-level entity records.
- A persisted reverse-reference index records required UUID dependencies and enables bounded ownership and deletion checks.
- The network membership manifest has an internal binary format marker (`ANX5`); it is **not** an API namespace or user-facing name. Formats are still pre-release.

`validateHydraulicNetworkWithDependencies()` extracts compact UUID dependency pairs while validating model semantics, without constructing a large per-entity hash-of-sets graph. The detailed diagnostic reference API remains available. Whole-network loading checks membership, object kinds and UUIDs, canonical outgoing references, and semantic consistency. Each complete load pins one immutable revision before reading the root, membership chunks or any entity, so concurrent commits cannot produce a mixed-generation aggregate.

## Complete network operations

```cpp
#include <aowis/native/hydraulic_network_persistence.h>

// repository and NetworkHydraulic network are already initialized.
const aowis::native::Revision revision =
    aowis::native::hydraulic::saveHydraulicNetwork(repository, "main", network);

// nullopt: no such network. Failures throw StorageError with their kind.
const std::optional<NetworkHydraulic> loaded =
    aowis::native::hydraulic::loadHydraulicNetwork(repository, "main", network.uuid);

// Explicit, reproducible historical read (does not follow a moving branch).
const std::optional<NetworkHydraulic> historical =
    aowis::native::hydraulic::loadHydraulicNetworkAtRevision(
        repository, revision.id, network.uuid);
```

`saveHydraulicNetwork()` validates and replaces one complete network atomically, without changing other networks on the branch. It encodes all incoming top-level entities and is therefore a **full-network operation**. The native store performs reference and expected-head checks before publishing one new revision. Whole-network operations still require substantial work and memory proportional to the network size.

## Indexed entity queries

```cpp
const std::optional<aowis::native::hydraulic::HydraulicEntity> pipe =
    aowis::native::hydraulic::loadHydraulicEntity(repository, "main", networkUuid, pipeUuid);
const std::optional<QList<QUuid>> pipeIds = aowis::native::hydraulic::listHydraulicEntityIds(
    repository, "main", networkUuid, aowis::native::hydraulic::HydraulicEntityType::Pipe);
```

`loadHydraulicEntity()` uses the reverse-reference membership index to find a single entity without reconstructing the network. It validates the requested record's type and canonical UUID; it does not perform a full network semantic audit on each read. `listHydraulicEntityIds()` returns one selected collection in its original order, without decoding every entity payload. A missing network/member returns `std::nullopt`; a corrupt or inconsistent stored object throws `StorageError(Corruption)`, and I/O or full-disk failures keep their own kind (HTTP 500/507). Branch-oriented queries pin a revision on entry; they never combine records from different branch heads. The corresponding `loadHydraulicEntityAtRevision()` and `listHydraulicEntityIdsAtRevision()` APIs accept an explicit revision hash, so several separate queries can be made against the same historical snapshot. A branch may advance between two *different* branch-oriented calls; clients requiring a consistent multi-call view must obtain the branch head once and pass its revision ID to each call.

## Atomic indexed editing

```cpp
HydraulicLinkPipe editedPipe = existingPipe; // supplied by the calling application
const std::vector<aowis::native::hydraulic::HydraulicEntityChange> changes = {
    {editedPipe.uuid, aowis::native::hydraulic::HydraulicEntity{editedPipe}}
};
aowis::native::hydraulic::HydraulicEditDiagnostics diagnostics;
const auto revision = aowis::native::hydraulic::editHydraulicNetwork(
    repository, "main", networkUuid, changes, &diagnostics);
```

`HydraulicEntity` is a variant of exactly those 18 persisted top-level types. An upsert supplies an object whose embedded UUID matches the change key. A deletion supplies `std::nullopt`. New members append to their collection; updates preserve the prior position. An atomic batch may include interdependent additions/deletions, and an identical/empty batch does not advance history.

Full saves and full-validation edit fallbacks read their starting membership and models at an immutable expected-head revision, then reject stale publication with the existing atomic compare-and-commit check. Indexed edit preparation continues using optimistic expected-head protection.

The editor re-encodes only changed entities when its indexed rules can prove validity. It reuses unchanged membership chunks, rewriting affected chunks (and the root only when chunk-descriptor membership changes). It also updates outgoing/incoming reference edges atomically. It verifies the expected branch head so a concurrent change cannot silently be overwritten.

### Indexed validation coverage

| Operation | Current behavior |
|---|---|
| Pipe dimensions/roughness, junction elevations, map-label text | Indexed existing-entity updates |
| Independent pipe endpoint rewiring | Indexed when no dependent incoming references require global checks |
| Materials, time patterns and curves | Indexed updates and safe insertion/deletion; referenced catalogue entries are protected |
| Reservoir/tank/pump/valve physical operating fields | Indexed updates for reviewed scalar fields with unchanged structural references |
| Reservoirs, tanks, pumps, valves, junctions and pipes | Indexed type-checked insertion and dependency-safe deletion, including related changes in one batch |
| Demand points and simple/rule controls | Indexed insertion/deletion and reviewed reference-changing updates |
| Map labels | Indexed insertion/deletion and supported scalar updates |
| Other edits, unsupported structural/metadata changes | Full canonical network loading and validation fallback |

Examples of enforced domain constraints include valid node types for pipe/pump/valve endpoints; curve/pattern types; assigned demand-point junctions being actual pipe endpoints; typed rule premises and actions; surviving inbound references; and network metadata/MSX dependencies. Safe deletion may remove a pipe and its otherwise-unreferenced junction in one transaction. Cross-network UUID ownership is checked.

`HydraulicEditDiagnostics` reports whether indexed scalar, topology, membership, catalogue, physical or dependent-update validation was used, and how many changed entity payloads the fast path validated. Physical value rules ([model value rules](model-value-rules.md)) are checked for every edited entity before any path, fast or full, may store it. **Fast-path availability is deliberately conservative**: it is not a guarantee that any arbitrary model edit is O(1). Full-model fallback is authoritative where local validation cannot establish safety.

## Whole-network merging

Hydraulic branches should use `mergeHydraulicBranches()` rather than the generic record merge. The hydraulic API can reconcile non-overlapping fields, accept explicit field/whole-object conflict choices, and coordinate compatible chunk append/deletion changes. Every newly combined divergent model is validated before publication. See [hydraulic merging](native-hydraulic-merging.md).

## Format, performance and limitations

The serializer, ordered membership and persistent reference-index formats are part of the pack repository format. Indexed entity reads and suitable edits avoid decoding the whole network, while full saves, fallbacks and proposed merged-snapshot validation still work over the whole aggregate. Solver feasibility and arbitrary conflicting membership reorder resolution remain separate work. The REST service also exposes canonical complete-network reads/saves, revision-pinned targeted entity reads, atomic validated edits and semantic merges, reusing this native library. See [REST design](rest-api-design.md) for HTTP request envelopes, size limits and operations still planned.

Run `./compile_linux_tests.sh --both`; the Qt-dependent `aowis-native-hydraulic-network-tests` checks serialization, edits, ownership, branch behavior, integrity, validated merging, historical revision reads and concurrent read/write snapshot consistency.
