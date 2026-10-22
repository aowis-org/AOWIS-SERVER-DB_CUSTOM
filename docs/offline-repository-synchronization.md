# Portable and incremental repository synchronization

Independent AOWIS native repositories exchange revision history through `.aowbundle` files. A bundle contains a branch tip, its immutable revision ancestry (including both parents of merges), Merkle data and reference-index objects, and all payload objects required for that transfer. It does not depend on the sender's physical pack layout.

## Core native APIs

- `Repository::exportBranchBundle(branch, path)` produces a complete self-contained transfer.
- `Repository::exportIncrementalBranchBundle(branch, baseRevision, path)` sends objects reachable from the branch tip but not from the known base revision.
- `Repository::importBranchBundle(path, destinationBranch)` verifies hashes and full object graph before publication, then creates or **fast-forwards** the destination branch. An already incorporated tip is an idempotent no-op. Divergent history is **never overwritten** automatically.

A base revision must be in the sender's ancestry and complete in the recipient's store. This is a base-revision delta, not general object-inventory negotiation. A first transfer to a genuinely unrelated recipient uses a full bundle.

## Offline example

```bash
./build-linux/aowis-native-sync export \
  /path/to/source blocks main /tmp/main.aowbundle
./build-linux/aowis-native-sync import \
  /path/to/destination pack /tmp/main.aowbundle remote_main

base="$(./build-linux/aowis-native-sync head \
  /path/to/destination pack remote_main)"
./build-linux/aowis-native-sync export-incremental \
  /path/to/source blocks main "$base" /tmp/update.aowbundle
./build-linux/aowis-native-sync import \
  /path/to/destination pack /tmp/update.aowbundle remote_main
```

Imports verify object hashes, structural record types, revision ancestry and object-graph completeness before publishing a branch. Merkle tries are additionally checked bottom-up: leaf UUIDs must follow their complete paths through all ancestor branches; compressed prefixes, branch-depth ordering, child slots, nonzero leaf value hashes and canonical prefix padding must be valid. These checks reject duplicate UUIDs and other correctly hashed but malformed tries. Immutable subtrees can be shared between revisions without validating their descendants repeatedly. Rejected bundles do not publish branch refs or append imported objects. The same checks apply to full and incremental imports and to both physical backends.

Model-indexed revisions undergo an additional semantic consistency check: canonical model-record encoding and sorted unique dependency UUIDs, nonzero/self-reference rules, live dependency targets, and *exact* equality between model-derived incoming references and the stored reverse-reference Merkle index. Every revision reachable from the imported tip is included, even old historical revisions or locally supplied ancestors of an incremental bundle. Every first model snapshot after a raw revision is checked in full; subsequent snapshots with a verified model-indexed parent validate Merkle differences in both the model and reverse-reference trees. Unchanged content-addressed subtrees inherit only the independently verified parent snapshot invariants. Changed model edges must exactly equal changed reverse-index edges, newly introduced targets must exist, and deleting a still-referenced UUID is rejected even if its referrers are unchanged. Both merge parents are verified before the merge revision; ancestry is traversed iteratively, avoiding C++ recursion over long histories. Immutable snapshots with the same pair of roots are checked once per import. Invalid histories are rejected before any imported data or branch ref is written; imported hashes are never silently rewritten to repair history. Both physical backends use the same rules.

**Integrity scope:** Imported bundles and persisted repositories use the same generic logical validation. `aowis-native-verify` performs full on-disk physical and logical verification by default (`--physical` restricts it to physical checks). Neither the importer nor this checker validates opaque domain-specific hydraulic semantics. The import location index, per-object logical visitation/type marks, and imported Merkle-tree summaries are file-backed and automatically discarded; the first-pass SHA-256 scan uses a fixed 64 KiB scratch buffer. Local objects referenced by incremental bundles still use in-memory fallback maps for visitation state and Merkle summaries. Revision ancestry and snapshot-root bookkeeping retain state proportional to history size. The baseline full-snapshot reference verifier sorts dependency edges in 8,192-entry memory chunks and merges file-backed sorted runs. Later revisions compare only changed Merkle paths, model references, and reverse-reference entries against an already verified parent, without retaining a complete in-memory edge map. Deterministic crash-recovery tests exist, but actual power-loss behavior still needs field validation. See [repository verification](repository-verification.md). Missing objects, invalid baselines and branch divergence are rejected. Exported IDs remain the same across pack and block physical backends.

## Branch collaboration

