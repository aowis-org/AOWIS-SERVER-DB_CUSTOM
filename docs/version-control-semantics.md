# DVMDB version-control semantics and full-history graph contract

**Decision record — agreed 2026-10-27.** This document is the canonical
product/engineering contract for projects, revisions, branches and HEADs in
AOWIS. It distinguishes **agreed semantics**, **implemented DVMDB behavior**,
and **work not yet implemented**. The all-parent graph API is now implemented
in DVMDB; the separate [UUIDv7 identity migration
roadmap](uuidv7-identity-migration-roadmap.md) records the agreed transition to
UUIDv7 **logical identities** plus SHA-256 **integrity**. New project IDs
already use UUIDv7; branch/revision UUID persistence remains **unimplemented**.
Current references to revision `Hash` and branch names below
describe the **legacy implemented representation**, not the target ID scheme.

Related documents: [native architecture](architecture.md),
[revision-history API](native-revision-history.md),
[merges](native-branch-merging.md),
[GUI/API backlog](gui-integration-requirements.md), and
[roadmap](roadmap.md). The [OpenAPI file](dvmdb-http-api.yaml) documents
implemented HTTP endpoints only.

## 1. Vocabulary and ownership — agreed

| Concept | Contract | Where it lives |
|---|---|---|
| Server/database source | A GUI representation of a remote DVMDB service or embedded local database. It is **not a revision-graph object**. | GUI connection/profile layer |
| Project | A named, UUID-identified entity with its own independent versioned repository. Multiple projects may be hosted by one service. **New IDs use UUIDv7; existing UUIDv4 projects remain readable.** | DVMDB `ProjectService` |
| Revision / commit | The **same concept**: an immutable revision with ordered parent revision IDs, bound cryptographically to a complete snapshot. **Target: UUIDv7 identity and separate SHA-256 content hash; today SHA is also the ID.** | DVMDB repository |
| Branch | A user-named, movable reference to **one** revision; not a revision and not an immutable ancestry node. **Target: stable branch UUIDv7 independent of its name.** | DVMDB repository refs |
| Branch HEAD | The immutable revision **currently referenced by a particular branch**. Each branch has exactly one current HEAD. | DVMDB branch ref |
| Selected revision | The object the user highlights or inspects; selection must not move any HEAD. | GUI |
| Working-copy base | Immutable revision from which a future editor session loads. Different from both the current branch HEAD and the highlighted revision. | Future GUI session |

AOWIS deliberately shows **all reachable history from every branch of every
visible project**, rather than implicitly hiding all branches except one
selected branch. The GUI may draw server and project cards above revision
history. Neither card becomes a revision or a parent edge in storage.

### Branch-local HEAD rather than Git's single checked-out HEAD

AOWIS uses `HEAD` to mean **branch tip** (`head(branch)`), so `main HEAD` and
`feature HEAD` may exist simultaneously. DVMDB has no repository-wide
checked-out HEAD and no server-global currently editable revision. That is
intentional for a multi-user service. A branch name is chosen by the user;
`HEAD` is the role of the revision to which that name currently points.

A revision may be the HEAD of **several** branches. A HEAD is **not required to
be a leaf** of the project's combined DAG: another branch may have committed
descendants of the revision while this branch remained at it. Displaying
`main · HEAD` and `feature · HEAD` together is therefore correct, whereas
labelling only global DAG leaves HEAD is wrong.

`HEAD` identifies the current *base for continuing work on that branch*, **not
a mutable revision**. Saving edits makes a **new immutable revision** and
advances the chosen branch's ref; it never changes old revision content.
Historical revisions can be read and used to start another branch. Selecting
or opening an old revision must not rewrite an existing branch.

## 2. Revision ancestry, branch references and the graph

In the diagrams below, time/topological ancestry runs **top to bottom**. The
arrows are a display convention; the stored revision references point in the
opposite direction, **from child to parent**. Labels in `[]` are *references*
attached to the revision, not extra ancestry nodes.

### Linear history

```text
Database source (GUI)
         |
Project A (DVMDB)
         |
        R0                parents = []
         |
        R1                parents = [R0]
         |
        R2 [main · HEAD]  parents = [R1]
```

`R0` is the initial, immutable empty raw revision created by the current
project/bootstrap process; it is not a separate mutable project state.

### Divergence and simultaneous branch HEADs

```text
        R0
         |
        R1
         |
        R2
       /  \
      R3   F3
      |     |
      R4    F4 [feature · HEAD]
      [main · HEAD]
```

