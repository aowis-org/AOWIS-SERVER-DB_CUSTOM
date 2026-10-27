# Immutable revisions, branch-local HEADs and historical reads

Branch reference storage is being replaced with UUIDv7-only files; see [BRANCH-1 format](uuidv7-branch-ref-format.md). Revision IDs remain SHA-256 until the separate revision-record v2 implementation.

For the agreed semantics, diagrams, GUI invariants and full-DAG API backlog,
see [version-control semantics](version-control-semantics.md). Branches are
named movable references to immutable revisions, not special revision nodes.
Every branch has its own HEAD; one revision can be the HEAD of several branches,
and a HEAD need not be a leaf of the combined project DAG. There is no
repository-wide checked-out HEAD or persistent branch-parent relationship.

**Currently** every native revision is identified by SHA-256 and stores the immutable model Merkle root, reverse-reference root (for model-indexed revisions) and ordered parent revision IDs. A future [UUIDv7 migration](uuidv7-identity-migration-roadmap.md) separates logical revision identity from immutable SHA-256 record and snapshot integrity; it is not implemented yet. Normal commits have one parent; divergent merges can have two. Older revisions remain readable while retained by reachable history.

## History and immutable lookup

- `Repository::head(branch)` returns current branch revision metadata.
- `Repository::history(branch, maxCount, after)` returns newest-first **first-parent** history with an exclusive revision-ID cursor. The public API defaults to 100 entries per page. Its implementation validates cursor ancestry and limits page size.
- `Repository::revision(id)` reads an arbitrary stored immutable revision by hash.
- `getAtRevision(id, uuid)` and `getModelAtRevision(id, uuid)` read object/model values at that revision, not at the moving branch head.
- `referencingModelAtRevision(id, uuid)` reads historical incoming dependencies.
- `diffRevisions(before, after)` compares two immutable UUID-to-hash states using Merkle pruning, reporting additions, modifications and deletions.

First-parent pagination is a browsing view, not the full merge DAG.
The separate `Repository::revisionGraph(maxCount, cursor)` and
`ProjectService::revisionGraph(projectId, maxCount, cursor)` expose the union
of all revisions reachable from every branch HEAD, following **both** merge
parents. The first request captures the sorted branch-name → HEAD set under
one repository/process lock; subsequent pages replay exactly that immutable
snapshot even if HEADs move. Results deduplicate hashes within the project.

A `RevisionGraphPage` contains `heads`, `revisions`, and an optional `next`
`RevisionGraphCursor` (`heads` + count of unique revisions already emitted).
No `next` means complete. A page contains 1..1000 revisions except for a
complete smaller/empty graph. Traversal follows a stable DFS, not a global
chronological/topological order. Do not infer commit timestamps or branch
origin events. Parents not yet present in the client's page cache are
normal **frontier edges**, not missing data. Graph display must wait for or
indicate the remaining pages. A new request without a cursor sees the current
HEADs. The API does not mutate storage.

Graph paging is stateless: each later page replays the pinned ancestry from
the start and skips already emitted unique hashes. This costs O(prefix length)
per page; it avoids server session retention but is not optimal for very large
repositories. A single call has a 1,024-branch and 250,000-revision traversal
budget and fails with a clear limit error rather than returning an incorrect
partial graph. The REST continuation token is authenticated and project-bound;
server restart invalidates it (re-fetch the first page). These budgets and
large URL tokens are candidates for a future bounded streaming or persistent
index design, not a reason to pretend this first API is infinitely scalable.

## UUIDv7 history and full-graph native API (Phase 2d)

New opt-in native APIs `Repository::historyV2(project, branch, limit, after)`
and `Repository::revisionGraphV2(project, limit, cursor)` are also exposed
through matching `ProjectService::historyV2(...)` and
`ProjectService::revisionGraphV2(...)` methods. **These are implemented in
native DVMDB; REST/CLI/default-service and GUI are NOT yet switched.**

- Revision IDs and ancestry references are typed UUIDv7 values, not SHA-256
  hashes. Every response also includes verified immutable record SHA-256;
  ordered parents carry UUIDv7 plus their expected SHA-256. The owner
  project UUID is validated by the resolver, not guessed from hashes.
