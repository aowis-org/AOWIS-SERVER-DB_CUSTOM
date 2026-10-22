# AOWIS Versioned Model Database — development roadmap

The AOWIS database has one native storage engine, usable directly as a C++ library and through an initial multi-project HTTP/TCP service. This roadmap prioritizes correctness and recovery while expanding network functionality.

## Current baseline

The repository implements immutable SHA-256 objects, copy-on-write Merkle UUID trees, revision-based atomic publication, model/reference indexes, hydraulic persistence, branches, merging, and full/incremental offline synchronization. Both native physical backends share the logical object and revision format.

Bundle imports check object hashes, reachable object types and graph completeness; they also validate canonical Merkle structure, including UUID-to-path correspondence, compressed prefixes, branch depth ordering, child slots and nonzero leaf targets. These validations precede object publication on both backends. Model-indexed imported revisions additionally require canonical model references, existing dependency targets, and exact equivalence between model-derived edges and the reverse-reference index (including historical and incremental revisions). The same generic logical validator is now available for existing repositories, with physical + logical verification the default mode of `aowis-native-verify`. Full native checks deliberately do not claim application-specific hydraulic semantic validation.

The initial multi-project HTTP service executable is implemented, with project/catalogue, branch and history endpoints. Hydraulic network reads/writes, indexed entity edits, revision diffs and semantic merges now have HTTP routes; bounded full/incremental binary HTTP replication is implemented, with file-backed bundle downloads; streaming upload requests remain a future milestone. The provisional access-policy library is not part of the planned HTTP service authorization model.

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

### 4. Crash recovery and fault injection — implemented initial recovery matrix

- A dedicated recovery-test engine (never shipped) injects abrupt `_exit()` and simulated `ENOSPC` at object writes, pack/index synchronization, branch-ref publication, block commit-marker publication, incremental import and compaction generation switches. Torn object/marker/ref writes are also exercised.
- Subprocess recovery on both physical backends checks the recovered head (old before publication, new after), complete native logical + physical verification, model/reference consistency, and successful subsequent commits or import retries. Tests also cover damaged derived indexes, offline copy/restore and cross-process handle restrictions.
- **Fixed:** a failed pack compaction after switching the generation pointer could leave a writable handle bound to the old pack. After a post-switch error, either backend now requires reopening; the recovery suite exercises this on the original handle.
- **Still required:** real power-loss tests (including filesystem cache/device behavior), sustained disk-exhaustion scenarios and a documented external backup/restore operational procedure. Deterministic process termination and injected `ENOSPC` cannot prove power-failure durability across every filesystem or storage device.

### 5. Multi-project HTTP server — implemented foundation

- Implemented `aowis-server-db` using Qt QHttpServer, configurable INI/CLI bind address, port, data root and default storage backend.
- Implemented durable UUID-named project catalogue, atomic staged publication, restart/locking checks, native C++ project manager, project and branch routes, history and readiness endpoints.
- The native engine and `AOWIS::NativeProjectService` remain callable directly without Qt; HTTP adapters use the same project manager.
- Authentication, authorization and transport TLS remain outside this service until the planned AOWIS-wide security architecture.
- Real TCP HTTP tests are included for Qt-enabled builds; requests are JSON-validated but size limits currently apply after Qt buffering. Further bounds, model routes, merge and synchronization are separate milestones.

### 6. Native model, history and merge REST API — implemented initial operations

- Implemented revision-pinned complete hydraulic network and targeted entity reads, diffs, atomic entity edit batches, full-network saves, conflict resolution and validated merge publication. Standalone preview-only merge, network enumeration and streaming large responses remain future work.
- Writes require expected branch heads enforced again at native publication; conflict responses preserve branch tips. API contracts include canonical ModelCodec payloads in Base64, not a duplicate JSON model schema.
- Qt HTTP integration tests exercise canonical codec round trips, immutable history, stale-write rejection, errors, edits and semantic merge conflicts/choices on both physical backends.

### 7. HTTP synchronization and resource bounds — bounded-buffer phase implemented

- Implemented full/incremental binary `.aowbundle` export and import on project-scoped HTTP routes, preserving SHA-256 objects and existing native validation. A transfer creates, fast-forwards, or idempotently acknowledges an existing tracking branch; divergences return 409.
- Configurable file byte/object-count/individual-object limits are enforced in native import/export and again at the HTTP boundary. Qt 6.11+ enforces maximum request-body size before full buffering. Downloads are also capped; temporary import files live in private unique directories.
- Native adversarial tests exercise limit rejection without branch/object writes on both physical backends. Qt integration tests exercise full/incremental transfers between backend types, truncated/forged content, retries, conflicting branches and interrupted uploads.
- **Implemented (download memory):** HTTP bundle GET now passes a private file-backed QIODevice to QHttpServerResponder, which reads it in chunks. The device owns its temporary directory until response completion or disconnect; tests cover large downloads, repeated reads and client cancellation.
- **Still required:** streaming request ingestion/upload, disk-backed graph-validation metadata for huge histories, CPU/time budgets, explicit concurrent-transfer quotas, and robust large-repository benchmarks. Qt still buffers incoming POST bodies; export preparation and import verification are synchronous.

### 8. Scalability and performance — merge ancestry optimization implemented

