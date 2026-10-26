# Immutable revisions, branch-local HEADs and historical reads

For the agreed semantics, diagrams, GUI invariants and full-DAG API backlog,
see [version-control semantics](version-control-semantics.md). Branches are
named movable references to immutable revisions, not special revision nodes.
Every branch has its own HEAD; one revision can be the HEAD of several branches,
and a HEAD need not be a leaf of the combined project DAG. There is no
repository-wide checked-out HEAD or persistent branch-parent relationship.

Every native revision is identified by SHA-256 and stores the immutable model Merkle root, reverse-reference root (for model-indexed revisions) and ordered parent revision IDs. Normal commits have one parent; divergent merges can have two. Older revisions remain readable while retained by reachable history.

## History and immutable lookup

- `Repository::head(branch)` returns current branch revision metadata.
- `Repository::history(branch, maxCount, after)` returns newest-first **first-parent** history with an exclusive revision-ID cursor. The public API defaults to 100 entries per page. Its implementation validates cursor ancestry and limits page size.
- `Repository::revision(id)` reads an arbitrary stored immutable revision by hash.
- `getAtRevision(id, uuid)` and `getModelAtRevision(id, uuid)` read object/model values at that revision, not at the moving branch head.
- `referencingModelAtRevision(id, uuid)` reads historical incoming dependencies.
- `diffRevisions(before, after)` compares two immutable UUID-to-hash states using Merkle pruning, reporting additions, modifications and deletions.

First-parent pagination is a browsing view, not the full merge DAG.
The GUI requirement is to show the union of histories reachable from **all**
branch heads, traversing both parents and deduplicating revisions within each
project. DVMDB does **not yet expose a single snapshot-consistent, paginated
full-DAG reader** in either native `ProjectService` or HTTP; this is the next
integration milestone, not an available endpoint. Use both parents for merge ancestry and reachability. When a branch advances during UI browsing, clients should hold the selected revision ID and use historical reads for a consistent view.

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
