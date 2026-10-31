# DVMDB version-control implementation roadmap

**Canonical execution plan — documentation, not an implementation-status claim.**

This is the authoritative *step-by-step delivery order* for the
[version-control contract](version-control-semantics.md),
[private working repositories](private-working-repositories.md),
[REST contract](rest-api-design.md),
[replication contract](offline-repository-synchronization.md). Security is
tracked independently in the [deferred security notes](security-permissions-design.md)
and is **not a milestone or dependency of this roadmap**.
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
8. **Security is outside the version-control implementation program.**
   Repository roles and UUIDs identify resources but do not authenticate users
   or enforce permissions. The independent
   [security backlog](security-permissions-design.md) is reference material,
   not a future VC stage. For now, do not expose unauthenticated writable HTTP
   services to untrusted/public clients.

Normative changes to this section require an explicit cross-repository design
review and coordinated edits to both roadmaps and the version-control contract.

## B. Approved implementation decisions — DG-01 through DG-06

**All six gates are decided.** These are binding defaults for VC-01 through
VC-08, not options to vote on again. Milestone work must still specify exact
wire formats, transaction boundaries and operational parameters before
persisting them. Changes to the decisions require an explicit coordinated
DVMDB/GUI specification update.

| Gate | Required by | Approved rule |
|---|---|---|
| **DG-01: repository-qualified branch references** | VC-03, VC-06 | Each repository has its own UUIDv7. A *logical* project and branch keep the **same UUIDv7** when cloned/fetched into another repository; a genuinely new branch gets a fresh UUIDv7. Each repository maintains an **independent mutable HEAD** under `(repositoryId, projectId, branchId)`. Store explicit tracking of remote repository/project/branch identity and observed HEAD/null; never infer correspondence from a name or accidentally share a mutable ref across repositories. Reject identity collisions and wrong-repository/project targets. |
| **DG-02: durable drafts and session concurrency** | VC-04 | One private DVMDB per intended user; independent draft UUIDv7 per editing session by default. Multiple sessions can read one draft but only **one active writer** may modify it. Use an expiring exclusive writer lease with **fencing token AND optimistic generation/version check**, verified atomically on every save. On expiration, permit explicit recovery/takeover, never deletion or stale overwrites. A successful Save is durable before acknowledgement. |
| **DG-03: idempotent operations and recovery** | VC-05, VC-07 | Every Commit and Publish carries a caller-generated UUIDv7 operation ID, a request-payload digest and target-repository/operation scope. Record pending/succeeded/rejected durably with the immutable outcome; identical retries return the same outcome, a reused ID with a different payload is rejected. On timeout query authoritative status under the **same ID**, never issue a fresh write blindly. Status retention and crash recovery must keep results queryable for the documented supported recovery window; do not expire unresolved operations. |
| **DG-04: pre-release format policy** | All stages | There is **no backward-compatibility or migration requirement** before AOWIS's first release. New formats may replace obsolete experimental formats cleanly; reject unsupported records explicitly instead of supporting transitional readers or retaining legacy layers. Do not silently corrupt data, rewrite a committed history or disguise a failing test fixture. Fresh test data may be initialized explicitly. |
| **DG-05: initial reconciliation support** | VC-08 | Support explicit Fetch, safe fast-forward and **three-way merge** using a committed common ancestor and two ordered parents, with model-entity UUID comparisons, manual conflict resolution and whole hydraulic-model validation. Detect delete/edit, simultaneous edits and domain-invalid combinations. An unresolved merge stays a private draft. No automatic rebase/replay, force Push, octopus merges, silent last-writer-wins or implicit Publish in the initial release. |
| **DG-06: initial remote publication** | VC-06, VC-07 | Clone keeps logical branch UUIDs. A locally created branch receives a new UUIDv7 and is explicitly provisioned on the selected remote **with that same UUIDv7**, using its verified project/source/fork identity and a durable idempotency key. First publication of an unborn branch uses `expectedRemoteHead: null`; branch absence is distinct from a branch whose HEAD is null. Reject UUID collisions, wrong target or stale HEAD atomically; do not synthesize remote branches after a failed Publish. |

