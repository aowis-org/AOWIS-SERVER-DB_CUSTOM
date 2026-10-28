# AOWIS / DVMDB version-control contract

**Normative product and system-design specification.** All native storage,
service, REST, replication and GUI behavior must conform to this contract.
This document defines behavior; it does not claim that every operation is
already implemented. [Native architecture](architecture.md),
[revision API](native-revision-history.md),
[merge semantics](native-branch-merging.md), and
[GUI integration](gui-integration-requirements.md) elaborate the same model.

## 1. Concepts and terminology

| Concept | Meaning | UUIDv7 | Mutability |
|---|---|---|---|
| Database/source | Server or embedded store hosting independent projects; a GUI connection may represent it | Connection/profile identity where applicable | Connection details, catalogue |
| Project | Container for one model history, branches, snapshots, and permissions | Project ID | Metadata and refs, not committed content |
| Branch | Persistent, named line of development with a movable pointer to the latest committed revision | Branch ID | Name/HEAD can change; creation provenance cannot |
| Revision / commit | One **finalized immutable** whole-model snapshot, with ordered revision parents | Revision ID | **Never** |
| Branch HEAD | The **latest committed revision referenced by that branch**; nullable for an unborn branch | The referenced revision's ID, not another object | Ref moves atomically |
| Working copy | A **private, mutable editor state** checked out from a committed revision (or the initial empty project state) | Working-copy ID | Editable by its owner/session |
| Base revision | Exact committed revision on which a working copy's current edits were based; nullable for an unborn branch | Revision ID or null | Pinned until explicit reconcile/rebase |
| Fork point | Exact immutable source revision from which a new branch was created | Revision ID or null | Immutable creation metadata |
| Record hash | SHA-256 commitment to a canonical immutable revision record | Not an identity | Never changes for a given revision |
| Snapshot root | Content-addressed Merkle root recursively committing to all persisted model records, references and index roots | Not an identity | Immutable for committed revisions |

**Do not call an uncommitted working copy a revision or HEAD.** A revision is
created **only** when a commit succeeds. A branch can have no committed HEAD
(`null`) before its first commit. `HEAD` is AOWIS shorthand for the tip of a
*specified branch*, not the editable state of a particular user. There is no
server-wide checked-out branch or single global HEAD.

A branch's history consists of the committed revisions reachable from its
HEAD, including shared ancestors and both parents of merge commits. This
is the meaning of *a branch is made of revisions*: the branch identifies a
line of development, but **does not exclusively own** any immutable ancestor.
Two branches can legitimately reference the same revision as HEAD; their
identities and working copies remain independent. A branch HEAD does not
need to be a leaf in the project-wide ancestry graph.

## 2. Identity and integrity

- Give every first-class project, branch, committed revision and working copy
  its own **UUIDv7**. Model entities likewise use their model-defined UUIDv7
  identities, never a SHA-256 hash as a logical object ID. Names are
  user-visible labels, never storage keys.
- Generate fresh IDs for new objects; reject accidental ID collisions. Keep
  identities stable across rename, export/import, server transport and
  repository reopening. UUIDv7 ordering is **not** commit ancestry.
- Compute SHA-256 over a **canonical, versioned, immutable revision record**
  that binds revision UUID, project UUID, ordered parent revision UUIDs,
  immutable metadata, snapshot root and relevant integrity roots. Include
  parent record-hash bindings, or verify them through a canonical parent map,
  so changing an ancestor cannot pass verification unnoticed.
- The snapshot's Merkle roots recursively bind **all** committed hydraulic
  and model objects; the complete model need not be copied into one flat hash
  input for each commit. Shared immutable subtrees may be deduplicated.
- Working copies may have transient checksums, but do not receive a committed
  `recordHash` and cannot be addressed as finalized revisions.
- Verify canonical hashes, ancestry closure, project ownership, parent
  ordering, UUID uniqueness, and domain references before accepting or
  publishing untrusted history. SHA-256 is integrity, **not authentication**.