- Implemented an early up-to-date exit, compact target-ancestor hash set and source-side pruning at the first common revision on each path. Multiple frontier candidates still undergo ancestry-based dominance checks; ambiguous criss-cross merge bases are rejected rather than guessed.
- Added a deterministic exhaustive-ancestry oracle, randomized raw and model-indexed merge DAGs, dominated common-frontier and criss-cross cases, long shared histories and both physical backends as regression tests.
- Added an isolated native merge-history conflict benchmark; it excludes publication/fsync while retaining merge-base and conflict validation. Benchmarks must use new random temporary repository paths for each run.
- Further improve merge ancestry for very deep unrelated histories if profiling shows a need (the target-ancestor set still scales with target history).
- **Implemented (first replication-memory step):** Native bundle import writes hash-to-offset metadata into a private, immediately unlinked temporary file and maps that fixed-size index read-only; the checksum preflight hashes objects with a fixed 64 KiB scratch buffer instead of allocating their full payloads. Full/incremental imports, invalid transfers, empty deltas and all four physical-backend combinations are regression-tested. This removes the per-object in-memory location map; it does **not** make logical verification constant-memory.
- **Implemented (second replication-memory step):** For bundle objects, logical verification now keeps a one-byte type/visited mask per hash in an unlinked, file-backed mapping keyed by the sorted bundle index. This removes both the heap-based per-imported-object type map and the duplicate reachable-object set. Local ancestors needed by incremental imports still use an in-memory fallback map. Both mappings are removed on success or error; the writable visitation mapping is preallocated so disk exhaustion is handled before writing to mapped pages. Tests exercise thousands of objects, correctly hashed but unreachable entries, retries, invalid imports and all four backend combinations. The location index requires 48 bytes per received object and the visitation table 1 byte per received object, plus kernel-managed resident pages.
- **Implemented (third replication-memory step):** Model/reference integrity checking no longer builds an in-memory set of all model UUIDs and a map-of-sets of all incoming edges for every immutable model snapshot. It emits `(target, source)` edges into 8,192-entry sorted chunks, spills and merges runs using a private unlinked temporary file, and checks the sorted result against the stored reverse-reference tree. Dependency existence is checked with a targeted Merkle lookup per distinct reference target. Small snapshots remain entirely in memory. Adversarial tests force multiple disk-backed runs and check missing incoming edges, publication safety, and temporary cleanup on both backends.
- **Implemented (fourth replication-memory step):** Imported Merkle-node verification summaries are indexed by bundle object ordinal in a private, unlinked, preallocated, file-backed table (60 bytes per transferred object; separate slots for raw, model and reference tries). The active recursion cache remains bounded by trie depth, and local historical objects needed for incremental imports retain a normal map fallback. Cross-backend tests exercise large shared histories, historical raw/model reuse of identical tries, integrity-report equivalence, malformed inputs and temporary cleanup. This improves heap scaling but uses additional temporary disk space.
- **Implemented (fifth replication-memory step):** Imported revision ancestry uses the bundle's existing file-backed visited-kind bitmap for membership queries and enumerates verified revision hashes directly from the sorted bundle index. The verifier no longer constructs a separate `std::set<Hash>` for every imported revision; locally resident incremental ancestors retain their in-memory set. Full/incremental transfers with two-parent histories and falsified incremental base hashes are regression-tested across all four backend combinations, with model-indexed and raw revisions.
- **Implemented (replication profiling foundation):** A standalone native replication benchmark and Linux runner build deterministic scattered-UUID raw/model histories, export full/incremental portable bundles, and import in isolated child processes. They measure importer wall time, exact process peak RSS (including a distinct post-audit peak), and sampled open scratch-file allocation (including unlinked files), with verified results for all four physical-backend transfers and a CTest smoke test. This is measurement infrastructure, not an importer memory guarantee or a replacement for representative long-history runs.
- **Outstanding:** Profile and bound remaining logical-verifier state (distinct snapshot-root pair tracking, locally resident object visits and Merkle summaries), and run the new measurements on substantially larger histories to identify actual bottlenecks. No constant-memory claim is made.
- **Outstanding:** Implement true streaming HTTP bundle uploads through an ingestion layer that exposes request chunks before Qt buffers them; QHttpServerRequest.body() alone cannot provide this. Retain strict limits, backpressure, disconnect cleanup and pre-publication validation.
- **Outstanding:** Benchmark representative 10k–1M hydraulic-entity projects, long revision histories, branching/merging, synchronization and compaction on both backends.
- **Implemented (large-benchmark workflow):** Native replication benchmarks can preserve checksummed deterministic fixtures separately from disposable import recipients (`--mode prepare` / `--mode imports`), with per-commit and export/verification progress reporting. Reusing fixtures avoids conflating expensive 100k-record source construction with importer latency and permits repeated comparable import measurements. A dedicated test rejects fixture mismatches and modified bundle contents.
- **Implemented (import-stage profiling):** Optional native import profiles and CSV columns distinguish bundle scanning/checksums, typed Merkle/revision traversal, model-reference consistency, reachability, branch validation, copying and durability/publication. They also count transferred and locally resident object reads. Direct API regression tests cover full/incremental/idempotent imports, malformed bundles and all four backend combinations. This measures the source of latency without relaxing full verification.
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