**Operational parameters still to specify in their implementation milestones:**
lease duration/renewal and fencing-token encoding; draft quotas, backup and
retention; completed operation-result retention and status API; native DVMDB transactional metadata, persistence and crash-recovery details; and model conflict reporting. These are
implementation parameters, **not reopened architectural decisions**.
Authentication and permissions are excluded from all VC milestones; do not
introduce them as prerequisites for implementing or accepting VC-00–VC-11.
Treat controlled deployment of unauthenticated HTTP repositories as separate
operational guidance, not as a version-control deliverable.

## Hydraulic API consolidation gate — before VC-05

**One public hydraulic revision model:** all committed hydraulic operations use
`ProjectId` + UUIDv7 `RevisionId` and explicit expected HEAD. The C++ API
names have no version suffix: `saveHydraulicNetwork`, `editHydraulicNetwork`,
`loadHydraulicNetworkAtRevision`, `listHydraulicEntityIdsAtRevisionLimited`,
`loadHydraulicEntityAtRevision`, and `mergeHydraulicBranches`, with
`HydraulicMergeSelections` for pinned merge choices. Draft Save and local
Commit remain separate operations; no SHA-identified commits or synthetic
initial revisions are introduced.

**Acceptance prerequisites before deleting the previous SHA code:**

1. Port the existing hydraulic persistence qualification suite, value-rule
   tests, HTTP fixtures and edit benchmarks to project-scoped UUIDv7 projects,
   branches and immutable revisions. Preserve the assertions for every one of
   the 18 entity collections, physical/topological validation, cross-entity
   references, stale-HEAD rejection, restart recovery and corrupt-data reads.
2. Transfer the proven *indexed* edit engine (scalar, topology, physical,
   dependencies, catalogue and bounded membership) to the UUIDv7 revision
   storage API. Some whitelisted paths now use indexed commits, but the
   remaining edits still reconstruct and re-save the aggregate; deleting the
   SHA engine early would lose tested behavior and performance baselines.
3. Carry over the full hydraulic merge matrix: concurrent inserts/deletes,
   field/object choices, stale pins, invalid candidates, fast-forward
   validation, two-parent ancestry and unchanged source branches.
4. Unify the encoder/manifest and validator helpers shared by draft and
   committed persistence. Keep project-qualified immutable readers and their
   record-identity checks. No fallback to SHA IDs or old repository branches.
5. Remove the SHA-only functions, selectors, diagnostics and obsolete test
   fixtures. Check **all** code, tests, benchmarks, HTTP routes, headers and
   docs for leftover old signatures or `*V2` hydraulic names. No compatibility
   wrappers remain in the released API.
6. Require the dedicated hydraulic persistence, value-validation, HTTP and
   merge tests, plus relevant benchmark comparisons, to pass after the old
   implementation is gone. Re-run full DVMDB qualification before VC-05.

**Current source checkpoint:** the project-scoped entry points have their
canonical unsuffixed names, and the SHA-based test/benchmark calls carry a
`Legacy` suffix to keep the two architectures distinguishable during test
migration. This is an intermediate code state, **not completed consolidation**:
the SHA implementation still exists solely to preserve extensive qualification
coverage. The canonical UUIDv7 edit path now uses the immutable revision index
for whitelisted existing-member scalar updates (pipe hydraulic scalars,
junction elevations and map-label text), physical/operating updates to
reservoirs, tanks, pumps and valves, existing catalogue payload updates,
independent existing-pipe endpoint rewires, and typed reference updates for
existing demand points and controls.
These paths check MODEL value rules, canonical references where applicable,
immutable-revision ownership and expected HEAD, preserve the membership
manifest, and return the existing revision on no-op. Ineligible or mixed topology
and dependency edits fall back to complete-network validation. New/deleted
entities and structural membership still take full-network validation on the
UUIDv7 path. Remaining indexed paths, merge qualifications and legacy suite
migration are still required. Do not add production callers to the SHA API
or declare the gate satisfied until those requirements and SHA removal pass.

