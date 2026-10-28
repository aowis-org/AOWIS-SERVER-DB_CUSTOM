# DVMDB version-control implementation roadmap

**Canonical execution plan — documentation, not an implementation-status claim.**

This is the authoritative *step-by-step delivery order* for the
[version-control contract](version-control-semantics.md),
[private working repositories](private-working-repositories.md),
[REST contract](rest-api-design.md),
[replication contract](offline-repository-synchronization.md), and
[security/permission design backlog](security-permissions-design.md).
The companion GUI plan is `AOWIS-SERVER-GUI/docs/dvmdb-integration-roadmap.md`.
They use identical milestone IDs, so individual implementation threads can be
handed off independently. **Do not restore the previous automatic empty
revision, writable HEAD, or branch-provenance graph-edge model.**

## A. Fixed contract — do not reopen during implementation

1. Projects, branches, committed revisions and private working copies are
   **distinct UUIDv7** resources. SHA-256 verifies canonical immutable records
   and their Merkle-rooted complete model state; it is not an object ID.
2. A project begins with **unborn `main`** (`headRevisionId: null`), zero
   commits, and no synthetic initial revision. First commit has zero parents.
   A second branch cannot be created until a committed fork point exists.
3. A branch is a persistent named **movable ref to an immutable commit**.
   HEAD is **the branch's latest committed revision**, never a writable model.
   Shared ancestry and shared/non-leaf HEADs are valid.
4. Editors mutate **independent private working copies**, never committed
   revisions. Standalone uses its own durable embedded DVMDB; basic desktop
   and browser/web clients require an explicitly configured **per-user private
   HTTP DVMDB**. Shared sources are never silent draft fallbacks.
5. **Save draft**, **Commit locally**, **Fetch**, **Publish/Push**, and
   **Reconcile/Merge** are different actions. Local commits advance only a
   personal repository branch; Publish conditionally advances the shared ref.
   Stale refs must not overwrite commits or drafts.
6. Branch creation from an immutable revision adds a new branch ref, **not a
   commit**. Persist source branch UUID and exact fork revision UUID. UTF-8
   names with spaces are supported; names are not storage identities.
7. History is **one Git-style revision DAG** with genuine zero/one/two-parent
   edges. Branches decorate their current HEADs; no invented branch-origin
   edges or duplicate committed revision nodes. Drafts live outside the DAG.
8. Security remains a **separate design phase**. Until it is implemented,
   private HTTP DVMDB deployment **must be isolated** and multi-user/public
   writable exposure is out of scope; a configured URL does not enforce
   ownership.

Normative changes to this section require an explicit cross-repository design
review and coordinated edits to both roadmaps and the version-control contract.

## B. Remaining decision gates — defaults proposed, not yet approved

These are **narrow implementation contracts**, not unresolved fundamentals.
Record the agreed choice in both project roadmaps before the indicated
milestone begins; then update the API/schema docs and tests in that milestone.

| Gate | Must resolve before | Precise decision and recommended default |
|---|---|---|
| **DG-01: repository-qualified references** | VC-03, VC-06 | A private repo and a shared repo have distinct repository UUIDs; a locally created tracking branch may have its **own** branch UUID. Persist mapping `(remoteRepositoryId, projectId, remoteBranchId) -> localBranchId`, exact observed remote HEAD/null, and source connection. Never assume same branch UUID means same mutable ref on two servers; preserve immutable commit UUIDs. Confirm repository identity across service restart and relocation. |
| **DG-02: private draft coordination** | VC-04 | Persist draft UUID, owner/session metadata, exact base revision/null, target local branch, model state, dirty/version counter. Give each session its own working copy by default; reopening the *same* draft uses version-checked writes (or a lease with crash expiry). Save must be durable before acknowledgement. Choose quota, recovery and retention defaults before shipping. |
| **DG-03: idempotent operation identity** | VC-05, VC-07 | Caller-supplied UUIDv7 operation ID plus request-payload digest, scoped to target repository/operation; persist `pending/succeeded/rejected` and immutable result. Replays of same ID+digest return the same result; same ID+different payload is rejected. Define status retention and crash-recovery boundaries, and distinguish timeout from known failure. |
| **DG-04: repository transition policy** | VC-01, VC-11 | Existing project stores may contain a fabricated root commit and non-null HEADs. Default: **never rewrite IDs or delete history silently**. Decide between explicit verified migration with backup and fail-closed fresh repository initialization for development fixtures. Migration must preserve project, branch and revision UUIDs when possible and never guess missing fork provenance. |
| **DG-05: reconciliation scope and initial UX** | VC-08 | Start with explicit fetch + fast-forward, committed three-way merge with ordered parents, and manual resolution of conflicts. Do not automatically rebase, force-push or publish partial hydraulic models; validate merged complete snapshots. Decide whether replay/rebase is in a later iteration. |
| **DG-06: first remote publication and branch provisioning** | VC-06, VC-07 | Define how a newly created local branch is mapped to/created on the shared repository, including `expectedRemoteHead: null`, fork provenance and idempotent branch creation. Never invent a remote ref on a failed Publish. Require an exact target repository/project/branch identity and explicit user choice where several remotes match. |

