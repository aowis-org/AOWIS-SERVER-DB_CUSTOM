# AOWIS Versioned Model Database

**AOWIS's native C++ database engine for versioned engineering models.** It stores canonical AOWIS model data in immutable, content-addressed objects, supports branches and merges, and can exchange complete or incremental repository histories between independent installations. It is AOWIS's native database backend and is usable directly as a C++ library.

This repository contains the **native storage engine**, canonical hydraulic-model persistence, revision and merge APIs, portable synchronization, and a standalone access-policy library. The `aowis-server-db` service provides **REST over HTTP/TCP** and manages multiple projects with a durable on-disk catalogue. See [service design](docs/rest-api-design.md) and [implementation roadmap](docs/roadmap.md).

> **Service status:** The multi-project HTTP server implements project/branch/history, revision diffs, canonical hydraulic reads/writes, atomic entity edits and validated merges. Bounded binary full/incremental HTTP bundle synchronization is available. Bundle downloads and uploads stream through project scratch files. Projects are served concurrently. Preview-only merges remain future work. HTTP currently has no authentication, authorization or TLS; restrict it to controlled development networks.

## System overview

```text
AOWIS GUI / other AOWIS services / remote clients
                      |
                REST over HTTP
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
| Physical storage | One append-only content-addressed object pack, with a reconstructible sidecar index |
| Model serialization | Native-owned `ModelCodec<T>` for AOWIS model definitions; configure-time schema coverage check for 68 persisted structures |
| Hydraulic data | Complete `NetworkHydraulic` persistence, all 18 top-level entity collections, ordered chunked membership, typed relationship validation, physical value rules ([model value rules](docs/model-value-rules.md)), indexed lookup/edit/delete |
| History | Immutable revisions with up to two parents, branches, paginated history, historical reads, Merkle diffs |
| Merging | Three-way native merges, hydraulic field-level merges, explicit conflict choices, coordinated membership reconciliation, semantic validation before publishing a divergent hydraulic merge |
| Replication | Portable full/incremental `.aowbundle` exchange, SHA-256, object graph, canonical Merkle trie and model-reference index verification, fast-forward-only import, divergence protection |
| Service | HTTP/TCP service (vendored cpp-httplib, worker thread pool) with durable multi-project catalogue, project create/list/read, branches, heads, history and revision metadata |
| Administrative access policy | Provisional UUID-based users/groups/roles/grants library and local CLI, **not currently enforced by any service** |

These are engineering capabilities, **not a production-readiness claim**. See [limitations and roadmap](docs/roadmap.md).

## Build and tests

Linux prerequisites: C++20 compiler, CMake, Ninja, OpenSSL development libraries and a POSIX filesystem with `flock`, `fsync`, `rename`, `pread` and `pwrite`. Qt6 Core and the pinned `AOWIS-SERVER-MODEL` submodule are required for typed model serialization and hydraulic persistence; the HTTP executable uses the vendored cpp-httplib header (`external/cpp-httplib`) and Qt6 Core only; the generic byte store can build without Qt.

```bash
./git_submodule_init.sh          # on initial checkout, if the Model submodule is not initialized
./compile_linux_tests.sh --both # Debug and Release CTest suites
./compile_linux.sh              # optimized Release binaries and benchmarks
```

`./git_submodule_init.sh` initializes the Model submodule and, where necessary, registers its Git submodule metadata. Commit any resulting gitlink or `.gitmodules` changes so future checkouts work.

When `AOWIS_NATIVE_WITH_MODEL_CODEC=ON` (the default), CMake requires Qt6 Core and the Model submodule; use `-DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF` for an explicitly native-only build. With Qt available, the tests cover the native store, model codec, hydraulic persistence, offline synchronization and the optional access-policy library. The project catalogue and HTTP route integration suites are included when their dependencies are enabled.

Useful CMake targets include `AOWIS::NativeModelCodec` and `AOWIS::NativeHydraulic`. The public namespaces are `aowis::native` and `aowis::native::hydraulic`.

### Native C++ API

```cpp
#include <aowis/native/native_store.h>
#include <aowis/native/hydraulic_network_persistence.h>

