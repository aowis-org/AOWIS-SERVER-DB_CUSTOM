# Hydraulic network storage, checkout and validated commits

`aowis::dvmdb::hydraulic` persists canonical AOWIS `NetworkHydraulic`
models, indexed entities and ordered membership. Its editor lifecycle
follows [version-control semantics](version-control-semantics.md): editing
occurs in private working copies, while committed revisions are immutable.

## Canonical model structure

A network root is one versioned model record with metadata and an ordered
manifest of membership-chunk UUIDs. Chunks hold at most 256 ordered member
UUIDs. Each top-level entity is an independently addressed canonical model
record; a reverse-reference index supports ownership and deletion checks.

The 18 top-level collections are pipe materials; time patterns; tank volume,
pump head, pump efficiency, valve headloss, valve characteristic and general
curves; reservoirs, junctions, tanks; demand points; pipes, pumps, valves;
simple controls, rule controls and map labels. Nested MSX settings and map
backdrop data belong to the appropriate network/model records, not
invented top-level entities.

Canonical encoding validates UUID kind, uniqueness, finite numeric values,
reference closure, membership order and applicable domain constraints. A
change to an existing UUID must not silently change its entity kind.

## Immutable reads

Reading a complete network or a targeted entity must pin one **committed
revision UUIDv7** (or access a user's own working-copy model explicitly).
Across several API calls, clients must specify the same committed revision
UUID to avoid mixing generations while a branch HEAD moves.

Historical reads are read-only. A historical revision cannot be edited in
place or implicitly selected as a branch's new HEAD. Opening the most recent
committed branch HEAD creates a **separate** private mutable working copy.

## Private checkout and editing

Checkout pins project UUIDv7, branch UUIDv7, committed base revision UUIDv7
(or explicit null for an unborn project), and working-copy UUIDv7 with
owner/session. Multiple users may base independent working copies on the
same committed revision. Each editor's edits change only their own copy.

DVMDB uses project-scoped UUIDv7 revisions for committed hydraulic operations.
The canonical native C++ entry points are `saveHydraulicNetwork`,
`editHydraulicNetwork`, `loadHydraulicNetworkAtRevision`,
`listHydraulicEntityIdsAtRevisionLimited`,
`loadHydraulicEntityAtRevision` and `mergeHydraulicBranches`.
Their write interfaces require a pinned expected committed HEAD; an unborn
project is edited through a private draft rather than by inventing a commit.

Currently the committed UUIDv7 edit implementation validates and re-encodes
the full network. The older SHA-revision indexed-edit suite still tests
bounded entity and membership updates, but those optimizations must be
ported to the project-scoped API before the older implementation is removed.
Full-network save and targeted edit must ultimately share one validated
candidate/write engine without losing indexed-update behavior or tests.

## Commit and concurrency

Committing a hydraulic working copy requires exact target branch UUID and
`expectedHeadRevisionId` from checkout. The full candidate network must
pass canonical reference and hydraulic-domain validation. Only then may
the service create a fresh immutable revision UUIDv7, persist its SHA-256
record/snapshot roots, and atomically move the branch's committed HEAD.

When another client has advanced HEAD, the stale commit fails. The working
copy remains available for compare/reconcile, and model-wide validation
runs again before any subsequent commit. Two edits to different UUIDs may
still conflict semantically (connectivity, endpoint type, membership
constraints). No silent overwrite is allowed.

## Branch merge and versioned model state

A merge pins both source and target committed HEADs, computes an ancestry-
based three-way candidate and validates the entire resulting hydraulic
network. A successful divergent merge publishes one immutable two-parent
revision. The source branch remains unchanged; outstanding private working
copies are never implicitly consumed. See [hydraulic merging](native-hydraulic-merging.md)
and [REST contract](rest-api-design.md).

## Verification

Tests cover canonical encoding/decoding, branch-local immutable reads,
private checkout ownership, concurrent edits against one base, stale-HEAD
rejection, complete-domain merge validation, canonical SHA-256, UUIDv7
identity preservation, and crash-safe HEAD publication.

## Draft event membership materialization

For an existing hydraulic network, the private working-copy
`POST /dvmdb/v1/working-copies/{draftId}/save-events` route accepts
canonical MODEL `events`, typed entity `changes`, and an optional bounded
`membership` array describing inserts/removals (`networkId`, `entityId`,
`kind`, `collectionIndex`, `inserted`). DVMDB, not the GUI, owns the native
manifest and chunk format. It validates canonical member payloads and
corresponding event before/after images, then includes affected native chunk
changes in the same fenced, generation-checked durable draft replacement.

This is **draft persistence**, not a commit or branch-head change. Membership
materialization requires an existing network root in the pinned base or saved
candidate. Network root creation and network-wide metadata edits require
separate handling. The HTTP route and service implementation exist, but the
full GUI hydration/queue/autosave/reopen integration is still incomplete; see
[working-copy storage](working-copy-storage-v1.md) and the
[current roadmap](roadmap.md).
