# Immutable revisions, branch-local HEADs and historical reads

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