// Use the native repository directly from C++.
aowis::native::Repository repository("/path/to/repository");
repository.createBranch("main");
// Use aowis::native::hydraulic::saveHydraulicNetwork(),
// loadHydraulicNetwork(), editHydraulicNetwork() and mergeHydraulicBranches()
// with the canonical AOWIS NetworkHydraulic model.
```

Engine failures, including hydraulic load failures, are `aowis::native::StorageError` exceptions (see `include/aowis/native/storage_error.h`); classify them by `StorageErrorKind`, never by message text. Load functions return `std::nullopt` only for "not found".

The project catalogue is also available as `AOWIS::NativeProjectService` without a Qt dependency. To start the HTTP service with defaults use `./build-linux/aowis-server-db` (binds to 127.0.0.1:8755, project storage `./aowis-db-data`); see [service documentation](docs/native-database-service.md) for INI/CLI options, systemd deployment and API operations. The [OpenAPI contract](docs/db-http-api.yaml) documents implemented routes.

For model semantics, query APIs and safe indexed edit rules, see [hydraulic persistence](docs/native-hydraulic-network.md). For branch/revision operations, see [revision history](docs/native-revision-history.md), [native merging](docs/native-branch-merging.md) and [hydraulic merging](docs/native-hydraulic-merging.md).

## Repository storage and maintenance

Each repository uses an append-only content pack and rebuildable index. Each model-indexed revision stores both its model-object Merkle root and a persistent reverse-reference Merkle root. Changes become visible when a durable branch reference is published; earlier revisions remain immutable. The pack is the sole supported physical repository format. An obsolete experimental block repository is rejected rather than silently opened as a pack; before applying this patch, export any such pre-release repository with the older executable and import its bundle afterward.

```bash
./build-linux/aowis-native-verify /path/to/repository             # full (default)
./build-linux/aowis-native-verify --physical /path/to/repository  # physical only
./build-linux/aowis-native-compact /path/to/repository
```

`aowis-native-verify` now performs complete **native storage** verification by default: physical objects, published branch/revision ancestry, canonical Merkle trees, and exact model-to-reverse-reference consistency. The optional `--physical` mode checks only physical stored objects. Bundle imports reuse the same logical validator, rejecting malformed history before publication. This does not yet check application-specific hydraulic semantics. See [repository verification](docs/repository-verification.md). Compaction retains objects reachable from all branch heads and their full parent histories and reclaims unreachable objects. **Compaction is offline** and requires exclusive access; close all repository handles first. See [storage architecture](docs/architecture.md).

The binary repository format is not yet frozen for the first public release. Back up important repositories separately; online backup, power-loss validation and production hardening remain outstanding work. Automated subprocess recovery tests cover simulated write failures, process termination and offline backup/restore for pack repositories; they do not simulate storage-device power failure.

## Offline repository synchronization

Portable bundles exchange immutable branches between independent repositories, without transferring physical storage pages. Import verifies the graph before creating or fast-forwarding a branch and **never silently overwrites divergent work**.

```bash
./build-linux/aowis-native-sync export \
  /path/to/source main /tmp/main.aowbundle
./build-linux/aowis-native-sync import \
  /path/to/destination /tmp/main.aowbundle remote_main
```

A subsequent bundle can contain only objects missing relative to a shared base revision. The native importer uses private, nameless scratch files in the repository's own `tmp/` directory (never `TMPDIR`) for its hash-to-offset index, one-byte-per-imported-object logical visitation marks, and per-kind Merkle-tree summaries, and checks bundle SHA-256 with a 64 KiB scratch buffer. These files are unlinked immediately, preallocated before writable mapping, and their mappings are evictable. Imported revision membership and semantic-scan enumeration reuse the file-backed visitation marks, without a second in-memory revision set. Objects already stored locally are verified boundaries, so an incremental import does work proportional to the delta, not to the history; model-reference edges are sorted with bounded in-memory runs. The same bundle format works through the bounded HTTP synchronization endpoints; downloads and uploads are streamed through project scratch files, without a full-size buffer in memory. Details: [portable synchronization](docs/offline-repository-synchronization.md).

## Benchmarking

Every benchmark creates its own repository. Supply a **nonexistent child directory** inside a fresh random temporary parent; never reuse the same repository path for a second run.

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-bench.XXXXXXXX)"
./build-linux/aowis-native-hydraulic-edit-benchmark \
  "$run_root/repository" 100000 20 3
```