## Current implementation state and immediate delivery priority

This section records the **source-level checkpoint**, not a claim that the full
user workflow already works. Documentation and unit tests never substitute for
an end-to-end checkout/edit/reopen/commit test against a real personal DVMDB.

| Area | Available in the checked source | Not yet accepted end to end |
|---|---|---|
| Project and branch history (VC-01/02) | Unborn project creation, nullable committed HEAD, immutable branch fork provenance, native/REST readers, corresponding GUI graph/inspector tests | Real multiclient history/graph acceptance across all deployment modes |
| Personal repository identity and checkout (VC-03/04) | Persistent repository UUID/roles, tracking metadata, private working-copy CRUD and fenced generation-checked saves; committed REST bundle transfer and identity-preserving unborn root checkout to a private clone target | Full editor activation and embedded/remaining deployment modes |
| Hydraulic editing (VC-04) | MODEL-owned canonical codec and reversible transactions; GUI event-save membership transport, durable event queue, typed record/membership adapter; native atomic journal and DVMDB-owned membership materialization; private-draft empty network initialization endpoint (prior targeted DVMDB suite: 3/3 passed) | GUI-driven first-network initialization, genuine editor session binding, restart/reload verification, global mutation coverage |
| Local Commit (VC-05) | Immutable revision and model persistence primitives | Working-copy-to-revision CAS, recoverable operation ID/status, usable Projects-tab Commit |
| Collaboration (VC-06–08) | History transfer, import, revision DAG and domain merge primitives | Unified user-facing Fetch/Publish/reconciliation acceptance using independent private repositories |
| Presentation and qualification (VC-09–11) | Graph and inspector regression coverage; extensive DVMDB durability/sync tests | Integrated GUI/Standalone/browser scenarios and complete release evidence |

**Draft hydraulic hydration contract:** `GET /dvmdb/v1/working-copies/:draft/networks/:network`
reads the immutable pinned base plus one saved candidate-overlay snapshot (no
working branch HEAD lookup), reconstructs server-owned membership chunks,
and validates the resulting complete canonical hydraulic model before returning
`draftId`, decimal `generation`, `networkId` and canonical `payloadBase64`.
The read is lease-free and never changes a revision. Backend HTTP regression
coverage includes empty unborn roots, saved entity insertion, restart,
committed bases, membership removal and a preserved branch HEAD. The GUI
client decodes and verifies the canonical MODEL bytes; the draft inspector
can preview a candidate-root network but editing/autosave is not yet connected.

**First-network bootstrap contract:** `POST /dvmdb/v1/working-copies/:draft/initialize-network`
accepts `sessionId`, `fencingToken`, decimal-string `expectedGeneration`,
UUIDv7 `networkId`, and nonempty `name`. It operates only on an untouched
unborn private draft, encodes a server-owned empty hydraulic manifest,
persists it using the existing fenced candidate write, and leaves HEAD null.
A same-writer retry at exactly the next generation returns the identical
saved root without another write. Conflicting retries and later attempts
return 409. The following `save-events` request can add members to that
root through atomic membership materialization. The targeted DVMDB first-network tests previously passed 3/3; the draft
network hydration extension requires a new compiled test run. Actual editor
integration remains open.

**Next actionable work (in order):** (1) connect first-network bootstrap to the
actual editor workflow; (2) bind the now-implemented validated draft hydration
read to a real `HydraulicData` editor session; (3) capture/queue/flush supported edits,
including creation/deletion, over interrupted network and restart; (4) add
local working-copy Commit with expected local HEAD and idempotent status;
(5) integrate the Commit action and prove first unborn commit end to end.
Broader Fetch/Publish/Merge polish follows this usable local workflow. Do not
mark VC-04 complete until a real editor can reopen an automatically saved
model byte-for-byte after process restart.

