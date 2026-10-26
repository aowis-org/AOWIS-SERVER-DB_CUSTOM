# Immutable revisions and historical reads

Every native revision is identified by SHA-256 and stores the immutable model Merkle root, reverse-reference root (for model-indexed revisions) and ordered parent revision IDs. Normal commits have one parent; divergent merges can have two. Older revisions remain readable while retained by reachable history.

## History and immutable lookup

- `Repository::head(branch)` returns current branch revision metadata.
- `Repository::history(branch, maxCount, after)` returns newest-first **first-parent** history with an exclusive revision-ID cursor. The public API defaults to 100 entries per page. Its implementation validates cursor ancestry and limits page size.
- `Repository::revision(id)` reads an arbitrary stored immutable revision by hash.
- `getAtRevision(id, uuid)` and `getModelAtRevision(id, uuid)` read object/model values at that revision, not at the moving branch head.
- `referencingModelAtRevision(id, uuid)` reads historical incoming dependencies.
- `diffRevisions(before, after)` compares two immutable UUID-to-hash states using Merkle pruning, reporting additions, modifications and deletions.

First-parent pagination is a browsing view, not the full merge DAG. Use both parents for merge ancestry and reachability. When a branch advances during UI browsing, clients should hold the selected revision ID and use historical reads for a consistent view.

## Branch creation from history

`Repository::createBranchAtRevision(newName, sourceBranch, id)` creates a new branch at a revision reachable from the source branch's **full parent graph**, without rewriting or moving the source and without adding a new commit. It is suitable for inspecting or continuing work from a historical state.

Compaction follows parent revision graphs from active heads and preserves their referenced objects. Revisions no longer reachable through any retained branch may be collected.

## Service exposure and limitations

Historical native methods are direct library operations, not authorization-gated by the storage engine. Project-scoped branch history, immutable revision metadata and revision diffs are exposed by the implemented HTTP/TCP service. Authentication and authorization are not enforced by that service pending an AOWIS-wide security design. See [service API](native-database-service.md) and [OpenAPI](dvmdb-http-api.yaml).
