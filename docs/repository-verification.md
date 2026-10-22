# Repository integrity verification

The AOWIS native database provides read-only physical and logical verification for both physical storage backends. The full checker uses the **same logical graph validator as replication import**, so accepted portable histories and persisted histories obey the same structural rules.

## Administrative commands

```bash
./build-linux/aowis-native-verify /path/to/repository
./build-linux/aowis-native-verify --full /path/to/repository
./build-linux/aowis-native-verify --physical /path/to/repository
```

**Full is the default.** A successful full check returns exit code 0 and prints physical object count, reachable objects, branch count, historical revision count, verified trie-node count and distinct model snapshot count. `--physical` skips logical checks and explicitly reports that they were not performed. Any failed check returns exit code 1; invalid CLI arguments return exit code 2.

## What full verification checks

1. **Physical objects:** The object-pack verifier checks every stored record's SHA-256, framing and index correspondence. This includes unreachable objects, not just published history.
2. **Published history:** Every branch head points to a correctly typed revision; all reachable parents, including both parents of merges, exist and have valid serialized revision metadata. Multiple branches and shared ancestry are accepted.
3. **Merkle structure:** Every tree reachable from a revision has canonical leaves/branches, legal compressed prefixes and depth ordering, correct UUID-to-child-slot paths and nonzero leaf value hashes. Associated raw/model/index payload objects must have the expected type and structure.
4. **Model graph:** Every model-indexed revision has canonical model records, valid nonzero/non-self dependencies to existing model UUIDs, and a reverse-reference index exactly matching the model records. Historical revisions are checked, not just current branch tips. Shared immutable model snapshots are verified only once during a check.

The checker holds the repository's interprocess lock throughout verification so branch heads do not change midway through a check. It does **not** alter branch heads, rewrite revisions, repair indexes semantically or discard invalid histories.

## Explicit limits

- **Domain-specific hydraulic validation is not run.** A full native-storage check verifies generic typed model envelopes, dependency indexes, revision history and canonical object structure. It does not establish that complete `NetworkHydraulic` objects obey application-specific rules. The CLI states this explicitly.
- **Physical maintenance may occur on open:** The repository constructor can rebuild the derived physical index or discard an incomplete, unpublished pack tail as part of its existing recovery behavior. Verification itself is read-only with respect to published logical state.
- **Unreachable valid objects:** Full verification checks their physical integrity but has no logical root from which to interpret their type. This is intentional; unreferenced objects may be left by interrupted transactions.
- **Resource use:** A full historical check can take substantial CPU, memory and storage I/O, particularly when a repository contains many distinct model snapshots. Per-snapshot dependency edges are sorted with a bounded 8,192-entry buffer and file-backed runs rather than an unbounded in-memory map of incoming references. Revision, Merkle-subtree and snapshot tracking remain in memory, so this is not yet a bounded-memory streaming audit.
- **No automatic repair:** If full verification fails, keep an untouched backup of the repository and investigate the reported corruption. Do not reset branch heads or regenerate model/reference objects silently: that would change published revision identities.

Run full verification after unexpected shutdowns and before/after backup recovery or replication troubleshooting. A separate recovery suite now exercises deterministic process termination, simulated `ENOSPC`, torn writes, index rebuilding, incremental-import retries and interrupted compaction on both physical backends. These tests establish software recovery behavior at controlled checkpoints, **not** hardware power-failure guarantees. Validate restored offline backups with this full verifier before reuse.
