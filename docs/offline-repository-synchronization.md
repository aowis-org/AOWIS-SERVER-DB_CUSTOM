# Portable and incremental repository synchronization

Independent AOWIS native repositories exchange revision history through `.aowbundle` files. A bundle contains a branch tip, its immutable revision ancestry (including both parents of merges), Merkle data and reference-index objects, and all payload objects required for that transfer. It does not depend on the sender's physical pack layout.

## Core native APIs

- `Repository::exportBranchBundle(branch, path)` produces a complete self-contained transfer. Path exports publish the finished, fsynced bundle atomically at a previously unused destination name (including on the incremental path). A crash before publication leaves the final name absent; a crash after publication leaves a complete bundle, though the name may not yet be durable until the parent directory is synchronized. Caller-owned descriptor exports are not published by the native engine.
- `Repository::exportIncrementalBranchBundle(branch, baseRevision, path)` sends the revisions reachable from the branch tip but not from the known base revision and, for each of them, the trie objects that differ from one of its parents (preferably a parent the recipient already holds). The exporter reads the base's revision ancestry and the changed trie paths only, not the base's trees. The bundle can contain objects the recipient already has (for example when an older snapshot is restored); the importer does not store them twice.
- `Repository::importBranchBundle(path, destinationBranch)` verifies hashes and full object graph before publication, then creates or **fast-forwards** the destination branch. An already incorporated tip is an idempotent no-op. Divergent history is **never overwritten** automatically.

A base revision must be in the sender's ancestry and complete in the recipient's store. This is a base-revision delta, not general object-inventory negotiation. A first transfer to a genuinely unrelated recipient uses a full bundle.

## Offline example

```bash
./build-linux/aowis-native-sync export \
  /path/to/source main /tmp/main.aowbundle
./build-linux/aowis-native-sync import \
  /path/to/destination /tmp/main.aowbundle remote_main

base="$(./build-linux/aowis-native-sync head \
  /path/to/destination remote_main)"
./build-linux/aowis-native-sync export-incremental \
  /path/to/source main "$base" /tmp/update.aowbundle
./build-linux/aowis-native-sync import \
  /path/to/destination /tmp/update.aowbundle remote_main
```

Imports verify object hashes, structural record types, revision ancestry and object-graph completeness before publishing a branch. Merkle tries are additionally checked bottom-up: leaf UUIDs must follow their complete paths through all ancestor branches; compressed prefixes, branch-depth ordering, child slots, nonzero leaf value hashes and canonical prefix padding must be valid. These checks reject duplicate UUIDs and other correctly hashed but malformed tries. Immutable subtrees can be shared between revisions without validating their descendants repeatedly. Rejected bundles do not publish branch refs or append imported objects. The same checks apply to full and incremental imports.

Model-indexed revisions undergo an additional semantic consistency check: canonical model-record encoding and sorted unique dependency UUIDs, nonzero/self-reference rules, live dependency targets, and *exact* equality between model-derived incoming references and the stored reverse-reference Merkle index. Every revision carried by the bundle is included, including both merge parents. Every first model snapshot after a raw revision is checked in full; subsequent snapshots with a verified model-indexed parent validate Merkle differences in both the model and reverse-reference trees. Unchanged content-addressed subtrees inherit only the independently verified parent snapshot invariants. Changed model edges must exactly equal changed reverse-index edges, newly introduced targets must exist, and deleting a still-referenced UUID is rejected even if its referrers are unchanged. Both merge parents are verified before the merge revision; ancestry is traversed iteratively, avoiding C++ recursion over long histories. Immutable snapshots with the same pair of roots are checked once per import. Invalid histories are rejected before any imported data or branch ref is written; imported hashes are never silently rewritten to repair history.

**Integrity scope:** Imported bundles and persisted repositories use the same generic logical validation. `aowis-native-verify` performs full on-disk physical and logical verification by default (`--physical` restricts it to physical checks). Neither the importer nor this checker validates opaque domain-specific hydraulic semantics. The import location index, per-object logical visitation/type marks, and imported Merkle-tree summaries are file-backed and automatically discarded; the first-pass SHA-256 scan uses a fixed 64 KiB scratch buffer. Local objects referenced by incremental bundles are boundaries (see below) and keep only small in-memory state. Revision ancestry and snapshot-root bookkeeping retain state proportional to history size. The baseline full-snapshot reference verifier sorts dependency edges in 8,192-entry memory chunks and merges file-backed sorted runs. Later revisions compare only changed Merkle paths, model references, and reverse-reference entries against an already verified parent, without retaining a complete in-memory edge map. Deterministic crash-recovery tests exist, but actual power-loss behavior still needs field validation. See [repository verification](repository-verification.md). Missing objects, invalid baselines and branch divergence are rejected. Object hashes and revision identities remain portable between independent pack repositories.

