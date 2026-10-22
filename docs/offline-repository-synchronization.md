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

Model-indexed revisions undergo an additional full logical check: canonical model-record encoding and sorted unique dependency UUIDs, nonzero/self-reference rules, live dependency targets, and *exact* equality between model-derived incoming references and the stored reverse-reference Merkle index. Every revision reachable from the imported tip is included, even old historical revisions or locally supplied ancestors of an incremental bundle. Immutable snapshots with the same pair of roots are checked once per import. Invalid histories are rejected before any imported data or branch ref is written; imported hashes are never silently rewritten to repair history. Both physical backends use the same rules.

**Integrity scope:** Imported bundles and persisted repositories use the same generic logical validation. `aowis-native-verify` performs full on-disk physical and logical verification by default (`--physical` restricts it to physical checks). Neither the importer nor this checker validates opaque domain-specific hydraulic semantics. Bounded-memory verification for very long histories and crash-recovery fault-injection tests remain separate planned work. See [repository verification](repository-verification.md). Missing objects, invalid baselines and branch divergence are rejected. Exported IDs remain the same across pack and block physical backends.

## Branch collaboration

Import into a dedicated tracking branch (for example `remote_main`), inspect it, and use generic or hydraulic-aware three-way merging into the working branch. A bundle import does not silently create a merge, rewrite history or resolve model conflicts.

## HTTP transport direction

The **bundle APIs, format and file-based CLI** are available now. HTTP transport will use project-scoped REST/binary transfer endpoints. The exact routes and streaming semantics are proposed in [REST design](rest-api-design.md), **not implemented yet**. The initial HTTP server will deliberately have no authentication, authorization enforcement or HTTPS pending AOWIS-wide security design.

Portable bundles verify integrity through SHA-256, **not authorship or permission to publish**. The application service is responsible for future authorization after the unified security design exists.