The fork is represented by `R3.parents = [R2]` and `F3.parents = [R2]`.
`main` points to `R4`; `feature` points to `F4`. No branch-object ancestry
node is stored between R2 and its descendants.

### Two branches sharing one HEAD, including a non-leaf HEAD

```text
       R0
        |
       R1 [main · HEAD]
        |
       F2 [feature · HEAD]
```

`main` still points to R1, even though feature's F2 descends from R1.
Similarly, immediately after branching from `main`, both named refs can
point to precisely the **same** revision, which is drawn only once with both
labels. Do not duplicate revision objects for each branch.

### Divergent merge

```text
           R1
          /  \
         R2   F2 [feature · HEAD]
          \  /
           M [main · HEAD]
```

`M.parents = [R2, F2]`: the first parent is the previous target branch
HEAD and the second is the source branch HEAD. A fast-forward can move the
target ref directly to an existing descendant without making M. Merging does
**not** delete or move the source branch. Full graph enumeration must follow
**both** parents; a first-parent-only list misses the F2 side of this merge
unless it happens to be reached through another queried ref.

**Graph ordering rule:** an ancestor appears above all its displayed
descendants. The order among unrelated siblings is not guaranteed to match
wall-clock time, because current native revisions carry no author/time/message
metadata. The renderer must produce stable, legible placement without
inventing historical timestamps or branch-origin events.

## 3. How DVMDB creates, advances and merges branches — implemented

| Action | Actual native operation and effect |
|---|---|
| Create project | `ProjectService::createProject()` initializes a new repository with `main` referencing an empty initial revision. |
| Create a source-less branch | `Repository::createBranch(name)` points it at the canonical empty raw revision; **does not** fork the current `main` HEAD. |
| Branch from a branch | `createBranch(name, fromBranch)` copies that branch's HEAD **at creation time**; no new revision. |
| Branch from historical revision | `createBranchAtRevision(name, sourceBranch, id)` points the new name to a revision reachable by **either** parent from the named source branch. Source remains unchanged; no new revision. |
| Commit/save | A successful commit creates an immutable child revision and publishes it as the chosen branch HEAD. Other refs do not move. |
| Fast-forward merge | Advances the target to an existing descendant; source remains at its tip. |
| Divergent merge | Creates a revision with ordered parents `[oldTargetHead, sourceHead]`, then moves only the target ref. Conflicts do not publish a revision or move a branch. |
| Bundle import | Does not rewind or silently overwrite divergent refs; already-contained incoming history is treated as up-to-date. |

There is currently **no public arbitrary branch-reset/rewind operation**.
The agreed AOWIS user rule is that an existing branch's HEAD is not manually
moved backwards: create a new branch from the selected historical revision
instead. Commits, merges and imports should continue to preserve this rule;
make it an explicit regression test whenever a new ref-changing API is added.
This rule describes the supported APIs, not a claim that editing repository
files outside DVMDB is safe or impossible.

The GUI must explicitly distinguish *create from empty*, *fork current HEAD*
and *fork selected historical revision*. In particular, a project-level
“new branch” gesture must not silently mean both “empty” and “from `main`”.

## 4. What DVMDB does **not** store

- No branch creation-event object, original source-branch identity or
  permanent branch-off timestamp. Revision divergence shows **where histories
  split**, not necessarily when a particular branch name was created.
- No persistent branch-parent hierarchy. Creating `feature` from `main` copies
  a revision hash; it does not establish `feature.parentBranch = main`.
- No global checked-out HEAD, working copy or server-wide selected branch.
- No immutable commit message, actor or timestamp in the current revision
  record. The revision currently contains `id`, content/index roots,
  `modelIndexed` and zero/one/two ordered parents.

Therefore a future GUI may render a separate **clickable branch reference
card** or a label at the referenced HEAD, but must **not** insert that card
into revision ancestry or claim it is the historical branch-creation event.
The exact branch-decoration style remains a GUI presentation decision.

## 5. Current read surface and missing full-graph contract

**Implemented native:** `Repository::listBranches()`, `head(branch)`,
`history(branch, maxCount, after)`, `revision(hash)` and
`revisionOnBranch(branch, hash)`; project-scoped service wrappers exist for
list/head/history/revision. `history` yields **newest-first first-parent
history** with an exclusive hash cursor, default page 100 and maximum 1,000.
It is not an all-parent graph iterator. `revision(hash)` returns stored
immutable revision metadata, including both merge parents.