The hydraulic edit benchmark reports import time, indexed edit/merge-related operation timings, membership chunk count and `peak_rss_kib`. It exercises real canonical model structures. A dedicated `aowis-native-merge-history-benchmark` measures ancestry/diff/conflict processing without a durable merge commit, using configurable shared/divergent histories. Separate low-level synthetic storage and model benchmarks are also available. Treat results as workload-specific and measure verification, durability and memory as well as latency. See [benchmark methodology](docs/benchmark-plan.md).

## Recovery testing and operational limits

The native test suite includes `aowis-native-recovery-tests`. It links a **separate test-only build** of the engine with deterministic fault checkpoints; ordinary library/server binaries contain no environment-variable crash hooks. The tests inject process exits, simulated `ENOSPC` and partial writes across object append, synchronization, branch publication, incremental import and compaction. They verify recovered physical/logical integrity, immutable model references, derived-index rebuilding and subsequent writes. An error **after** an atomic branch-ref rename can mean the operation already took effect: reopen the repository and inspect its head before retrying. An interrupted compaction **after** generation-pointer publication requires reopening the handle. External backups must be made with repository handles closed, or from a separately coordinated filesystem snapshot, and fully verified after restore.

These tests exercise deterministic software failure paths; they cannot by themselves establish durability under a real electrical outage or failing storage device. Device-level crash testing, prolonged disk-full conditions and production recovery procedures still need verification.

## HTTP/TCP project service

The chosen server design is:

- **`aowis-server-db`**, built on the vendored **cpp-httplib** with a worker thread pool, listening on a configurable TCP bind address and port using **HTTP (not HTTPS yet)**.
- **Multiple projects**, each identified by UUID and owning an independent native versioned repository, managed by one process. Clients never supply server filesystem paths as project identifiers.
- Implemented versioned REST resources for projects, branches and revision metadata, with JSON responses. Hydraulic networks/entities, merges/conflicts and bounded binary full/incremental synchronization are available.
- Revision-pinned hydraulic network/entity/membership reads (including historical APIs) and optimistic revision-head checks for mutations, preventing mixed snapshots and silent concurrent overwrites.
- **No authentication or authorization enforcement in the initial REST server.** An AOWIS-wide security concept will be designed and introduced later. Existing provisional policy code is not the REST security design.
- File-based synchronization and direct native C++ library access remain supported alongside the HTTP project service.

The project/branch/revision, hydraulic edit/merge and bounded binary synchronization APIs are implemented. Bundle transfer endpoints use `GET` and `POST /api/v1/projects/{projectId}/branches/{branch}/bundle` (optional `?base={revision}` for incremental download); use the offline CLI for transfers beyond configured HTTP limits. See [service documentation](docs/native-database-service.md), [OpenAPI](docs/db-http-api.yaml) and the [roadmap](docs/roadmap.md).

## Documentation

| Document | Subject |
|---|---|
| [Architecture](docs/architecture.md) | Logical storage, physical formats, durability and limits |
| [REST API design](docs/rest-api-design.md) | Agreed service boundaries, proposed resources and wire contracts |
| [Roadmap](docs/roadmap.md) | Remaining implementation work |
| [Database service](docs/native-database-service.md) | Implemented multi-project HTTP service, configuration, catalogue and endpoints |
| [Hydraulic model](docs/native-hydraulic-network.md) | Persistence, entity queries, indexed editing, validation |
| [Canonical serialization](docs/canonical-model-codec.md) | Model codec/schema contract |
| [Model records](docs/model-records.md) | Generic typed envelope and reference index |
| [Revision history](docs/native-revision-history.md) | Historical reads and branching |
| [Branch merging](docs/native-branch-merging.md) | Native three-way merge |
| [Hydraulic merging](docs/native-hydraulic-merging.md) | Field and structural conflict resolution |
| [Portable sync](docs/offline-repository-synchronization.md) | Full and incremental bundles |
| [Benchmarks](docs/benchmark-plan.md) | Workloads, commands and measurement rules |

`AOWIS-SERVER-MODEL` is an external model-definition submodule; its README is maintained in that repository, not rewritten here.


The HTTP server exposes canonical hydraulic network reads/writes, revision-pinned entity queries, atomic edits, revision diffs and validated hydraulic merges when built with the AOWIS model codec. See [HTTP service](docs/native-database-service.md) and [OpenAPI](docs/db-http-api.yaml) for implemented routes and request limits.

The [benchmark guide](docs/benchmark-plan.md) covers canonical hydraulic operations, replication, recovery, and maintenance on the native pack backend.
