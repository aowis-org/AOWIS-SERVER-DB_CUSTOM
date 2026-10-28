# GUI / native / REST integration requirements

**Normative integration contract.** See
[version-control semantics](version-control-semantics.md) for all storage
invariants and [revision history](native-revision-history.md) for the native
read model.

## Required domain model

Native and REST expose project, branch, revision and private working-copy
UUIDv7 identities. SHA-256 record and snapshot hashes remain **separate
integrity fields**. A branch record has a nullable committed HEAD and
immutable `originBranchId` / `forkRevisionId`; it is not a commit vertex.
A new project creates `main` with no committed HEAD or fabricated initial
revision. Working copies are independent mutable session state.

## Required operations

1. **Catalogue:** list sources/projects/branches, including unborn branches,
   stable UUIDv7s, names, current committed HEADs and immutable provenance.
2. **History:** fetch a pinned, paginated, complete project DAG with all
   branch HEADs, zero/one/two committed parents and SHA-256 bindings; reject
   invalid graph closure or changing pinned HEADs.
3. **Checkout:** make an owned private working copy based on an exact
   committed revision UUIDv7, or null for an unborn first checkout.
4. **Edit:** change only that working copy, retaining dirty state and
   revision base across UI navigation or reconnection.
5. **Commit:** submit the validated model with target branch UUIDv7,
   expected committed HEAD UUID/null and idempotency key; only a successful
   atomic compare-and-swap creates a finalized revision and moves the HEAD.
6. **Conflict/reconcile:** on stale expected HEAD, preserve local edits,
   fetch the new committed head, compare/merge and validate the complete
   hydraulic state before an explicitly requested subsequent commit.
7. **Branch from revision:** accept the exact immutable source revision UUID,
   optional source branch UUID, store both creation-origin identities; do not
   create a revision just to name the branch. Uncommitted edits require an
   explicit commit-first or exclude-edits choice.
8. **Merge:** use expected source/target committed HEADs, both ordered parent
   IDs for a divergent merge and hydraulic-aware validation; no silent reset
   of private working state.

HTTP must distinguish invalid IDs, missing objects, forbidden operations,
validation failure and **409 stale HEAD**; a missing expected HEAD is not an
explicit `null` expected HEAD. Uncertain write results require a safe
idempotency query rather than blind automatic retry. The native service must
provide equivalent behavior.

## Graph and inspector invariants

- Branch lanes organize committed revision histories; revisions alone form
  ancestry vertices. A branch is not a leaf dangling from a revision.
- Shared immutable revisions remain a **single logical UUIDv7 object**.
- The branch inspector shows `originBranchId` and `forkRevisionId` without
  a second provenance edge in the revision history graph.
- A private working copy is displayed as an editable workspace, not a
  committed HEAD node. Its pinned base may differ from current branch HEAD.
- A project with no commits displays `main` and `No commits yet`.
- UI selection/filtering/camera/inspector changes must not mutate refs,
  create revisions or discard uncommitted work.

## Validation matrix

- Empty project, first commit and independent branch creation.
- Simultaneous working copies; stale-base rejection and safe reconciliation.
- Two divergent commits and two-parent merge.
- Shared and non-leaf branch HEADs.
- Branch rename and provenance surviving restart/replication.
- Invalid hashes, unbounded/partial graph results, offline read-only states.
- Native versus REST agreement on UUIDs, nullable HEADs, integrity hashes
  and 409 semantics.

The acceptance target is this contract, not a particular version of a
prototype GUI or backend route.
