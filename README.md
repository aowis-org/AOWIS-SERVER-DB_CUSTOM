# AOWIS Native Versioned Model Database — experimental engine

A native C++ versioned model database whose authoritative data is stored in
persistent, content-addressed trees and binary object packs. This repository
owns its canonical V5 model codec and hydraulic reference validation. The only
external AOWIS project dependency is the pinned `AOWIS-SERVER-MODEL` submodule;
there is no SQLite, PostgreSQL, or other database-repository dependency.

The physical storage and model-persistence APIs are implemented, but the full
authentication, distributed synchronization, multi-user server, and conflict
resolution capabilities are not yet implemented.

## Initialize the Model submodule

A newly created repository may contain `.gitmodules` but not its required
`160000` Git index entry (the *gitlink*). Unlike a directory, a gitlink cannot
be transported in a normal ZIP patch. The helper takes care of registering it:

```bash
./git_submodule_init.sh
```

On its **first** run it registers the Model repository defined in `.gitmodules`
and stages the Model commit as a gitlink. **Commit the registration once**:

```bash
git add .gitmodules external/AOWIS-SERVER-MODEL
git commit -m "Register AOWIS Model submodule"
```

Subsequent runs use `git submodule update --init --recursive` and therefore
checkout the exact pinned commit. `git_submodule_update.sh` is for intentionally
advancing the gitlink. The configured URL uses the `github-aowis` SSH host
alias; that alias must be configured locally or the `.gitmodules` URL should
be changed to a GitHub URL your environment can access before registration.

The low-level object store builds without Qt or the Model library. Typed
model persistence requires the pinned Model submodule and Qt Core.

## Full hydraulic model persistence

The optional canonical V5 adapter now saves and loads a complete
`NetworkHydraulic` aggregate on both native backends, with all 18 top-level
entity collections stored individually, typed reference validation, ordered
network membership, atomic replacement and branch-specific reopening. This
is separate from the earlier junction/pipe-only microbenchmarks. See
[complete network persistence](docs/native-hydraulic-network.md) for API,
format and remaining limitations. It uses this repository's native-owned
canonical V5 `ModelCodec` and hydraulic model validator.

## Build and test (Linux)

Requires a C++20 compiler, CMake, Ninja, OpenSSL's development files, and a
POSIX filesystem with working `flock`, `fsync`, `rename` and `pread`/`pwrite`.

```bash
./compile_linux_tests.sh --both  # Debug and Release tests only
./compile_linux.sh               # Release-optimized library and benchmark
```

The test script deliberately builds only test targets in
`build-linux-tests-debug` and `build-linux-tests-release`; it does **not**
build the benchmark in `build-linux`. Always run `./compile_linux.sh` before
using the benchmark path below. The default `build-linux` configuration is
explicitly **Release** so performance measurements are not taken from an
unoptimized build.

Run the separate prototype benchmark on a **new, nonexistent directory**:

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-benchmark.XXXXXXXX)"
./build-linux/aowis-native-benchmark "$run_root/repository" 100000 20 10 unique
# For comparison with the original Stage 1 benchmark, use 'legacy' instead.
```

Arguments: repository directory, number of objects, number of subsequent
revisions, edits per revision, optional payload mode (`unique` by default,
`legacy` to reproduce Stage 1). It prints import time, mean revision time,
diff time, scan time, content-pack size, unique packed-object count and time
spent reopening through the sidecar (`reopen_ms`) and separately hash-checking
all pack records (`verify_ms`). The sidecar size is `index_bytes`. Every commit calls `fsync` before
publishing its branch ref; the benchmark is not memory-only.

The `unique` fixture deliberately assigns distinct deterministic 128-byte
payloads to distinct objects and revisions. Stage 1's `legacy` fixture
repeated the same value every 256 objects, causing unusually high value
blob deduplication. Always report `payload_mode` when sharing benchmark
results. Both fixtures use scattered deterministic 16-byte keys. They are storage
microbenchmarks and do not exercise complete hydraulic model persistence.

## Experimental block-backed revision repository

The default `Repository(directory)` continues using the proven Stage-7 pack.
`Repository(directory, PhysicalStore::ExperimentalBlocks)` enables a separate,
explicitly experimental packed-block backend that uses **the same** copy-on-write
Merkle trees, model validation, revision serialization and atomic branch refs.
The two formats are deliberately not interchangeable at the same directory.
One writable handle at a time is enforced. Block retention compaction is
available offline, but server concurrency and online maintenance are not
implemented. See `docs/block-pack-experiment.md` for publication/recovery
semantics and known limitations.

Compare the two physical implementations with fresh directories:

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-compare.XXXXXXXX)"
./build-linux/aowis-native-v5-model-benchmark "$run_root/stage7" 100000 20 10
./build-linux/aowis-native-v5-model-benchmark "$run_root/blocks" 100000 20 10 block
```

The `block` argument opts in to experimental block storage; omit it for the
Stage-7 baseline. Identical model/revision hashes and branch publication are
asserted in the native tests. These measurements compare physical backends
within the native database, not complete end-user workloads.