Import into a dedicated tracking branch (for example `remote_main`), inspect it, and use generic or hydraulic-aware three-way merging into the working branch. A bundle import does not silently create a merge, rewrite history or resolve model conflicts.

## HTTP bundle synchronization

The same immutable `.aowbundle` format is available over HTTP, with project/branch scoping by ID rather than filesystem path:

- `GET /api/v1/projects/{projectId}/branches/{branch}/bundle` exports a full bundle.
- `GET .../bundle?base={revisionHash}` exports a delta relative to a known ancestor.
- `POST .../bundle` accepts the binary body (`Content-Type: application/vnd.aowis.bundle`), creates or fast-forwards the target branch, and returns `created`, `fast_forward` or `up_to_date` with its new head and imported object count.

For example, download from one server and import to another:

```bash
curl --fail -o update.aowbundle 'http://127.0.0.1:8755/api/v1/projects/PROJECT_ID/branches/main/bundle'
curl --fail -X POST -H 'Content-Type: application/vnd.aowis.bundle' \
  --data-binary @update.aowbundle \
  'http://127.0.0.1:8756/api/v1/projects/OTHER_PROJECT_ID/branches/remote_main/bundle'
```

The importer performs the same hash, trie, revision and model/reference validation as offline imports before writing objects or publishing a branch. Reimports are idempotent; divergent histories are HTTP 409. Full/delta endpoints return HTTP 413 for configured transfer ceilings.

**Current resource model:** HTTP downloads use a file-backed `QIODevice` supplied to `QHttpServerResponder`: Qt sends file contents in chunks, and the private export directory is deleted when the responder releases the device, including on disconnect. HTTP uploads still buffer complete request bodies in Qt before staging them in a private temporary directory. The default binary transfer ceilings are **32 MiB per bundle**, **100,000 objects** and **8 MiB per object**, configurable via `[sync]` and corresponding `--sync-*` CLI switches. Qt 6.11+ rejects bodies above the configured ceiling while receiving them; older Qt versions only enforce the route-level limit after Qt has buffered the body. Native import and export enforce all three ceilings independently. The native importer hashes individual bundle objects in 64 KiB chunks and indexes object locations in a private, unlinked temporary file (48 bytes per transferred object). Its file-backed location index is mapped read-only. Logical verification also uses a private, unlinked, one-byte-per-imported-object file mapping to track reachable object types without duplicating per-object heap maps/sets. This temporary space is preallocated so disk exhaustion fails as an ordinary import error, rather than a sparse-mapping SIGBUS. The kernel manages mapped-page residency. Imported Merkle-node summaries use a preallocated, file-backed table of 60 bytes per transferred object (three independently validated tree-kind slots, including for objects that are not trees); cold pages can be evicted by the OS. Imported revision membership and revision enumeration now reuse the file-backed visited-kind marks, avoiding a separate heap-allocated set of imported revision hashes. Revision graph traversal still uses a pending stack; locally resident incremental ancestors, their Merkle summaries, and distinct model snapshot-root pairs still consume RAM proportional to their respective sizes. Initial model snapshots process dependency edges in 8,192-entry in-memory chunks (256 KiB of edge data), spilling sorted runs to a private unlinked temporary file as necessary. Subsequent model-indexed revisions with a verified parent compare only changed model and reference-index paths; they temporarily hold their edge changes in memory. When compressed Merkle structure changes, the affected subtree leaves are merged by UUID. This optimizes histories with mostly unchanged data but does not impose a fixed memory ceiling on a single very large revision change. The run merge retains one small heap entry per run and traverses each reverse-index value in turn; decoded individual model objects and reverse-index values still require memory. HTTP still buffers whole upload request bodies; downloaded bundles no longer require a second full-size response buffer. Consequently this is *not* end-to-end constant-memory streaming, and verification may be CPU-intensive for long histories. The native CLI remains the path for large transfers until true HTTP streaming and the remaining history-state optimizations are implemented. Qt QHttpServerRequest exposes an already-assembled QByteArray body, not an incremental upload device; genuine streaming uploads need a separate ingestion transport rather than writing that body to disk after reception. Concurrent HTTP handler execution is synchronous and can temporarily delay unrelated requests; do not treat these limits as an authorization/security boundary.

The initial HTTP server deliberately has no authentication, authorization enforcement or HTTPS pending the AOWIS-wide security design.

Portable bundles verify integrity through SHA-256, **not authorship or permission to publish**. The application service is responsible for future authorization after the unified security design exists.