- `historyV2` preserves the existing **first-parent, newest-first** browsing
  semantics. An exclusive `after` UUID must occur on the branch's current
  first-parent history. It does not traverse the second side of a merge.
- `revisionGraphV2` captures all sorted branch identities (UUIDv7), display
  names, UUIDv7 HEADs and their expected SHA-256 in **one repository-locked
  snapshot**. Following all parent links returns the deduplicated reachable
  UUIDv7 DAG (including both merge parents, shared HEADs and non-leaf HEADs).
- `RevisionGraphCursorV2` pins those branch refs plus the number of visited
  unique UUIDs. Later pages replay the **pinned immutable heads**, so branch
  advancement cannot inject newer revisions into an ongoing read. Missing
  immutable parents and mismatched SHA bindings fail explicitly; parent
  edges across pages are a normal loading frontier, not omitted history.
- Page-size limits are 1..1000; the same 1024-branch and 250000-revision
  budget applies. Cursors are native values, **not an HTTP wire schema**.
  Paging uses replay, so deep graphs are not yet optimized for very large
  server workloads. Never claim a partial page is the complete revision DAG.
- The direct v2 resolver validates each revision's project/UUID/digest and
  every parent UUID-to-expected-hash binding. The full graph reader then
  visits all parents rather than redundantly re-verifying the complete
  ancestry recursively per node. This retains linear per-page ancestry
  validation work before replay overhead.

The SHA-identity `history`/`revisionGraph` APIs still exist during this
pre-release **internal cutover**. They cannot read UUIDv7 HEADs; no legacy
repository conversion or old on-disk format support is being added.

## Branch creation from history

`createBranch(name)` without a source starts at the canonical empty raw root
revision; `createBranch(name, sourceBranch)` starts at that branch's **current**
HEAD without a new revision. Neither operation records a branch-creation event.
A project created via `ProjectService` initializes `main` with the empty
revision. The GUI must not confuse a source-less branch with a fork from
`main`.

`Repository::createBranchAtRevision(newName, sourceBranch, id)` creates a new branch at a revision reachable from the source branch's **full parent graph**, without rewriting or moving the source and without adding a new commit. It is suitable for inspecting or continuing work from a historical state.

Branch HEADs are not manually rewound by the supported public APIs: normal
commits create a child, merges fast-forward or create a two-parent revision,
and bundle import refuses divergent overwrites. An older revision can become
the HEAD of a **new** branch without moving its source branch.

Compaction follows parent revision graphs from active heads and preserves their referenced objects. Revisions no longer reachable through any retained branch may be collected.

## Service exposure and limitations

Historical native methods are direct library operations, not authorization-gated by the storage engine. Project-scoped branch history, immutable revision metadata and revision diffs are exposed by the implemented HTTP/TCP service. Authentication and authorization are not enforced by that service pending an AOWIS-wide security design. See [service API](native-database-service.md) and [OpenAPI](dvmdb-http-api.yaml).

## UUIDv7-only branch references and transfers (BRANCH-1)

Every branch in a new repository has a UUIDv7 identity; its display name and
SHA-256 revision HEAD live in a single atomic record stored under that UUID.
Branch names never become filenames. A branch can be renamed without moving
its record or changing identity or HEAD. Repositories using name-keyed branch
refs are rejected and must be recreated (pre-release breaking change).

Both ordinary and explicitly identified native export functions emit
`AOWSYNC3`/`AOWSYNC4` by default; REST and CLI exports do so as well. These
carry the stable UUIDv7 branch identity, while the immutable revision graph
still uses SHA-256 identifiers until Phase 2. `AOWSYNC1`/`AOWSYNC2` imports
are rejected. The importer rejects identity rebinding and duplicate UUIDs,
even if the revision HEAD happens to be identical. Creating a new branch
from an existing revision creates a new *branch* UUID, never a new revision.

See [BRANCH-1](uuidv7-branch-ref-format.md) and
[transfer format](offline-repository-synchronization.md). There is no legacy
branch-identity migration route under this pre-release storage contract.
