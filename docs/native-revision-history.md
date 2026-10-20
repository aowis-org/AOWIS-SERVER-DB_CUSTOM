# Native revision history and historical reads

The native repository already persists immutable, SHA-256-addressed revision
objects. Each revision records its model-data Merkle root, incoming-reference
Merkle root (when present), and ordered parent revision hashes. An edit creates
another revision; earlier revision objects and their data remain immutable.

## Browsing history

`Repository::history(branch, maxCount, after)` enumerates newest to oldest
along the **first-parent** chain of a branch. The default page size is 100,
and the maximum is 1,000. Pass the last returned revision's `.id` as the
exclusive `after` cursor to request the next page. An unrelated-branch cursor,
a zero hash, or a second-parent-only merge cursor is rejected. The initial
empty revision is returned as the final history entry; requesting the page
after that entry returns an empty vector.

A branch may advance between page requests; continuing from a still-reachable
first-parent cursor does not duplicate or omit earlier revisions. These
operations read immutable metadata but do not create a snapshot-wide reader
transaction across separate API calls. AOWIS should store the revision ID it
is displaying and use revision-specific reads for the rest of the view.

`Repository::revision(id)` returns revision metadata by content hash. It
rejects zero, absent and malformed revision objects. Revisions written by the engine have no parent (initial revision), one parent
(normal commit), or two parents (conflict-free three-way merge). The first
parent is the previous target head and the second is the merged source head.
See [native branch merging](native-branch-merging.md).

## Historical data and comparisons

- `getAtRevision(revisionId, uuid)` reads the raw value at that revision.
- `getModelAtRevision(revisionId, uuid)` returns a typed `ModelRecord` only
  when the selected revision is model-indexed.
- `referencingModelAtRevision(revisionId, uuid)` reads the exact historical
  reverse-reference index; it does **not** use the current branch head.
- `diffRevisions(before, after)` computes added, modified and removed UUID
  entries by comparing the two immutable Merkle roots, even across branches.
  Each `Difference` reports the old and new **object hashes**, not payloads.

These are generic native storage APIs. They do not yet expose a historical
`NetworkHydraulic` aggregate or human-readable field-wise change log.

## Branching from an older revision

`createBranchAtRevision(newName, sourceBranch, revisionId)` creates a new
branch pointing to a revision reachable through the source branch's full
parent graph. It does **not** move or rewrite the source head and does not
create a new revision. The operation validates reachability and publishes the
new branch ref atomically under the repository lock.

AOWIS can use this for a historical inspection/edit branch or for an explicit
restore workflow, rather than destructively moving an existing branch head.
A full undo/redo action system and hydraulic field-wise conflict resolution
are not yet implemented; object-level merging is described in
[native branch merging](native-branch-merging.md).

## Security and retention

The low-level APIs that accept a revision ID are intentionally content-hash
addressed and currently **not authorization-gated**: a caller with direct
repository access can read any retained revision it knows the hash of, whether
or not that revision belongs to a particular branch. Do not expose them to
untrusted remote users without a separate project/user/permission layer.

Compaction traces all active branch heads and their parent histories, retaining
data reachable from those revisions. Old revisions therefore remain readable
after compaction as long as they are retained by a branch ancestry path.
Unreferenced revisions are eligible for garbage collection. No pre-release
format migration is introduced by these APIs.