**Review rule:** An implementation thread may propose and implement the
recommended default, but must record the decision and acceptance test before
changing persistent formats or wire schemas. Deferred permissions are tracked
under VC-12, not silently treated as solved.

## C. Required implementation sequence

Each milestone below is a standalone **mergeable checkpoint**. Maintain the
canonical native and REST contract together; don't ship a GUI path which
writes data that its own reader cannot reload. Prefer a small patch with
changed files only, a fresh timestamp and reproducible tests per checkpoint.

### VC-00 — Audit, freeze the contract and establish baseline

**Dependency:** none. **Lead:** DVMDB; GUI participates.

- Inventory current `Repository`, `ProjectService`, HTTP routes, branch
  records, revision records, graph readers, bundle formats, CLI and tests.
- Enumerate every assumption that HEAD cannot be null, project creation
  produces R0, revision `id` is SHA, or branch creation acts like a commit.
  Include native and REST response shapes and all existing test fixtures.
- Inventory embedded GUI DVMDB submodule/header compatibility. Choose one
  explicit DVMDB API baseline for coordinated integration.
- Create a failing acceptance matrix for unborn `main`, first commit,
  shared HEADs, two-user concurrency, reload, restore and cross-repo sync.
- Classify existing test repositories and format versions: convertible,
  read-only, or explicit fresh initialization (see DG-04).

**Done when:** current implementation gaps are listed against exact APIs and
fixtures; both teams agree on the baseline, DG-04 direction, and test commands.

### VC-01 — Unborn projects, nullable committed HEADs and graph API

**Dependency:** VC-00 and DG-04. **Lead:** DVMDB.

- Make `CreateProject` atomically create project UUIDv7 and `main` branch
  UUIDv7 with **explicit null committed HEAD**, zero revisions, no R0 object.
- Make native HEAD APIs and `ProjectService` represent null explicitly
  (prefer typed optional), rather than using an all-zero UUID/sentinel.
- Upgrade branch-ref canonical storage/verification, reopen/recovery,
  compaction and history traversal to understand unborn branches.
- Update REST project creation, branch listing, history and pinned graph
  responses (nullable `headRevisionId`, no fake ancestry). Missing JSON
  field is not equivalent to an explicit null value in *write* preconditions.
- Block history traversal and historical branching until a real committed
  revision exists; ensure graph pagination works for zero revisions.
- Add crash/restart, invalid-null and native/HTTP parity regressions.

**Done when:** new `main` has null HEAD across fresh create, restart, REST,
CLI/graph and bundle round-trip, with no committed record. No root sentinel.

### VC-02 — Exact branch creation, identity and provenance

**Dependency:** VC-01. **Lead:** DVMDB.

- Create a branch only from an **existing immutable revision**. Require a
  committed fork ID, verify same project and source-branch reachability when
  a source UUID is provided; reject unborn source or uncommitted draft.
- Persist new branch UUID, name, its initial committed HEAD, immutable
  `originBranchId` and exact immutable `forkRevisionId`. Make rename and
  deletion preserve/determine valid provenance without inventing ancestry.
- Keep branch-name rules consistent for native and REST, including UTF-8
  spaces; reject invalid controls and path delimiters.
- Return stable branch UUID and fork identity from native and REST writes.
  Preserve them through reload, historical commits, bundles and rename.

**Done when:** branching doesn't increment revision count or alter source
HEAD, and a later source commit cannot change the recorded fork point.

### VC-03 — Repository roles, identities and tracking schema

**Dependency:** VC-00; DG-01 and DG-06. **Lead:** DVMDB + GUI config.

- Define the durable **repository identity** for both private and shared
  DVMDBs, independent of project UUIDs and GUI connection profile names.
- Implement explicit local/remote branch-ref namespaces and
  repository-qualified tracking records. Avoid rewriting imported revision
  UUIDs; map local and remote branch ref IDs explicitly.
- Define fetch/checkout targeting metadata, a local repo's remote registry,
  and stable detection of wrong-repo or wrong-project endpoints.