**Verification note:** Previously reported targeted GUI tests passed for
`aowis-personal-working-copy-client`, `aowis-personal-edit-queue` and
`aowis-hydraulic-edit-transaction`. DVMDB hydraulic membership HTTP code and
fixture are present in the supplied snapshot, but this document does **not**
claim its revised integration test passed unless that run is separately
recorded. There is no claimed end-to-end local Commit acceptance yet.

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
- Define the current pre-release format and reject obsolete experimental
  storage explicitly; no compatibility decoder or migration work is required.

**Done when:** current implementation gaps are listed against exact APIs and
fixtures; both teams agree on the current pre-release format policy and
reproducible test commands.

The [implementation status above](#current-implementation-state-and-immediate-delivery-priority)
supersedes the initial audit; do not restart the initial inventory.

### VC-01 — Unborn projects, nullable committed HEADs and graph API

**Dependency:** established domain contract. **Lead:** DVMDB.

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
- Implement repository-qualified branch-HEAD ref namespaces and tracking
  records. Preserve the **same logical branch UUID** on clone/fetch; each
  repository moves its own HEAD independently. Independently created branches
  get fresh UUIDs, preserved on first Publish. Never rewrite revision UUIDs.
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
- Use transactional multi-entity saves requiring a live exclusive writer
  lease **and** its fencing token **and** the expected draft generation. A
  later session can take over an expired lease without permitting a stale
  writer to overwrite that draft. Draft Save never commits.
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
- Persist idempotency status, request-payload digest and final result; retry
  with the same operation UUID and same payload returns its original outcome.
  Same UUID with changed payload fails. A timeout triggers authoritative
  status lookup; pending outcomes remain recoverable across restart.
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
  idempotent operation, preserving the new branch UUIDv7 across repositories;
  never guess target branch names/IDs. Check absence separately from null HEAD.
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

### VC-11 — Storage integrity, replication, qualification and release gates

**Dependency:** VC-01 through VC-10. **Lead:** DVMDB + GUI.

- Freeze the first-release canonical format; reject unsupported pre-release
  data clearly. Test backup/restore, reopen, compaction and power-loss recovery.
  Do not add compatibility or migration infrastructure for experimental data.
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
passes, unsupported data is rejected safely, and test evidence is recorded.

**Scope boundary:** This roadmap ends at **VC-11**. Authentication,
authorization, permissions, audit and public-service security qualification
belong to a separate, unscheduled AOWIS-wide design effort. They have **no VC
number**, and they are not part of the database/graph integration acceptance
criteria. Preserve ideas in the [deferred security notes](security-permissions-design.md)
without adding implementation tasks to this roadmap.

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
12. Clone `main` from shared S into private A/B: each branch retains the
    **same logical UUIDv7**, while all three repository-qualified HEADs can
    differ. First Publish of a newly created branch preserves its new UUID,
    refuses collisions, and distinguishes absent branch from null HEAD.
13. Two sessions reopen the same private draft: the exclusive lease holder
    can Save with the correct fencing token and generation; stale saves
    after expiry/takeover fail without losing durable data.
14. Commit/Publish succeed but their responses are lost; lookup using the
    original operation UUID reports one durable result, replays do not create
    duplicates, and mismatched payloads are rejected.
15. Existing committed R0 remains valid after upgrade; new project creation
    produces an unborn `main`. Unsupported pre-release formats fail closed with
    recoverable original records and an explicit backup.
16. Two branches change the same hydraulic model from a common ancestor:
    manual three-way conflict resolution and whole-model validation are
    required, with no automatic rebase or premature Publish.

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

**Next task:** Complete the current VC-04 private draft editing path as
specified in the status section above. Then implement VC-05 local Commit,
without restarting VC-00 or adding unrelated features.
