# AOWIS Distributed Versioned Model Database

**Repository:** https://github.com/aowis-org/AOWIS-SERVER-DVMDB

**AOWIS's native C++ database engine for versioned engineering models.** It stores canonical AOWIS model data in immutable, content-addressed objects, supports branches and merges, and can exchange complete or incremental repository histories between independent installations. It is AOWIS's native database backend and is usable directly as a C++ library.

This repository contains the **native storage engine**, canonical hydraulic-model persistence, revision and merge APIs, portable synchronization, and a standalone access-policy library. The `aowis-server-dvmdb` service provides **REST over HTTP/TCP** and manages multiple projects with a durable on-disk catalogue. See [service design](docs/rest-api-design.md) and [implementation roadmap](docs/roadmap.md). The [UUIDv7 identity migration](docs/uuidv7-identity-migration-roadmap.md)
now includes the [frozen v2 record contract](docs/uuidv7-identity-format-v2.md)
and its first implementation slice: **new projects receive UUIDv7 IDs**;
legacy UUIDv4 projects remain readable. Revisions and branch HEADs still use
SHA-256 revision addresses until the later storage/replication migration.

> **Service status:** The multi-project HTTP server implements project/branch/history, revision diffs, canonical hydraulic reads/writes, atomic entity edits and validated merges. Bounded binary full/incremental HTTP bundle synchronization is available. Bundle downloads and uploads stream through project scratch files. Projects are served concurrently. Preview-only merges remain future work. HTTP currently has no authentication, authorization or TLS; restrict it to controlled development networks.

## GUI integration status

The server exposes complete paginated all-parent revision history to both
native and REST clients. Recent GUI builds integrate that P0 graph reader;
older GUI versions may still use first-parent history. The GUI can discover
projects and branches, create projects/branches, and aggregate several
checked databases. A separate standalone build can use DVMDB directly as a
C++ library. **The GUI does not yet check out hydraulic networks or save
network revisions**, even though some underlying DVMDB read/write APIs exist.

Project/branch rename/delete, network enumeration, editable annotations,
authentication and release-tag workflows remain unimplemented contracts.
The next identity migration must be implemented across both GUI readers,
not merely as a displayed UUID field. See the [GUI contract and remaining gaps](docs/gui-integration-requirements.md) and the GUI's
`docs/dvmdb-integration-roadmap.md`. The
[OpenAPI definition](docs/dvmdb-http-api.yaml) describes implemented server
routes, not proposed GUI capabilities.

## System overview

