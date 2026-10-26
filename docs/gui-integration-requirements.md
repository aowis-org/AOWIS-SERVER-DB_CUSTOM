# Planned GUI integration contract — project, branch and revision management

**Status (2026-10-26): remaining server/API requirements, not an inventory of
everything the GUI can do.** The GUI now uses the implemented REST and native
service APIs to enumerate projects, branches and limited first-parent revision
history, and to create projects and branches (including a fork from a selected
revision). It also monitors multiple REST databases automatically and filters
their visibility. Its separate in-memory preview remains a demonstration;
GUI checkout, hydraulic revision saving and general lifecycle CRUD are
**not** implemented. See the GUI's `docs/dvmdb-integration-roadmap.md` and
`docs/dvmdb-quickstart.md`.

Use the **existing** [OpenAPI](dvmdb-http-api.yaml),
[native service](native-database-service.md) and
[revision history](native-revision-history.md) for implemented behavior.
Do not publish speculative paths into the live OpenAPI.

## Implemented primitives and current GUI use

- `GET/POST /projects`; `GET /projects/{projectId}`; list/create branches;
  branch heads, first-parent paginated history, immutable revision lookup;
  revision diffs (at most 1,000 changes, no pagination), historical network
  reads, optimistic `expectedHead` full-network writes, atomic entity edits,
  hydraulic merges and full/incremental `.aowbundle` transfer.
- Revision IDs are immutable content hashes. Native revision metadata stores
  model/content roots and up to two parents; branch history is a **first-parent
  traversal** and does not by itself describe the whole graph.
- No HTTP authentication or authorization enforcement is implemented.
- The existing HTTP service lists hydraulic **entities within a known network**;
  it does **not** provide an API to discover all network IDs in a project.

## P0: required before real checkout/save functionality

1. **Project lifecycle**: rename and delete/archive/restore project operations,
   stable UUID preserved across rename, uniqueness rules (including Unicode /
   case behavior), `If-Match`/version guards, confirmation/impact reporting,
   deleted-project retention and admin access rules. The current catalog
   supports create/list/get but not rename/delete.
2. **Branch lifecycle**: rename/delete/archive/restore, stable branch identity
   or an explicit migration contract for branch-name IDs, protected/default
   branch rules, race-safe expected-head and expected-metadata versions, and
   explicit branch-from-revision support across the full ancestry DAG.
   Today branch listing and creation exist, but management endpoints do not.
3. **Network enumeration**: list networks at a pinned project revision or
   branch snapshot, including network UUID, human name, type and summary;
   specify whether an empty project has zero networks and how the first
   network is created. Avoid implicit selection of a random network.
4. **Checkout/session**: consistent snapshot-pinned network+revision discovery
   and explicit branch-head identity; provide a way to retrieve revision and
   corresponding network without a race. GUI holds working copy, loaded base,
   server HEAD separately. Unknown or stale HEAD causes 409, preserves local
   changes, and offers comparison/reconciliation.
5. **Immutable commit metadata**: when creating a revision, accept commit
   message/title (short summary) and optional detailed description, server-
   assigned creation timestamp (UTC, timezone-safe), actor principal ID plus
   display name, provenance/origin (GUI/API/import/bundle/merge), schema/client
   version, explicit action type, and ordered parent references. Decide which
   fields participate in the revision hash *before* changing the canonical
   format; never silently mutate hashed metadata. Deterministic versioned
   encoding, bundle compatibility and deduplication must be specified.
6. **Revision annotations**: separate, mutable, versioned records keyed by
   immutable revision hash for editable title/description, freeform labels,
   keywords/tags, validation status, reviewer notes, and links to issue IDs.
   Annotation edits must never create a new hydraulic revision or change the
   revision hash. Optimistic concurrency, auditing, indexing and permissions
   are required. Distinguish commit message (immutable) from annotations
   (editable) in both API and GUI.

## P1: rich revision/branch explorer

- **Graph navigation**: efficient DAG page/query from selected heads, with
  *both* parent hashes, lane/branch references, multi-head reachability,
  bounded ancestor paging, stable cursors and unpaged-parent indicators.
  Pagination of first-parent `history` alone is insufficient for a git-like
  graph. Filtering by author, date range, branch and tags plus full-text
  annotation search should be possible without downloading every revision.
