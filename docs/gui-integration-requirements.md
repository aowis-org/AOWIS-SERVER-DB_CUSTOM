# GUI integration: implemented API, agreed graph semantics and remaining work

**Status: 2026-10-27.** The canonical decisions about revisions, branches and
branch-local HEADs are in [version-control semantics](version-control-semantics.md).
That document is the reference for both native/REST implementers and the GUI.
This file is the **remaining integration backlog**, not a second competing
version-control model or a claim that planned routes already exist.

Use [OpenAPI](dvmdb-http-api.yaml) and the
[native database service](native-database-service.md) for **implemented** HTTP
operations. Do not add hypothetical resources to live OpenAPI.

## Current implementation

**DVMDB native and HTTP already provide:** project create/list/get;
branch list/create (source-less, from another branch HEAD, or from a revision
reachable from that branch through either merge parent); branch-local HEAD;
first-parent paginated history; immutable revision lookup and diffs; known
network reads and expected-HEAD full-network saves; atomic entity edits;
validated merges; full/incremental portable bundles. The native repository
persists ordered revision parent hashes and can look up a stored revision by
hash. Native import/merge semantics do not arbitrarily rewind branch refs.

**The GUI already provides:** persisted local/remote database sources,
automatic REST health/catalogue monitoring, per-source graph visibility,
project/branch creation and read-only browsing of *bounded first-parent*
history, with a Qt-independent graph placement component. It does **not** yet
have complete merge ancestry enumeration, network checkout, working-copy
saving, project/branch rename/delete or review/annotation workflows. GUI
server cards and selected revisions are presentation/session state, **not**
new DVMDB revision objects.

**Not implemented as infrastructure:** REST authentication/authorization or
TLS; a list-networks-in-project endpoint; immutable commit author/message/
timestamp records; revision annotations and rich project/branch metadata;
a single consistent, paginated full-DAG API.

## P0 — complete multi-branch history graph (next milestone)