```text
AOWIS GUI / other AOWIS services / remote clients
                      |
                REST over HTTP
                      |
                 aowis-server-dvmdb
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
| Service | HTTP/TCP service (vendored cpp-httplib, worker thread pool) with durable multi-project catalogue, project create/list/read, branches, heads, first-parent history, complete paged revision-DAG discovery and immutable revision metadata |
| Administrative access policy | Provisional UUID-based users/groups/roles/grants library and local CLI, **not currently enforced by any service** |

These are engineering capabilities, **not a production-readiness claim**. See [limitations and roadmap](docs/roadmap.md).

## Fastest way to start the HTTP server

From the DVMDB checkout, run:

```bash
./compile_linux.sh
```

This builds and **starts** the HTTP service in the foreground. Leave that
terminal running; press Ctrl+C to stop it. It binds to `127.0.0.1:8755` by
default. In a second terminal:

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

Readiness returns `{"status":"ready"}`; an empty new catalogue returns
`{"projects":[]}`. The storage-root selection is deliberate: an existing
`./aowis-dvmdb-data` is reused, otherwise the per-user data location is
selected. No flag is required merely because an old data directory exists.
The selected path is printed at startup; it is **not migrated**. Explicit
`--data-root` or INI configuration wins. Do not point a standalone
embedded-library instance and the HTTP daemon at the same live root.

See the [HTTP service runbook](docs/native-database-service.md) for
configuration and [operations](docs/operations.md) for backups and recovery.
The service currently has **no TLS, authentication or authorization**:
keep the listener on a trusted local/development network.

## Build and tests

Linux prerequisites: C++20 compiler, CMake, Ninja, OpenSSL development libraries and a POSIX filesystem with `flock`, `fsync`, `rename`, `pread` and `pwrite`. Qt6 Core and the pinned `AOWIS-SERVER-MODEL` submodule are required for typed model serialization and hydraulic persistence; the HTTP executable uses the vendored cpp-httplib header (`external/cpp-httplib`) and Qt6 Core only; the generic byte store can build without Qt.

```bash
./git_submodule_init.sh          # on initial checkout, if the Model submodule is not initialized
./compile_linux_tests.sh --both # Debug and Release CTest suites
./compile_linux.sh              # build Release and start server, only if every build stage succeeds
./compile_linux.sh --build-only # build Release binaries and benchmarks, without starting server
# tools/benchmark_native_index.py rebuilds the exact Release target named by --binary.
# build-linux and build-linux-tests-release are distinct; do not mix their paths.
```

The build script checks configuration, full build, the HTTP-server target and the executable before launching it. A failed or interrupted build will never fall back to the server left by a previous build. Use `--build-only` for CI, backup procedures and workflows that must not start a database process.

An **unconfigured** Linux server reuses `./aowis-dvmdb-data/` if that legacy root already exists, so an existing checkout can start the HTTP service without extra flags. Otherwise, it uses `$XDG_DATA_HOME/aowis-dvmdb/` (normally `~/.local/share/aowis-dvmdb/`) outside the source checkout. If both roots exist, the legacy root takes precedence and the server prints which path it selected; no data is copied or merged. Explicit `--data-root` and `[storage] root=` override automatic selection. `catalogue.lock` is runtime data inside the selected root, not a configuration file; do not delete a live lock. The old checkout-local directory remains ignored by Git and excluded from source ZIPs.

`./git_submodule_init.sh` initializes the Model submodule and, where necessary, registers its Git submodule metadata. Commit any resulting gitlink or `.gitmodules` changes so future checkouts work.

`./tools/zip_dvmdb.sh` creates `../dvmdb.zip` with the checked-out source files of the project **and every initialized Git submodule recursively**, including modified and new non-ignored files. The archive does not include Git metadata, generated/ignored files, or a legacy checkout-local `aowis-dvmdb-data/` root (even if mistakenly tracked). Packaging fails if a submodule is not initialized; run `git submodule update --init --recursive` before creating a snapshot. The ZIP is a standalone source snapshot, not a Git checkout. The packaging behavior is covered by `aowis-dvmdb-zip-submodule-tests`.

### AOWIS DVMDB project and patch convention

This is **AOWIS-SERVER-DVMDB**: AOWIS is the organization and model ecosystem,
DVMDB is this database product. The single supported source ZIP and patch tools
are:

```bash
./tools/zip_dvmdb.sh       # ../dvmdb.zip, rooted at AOWIS-SERVER-DVMDB/
./tools/apply_patch.sh ~/Downloads/aowis-dvmdb-example.zip
```

Patch filenames must begin `aowis-dvmdb-` and ZIP entries must be rooted at
`AOWIS-SERVER-DVMDB/`. No legacy ZIP command, header namespace, or HTTP route
aliases are installed. The REST prefix is `/dvmdb/v1` (not `/api/v1`).
Existing clients must change their URLs and C++ API references accordingly.

**Storage is still AOWIS storage.** The immutable object bytes, model type
identifiers, pack/index markers, project manifests and replication bundle
framing remain in their original AOWIS formats. Renaming these would change
content-derived revision IDs and corrupt access to existing AOWIS histories;
they are not `DB_CUSTOM` branding. The DVMDB rename is an API and deployment
migration, not an on-disk rewrite.

**Existing deployments:** Stop writers, take a verified offline backup and
point the renamed service at the verified existing data root (or restore it
into a new root). The installed executable, configuration, systemd unit,
public header paths, CMake aliases and HTTP base URL have changed. See
[operations](docs/operations.md) and the [DVMDB migration notes](docs/dvmdb-rename-migration.md).

When `AOWIS_DVMDB_WITH_MODEL_CODEC=ON` (the default), CMake requires Qt6 Core and the Model submodule; use `-DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF` for an explicitly native-only build. With Qt available, the tests cover the native store, model codec, hydraulic persistence, offline synchronization and the optional access-policy library. The project catalogue and HTTP route integration suites are included when their dependencies are enabled.

Useful CMake targets include `AOWIS::DvmdbModelCodec` and `AOWIS::DvmdbHydraulic`. The public namespaces are `aowis::dvmdb` and `aowis::dvmdb::hydraulic`.

### Native C++ API

```cpp
#include <aowis/dvmdb/native_store.h>
#include <aowis/dvmdb/hydraulic_network_persistence.h>

