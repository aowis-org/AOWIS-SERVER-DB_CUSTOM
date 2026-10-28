# Private DVMDB working repositories and synchronization

**Normative lifecycle and deployment design.** See
[version-control semantics](version-control-semantics.md),
[offline synchronization](offline-repository-synchronization.md), and
[security design backlog](security-permissions-design.md).
The coordinated backend/GUI implementation sequence and unresolved wire/storage
decisions are recorded in the [implementation roadmap](roadmap.md).
The exact wire endpoints and configuration keys are implementation work;
this document does not claim that they already exist.

## Repository versus working copy

A **working repository** is one user's private, durable DVMDB repository.
It stores checked-out project histories, local immutable commits, local
branch references, remote-tracking state, and any number of independently
identified uncommitted **working copies**. A working copy is one editable
model state inside that repository, not another repository and not a revision.
It has a UUIDv7, base committed revision (nullable for the initial unborn
`main`), target local branch UUID, dirty state, and recovery information.
Two editor sessions—even for the same user—must not silently edit the same
working-copy object concurrently. Create distinct working copies by default;
if reopening a saved copy, allow multiple readers but **one active writer**.
Every Save must atomically verify an unexpired exclusive writer lease,
**fencing token and expected persisted generation**; stale writers must not
overwrite a recovered draft. Expired leases permit explicit recovery/takeover
without deleting content; acknowledged Save is durable.

## Deployment modes

| Client | Personal working repository | Shared repositories |
|---|---|---|
| Standalone desktop | Its built-in/native DVMDB at a configured durable location | Optional local or HTTP DVMDB sources |
| Basic desktop GUI without embedded DVMDB | Explicitly configured private DVMDB service over HTTP | Separately configured HTTP DVMDB sources |
| Browser/web GUI | Explicitly configured private DVMDB service over HTTP | Separately configured HTTP DVMDB sources |

One configured working repository belongs to **one intended user**, not one
browser tab. It may hold several projects and separate drafts. Multiple GUI
sessions can reconnect to that user's repo with explicit draft selection.
The shared repository is authoritative for **published** project/branch
HEADs. The personal repo is authoritative for **unpublished local commits**
and **private draft state**. Never silently use a shared project source as
the user's working repository or silently switch to another repository after
a connection failure.

For non-standalone clients, a configured private HTTP repository is required
for editing. Without a reachable configured working repository, the GUI
may display cached/published history read-only, but must not pretend edits
are durable. A browser may additionally maintain temporary UI state, but
browser memory/storage is not an authoritative DVMDB working repository.
Offline edits on an HTTP-only client are not promised by this architecture.

## Persistent repository identity (VC-03)

The DVMDB catalogue root persists a UUIDv7 in `repository.id` using the
`AOWIS-REPOSITORY-1` marker. Existing catalogue roots acquire an identity
on their first opening with `ProjectService` and keep it across restarts and
offline backups. A malformed existing identity fails closed instead of being
regenerated. Separate catalogue roots receive separate UUIDs; the identity
belongs to the DVMDB root, not to a GUI profile name, connection URL, project,
branch, or user. `ProjectService::repositoryId()` and read-only
`GET /dvmdb/v1/repository` expose the same value. It is **not authorization**.

This is the identity foundation only. Repository-qualified remote-tracking
records, clone/fetch, and private-working-repository configuration remain
separate VC-03/VC-06 implementation tasks; this endpoint does not enable
editing or implicitly select a private repository.

## Clone/checkout and branch tracking

A project and its logical branches keep their UUIDv7 identities across clone,
fetch and publication; immutable revision UUIDs and canonical SHA-256 hashes
also survive unchanged. When cloning `main` from shared repository S into
personal repository A, its logical branch ID is the **same** `main` UUIDv7 in
both stores. Crucially, the mutable HEADs are *not shared memory*: the
repository-qualified ref key `(repositoryId, projectId, branchId)` makes
A's local HEAD and S's published HEAD independent.

A local branch created as a **new** logical branch receives a new UUIDv7,
which is used on first explicit provisioning on the selected remote.
Remote-tracking metadata stores remote repository UUID, project UUID,
branch UUID, observed remote HEAD/null, and source connection. A tracking
record is not a fork or a revision-parent edge. Reject repository/project
mismatch, ID collision and ambiguous name-based matching.

