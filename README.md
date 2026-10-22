# AOWIS Versioned Model Database

**AOWIS's native C++ database engine for versioned engineering models.** It stores canonical AOWIS model data in immutable, content-addressed objects, supports branches and merges, and can exchange complete or incremental repository histories between independent installations. It is AOWIS's native database backend and is usable directly as a C++ library.

This repository contains the **native storage engine**, canonical hydraulic-model persistence, revision and merge APIs, portable synchronization, and a standalone access-policy library. The intended server interface is a **REST API over ordinary HTTP/TCP**, with a single `aowis-server-db` process managing multiple projects. **That REST server is planned, not implemented yet.** See [service design](docs/rest-api-design.md) and [implementation roadmap](docs/roadmap.md).

> **Service status:** No network/database service executable is currently provided. The multi-project Qt QHttpServer REST interface described below is the next planned component, not a working endpoint. Its initial HTTP/TCP interface will have no authentication, authorization enforcement, or HTTPS pending an AOWIS-wide security design. Do not expose it to untrusted networks or send sensitive data over it.

## System overview

```text
AOWIS GUI / other AOWIS services / remote clients
                      |
                REST over HTTP                    PLANNED
                      |
                 aowis-server-db
          configurable TCP address + port
          multiple projects / repositories
                      |
        +-------------+-------------+
        |                           |
  project repository A       project repository B
        |                           |
        +------ native C++ engine --+
               model objects / Merkle indexes
               revisions / branches / merging
               full and incremental replication
```

The storage engine and hydraulic-model APIs can be called **directly from C++**. Standalone AOWIS builds can use the library without an HTTP connection.

## Implemented functionality

| Area | Available today |
|---|---|
| Native storage | Immutable SHA-256 objects, copy-on-write Merkle UUID index, durable branch refs, recoverable index, integrity verification, offline compaction |
| Physical storage | Append-only object packs (`PhysicalStore::Stage7Pack`, default) and block-packed storage (`PhysicalStore::ExperimentalBlocks`, opt-in) |
| Model serialization | Native-owned `ModelCodec<T>` for AOWIS model definitions; configure-time schema coverage check for 68 persisted structures |
| Hydraulic data | Complete `NetworkHydraulic` persistence, all 18 top-level entity collections, ordered chunked membership, typed relationship validation, indexed lookup/edit/delete |
| History | Immutable revisions with up to two parents, branches, paginated history, historical reads, Merkle diffs |
| Merging | Three-way native merges, hydraulic field-level merges, explicit conflict choices, coordinated membership reconciliation, semantic validation before publishing a divergent hydraulic merge |
| Replication | Portable full/incremental `.aowbundle` exchange, SHA-256, object graph, canonical Merkle trie and model-reference index verification, fast-forward-only import, divergence protection |
| Service | Multi-project HTTP/TCP REST server **planned; not yet implemented** |
| Administrative access policy | Provisional UUID-based users/groups/roles/grants library and local CLI, **not currently enforced by any service** |

These are engineering capabilities, **not a production-readiness claim**. See [limitations and roadmap](docs/roadmap.md).

## Build and tests

Linux prerequisites: C++20 compiler, CMake, Ninja, OpenSSL development libraries and a POSIX filesystem with `flock`, `fsync`, `rename`, `pread` and `pwrite`. Qt6 Core and the pinned `AOWIS-SERVER-MODEL` submodule are required for typed model serialization and hydraulic persistence; the generic byte store can build without Qt.

```bash
./git_submodule_init.sh          # on initial checkout, if the Model submodule is not initialized
./compile_linux_tests.sh --both # Debug and Release CTest suites
./compile_linux.sh              # optimized Release binaries and benchmarks
```

`./git_submodule_init.sh` initializes the Model submodule and, where necessary, registers its Git submodule metadata. Commit any resulting gitlink or `.gitmodules` changes so future checkouts work.

When `AOWIS_NATIVE_WITH_MODEL_CODEC=ON` (the default), CMake requires Qt6 Core and the Model submodule; use `-DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF` for an explicitly native-only build. With Qt available, the tests cover the native store, model codec, hydraulic persistence, offline synchronization and the optional access-policy library. A REST server and its HTTP integration tests will be added separately.

Useful CMake targets include `AOWIS::NativeModelCodec` and `AOWIS::NativeHydraulic`. The public namespaces are `aowis::native` and `aowis::native::hydraulic`.

### Native C++ API

```cpp
#include <aowis/native/native_store.h>
#include <aowis/native/hydraulic_network_persistence.h>

// Use the native repository directly from C++.
aowis::native::Repository repository("/path/to/repository",
                                     aowis::native::PhysicalStore::Stage7Pack);
repository.createBranch("main");
// Use aowis::native::hydraulic::saveHydraulicNetwork(),
// loadHydraulicNetwork(), editHydraulicNetwork() and mergeHydraulicBranches()
// with the canonical AOWIS NetworkHydraulic model.
```

For model semantics, query APIs and safe indexed edit rules, see [hydraulic persistence](docs/native-hydraulic-network.md). For branch/revision operations, see [revision history](docs/native-revision-history.md), [native merging](docs/native-branch-merging.md) and [hydraulic merging](docs/native-hydraulic-merging.md).

## Repository storage and maintenance

The default repository uses an append-only content pack and rebuildable index. Each model-indexed revision stores both its model-object Merkle root and a persistent reverse-reference Merkle root. Changes become visible when a durable branch reference is published; earlier revisions remain immutable. Both supported physical backends share the same logical object and revision identities, but **their directories are not interchangeable**.