**Incremental import work follows the delta.** Objects that an incremental bundle references but does not carry must already be stored locally. Everything in a repository was verified when it was committed or imported, so such an object is a *boundary*: the importer reads that one object and checks it where it attaches to the bundled graph, without walking the subtree or ancestry below it again.

- A local trie node supplies its parent check from one read: a leaf's UUID, or a branch's depth and canonical prefix. Depth ordering, child slot and prefix are checked against the bundled parent exactly as for bundled nodes.
- Value kinds stay enforced. Every model or reverse-index leaf that differs from the verified parent snapshot is decoded by the semantic check, so a local subtree grafted into the wrong kind of tree is rejected there. A local subtree used as a raw tree must hold `V` values (one leaf decides, values of a stored subtree are homogeneous). Values referenced by bundled leaves are always read and checked.
- A local revision must be a valid revision object; its snapshot counts as verified and its ancestry is not walked. The base (and, for a fast-forward, the current branch head) must be a boundary or an ancestor of one; only that last case reads local revision history.

A 1-record change on a 2,000-record model reads about 100 local objects, independent of the history length; previously the importer re-verified the whole local history (about 9,400 reads at 50 commits, 200,000 at 800). `aowis-native-verify` still verifies complete histories.

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

**Full export memory behavior:** The native full-history exporter keeps its
reachable-hash deduplication table in private, unlinked scratch files under the
repository `tmp/` directory and externally sorts hashes in fixed-size chunks.
Objects are still emitted in ascending SHA-256 order and bundle encoding is
unchanged. The scratch table uses positional file I/O (not writable mmap), so
out-of-space errors remain catchable. Peak working memory excludes a full
in-memory reachable-hash set, but still includes the traversal stack, sort-run
metadata, and one decoded object at a time. Very large exports consume disk
space; global scratch quotas and performance measurements remain future work.
Incremental exports retain their in-memory delta set and fast Merkle-difference
traversal.

**Current resource model:** HTTP downloads are streamed in 64 KiB chunks from a nameless scratch file in the project repository's `tmp/`, which disappears when the response ends, including on disconnect. HTTP uploads are streamed into such a scratch file while they arrive (no complete body in memory; a full disk returns 507). No transfer file uses `TMPDIR`. The default binary transfer ceilings are **32 MiB per bundle**, **100,000 objects** and **8 MiB per object**, configurable via `[sync]` and corresponding `--sync-*` CLI switches. The HTTP server rejects bodies above the configured ceiling before or while reading them, on every platform. Native import and export enforce all three ceilings independently. The native importer hashes individual bundle objects in 64 KiB chunks and indexes object locations in a private, nameless scratch file in the repository's `tmp/` (48 bytes per transferred object). Its file-backed location index is mapped read-only. Logical verification also uses a private, unlinked, one-byte-per-imported-object file mapping to track reachable object types without duplicating per-object heap maps/sets. This temporary space is preallocated so disk exhaustion fails as an ordinary import error, rather than a sparse-mapping SIGBUS. The kernel manages mapped-page residency. Imported Merkle-node summaries use a preallocated, file-backed table of 60 bytes per transferred object (three independently validated tree-kind slots, including for objects that are not trees); cold pages can be evicted by the OS. Imported revision membership and revision enumeration now reuse the file-backed visited-kind marks, avoiding a separate heap-allocated set of imported revision hashes. Revision graph traversal still uses a pending stack; boundary objects and distinct model snapshot-root pairs consume RAM proportional to the delta. Initial model snapshots process dependency edges in 8,192-entry in-memory chunks (256 KiB of edge data), spilling sorted runs to a private unlinked temporary file as necessary. Subsequent model-indexed revisions with a verified parent compare only changed model and reference-index paths; they temporarily hold their edge changes in memory. When compressed Merkle structure changes, the affected subtree leaves are merged by UUID. This optimizes histories with mostly unchanged data but does not impose a fixed memory ceiling on a single very large revision change. The run merge retains one small heap entry per run and traverses each reverse-index value in turn; decoded individual model objects and reverse-index values still require memory. Decoded objects are still held in memory one at a time, so this is *not* end-to-end constant-memory streaming, and verification of a full bundle may be CPU-intensive for long histories. The native CLI remains the path for transfers above the configured HTTP ceilings and until the remaining history-state optimizations are implemented. HTTP transfers stream through scratch files and run on worker threads; an import or export serializes only with other scoped operations of the same project; other projects are not blocked. Do not treat these limits as an authorization/security boundary.

The initial HTTP server deliberately has no authentication, authorization enforcement or HTTPS pending the AOWIS-wide security design.

Portable bundles verify integrity through SHA-256, **not authorship or permission to publish**. The application service is responsible for future authorization after the unified security design exists.