- **Branch metadata**: display title/name, description, purpose/category,
  structured tags, createdBy/createdAt, lastUpdatedBy/lastUpdatedAt, review
  state (draft/in review/approved/archived), protection/locking, default-branch
  flag and optional responsible owner/team. Branch HEAD and mutable metadata
  have separate concurrency tokens.
- **Project metadata**: project human title, description, tags, category,
  owners/members, creation/last-update timestamps, last-active branch,
  state (active/archived), and optional cover/thumb (external asset reference).
  Search/filter/sort, pagination and bulk operations require stable semantics.
- **Tag design**: separate lightweight user-defined **labels** on projects,
  branches and revision annotations from **release tags** that point at an
  immutable revision (named refs). Release refs need uniqueness, move/delete
  policy (possibly protected), creator/time, description, and audit history;
  no ambiguous reuse of 'tag' for both concepts. Normalize/validate labels,
  preserve case/display names, limit lengths/counts and expose search indexes.
- **Review/provenance**: merge/commit message, reviewer decisions, approvals,
  validation/test status, optional signature / verification status, import
  source, client version, change statistics, and typed links to jobs,
  simulations, tickets and attachments. Never expose or store secrets in
  provenance. Signed data needs specified canonical bytes and verification.
- **Diff pagination**: current 1,000-change cap returns 413; provide bounded
  cursor paging / streaming aggregate summaries so large revisions can be
  compared without partial or silently truncated results.
- **Merge dry-run**: conflict-preview / merge-base / typed diff and validation
  without publishing. Existing `POST /merges` is mutating and 409-driven, not
  a standalone dry-run. Explicit conflict-resolution choices are pinned to
  both branch heads; expose a stable preview token or require revalidation.
- **Activity and audit**: immutable events for project/branch lifecycle,
  annotation edits, tag moves, ACL edits, merges, and bundle imports; actor,
  server time, old/new versions, correlation ID and pagination.

## P2: operational requirements and cross-cutting decisions

- Authentication/authorization and authorization-gated listings, read-only
  roles, per-project edit/merge/delete capabilities, protected branches,
  retention policies and recovery. Current HTTP interface must not be treated
  as production-safe on an untrusted network.
- UUID-based identities, explicit `not_found` versus `forbidden`, safe
  idempotency keys for creates, optimistic concurrency/ETags for metadata
  mutations and documented retryability. Standard structured 409 conflicts,
  no last-writer-wins overwrites.
- Support project lifecycle in offline bundle exchange: metadata provenance,
  release refs, annotations and permission scope; clearly distinguish portable
  history from local-only ACL/audit information. Plan backward compatible
  readers/writers and schema migrations.
- Responsive large repositories: limits, cursor stability, incremental graph
  refresh, cancellation, request deadlines, reconnect, asynchronous work,
  caching/invalidations and cross-user notifications when HEAD changes.
- If metadata is placed in an SQL component, every schema, query, migration,
  concurrency transaction and pagination method **must work with both SQLite
  and PostgreSQL**, not rely on dialect-specific behavior without equivalent
  implementations. The current native store itself is not a claim of either
  SQL back-end integration.

## Accepted GUI behavioral contract

**Target behavior, not currently implemented as a complete GUI editing
session:** opening a branch pins its HEAD and network snapshot; local edits
mark the working model dirty; saving creates a new immutable revision against
the exact expected HEAD; historical snapshots open read-only; switching
away from unsaved edits offers Save / Discard / Cancel; conflicts preserve
the working copy; merges are explicit, reviewable operations. Revision
annotations are separate from immutable model data.

**Already implemented in the GUI:** project/branch discovery, read-only
revision-history browsing (limited first-parent pages), project creation,
branch creation/fork-from-revision, multi-source visibility, automatic remote
health monitoring and catalogue refresh. Do not confuse these with network
checkout, Save-to-DVMDB, rename/delete or full DAG traversal.

## Suggested implementation order

1. Finalize canonical metadata/annotation storage semantics and compatibility,
   project/branch lifecycle API, network listing and server-side permission model.
2. Add repository/service tests (including SQLite and PostgreSQL variants if
   any SQL is introduced), OpenAPI schemas, pagination and concurrent-HEAD
   conflict tests; avoid introducing API paths before implementation.
3. Integrate a GUI ProjectSession and adapter. Test GUI-only navigation and
   selections separately from server-backed CRUD/checkout/save.
4. Add graph DAG pagination, compare, branch-from-history, dry-run merge,
   review/approval UX and bundle interactions after basic persistence is safe.