```bash
./build-linux/aowis-native-verify /path/to/repository             # full (default)
./build-linux/aowis-native-verify --physical /path/to/repository  # physical only
./build-linux/aowis-native-compact /path/to/repository
```

`aowis-native-verify` now performs complete **native storage** verification by default: physical objects, published branch/revision ancestry, canonical Merkle trees, and exact model-to-reverse-reference consistency. The optional `--physical` mode checks only physical stored objects. Bundle imports reuse the same logical validator, rejecting malformed history before publication. This does not yet check application-specific hydraulic semantics. See [repository verification](docs/repository-verification.md). Compaction retains objects reachable from all branch heads and their full parent histories and reclaims unreachable objects. **Compaction is offline** and requires exclusive access; close all repository handles first. See [storage architecture](docs/architecture.md) and [block storage](docs/block-pack-experiment.md).

The binary repository format is not yet frozen for the first public release. Back up important repositories separately; online backup, power-loss validation and production hardening remain outstanding work. Automated subprocess recovery tests cover simulated write failures, process termination and offline backup/restore for both backends; they do not simulate storage-device power failure.

## Offline repository synchronization

Portable bundles exchange immutable branches between independent repositories, including when the sender and receiver use different physical storage formats. Import verifies the graph before creating or fast-forwarding a branch and **never silently overwrites divergent work**.

```bash
./build-linux/aowis-native-sync export \
  /path/to/source blocks main /tmp/main.aowbundle
./build-linux/aowis-native-sync import \
  /path/to/destination pack /tmp/main.aowbundle remote_main
```

A subsequent bundle can contain only objects missing relative to a shared base revision. The **bundle format and validation algorithms are implemented today**; HTTP transfer endpoints will expose them when the REST service is built. Details: [portable synchronization](docs/offline-repository-synchronization.md).

## Benchmarking

Every benchmark creates its own repository. Supply a **nonexistent child directory** inside a fresh random temporary parent; never reuse the same repository path for a second run.

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-bench.XXXXXXXX)"
./build-linux/aowis-native-hydraulic-edit-benchmark \
  "$run_root/repository" 100000 20 3 block
```

The hydraulic edit benchmark reports import time, indexed edit/merge-related operation timings, membership chunk count and `peak_rss_kib`. It exercises real canonical model structures. Separate low-level synthetic storage and model benchmarks are also available. Treat results as workload-specific and measure verification, durability and memory as well as latency. See [benchmark methodology](docs/benchmark-plan.md).

## Recovery testing and operational limits

The native test suite includes `aowis-native-recovery-tests`. It links a **separate test-only build** of the engine with deterministic fault checkpoints; ordinary library/server binaries contain no environment-variable crash hooks. The tests inject process exits, simulated `ENOSPC` and partial writes across object append, synchronization, branch publication, incremental import and compaction. They verify recovered physical/logical integrity, immutable model references, derived-index rebuilding and subsequent writes. An error **after** an atomic branch-ref rename can mean the operation already took effect: reopen the repository and inspect its head before retrying. An interrupted compaction **after** generation-pointer publication requires reopening the handle. External backups must be made with repository handles closed, or from a separately coordinated filesystem snapshot, and fully verified after restore.

These tests exercise deterministic software failure paths; they cannot by themselves establish durability under a real electrical outage or failing storage device. Device-level crash testing, prolonged disk-full conditions and production recovery procedures still need verification.

## Intended REST server (not yet implemented)

The chosen server design is:

- **`aowis-server-db`**, built on **Qt QHttpServer**, listening on a configurable TCP bind address and port using **HTTP (not HTTPS yet)**.
- **Multiple projects**, each identified by UUID and owning an independent native versioned repository, managed by one process. Clients never supply server filesystem paths as project identifiers.
- Versioned REST resources for projects, branches, revisions, hydraulic networks/entities, merges/conflicts and full/incremental synchronization; JSON for normal operations and binary streams for large data.
- Revision-pinned hydraulic network/entity/membership reads (including historical APIs) and optimistic revision-head checks for mutations, preventing mixed snapshots and silent concurrent overwrites.
- **No authentication or authorization enforcement in the initial REST server.** An AOWIS-wide security concept will be designed and introduced later. Existing provisional policy code is not the REST security design.
- File-based synchronization and direct native C++ library access remain supported alongside the planned REST service.

The route examples and open decisions are in [REST API design](docs/rest-api-design.md), and the implementation sequencing is in [roadmap](docs/roadmap.md). **No REST server executable or routes are currently implemented.**

## Documentation

| Document | Subject |
|---|---|
| [Architecture](docs/architecture.md) | Logical storage, physical formats, durability and limits |
| [REST API design](docs/rest-api-design.md) | Agreed service boundaries, proposed resources and wire contracts |
| [Roadmap](docs/roadmap.md) | Remaining implementation work |
| [Database service design](docs/native-database-service.md) | Planned service architecture and operational requirements |
| [Hydraulic model](docs/native-hydraulic-network.md) | Persistence, entity queries, indexed editing, validation |
| [Canonical serialization](docs/canonical-model-codec.md) | Model codec/schema contract |
| [Model records](docs/model-records.md) | Generic typed envelope and reference index |
| [Revision history](docs/native-revision-history.md) | Historical reads and branching |
| [Branch merging](docs/native-branch-merging.md) | Native three-way merge |
| [Hydraulic merging](docs/native-hydraulic-merging.md) | Field and structural conflict resolution |
| [Portable sync](docs/offline-repository-synchronization.md) | Full and incremental bundles |
| [Benchmarks](docs/benchmark-plan.md) | Workloads, commands and measurement rules |

`AOWIS-SERVER-MODEL` is an external model-definition submodule; its README is maintained in that repository, not rewritten here.