## 3. Creating a project: unborn `main`

`CreateProject(name)` must atomically create a new project UUIDv7 and a new
branch UUIDv7 named `main`, with:

- `main.headRevisionId = null` (an **unborn** branch);
- no committed revisions, no empty synthetic revision R0;
- an empty, editable model state that an editor can check out into its **own
  working copy**; no globally shared mutable HEAD;
- no creation-origin branch or fork revision for `main`.

The project and empty branch can be displayed immediately; the history area
contains **zero committed revision cards**. Starting an editor session makes
(or opens) a private working copy, not a commit. The first successful commit
creates immutable revision R0 (zero parents) and moves `main.head` from null
to R0 in one conditional publication.

## 4. Branch lifecycle and provenance

Creating a branch **from an already committed revision** never creates a
commit and never mutates the source branch. Supply its exact `sourceRevisionId`
and, if the user selected an originating branch, `sourceBranchId`.

DVMDB must persist both as immutable creation metadata:

- `originBranchId`: source branch UUIDv7, when known;
- `forkRevisionId`: exact finalized revision UUIDv7 at creation, when one
  exists (not the source branch's current or future HEAD).

For a new branch from committed Rn:

1. Verify that Rn exists in the same project, is finalized, and is reachable
   from the specified source branch when that source is supplied.
2. Create a new branch UUIDv7, with its own editable-name label.
3. Set its committed HEAD to Rn and persist both creation-origin fields.
4. Return the new branch UUID and exact Rn. **No new revision is created.**

Creating a branch from **a user's uncommitted edits** is a different request.
It must explicitly offer to **commit those edits first** (subject to the
ordinary concurrency check) or start from the last committed base without
the edits. Never silently commit someone else's working copy. An unborn
`main` may be branched only using a deliberately specified empty-state/unborn
policy; it must not be given a fictitious committed fork revision. The exact
handling of multiple unborn branches is defined in the branch-creation API,
not inferred from a missing SHA or zero UUID.

Branches support readable UTF-8 names including spaces. Validate UTF-8,
length and prohibited control/path-delimiter characters; names must be unique
under the repository's documented comparison rule. Rename retains UUIDv7,
HEAD and creation provenance. If the source branch is later renamed, its
origin UUID remains stable. If source branches are removed, do not fabricate
new provenance or erase a valid recorded fork point.

## 5. Working copies and multi-user concurrency

Editing occurs **only** in a separate working copy; no user edits the stored
revision object. A working copy records at least:

- unique `workingCopyId` (UUIDv7), owner/session, owning project UUID;
- selected target branch UUIDv7 (or explicit detached/historical mode);
- exact base committed revision UUIDv7 or `null` for an unborn base;
- mutable model data, dirty state and validation status;
- local or service-managed persistence and recovery policy.

Alice and Bob may each check out `main` at R7, obtaining **distinct private
working copies** that both pin R7. They may edit independently. The number
of working copies does not change the number of branches or committed HEADs.
A working copy may become stale as the branch advances; stale data must not
be discarded or silently promoted to a new base. Access to private drafts
and their cleanup must respect session ownership and eventual authorization.

A read or edit operation spanning multiple model entities must use one pinned
base snapshot. Changing the active GUI selection must never implicitly move
a branch HEAD or change a working copy's pinned base.

## 6. Commit protocol (optimistic concurrency)

Every commit request supplies:

- project UUIDv7, target branch UUIDv7 and working-copy identity;
- `expectedHeadRevisionId`: the committed branch HEAD observed by the editor
  when the working copy's changes were based on it, **including explicit null
  for an unborn branch**;
- proposed complete and validated model snapshot (or a bounded change set
  that deterministically yields it), commit metadata and idempotency key.

DVMDB must perform, as one atomic logical transaction:

1. Validate permissions, project/branch state, complete model consistency and
   canonical references; resolve and hash the exact proposed snapshot.
2. Read and lock (or compare-and-swap) the current branch HEAD. **If it
   differs from `expectedHeadRevisionId`, do not move HEAD and report a
   stale-base conflict.**
3. Create and durably record a fresh immutable revision UUIDv7 with the
   expected HEAD as its sole parent (zero parents for the first commit).
4. Publish the new branch HEAD only **after** the snapshot and immutable
   revision record are durably available. Return the new revision UUIDv7 and
   record SHA-256.

No failed, timed-out or stale commit may overwrite another user's successful
commit. If the server may have published despite an interrupted response,
query the authoritative branch/revision status by request ID or idempotency
key **before retrying**. A rejected publication may leave unreachable prepared
objects, but not an observable partial commit or partially moved HEAD.

A changed HEAD means **concurrency collision**, not necessarily conflicting
content. After an explicit refresh, the editor may compute a three-way
reconciliation between the pinned base, the current branch HEAD and local
changes. Non-overlapping edits may be merged; overlapping or semantically
incompatible edits require user resolution. No automatic rebase, force push,
last-writer-wins or branch reset is allowed as a side effect of ordinary Save.
Hydraulic semantic validation must run on the **complete merged network**, not
merely on independent entity UUID updates.

### Example: two editors

```text
main HEAD = R7  (immutable)
Alice's working copy A: base R7, edits pump P
Bob's working copy B:   base R7, edits pipe Q

Alice commits A with expectedHead R7 -> R8 (parent R7), HEAD becomes R8.
Bob commits B with expectedHead R7 -> CONFLICT; main remains at R8.
Bob reconciles against R8, validates complete model, then commits
  with expectedHead R8 -> R9 (parent R8), HEAD becomes R9.
```

## 7. Revision DAG and merging

The **only** immutable ancestry edges connect committed revisions. Each
revision has zero, one or two ordered parents. Parent UUIDs must refer to
committed revisions in the same project; the graph is acyclic and complete.
One-parent commits advance a development line. Two-parent merge commits bind
both histories; target and source parents have an explicit canonical order.

Branch creation is **not** a revision and does not create a revision-DAG
edge. A committed fork becomes visible as a divergence **only when the
branches make separate commits**. Example:

```text
            R0
             |
            R1  (main and feature may both point here)
           /  \
          R2   F2
          |    |
     main HEAD  feature HEAD
```

The graph need not render a separate edge for `originBranchId` or
`forkRevisionId`. These are provenance metadata, shown in the branch
inspector. A branch can be presented as a *lane* of development while shared
commits are logically single revision objects. GUI lanes and headers must not
create duplicate revision identities.

Merge must compare both source and target committed HEADs against expected
values before publishing. An up-to-date merge is a no-op; fast-forward may
advance the target ref to an already committed descendant; a divergent merge
creates **one** new immutable two-parent revision only after validation.
Where editors have uncommitted changes, merging must not silently overwrite
or consume their private working copies. Source-branch HEAD remains unchanged
unless an explicit separate operation requests otherwise.

## 8. Reading history and graph presentation

- Native and REST readers expose **the same** project/branch/revision UUIDv7
  contract. No SHA digest is used as an entity ID.
- History traversal includes every branch HEAD and both parents of merge
  revisions; graph pagination must pin a stable branch-HEAD snapshot and
  validate closure before presenting a complete graph.
- The GUI displays server/database and project organization, **branch lanes
  with committed revisions** and a clearly marked branch-local committed
  `HEAD`; the independent private working copy is displayed in an editor or
  workspace section, **not** as a committed revision.
- An unborn project displays `main` and a clear **No commits yet** state.
  It must not display a synthetic R0.
- A branch whose HEAD is also another branch's HEAD is legitimate. A HEAD
  may be an ancestor of commits on another branch. Do not assume global DAG
  leaves alone carry HEAD labels.
- No arbitrary dashed branch-origin connection is required in the revision
  graph; provenance belongs in the branch inspector. The graph's history
  edges must mean revision parenthood only. Display links between GUI source,
  project and branch containers are organization, not ancestry.
- UI selection, source filtering, folding, camera movement and inspector tab
  changes are read-only operations; they never change HEAD or working copies.

## 9. API, replication, atomicity and durability requirements

The API contract must make these distinct operations explicit:

- create project -> project UUID, unborn `main` branch UUID, null HEAD;
- list/get branches -> UUID, name, nullable HEAD, `originBranchId`,
  `forkRevisionId`;
- checkout/start working copy -> UUID, ownership, exact base revision/null;
- read immutable revision/snapshot -> UUID, SHA-256 record/snapshot roots,
  ordered parents and immutable metadata;
- create branch from **committed** revision, with optional source branch UUID;
- commit working copy with **exact expected HEAD** and idempotent result;
- get paginated full revision DAG and branch HEADs under a pinned snapshot;
- compare, reconcile and merge with explicit expected HEADs.

REST errors must distinguish invalid input, missing objects, permission
failure, stale expected HEAD (`409 Conflict`), semantic validation failure,
and transient service failure. An absent expected HEAD is **not** equivalent
to an explicit null expected HEAD on an unborn branch.

Offline/full/incremental replication must preserve project/branch/revision
UUIDs, immutable branch-origin UUIDs and fork UUIDs, all committed parent
IDs, all integrity hashes and exact branch HEAD states (including unborn).
Never silently rewrite UUIDs or fabricate an ancestry edge to resolve a
replication conflict. Imported branch HEAD publication must obey the same
atomic durability and concurrency rules as a local commit. Copies of a
private uncommitted working state are **not** committed history and require
a distinct explicitly authorized backup/synchronization protocol.

## 10. Required acceptance scenarios

1. New project: UUIDv7 project and `main` exist, `main.head = null`, history
   has zero revisions; checkout has private mutable data.
2. First commit creates one immutable UUIDv7 R0; a second commit creates R1
   and advances only the targeted branch.
3. Creating `feature` from committed R0 creates a distinct branch UUIDv7,
   records source branch and exact R0, does not add a revision.
4. Branch creation requested from uncommitted edits explicitly asks whether
   to commit first or fork from the unchanged committed base.
5. Two users edit the same branch from one base; only one stale-base commit
   can advance that HEAD. The other retains a recoverable working copy.
6. After reconciliation, a subsequent commit has the correct immutable
   parent and validates the complete hydraulic model.
7. Sharing committed R0 across multiple HEADs does not duplicate it; branch
   lanes are distinct, with provenance in the inspector, not extra history
   edges.
8. Rename, service restart and bundle import/export preserve branch UUID,
   origin UUID, fork UUID and complete committed ancestry.
9. Native and REST return equivalent UUIDv7 IDs, hashes, null unborn HEADs,
   pagination and conflict semantics.
10. Failures, cancellations and ambiguous network responses never reset a
    published HEAD or silently lose a user's uncommitted work.

## 11. Parameters requiring explicit implementation design

The core lifecycle above is fixed. The following choices require a concrete
native/REST/GUI agreement **before** their respective operations ship:

- Whether and how a second **unborn branch** may be created when no committed
  fork revision exists; any such operation must represent null honestly and
  must not create a hidden revision.
- Where private working copies are persisted (client-local, server-managed or
  both), draft retention/expiry, session recovery and ownership permissions.
- Required human commit metadata, monotonic/idempotent request-ID retention,
  and handling of unknown outcomes following connection loss.
- Exact merge resolution UI, full hydraulic validator rules, and how failed
  semantic merges are represented to collaborators.
- Revision-lane positioning for shared ancestors, many branch HEADs on one
  committed revision and two-parent merges. The *semantics* of all edges are
  fixed even where pixel layout choices are not.

These choices are not permission to change the immutable commit / private
working-copy separation, UUIDv7 identity, branch provenance or atomic
expected-HEAD publication.

**This contract overrides convenience assumptions in any API or GUI design.**
A feature is complete only when its storage, service, replication and GUI
behavior agree with the invariants above.