- Expose identity and tracking through native service/REST without treating
  a user-provided repository UUID or HTTP URL as authentication.

**Done when:** two independent private DVMDBs can track the same shared
branch without sharing writable refs or confusing repository identity.

### VC-04 — Durable private working-copy lifecycle

**Dependency:** VC-01, VC-03, DG-02. **Lead:** DVMDB.

- Store UUIDv7 draft records with local branch UUID, base revision/null,
  per-user/session ownership metadata, explicit schema/version, dirty state,
  candidate model changes, validation state and last persisted generation.
- Implement create/checkout, read/list/reopen, edit/save/autosave, validate,
  discard and recovery APIs in native core and private HTTP service.
- Use transactional multi-entity saves and optimistic draft-generation
  checks (or explicitly specified lease model). Draft save never commits.
- Make first checkout from unborn `main` an empty editable model. Checkouts
  from committed revisions verify snapshot/Merkle integrity first.
- Preserve drafts through process death, network disconnect, refresh and
  unrelated repository operations. Never store private data in shared refs.
- Test multiple editor sessions against one private DVMDB, restarts,
  interrupted writes, ENOSPC and wrong-draft updates.

**Done when:** separate drafts survive restart; stale draft save cannot
overwrite another session; committed history never changes on Save.

### VC-05 — Atomic local Commit and operation-result recovery

**Dependency:** VC-04, DG-03. **Lead:** DVMDB.

- Implement Commit from an identified *persisted working copy*, including
  expected **local** HEAD UUID/null, chosen parent ordering and operation
  UUID. Validate complete model references and hydraulic constraints.
- Create exactly one immutable UUIDv7 revision with canonical SHA-256 and
  Merkle integrity. Publish the new local HEAD with an atomic compare-and-
  swap and durable status result; first commit has zero parents.
- On local HEAD race, return explicit conflict and current local HEAD,
  retaining draft and any already stored immutable objects. No blind replay.
- Persist idempotency status and result; retry with same operation UUID and
  payload returns original outcome. A timeout triggers status lookup.
- Document post-commit draft handling: success does not implicitly delete
  the user's working copy; rebasing/continuing edits is explicit.

**Done when:** two private sessions race to Commit: only one local HEAD
moves; the other's draft survives. Crash/retry returns one logical commit.

### VC-06 — Fetch, clone, tracking updates and verified transfer

**Dependency:** VC-03, VC-05, DG-06. **Lead:** DVMDB.

- Implement fetch into a personal DVMDB via local and HTTP transports,
  copying verified committed objects/history (including both merge parents)
  without changing personal drafts or local branch refs.
- Pin a remote branch HEAD/null and snapshot generation during pagination;
  validate ancestry closure and all hashes before accepting a fetch result.
- Persist updated remote-tracking refs with source repository/project/branch
  identity. Support empty/unborn remote projects explicitly.
- Adapt full/incremental bundles and staging to transferred branch IDs,
  nullable refs, fork point metadata, and no implicit publication to local
  development branch. Verify replay, wrong-project rejection and quotas.

**Done when:** one shared project can be fetched by two private repos; their
local branches/drafts remain isolated and tracking state is reproducible.

### VC-07 — Remote Publish/Push with atomic expected-HEAD semantics

**Dependency:** VC-05, VC-06 and DG-03/DG-06. **Lead:** DVMDB.

- Transfer missing immutable objects, then conditionally advance exactly
  one **shared** branch ref using explicit expected remote HEAD UUID/null.
- Enforce fast-forward against the target ref, project ownership, integrity
  closure and hydraulic validation before exposing the new shared HEAD.
- Make branch creation/provisioning on first publish a separate explicit,
  idempotent operation when required; never guess target branch names/IDs.
- Persist per-operation UUID, payload digest and authoritative result in
  shared DVMDB. Resolve ambiguous HTTP timeouts by status lookup.
- On stale expected HEAD, return conflict/current remote HEAD. Do not roll
  back successful local commits, erase drafts or silently force/reset refs.
- Cover concurrent publishers and remote death during object staging,
  HEAD CAS, status persistence and response delivery.

**Done when:** Alice's Publish advances shared R7->R8; Bob's competing
R7->B8 Publish is rejected, B8 survives privately, and retries are safe.

### VC-08 — Reconciliation and hydraulic-aware merging

**Dependency:** VC-07 and DG-05. **Lead:** DVMDB.

- Implement pinned common-ancestor analysis and deterministic field/object
  changes, including delete-versus-edit and reference-integrity conflicts.
- Provide preview/compare of local tip, remote tip and proposed merge base;
  keep draft and immutable local history untouched during analysis.