// Use the native repository directly from C++.
aowis::dvmdb::Repository repository("/path/to/repository");
repository.createBranch("main");
// Use aowis::dvmdb::hydraulic::saveHydraulicNetwork(),
// loadHydraulicNetwork(), editHydraulicNetwork() and mergeHydraulicBranches()
// with the canonical AOWIS NetworkHydraulic model.
```

Engine failures, including hydraulic load failures, are `aowis::dvmdb::StorageError` exceptions (see `include/aowis/dvmdb/storage_error.h`); classify them by `StorageErrorKind`, never by message text. Load functions return `std::nullopt` only for "not found".

The project catalogue is also available through the `AOWIS::DvmdbProjectService` CMake target (`aowis::dvmdb::ProjectService` in C++) without a Qt dependency. Running `./build-linux/aowis-server-dvmdb` directly listens on `127.0.0.1:8755` by default; the active storage root is selected using CLI/INI, an existing working-directory legacy root, or the per-user default **in that order**. See [service documentation](docs/native-database-service.md) for INI/CLI options, systemd deployment and API operations. The [OpenAPI contract](docs/dvmdb-http-api.yaml) documents implemented routes.

For model semantics, query APIs and safe indexed edit rules, see [hydraulic persistence](docs/native-hydraulic-network.md). For branch/revision operations, see [revision history](docs/native-revision-history.md), [native merging](docs/native-branch-merging.md) and [hydraulic merging](docs/native-hydraulic-merging.md).

## Repository storage and maintenance

Each repository uses an append-only content pack and rebuildable index. Each model-indexed revision stores both its model-object Merkle root and a persistent reverse-reference Merkle root. Changes become visible when a durable branch reference is published; earlier revisions remain immutable. The pack is the sole supported physical repository format. An obsolete experimental block repository is rejected rather than silently opened as a pack; export such a pre-release repository with its compatible older executable and import its bundle into a fresh pack repository.

```bash
./build-linux/aowis-dvmdb-verify /path/to/repository             # full (default)
./build-linux/aowis-dvmdb-verify --physical /path/to/repository  # physical only
./build-linux/aowis-dvmdb-compact /path/to/repository
```

`aowis-dvmdb-verify` now performs complete **native storage** verification by default: physical objects, published branch/revision ancestry, canonical Merkle trees, and exact model-to-reverse-reference consistency. The optional `--physical` mode checks only physical stored objects. Bundle imports reuse the same logical validator, rejecting malformed history before publication. This does not yet check application-specific hydraulic semantics. See [repository verification](docs/repository-verification.md). Compaction retains objects reachable from all branch heads and their full parent histories and reclaims unreachable objects. **Compaction is offline** and requires exclusive access; close all repository handles first. See [storage architecture](docs/architecture.md).

For whole-service **offline** backups and verified restores, use
`tools/offline_backup.py` with a freshly built `aowis-dvmdb-verify`. The
utility refuses active server/repository handles and publishes only a complete
verified copy under a new directory name. See [operations](docs/operations.md)
for commands and limitations. The `aowis-dvmdb-offline-backup-tests` CTest
covers multiple projects, branches, active-user refusal and independent restore.
The `aowis-dvmdb-upgrade-restore-tests` drill additionally checks a complete
cold relocation containing frozen historical packs, divergent branches and
post-upgrade writes, including a second backup/restore cycle.

The binary repository format is not yet frozen for the first public release. An old pre-release pack/bundle fixture is retained only to test explicit rejection of unsupported branch formats; see [format compatibility](docs/format-compatibility.md). Back up important repositories separately; online backup, physical-controller power-loss validation and production hardening remain outstanding work. Automated subprocess recovery tests cover simulated write failures, process termination and offline backup/restore for pack repositories; they do not simulate storage-device power failure.

## Offline repository synchronization

Portable bundles exchange immutable branches between independent repositories, without transferring physical storage pages. Import verifies the graph before creating or fast-forwarding a branch and **never silently overwrites divergent work**.

```bash
project_uuid="<project-uuidv7>"
./build-linux/aowis-dvmdb-sync export \
  /path/to/source "$project_uuid" main /tmp/main.aowbundle