## Implemented now

- Typed SHA-256 objects in one append-only binary `objects.pack`.
- Persistent `objects.idx` with checksummed hash/offset/length entries: fast
  normal reopening without scanning and hashing every stored payload.
  Missing/torn/corrupt indexes are rebuilt from the authoritative pack.
- Raw 32-byte hashes in the in-memory hash index (no per-object hex strings).
- Explicit full-pack integrity verification via `aowis-native-verify`.
- Integrity verification on content reads, rebuilds, recovered pack tails and
  explicit full-pack verification (not on every routine reopen).
- A compressed, 16-way, copy-on-write UUID trie with immutable node hashes.
- Grouped batch updates that persist each affected subtree only in its final
  form, instead of appending intermediate roots on initial import.
- Canonical prefix encoding: the same key/value map produces the same Merkle
  root independently of insertion order and batch boundaries.
- Persistent single-parent revisions and independent branch heads.
- Atomic ref publication after flushing appended data; orphan content is safe.
- Repository-wide cross-process lock for read/write coordination.
- Key lookup, sorted enumeration, insert/update/delete and Merkle-aware diff.
- Recovery by truncating **incomplete trailing records only** (bad complete
  records are rejected, never silently ignored).
- Typed model-envelope commits with atomic UUID-reference existence validation.
- A second immutable, revision-scoped UUID reverse-reference tree for model
  dependency lookups and sublinear deletion checks. Primary model data and
  reverse-reference roots are published atomically in the same revision.
- Raw low-level benchmark writes cannot modify model-indexed branches.
- Tests covering reopen, edits, deletion, branch isolation, historical roots,
  insertion-order-independent roots, large mixed batches, diff, torn tails,
  corruption and two handles.

## Important limitations

- Linux/POSIX prototype, not cross-platform or production-hardened.
- Active append-only pack generation plus persistent sidecar. Startup still constructs
  an in-memory hash-to-offset map in O(number of packed objects); multi-pack
  indexing and paging remain unimplemented.
- Sidecar adds ~52 bytes per physical object, in addition to the pack's
  45-byte headers. Reachability compaction is available explicitly; no online
  garbage collection, memory cache or compression yet.
- Each `commit` writes a revision immediately; staged, uncommitted working
  transactions and undo/redo need additional design.
- No merge computation or merge-parent publication, typed hydraulic/network
  membership indexes, complete hydraulic validation, authentication, project isolation
  or HTTP API yet.
- No independent remote synchronization or replication yet.
- Only simple ASCII branch names (letters, digits, `_` and `-`).
- Disk formats are **experimental and intentionally not migration-stable**.
  Stage 5 changes revision serialization to publish the reference-index root.
  Older prototype repositories are intentionally not supported: benchmark in
  fresh paths and do not attempt to open Stage 4 repositories using Stage 5.

Native model-record benchmark (synthetic junction/pipe topology; **NOT** the
native-owned AOWIS V5 codec):

```bash
./compile_linux.sh
run_root="$(mktemp -d /tmp/aowis-db-custom-model.XXXXXXXX)"
./build-linux/aowis-native-model-benchmark "$run_root/repository" 100000 20 10
```

See [docs/architecture.md](docs/architecture.md),
[docs/model-records.md](docs/model-records.md), and
[docs/benchmark-plan.md](docs/benchmark-plan.md).


## Native-owned canonical V5 model codec

With Qt6 Core and the pinned Model submodule available, CMake builds
`aowis-native-v5-model-benchmark`, `aowis-native-v5-codec-tests`, and
`aowis-native-hydraulic-network-tests`. Serialization, field-merging helpers
and hydraulic validation are implemented and compiled from this repository.
The field-layout checker covers all 68 persisted Model structures.
See [docs/native-serialization.md](docs/native-serialization.md) and
[docs/canonical-model-codec.md](docs/canonical-model-codec.md).

## Stage 18 — Incremental hydraulic network entity commits

`editHydraulicNetwork()` can now atomically upsert or remove any of the 18
canonical top-level hydraulic entity types from an existing network. Changed
entity payloads are V5-encoded individually, and an unchanged member list does
not rewrite the network manifest. New entities append to their collection,
and deletions update membership and root dependencies atomically. The complete
native hydraulic validator is used before commit, with expected-head concurrency
protection and reverse-reference maintenance. See
[docs/native-hydraulic-network.md](docs/native-hydraulic-network.md).

The API intentionally leaves validation as a full-network read: *writing* is
incremental, but validation is not yet incremental. This stage adds no new
physical storage format or incompatibility. The `aowis-native-hydraulic-network-tests`
target checks the edit API when Qt6 and the native V5 codec are available.

## Stage 6 — Git-inspired persistent pack index

The repository now contains both `objects.pack` and an append-only
`objects.idx` index. Revision hashes and existing Stage 5 pack records have
**not** changed. The sidecar is regenerated automatically if missing or
corrupted; only complete corrupt pack objects require manual intervention.
Fast open no longer cryptographically checks *every* object, so full
verification is a deliberate action:

```bash
./compile_linux.sh
./build-linux/aowis-native-verify /path/to/existing/repository
```

To benchmark the native engine with a fresh temporary repository:

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-stage6.XXXXXXXX)"
./build-linux/aowis-native-v5-model-benchmark "$run_root/repository" 100000 20 10
```

Report `reopen_ms`, `verify_ms`, `index_bytes`, `pack_bytes`, and the complete
model-edit workload together. Startup gains do not make pack reads or model
validation inherently faster. This stage does **not** implement multi-pack indexing, compression, or
online garbage collection; Stage 7 adds explicit offline reachability compaction.

## Stage 16 — Offline block-pack compaction

The experimental block-backed repository now supports the same conservative
reachability rule as Stage 7: **every reachable branch head, its full parent
history, both Merkle roots, and all leaf payloads** are retained. Unpublished
objects left by failed branch-ref updates can be reclaimed.

Maintenance writes a complete, verified replacement in a new
`blocks/generation-<random-token>/` directory. Only after synchronizing it
does it atomically replace `blocks/candidate.active`. The previous pack is
retired afterward; a crashed compaction can leave an unused generation, but
will not publish an incomplete one. The physical block layout and revision
identities are unchanged. The default Stage-7 backend is unchanged.

`aowis-native-compact` and `aowis-native-verify` select the storage backend
from the existing `storage.format` marker. The block mode remains single-handle
only, and compaction requires exclusive repository lifetime access. No online
GC, fault-injection matrix for power loss, or multi-writer server support is
claimed. The tools refuse to create a new repository accidentally.

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-stage16.XXXXXXXX)"
./build-linux/aowis-native-v5-model-benchmark \
  "$run_root/blocks" 100000 20 10 block
./build-linux/aowis-native-compact "$run_root/blocks"
./build-linux/aowis-native-verify "$run_root/blocks"
```

A benchmark with no abandoned objects should show roughly unchanged counts
and size: maintaining historical revisions is intentional.

## Stage 7 — Offline, history-preserving pack compaction

`aowis-native-compact` rebuilds a new immutable pack from all objects reachable
from **every** branch head, **every** parent revision, both primary and
reverse-reference roots, all tree nodes and leaf payloads. It does not expire
revision history, prune branches, or change SHA-256 object identities. Only
unreachable objects (including interrupted/unpublished writes) are reclaimed.

The new pack and index are synced before an atomic `objects.current` marker
publishes the generation. The old generation remains intact until that
publication is durable, and is then retired. New openings follow the marker;
legacy repositories without one continue using `objects.pack`/`objects.idx`.
An interrupted compaction may leave an unused `objects.<generation>.pack`
and `.idx` pair. Those files are not automatically deleted during recovery;
clean them up only after confirming the active marker, with the repository
closed and an independent backup. Never remove an active generation.

Compaction is **offline maintenance**. It refuses to run while any other
`Repository` instance has the directory open, including in another process.
All handles hold a shared lifetime lock; compaction requires exclusive access.
A successful compacted repository remains editable. Running compaction on
a history containing only reachable records may reclaim zero bytes: **this is
expected** because audit-worthy revisions are deliberately retained.

```bash
./compile_linux_tests.sh --both
./compile_linux.sh
run_root="$(mktemp -d /tmp/aowis-db-custom-stage7.XXXXXXXX)"
./build-linux/aowis-native-v5-model-benchmark "$run_root/repository" 100000 20 10
./build-linux/aowis-native-compact "$run_root/repository"
./build-linux/aowis-native-verify "$run_root/repository"
```

This is a Linux/POSIX experimental implementation, not yet an online GC or
production backup/recovery service. For crash-sensitive deployments, retain
a separate verified backup; live backup snapshots and explicit fault-injection
coverage remain future work.

## Experimental block-packing comparison

Stage 7 remains the authoritative native storage format. For an offline,
read-only comparison with Git-inspired grouped objects, see
[Block-packed candidate](docs/block-pack-experiment.md). The
`aowis-native-pack-experiment` tool writes candidate files to a new directory
and does **not** convert or modify the repository.

## Stage 10 — Experimental writable block packs

The optional `BlockStorePrototype` supports persistent SHA-256 indexed reads
and crash-conscious atomic **object batches** in an isolated block-packed
candidate directory. It does not replace the authoritative Stage 7 repository
format or atomically coordinate a revision head with a block batch. See
[docs/block-pack-experiment.md](docs/block-pack-experiment.md) for guarantees
and limitations.

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-stage10.XXXXXXXX)"
./build-linux/aowis-native-model-benchmark "$run_root/repository" 10000 3 10
./build-linux/aowis-native-pack-experiment "$run_root/repository" "$run_root/candidate" 65536
./build-linux/aowis-native-block-store-benchmark "$run_root/candidate" 100
```

The last command performs indexed SHA-256 reads, commits a batch, reopens and
verifies all objects. It **modifies only the candidate directory**; keep
repository roots and candidate directories separate.