- Offer user-approved fast-forward or manual three-way resolution; validate
  complete hydraulic semantics, not just record-level non-overlap.
- Create a validated two-parent immutable merge revision in the private
  repository with deterministic, documented parent order and exact local
  expected HEAD. Publish only via VC-07's remote CAS, rechecking if remote
  advances again. Source HEAD does not move implicitly.
- Reserve rebase/replay as a separately approved later enhancement.

**Done when:** conflicting and compatible concurrent edits behave
predictably, merge ancestry is real, and aborted merges lose no private work.

### VC-09 — Full history, graph and inspector contract completion

**Dependency:** VC-01, VC-02, VC-07 for complete operation metadata.
**Lead:** DVMDB API + GUI.

- Expose one pinned, paginated revision DAG for all branch HEADs: zero/
  one/two parents, nullable branch HEAD, revision UUIDs, record hashes,
  immutable fork metadata, and complete multi-parent ancestry.
- Ensure branch labels aren't treated as ancestry vertices and a shared/non-
  leaf committed HEAD can have multiple branch labels.
- Update native and REST readers and focused tests for changing pinned
  heads, invalid cursor, missing parent, wrong project and compromised hash.
- Provide inspector-ready fields for source/fork, current shared and local
  HEADs, and a distinct private draft status. GUI does not fabricate IDs.

**Done when:** both clients render the same actual commit DAG, empty project,
shared tips, divergent commits and two-parent merge without fake edges.

### VC-10 — GUI workflows and Standalone/HTTP parity

**Dependency:** VC-04 through VC-09. **Lead:** GUI with DVMDB integration.

- Implement explicit private repo configuration and status, working-copy
  checkout/recovery, Save draft, Commit locally, Fetch, Publish, conflict
  inspector and branch-from-committed-revision actions.
- Keep the committed graph read-only; never render drafts as committed
  revisions. Separate committed HEAD from editor's pinned base.
- Use exactly the same identity/expected-HEAD/idempotency semantics for
  embedded/native Standalone and remote HTTP basic/browser clients.
- Provide non-destructive progress, cancellation and ambiguous-result
  recovery for all mutating operations. Never treat a network timeout as
  proof that the operation did not happen.

**Done when:** real GUI integration tests exercise two independent private
repos and one shared repo, including failed Push and later successful merge.

### VC-11 — Storage migration, replication, qualification and release gates

**Dependency:** VC-01 through VC-10; DG-04. **Lead:** DVMDB + GUI.

- Freeze canonical format versions; test explicit conversion or fail-closed
  unsupported pre-release data, backup/restore, reopen and compaction.
- Test full/incremental export/import of branch UUIDs, immutable fork UUID,
  null HEADs, record hashes, ordered ancestry and merge history. Draft backups
  are a separate workflow and never masquerade as committed bundles.
- Fault-inject crash/kill, ENOSPC, delayed HTTP responses, duplicate events,
  corrupted blobs, stalled operations, high concurrency and pagination
  limits. Ensure operation-status recovery after every failure boundary.
- Verify project/branch naming, Unicode, identity stability, versioned
  metadata and complete hydraulic validation across all transports.
- Run relevant native, REST, CLI, sync, durability and Qt tests **in the
  actual Linux/Standalone/browser build configurations**; record commands,
  results and failures instead of claiming unrun tests passed.

**Done when:** documented acceptance matrix from `version-control-semantics.md`
passes, no unsafe data migration is implicit, and test evidence is recorded.

### VC-12 — Dedicated security, permission and operational hardening

**Deferred design milestone; mandatory gate before public/multi-user writable
service deployment.** Keep implementation work separately tracked in
[security-permissions-design.md](security-permissions-design.md).

- Threat-model native and HTTP operations, drafts, repo tracking, import,
  branch mutation, publishing, admin actions and source trust.
- Design authenticated user, device and service identities, ownership,
  authorization, audit, credential storage, TLS, quotas and revocation.
- Enforce server-side single-user draft isolation; a claimed `ownerId` or
  configured URL is never proof of authorization. Test cross-user denials.
- Until qualified, allow only deployment-isolated private working DVMDBs
  and trusted development/shared sources; clearly label insecure modes.

**Done when:** separate security review, penetration/abuse tests and
release authorization permit multi-user/public writable operation.

## D. Cross-repository delivery and checks

