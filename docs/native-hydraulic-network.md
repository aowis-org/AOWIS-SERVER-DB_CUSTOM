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

An implementation may perform efficient UUID-indexed entity editing,
copy-on-write membership updates and Merkle-subtree reuse. A full-network
save may re-encode the whole aggregate, while targeted edits reuse unchanged
records. Both paths must produce a complete, deterministically validated
candidate state, not partially publish independent entity mutations.

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