For a new remote branch, **absence and unborn null HEAD are distinct**:
provision the new branch explicitly and idempotently with the same UUIDv7,
then conditionally Publish. For an already existing unborn remote branch,
first Commit publication compares against `expectedRemoteHead: null`.
Neither operation silently creates an unrelated remote branch after failure.

1. Fetch or initialize the selected project in the personal repository.
   For an unborn project, track `main` with a null committed HEAD.
2. Checkout a pinned local committed revision (or null for unborn `main`)
   into a durable private working copy. Store the complete model changes
   only in that draft until the user commits.
3. Save/autosave persists draft state in the **personal** DVMDB. It never
   commits, advances HEAD or publishes to the shared repository.
4. **Commit** validates the complete model, creates one immutable UUIDv7
   revision in the personal repository and conditionally advances the
   **local** target branch from the expected local HEAD (including null).
   Failure retains the draft; success does not change the shared HEAD.
5. **Fetch** updates remote-tracking refs and missing immutable objects but
   never overwrites local drafts or silently changes local branch HEADs.
6. **Publish/Push** sends the missing verified immutable objects and then
   conditionally updates the **shared** branch from its expected remote
   HEAD. The remote update must be fast-forward/valid under its policy;
   otherwise return an explicit stale-remote/non-fast-forward conflict.
   A failed publish keeps successful local commits and the user's drafts.
7. **Reconcile** explicitly incorporates new remote commits by fast-forward,
   explicit fast-forward or validated three-way merge; do not automatically
   rebase/replay in the initial implementation. Preserve the private draft
   when hydraulic or structural conflicts remain. Re-attempt Publish with a fresh expected remote
   HEAD; never silently force the shared branch.

The first project commit requires the local unborn HEAD to be null. Its
first publication requires the shared unborn HEAD to remain null. Only
that first committed revision creates the root history node. Creating a
branch from an unborn HEAD is rejected; make the first commit first.

## Atomicity, recovery and duplicate requests

Local commits and remote publications are **two different transactions**.
Commit and Publish each use a caller-generated UUIDv7 operation ID,
repository/operation scope, canonical request-payload digest and durable
`pending`, `succeeded` or `rejected` outcome lookup. Replays with the same
ID/payload return the original outcome; changed payload under an existing ID
is rejected. An unresolved operation is never discarded by retention cleanup.
A timed-out local commit may already exist; a timed-out Publish may have
moved the shared HEAD. Resolve both by checking the authoritative repository
and operation status before retrying. Transport failure must never imply
that a local commit should be rolled back or replayed with a fresh ID.

A personal repository must provide durable autosave and restart recovery
for its own drafts, transactional local branch updates, and consistent
snapshots across multi-entity hydraulic edits. Backup of private draft
state is separate from committed-revision replication bundles. Retention,
quota, lease TTL/renewal and draft-recovery policies require explicit
user-facing controls and a documented operational policy at VC-04/VC-05.
Existing repository history is preserved through versioned transactional
metadata migrations, with verification and backup/rollback; new projects
alone adopt unborn `main`. No historical R0 is silently deleted.

## Acceptance scenarios

- Two users clone the same project at R7 into distinct working repositories,
  independently save uncommitted edits, and do not change shared `main`.
- Alice commits R8 *locally*; the shared branch remains R7 until Publish.
- Alice publishes R8; Bob locally commits B8 from R7, then Bob's Publish is
  rejected. B8 and his unsaved drafts remain intact in his personal DVMDB.
- Bob fetches R8, reconciles with B8 into a validated successor, then
  publishes conditionally without losing either user's content.
- A restarted standalone application or HTTP-connected GUI recovers the
  correct private draft UUID and pinned base from the same personal repo.
- A missing private HTTP DVMDB disables editing; public history is still
  readable when its source remains reachable, without unsafe fallback.
- Project/branch/revision UUIDs and canonical hashes retain their meanings
  across repositories; local and shared branch HEADs advance independently
  even while both use the same logical branch UUIDv7.
- Two sessions reopen one saved draft: only the live leased/fenced writer
  with the expected generation can Save; expiration/takeover invalidates
  stale writes without losing the last durable content.
- A Publish response is lost after success: status lookup by original
  operation UUID reports the single successful remote HEAD advancement.
- Existing projects with previously committed initial R0 retain R0 after
  upgrade; newly created projects start with unborn `main` and no R0.

**Security limit:** Selecting a private HTTP repository in configuration
is not access control. Until the dedicated [security plan](security-permissions-design.md)
is implemented, deployment must isolate these repositories from other users.