| Milestone | DVMDB checkpoint | GUI checkpoint |
|---|---|---|
| VC-00 | Inventory/API-format gap matrix, decision gates | Widget/source/writer dependency inventory |
| VC-01 | Unborn ref, null HEAD native/REST | Display unborn `main`, empty graph, no fake R0 |
| VC-02 | Exact fork provenance/native-REST branch APIs | Branch editor from committed UUID and inspector |
| VC-03 | Repository identity and qualified tracking schema | Explicit embedded/private HTTP working repo config |
| VC-04 | Persistent draft schema and APIs | Checkout, edit, autosave, recovery UI |
| VC-05 | Local commit CAS/idempotency/status | Local Commit UX, stale-local conflict handling |
| VC-06 | Verified fetch/clone/remote-tracking/bundles | Fetch status and local-vs-remote branch display |
| VC-07 | Atomic Push/ref publication and result lookup | Publish UX and stale-remote recovery |
| VC-08 | Merge/compare/reconciliation and domain validation | Conflict viewer and explicit user-approved resolution |
| VC-09 | Pinned UUIDv7 graph including null/multi-parent | Genuine Git-style history, labels and inspector |
| VC-10 | Transport/native integration support | End-to-end embedded, basic and browser workflows |
| VC-11 | Format, fault, durability and replica qualification | Real Qt/Standalone/browser acceptance evidence |
| VC-12 | Security authorization/audit qualification | Authentication, permissions and denied-action UI |

**Delivery order:** DVMDB contract/API for a milestone first; GUI can add
read-only or disabled scaffolding in parallel, but must not activate editing
until the matching native **and** REST semantics are implemented. Every patch
should state its prerequisite milestone(s) and test status.

## E. Acceptance scenarios for the whole program

1. New project: `main.head == null`, revision count zero, no fake R0.
2. First private checkout from unborn `main`; Save survives crash and does
   not change a committed HEAD; local Commit creates zero-parent R0.
3. First Publish uses expected shared null; succeeds only while shared
   `main` remains unborn, with reproducible authoritative result.
4. Two branches share committed R0 without a new commit; fork provenance is
   immutable, including after source rename, restart and replication.
5. Commits on independent branches create one logical R1/F1 each and genuine
   diverging ancestry; merge produces one verified two-parent revision.
6. Alice and Bob's separate private DVMDBs can both commit from shared R7;
   only the first compatible Publish advances the shared HEAD; the other
   retains all its local history and drafts.
7. Uncertain timeout at every local Commit/Publish stage can be resolved by
   operation ID; retries cannot publish duplicate logical commits.
8. Wrong project, wrong remote repository, wrong branch, corrupt hashes,
   invalid UUIDs, missing parents and invalid model references all fail
   safely without moving the shared HEAD.
9. Shared/non-leaf HEADs and >1 branch per revision render correctly;
   committed revision objects appear once; drafts remain separate.
10. Offline/unavailable personal HTTP repo disables editing, never silently
    swaps to the shared server or volatile RAM; Standalone uses its embedded
    durable working repository.
11. Backup, recovery, compaction and full/incremental sync retain all UUIDv7
    identities, SHA integrity bindings, fork metadata, nullable HEADs and
    meaningful remote-tracking state.
12. Private HTTP deployment remains isolated until VC-12 security sign-off.

## F. Starting another implementation thread

Provide **the newest DVMDB and GUI ZIP snapshots** (not just an old patch),
this roadmap, and the matching milestone in the companion GUI roadmap.
Ask for **one named milestone or independently testable substep**. In that
thread, follow this handoff template:

1. **Baseline:** inspect actual code/APIs and compare against this roadmap;
   do not assume the previous chat's proposed patch was applied.
2. **Decision gate:** identify any DG-xx relevant to the milestone, write
   the resolved contract in both docs, then implement; do not improvise
   formats or wire fields in the middle of the patch.
3. **Scope:** list changed API/data-format contracts, native/REST callers,
   GUI dependencies, crash/compatibility implications and test fixtures.
4. **Implementation:** implement a complete vertical slice with typed UUIDv7
   identities and explicit expected-ref semantics. No fake revision nodes.
5. **Evidence:** run available tests and document exact successes, failures
   and unexecuted Qt/HTTP tests. Include negative/race/fault cases.
6. **Deliverable:** ZIP patch of **changed/new files only**, under the
   repository's root hierarchy, with fresh modification timestamps; provide
   the repository's one-line `./tools/apply_patch.sh ~/Downloads/<zip>.zip`
   command and one-line commit message. Do not package entire repositories.
7. **Handoff:** update the roadmap milestone with `Not started`, `In
   progress`, `Blocked` or `Verified` **and the evidence**. Never mark a
   milestone complete based on ZIP packaging or syntax checks alone.

**Next recommended thread:** VC-00 and DG-04 baseline/format audit, then
VC-01 unborn projects with nullable committed HEAD across storage, service,
REST, replication and GUI read-only display.
