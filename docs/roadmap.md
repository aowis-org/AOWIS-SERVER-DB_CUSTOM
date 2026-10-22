# AOWIS Versioned Model Database — development roadmap

The AOWIS database has one native storage engine, usable directly as a C++ library and, eventually, through a multi-project HTTP/TCP service. This roadmap prioritizes database correctness and recovery before introducing the network service.

## Current baseline

The repository implements immutable SHA-256 objects, copy-on-write Merkle UUID trees, revision-based atomic publication, model/reference indexes, hydraulic persistence, branches, merging, and full/incremental offline synchronization. Both native physical backends share the logical object and revision format.

Bundle imports check object hashes, reachable object types and graph completeness; they also validate canonical Merkle structure, including UUID-to-path correspondence, compressed prefixes, branch depth ordering, child slots and nonzero leaf targets. These validations precede object publication on both backends. Model-indexed imported revisions additionally require canonical model references, existing dependency targets, and exact equivalence between model-derived edges and the reverse-reference index (including historical and incremental revisions). The same generic logical validator is now available for existing repositories, with physical + logical verification the default mode of `aowis-native-verify`. Full native checks deliberately do not claim application-specific hydraulic semantic validation.

The HTTP service executable is not yet implemented. The provisional access-policy library is not part of the planned HTTP service authorization model.

## Implementation order

### 1. Cross-check model references during replication — implemented

- Independently reconstruct incoming-reference relationships for every model-indexed revision reachable from an imported tip and compare them exactly against the stored reverse-reference Merkle index.
- Reject missing, extra, dangling, invalid zero/self and noncanonical dependency references, including invalid historical revisions; preserve original revision hashes.
- Reuse verified immutable model/index root pairs across historical revisions within one import.
- Regression-test SHA-256-valid forged histories and unsafe-deletion bypasses, full/incremental replication, and both physical backends (including cross-backend transfers).

### 2. Comprehensive native repository verification — implemented

- Shared importer/on-disk verifier checks all physical objects, complete published ancestry, canonical Merkle paths and generic model/reference consistency across both storage backends.
- Full verification is the default; `--physical` checks physical integrity only, and the CLI explicitly states that application-specific hydraulic semantics are not checked.
- Adversarial tests inject hash-valid logical corruption directly into both physical backends and check correct rejection, physical/logical separation, CLI status codes, valid merges and compaction.
- Future: a separate domain-aware semantic audit, bounded-memory verification and fault-injection/recovery procedures.

### 3. Consistent snapshots and concurrency — implemented

- Branch-based hydraulic network loads and targeted member/ID-list queries capture one revision before reading any root, membership chunks, reverse-reference indexes or entities. Explicit `*AtRevision()` APIs permit stable historical snapshots and multi-call consistency while branch heads advance.
- Complete saves and conservative edit fallbacks use their starting revision for preparation; `commitModel()` still atomically checks the expected head at publication. Indexed edits retain the same stale-writer protection.
- Qt-enabled regression tests exercise historical root/chunk/record visibility, invalid/missing revisions, and concurrent writers/readers with whole-network generation markers on both physical backends.
- Future: domain-specific semantic repository audits, and more systematic concurrency scheduling/fault injection as part of the crash-recovery stage.

### 4. Crash recovery and fault injection

- Exercise failures and abrupt process termination at object writes, pack/index sync, branch-ref publication, block generation switches, bundle imports and compaction.
- Verify each recovered repository with both physical and logical checks; cover disk-full conditions, multi-process locking, and backup/restore procedures.

### 5. Multi-project HTTP server

- Implement `aowis-server-db` using Qt QHttpServer with configurable bind address and TCP port.
- Add a durable project catalogue, repository lifecycle manager, branch/project management and readiness endpoints.
- Keep the native engine available as a direct C++ library and make HTTP handlers use the same application operations.
- Defer authentication, authorization and transport TLS to the planned AOWIS-wide security architecture.

### 6. Native model, history and merge REST API

- Expose historical model/entity reads, diffs, atomic edit batches, network reads, branch history, merge previews, conflict resolutions and validated merge publication.
- Require expected revision heads on writes; use stable errors and a published OpenAPI contract.
- Test equivalence of HTTP operations and direct-library operations.

### 7. HTTP synchronization and resource bounds

- Transport full/incremental native bundles without changing content hashes or inventing a second persistence protocol.
- Limit transfer size, object size/count, verification memory/work, temporary disk use and simultaneous transfers.
- Verify that corrupted or incomplete transfers cannot modify the visible branch; test cross-backend synchronization and retries.

### 8. Scalability and performance

- Improve common-ancestor search for long histories and shared merge ancestry while preserving correct merge-base semantics.
- Profile and bound bundle verification memory, using disk-backed structures or streaming where needed.
- Benchmark representative 10k–1M hydraulic-entity projects, long revision histories, branching/merging, synchronization and compaction on both backends.
- Use unique fresh temporary directories for every benchmark run.

### 9. Build strictness, maintainability and release preparation

- **Implemented:** Fail CMake configuration when model codec support is requested but Qt6 is unavailable; retain an explicit native-only build mode.
- Require Debug and Release builds and corresponding native/Qt-enabled tests, including adversarial regression coverage.
- Eliminate obsolete development-stage names in public interfaces, apply the project's C++ conventions and keep documentation descriptive of the current architecture.
- Establish and freeze the first public persistence format before introducing any historical migration machinery.

## Principles

- Use the native storage engine exclusively; do not add SQL adapters or a general-purpose SQL language.
- Preserve immutable object and revision identities across physical backends and transport mechanisms.
- Validate untrusted imports before publication. Hash correctness alone is not database correctness.
- Prefer comprehensive regression, adversarial and fault-injection tests over optimistic production-readiness claims.
- Never weaken model validation for benchmarking or expediency.
- Keep implementation documentation synchronized with actual capabilities, especially when a feature is not yet implemented.