./build-linux/aowis-dvmdb-sync import \
  /path/to/destination "$project_uuid" /tmp/main.aowbundle remote_main
```

An incremental bundle supplies an authenticated UUIDv7 base revision:
`aowis-dvmdb-sync export-incremental REPOSITORY PROJECT_UUID BRANCH BASE_UUID BUNDLE`.
Full bundles use `AOWREV21`, incremental bundles use `AOWREV22`; neither accepts
SHA-256 revision IDs or SHA-era `AOWSYNC3/4` on this canonical CLI. SHA-256
still authenticates immutable records and content. See
[portable synchronization](docs/offline-repository-synchronization.md) and
[UUIDv7 transfer contract](docs/uuidv7-revision-replication-v2.md).

## Benchmarking

Every benchmark creates its own repository. Supply a **nonexistent child directory** inside a fresh random temporary parent; never reuse the same repository path for a second run.

```bash
run_root="$(mktemp -d /tmp/aowis-dvmdb-bench.XXXXXXXX)"
./build-linux/aowis-dvmdb-hydraulic-edit-benchmark \
  "$run_root/repository" 100000 20 3
```

The hydraulic edit benchmark reports import time, indexed edit/merge-related operation timings, membership chunk count and `peak_rss_kib`. It exercises real canonical model structures. A dedicated `aowis-dvmdb-merge-history-benchmark` measures ancestry/diff/conflict processing without a durable merge commit, using configurable shared/divergent histories. The `tools/benchmark_native_index.py` driver separately measures pack-index reopen, RAM, lookup, sidecar rebuild, writes, compaction, and multiple open projects at configurable physical object counts. Separate low-level synthetic storage and model benchmarks are also available. Treat results as workload-specific and measure verification, durability and memory as well as latency. See [benchmark methodology](docs/benchmark-plan.md).

## Recovery testing and operational limits

The native test suite includes `aowis-dvmdb-recovery-tests`. It links a **separate test-only build** of the engine with deterministic fault checkpoints; ordinary library/server binaries contain no environment-variable crash hooks. The tests inject process exits, simulated `ENOSPC` and partial writes across object append, synchronization, branch publication, incremental import and compaction. They verify recovered physical/logical integrity, immutable model references, derived-index rebuilding and subsequent writes. An error **after** an atomic branch-ref rename can mean the operation already took effect: reopen the repository and inspect its head before retrying. An interrupted compaction **after** generation-pointer publication requires reopening the handle. External backups must be made with repository handles closed, or from a separately coordinated filesystem snapshot, and fully verified after restore. See the [operator recovery and backup runbook](docs/operations.md) for coordinated snapshots, disk-full response, publication ambiguity and offline compaction.

A separate `aowis-dvmdb-physical-enospc-tests` CTest uses the **production**
native library on a private 32 MiB Linux tmpfs. It forces actual kernel ENOSPC
while committing, exporting and importing, then checks published heads,
repository reopening and retries. It skips explicitly where user/mount
namespaces are disabled. Neither injected faults nor real tmpfs exhaustion
establish durability under electrical outages or failing storage hardware;
device-level and deployment-filesystem qualification remain outstanding.

## HTTP/TCP project service

The chosen server design is:

- **`aowis-server-dvmdb`**, built on the vendored **cpp-httplib** with a worker thread pool, listening on a configurable TCP bind address and port using **HTTP (not HTTPS yet)**.
- **Multiple projects**, each identified by UUID and owning an independent native versioned repository, managed by one process. Clients never supply server filesystem paths as project identifiers.
- Implemented versioned REST resources for projects, branches and revision metadata, with JSON responses. Hydraulic networks/entities, merges/conflicts and bounded binary full/incremental synchronization are available.
- Revision-pinned hydraulic network/entity/membership reads (including historical APIs) and optimistic revision-head checks for mutations, preventing mixed snapshots and silent concurrent overwrites.
- **No authentication or authorization enforcement in the initial REST server.** An AOWIS-wide security concept will be designed and introduced later. Existing provisional policy code is not the REST security design.
- File-based synchronization and direct native C++ library access remain supported alongside the HTTP project service.

The project/branch/revision, hydraulic edit/merge and bounded binary synchronization APIs are implemented. Bundle transfer endpoints use `GET` and `POST /dvmdb/v1/projects/{projectId}/branches/{branch}/bundle` (optional `?base={revision}` for an incremental download; every export already carries UUIDv7 branch identity); use the offline CLI for transfers beyond configured HTTP limits. See [service documentation](docs/native-database-service.md), [OpenAPI](docs/dvmdb-http-api.yaml) and the [roadmap](docs/roadmap.md).

## Documentation

| Document | Subject |
|---|---|
| [DVMDB migration](docs/dvmdb-rename-migration.md) | Public API and deployment rename; storage-format boundary |
| [Architecture](docs/architecture.md) | Logical storage, physical formats, durability and limits |
| [REST API design](docs/rest-api-design.md) | Agreed service boundaries, proposed resources and wire contracts |
| [Roadmap](docs/roadmap.md) | Remaining implementation work |
| [Database service](docs/native-database-service.md) | HTTP quickstart, storage-root precedence, configuration, health and endpoints |
| [GUI integration contract](docs/gui-integration-requirements.md) | Implemented GUI-facing primitives and remaining backend gaps |
| [OpenAPI](docs/dvmdb-http-api.yaml) | Actual implemented HTTP route/schema contract |
| [Operations](docs/operations.md) | Coordinated backups, restore verification, disk-full response and maintenance |
| [Hydraulic model](docs/native-hydraulic-network.md) | Persistence, entity queries, indexed editing, validation |
| [Canonical serialization](docs/canonical-model-codec.md) | Model codec/schema contract |
| [Model records](docs/model-records.md) | Generic typed envelope and reference index |
| [Version-control semantics](docs/version-control-semantics.md) | Agreed branch/HEAD/revision model, all-branches graph and API gaps |
| [Revision history](docs/native-revision-history.md) | Historical reads and branching |
| [Branch merging](docs/native-branch-merging.md) | Native three-way merge |
| [Hydraulic merging](docs/native-hydraulic-merging.md) | Field and structural conflict resolution |
| [Portable sync](docs/offline-repository-synchronization.md) | Full and incremental bundles |
| [Benchmarks](docs/benchmark-plan.md) | Reproducible workloads and measurement methodology |
| [Engineering findings](docs/engineering-findings.md) | Measured storage and replication behavior, design tradeoffs and limitations |
| [Format compatibility](docs/format-compatibility.md) | Frozen pre-release pack/bundle fixtures and upgrade policy |
| [Durability qualification](docs/durability-qualification.md) | SIGKILL, ENOSPC, optional QEMU VM power-cut and HTTP soak |

`AOWIS-SERVER-MODEL` is the external model-definition submodule; its
README is maintained in that repository. The specialized documents above
cover the model codec, HTTP contracts, failure recovery and all operational
limits.

## Qualification commands

The short Debug/Release suites include genuine disk-full and process-restart
cases as well as an HTTP real-process stress test; the VM-worker **smoke** is
not a QEMU run. A separate real QEMU/KVM drill has passed four ext4 and four
XFS whole-VM cuts with acknowledged-history recovery; it remains an opt-in
durability drill, not a default CTest. The longer drills are opt-in:

```bash
# QEMU ext4/XFS guest power-cut test (Docker provisions QEMU and Debian guest):
./tools/prepare_vm_powercut_docker.sh  # optional; run wrapper also builds image
./qualify_vm_powercut_docker.sh --rounds 4

# Five-minute real-process HTTP mixed import/export soak:
./build-linux-tests-release/aowis-dvmdb-http-transfer-stress-tests \
  --soak-seconds 300 ./build-linux-tests-release/aowis-server-dvmdb

# Same soak with Linux RSS/CPU/FD evidence in a new run directory:
python3 tools/qualify_http_resources.py --seconds 300
```

Docker uses host networking by default to avoid Docker bridge DNS failures;
the actual durability drill still runs QEMU and kills the entire VM. The
procedure, expected evidence and hardware-level caveats are documented in
[durability qualification](docs/durability-qualification.md). Saved QEMU,
CTest and HTTP-soak evidence can be consistency-checked and catalogued with
`tools/qualification_evidence.py` (see the durability runbook). Historical
storage and replication comparisons are in [engineering findings](docs/engineering-findings.md).