Current `history(branch, maxCount, after)` follows **only the first parent**;
requesting each branch independently and merging the responses does not
promise a full graph. We have agreed that the GUI should display **every
revision reachable from all branch HEADs of every visible project**, with
both merge parents and one revision card per immutable revision identity per
project. Each branch carries its own `HEAD` marker; HEAD is not necessarily a
global graph leaf. See [the graph contract](version-control-semantics.md#5-current-read-surface-and-missing-full-graph-contract).

Implement in this order:

1. **Native repository graph reader:** capture a coherent branch-name → HEAD
   map and traverse all ordered parents. Deduplicate revision IDs within the
   project. Expose bounded pages, stable cursors or a pinned head-set token,
   frontier/missing-parent indicators and an explicit complete flag; define
   how concurrent branch moves, expiry and cancellation behave.
2. **ProjectService equivalent:** hold the appropriate project scope and
   serialize only where necessary for a coherent snapshot. Avoid treating
   independent read calls as one atomic snapshot without an explicit pin.
3. **REST equivalent:** implement a bounded HTTP resource with the same model,
   response limits and errors as native. Add OpenAPI **only with working code**.
   `GET /dvmdb/v1/projects/{id}/revision-graph` is a suggested route name,
   **not a deployed endpoint**.
4. **GUI adapter:** both embedded and REST consume identical project/branch/
   revision graph data. Preserve server/project namespaces (different
   projects can have identical revision hashes). Indicate partial/paged data
   rather than rendering a seemingly complete but truncated graph.
5. **Regression/acceptance:** one linear branch; two refs sharing HEAD;
   historical branch fork; non-leaf branch HEAD; divergent branches and
   two-parent merge; fast-forward; many projects/sources with overlapping
   hashes; deterministic paging; branch advancement mid-load; cancellation
   and resource bounds. Add real native and loopback HTTP contract tests.

A Git-style visualization is compatible with branch refs: **no persisted
branch-creation node is required**. A clickable branch reference decoration
may be rendered at the revision it currently references. It must not masquerade
as a historical fork event or an ancestor edge.

## P1 — network discovery, checkout and optimistic saving (deferred by decision)

The GUI's future working-copy/session tracks **selected revision**, **pinned
base revision**, **branch and its observed HEAD**, and **dirty local model** as
different fields. Inspecting an older revision does not rewind the branch.
Saving is a new immutable revision based on the exact expected branch HEAD;
HTTP mutation endpoints already accept `expectedHead`. Native integration
must enforce the same rule for GUI sessions even where low-level APIs accept
an optional expected HEAD. On 409/conflict, preserve local changes and make
reconciliation explicit; do not blindly retry a write.

Needed before checkout UX is complete:

- Enumerate hydraulic networks at a pinned project/branch revision, with UUID,
  display name and summary. Current routes read known networks and entities;
  they do not discover *which networks exist* in a project.
- Specify empty-project/first-network creation behavior and revision-pinned
  reads across multi-request loading. Never silently choose a random network.
- Build GUI working-copy/session lifetime, dirty-state, Save/Discard/Cancel,
  failed-save recovery and network editing without binding the working state
  to a transient tree selection or refresh.
- Add native and HTTP checkout/save integration tests, including concurrent
  editors, stale `expectedHead`, historical inspection, cancellation and
  success/failure publication ambiguity.

Network checkout/save is **not blocked** on implementing project/branch
rename/delete, rich annotations, complete security or commit metadata; those
are distinct initiatives and must not be presented as prerequisites to a
minimal trusted-development workflow. Security **is** a prerequisite before
exposing the HTTP service to untrusted networks.

## P2 — management and richer metadata (separate roadmap items)

- **Project lifecycle:** rename, archive/delete/restore, stable UUID,
  concurrency guards, case/Unicode naming policy, confirmation, retention and
  administrative permissions. Implemented today: create/list/get only.
- **Branch lifecycle:** rename, archive/delete/restore, branch-name migration
  policy or explicit stable ID, default/protected branch rules and
  race-protected mutable metadata. Existing `createBranchAtRevision()` already
  supports historical forks; do not list that as a missing primitive.
- **Immutable commit metadata:** commit message/title, author/actor, server UTC
  time, description, provenance, client/schema version and ordered parents.
  Decide deterministic canonical encoding and what participates in the
  revision hash before any format change; provide migration/bundle readers.
- **Mutable revision annotations:** versioned description, editable labels,
  validation/review status, notes, external links and search. These records
  must not mutate the immutable revision or its hash. Specify concurrency,
  auditability, indexing and permissions separately from commit metadata.
- **Branch metadata:** description/purpose, creator/time, ownership, review
  state, protections, optional default flag. Keep mutable metadata versions
  separate from HEAD revisions. **Actual branch-creation history is not
  recoverable from current refs**; introducing durable events is a separate
  design choice, not necessary for DAG drawing.
- **Project metadata:** description, tags, category, owners, timestamps,
  archive state and search/filter/bulk operations with stable IDs.
- **Tags and labels:** distinguish editable freeform labels from named release
  refs pointing at immutable revisions. Decide release-ref move/protect/delete
  rules; do not conflate them with branch HEAD.
- **Review/provenance:** author/approver decisions, validation, signatures,
  integration references and activity/audit event feeds.
- **Comparison and merges:** paginated/streaming diffs (current response caps
  at 1,000 changes/413), non-mutating dry-run merge, pinned conflict choices,
  preview tokens or revalidation; current `POST /merges` performs a write on
  success and is **not** a dry run.

## Cross-cutting requirements and release boundary

- Define the AOWIS-wide identity, TLS, permissions and audit system before
  untrusted deployment. Today's plain HTTP endpoint does not authenticate or
  authorize callers. Do not mistake provisional native access-policy code
  for secured REST enforcement.
- Provide idempotency keys where useful, structured `not_found`/`conflict`
  errors, `expectedHead` or metadata ETags, explicit retryability and no
  last-writer-wins behavior. Keep HTTP/native behavior aligned.
- Preserve portable bundle compatibility when adding metadata, refs,
  retention or lifecycle operations. Distinguish exported shared history from
  local-only permissions and audit trails.
- Bound server memory/work per request, paginate, support cancellation,
  deterministic cursors, deadline/reconnect behavior and incremental updates.
- If any future metadata store uses SQL, **every query, migration,
  concurrency transaction and paging strategy must work with both SQLite and
  PostgreSQL**. The current native engine does not use those SQL backends.

## Order and status discipline

**Now:** implement P0 graph enumeration and both backends; test with real merge
histories. **Later by explicit product decision:** P1 working copy and network
editing. **Separately:** P2 metadata, lifecycle, review and security.
Maintain implementation status in this file and the
[roadmap](roadmap.md); keep the [semantics contract](version-control-semantics.md)
independent of scheduling changes. Test names, OpenAPI and examples should
never imply a feature exists before it ships.