**Implemented HTTP:** `GET /dvmdb/v1/projects/{id}/branches`,
`GET .../branches/{branch}/head`,
`GET .../branches/{branch}/history?limit=&after=`, and
`GET /dvmdb/v1/projects/{id}/revisions/{hash}`. The live
[OpenAPI contract](dvmdb-http-api.yaml) describes these operations. The
current GUI combines bounded first-parent pages and therefore must not claim
to show every reachable revision in a complex merge DAG.

**Implemented in DVMDB, not yet wired into the GUI:**
`Repository::revisionGraph(maxCount, cursor)` and
`ProjectService::revisionGraph(projectId, maxCount, cursor)`, exposed through
`GET /dvmdb/v1/projects/{id}/revision-graph?limit=&cursor=`. The initial
call captures one branch-name → HEAD snapshot under a repository lock. Each
page traverses **both** merge parents, returns each reachable revision once
within that project and reports `complete` / `nextCursor`. Pagination pins
original heads even after branch advances. HTTP cursors are signed,
project-specific, and expire on service restart. The traversal order is a
stable DFS, not a timestamp or global topological sort. The GUI still needs
migration from its existing first-parent catalogue reader.

### Implemented graph-reader contract and remaining acceptance work

1. **Implemented:** capture a **consistent set of branch names and HEAD hashes** for one
   project at one logical observation point. No mixed-time per-branch lists.
   Decide what happens if branches change during a multi-page query (pinned
   head-set/token preferred).
2. **Implemented:** traverse **both** ordered parent links of every reachable revision from
   those pinned heads, deduplicating immutable hashes **within a project**.
   Include branch-name → HEAD mappings in the result, even when heads coincide.
3. **Implemented:** return bounded pages with a stable, resumable traversal cursor and an
   explicit `complete`/frontier indicator. Show parent IDs even if a parent
   has not yet been loaded; GUI must not invent an edge endpoint or falsely
   mark the graph complete.
4. **Partly implemented:** deterministic traversal order, bounds, cancellation, stale/expired
   cursor behavior, failure handling and resource budgets. Different project
   scopes must not be merged merely because hashes match (e.g., identical
   empty roots). Native and HTTP results must obey the **same** contract.
5. **Implemented:** preserve existing first-parent `history` behavior for its current callers;
   introduce full-DAG reading separately, not by silently changing history
   pagination semantics.
6. **Partly implemented:** include regression coverage for deep history, historical forks, two-parent
   merges, shared HEADs, branches whose HEADs are non-leaf revisions,
   concurrent HEAD advances, paging/frontiers, cancelled loads and complete
   deduplication.

**Graph scope:** “full history” means the union of revisions **reachable from
published branch HEADs** in the selected project, not every physically
present unreferenced object. Offline compaction may collect unreachable
history. This contract doesn't make branch creation logs, wall-clock ordering
or remote authentication appear where none is stored.

## 6. GUI display and interaction invariants — agreed

- The database source is a GUI-managed container. Display all selected/visible
  servers and their projects simultaneously; group each project's actual
  revision DAG under its project. Source/project identity namespaces must be
  maintained even where hashes or names coincide.
- Draw ancestry from oldest ancestors at the top toward newer descendants
  below. Forks and merge edges follow actual revision parent IDs.
- Attach every named branch and **HEAD** indication to the immutable revision
  it currently points to. One revision may carry several branch labels;
  HEAD need not be a leaf. Branch label/card clicks may inspect the branch,
  while revision clicks inspect the immutable revision.
- Branch-reference visuals do **not** create revision nodes and do not
  establish parent-child relationships between branches.
- Sidebar selection and graph selection refer to the same identities;
  selecting must not move branch HEADs or unexpectedly re-fit the camera.
- Graph loading and refreshing distinguish complete from partial history.
  The GUI must not show fake revisions to fill missing pages.
- Network checkout and a working-copy session are **deferred**. Eventually a
  working copy pins a base revision, handles dirty edits and saves only using
  the exact expected branch HEAD; stale/conflicting saves preserve local work.

## 7. Decisions still open (not blockers for basic graph semantics)

- The exact style/location of branch reference decorations (badge versus
  clickable sidecard), while maintaining the invariants above.
- Immutable commit metadata format, annotations, authors and timestamps;
  migrations and content-hash compatibility need separate design.
- Retention policies for unreferenced revisions, and optional durable branch
  creation/audit events. Such events would be *metadata*, not revision DAG
  parent links.
- UUIDv7 identity, canonical revision hashing, legacy SHA migration and
  cross-replica mapping are specified for design sign-off in the
  [identity roadmap](uuidv7-identity-migration-roadmap.md). Existing P0 graph
  cursors and budgets are implemented; they require versioning on identity
  migration.
