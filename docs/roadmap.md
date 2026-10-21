# AOWIS Versioned Model Database — development roadmap

This roadmap covers planned capabilities for the AOWIS native database engine and service.

## Current baseline

The repository already implements content-addressed packs, immutable Merkle trees, canonical model serialization, complete hydraulic model persistence, indexed edits, history, branch merging with semantic validation, and full/incremental offline bundles. It also contains an optional, standalone provisional access-policy library and CLI. There is **no database service executable yet**. See the [README](../README.md) for exact current status.

## Planned work

### 1. Establish REST service foundation

- Build the REST service on the native engine and its existing bundle, integrity, revision, merge and direct-library APIs.
- Implement `aowis-server-db` with **Qt QHttpServer** on configurable **HTTP/TCP** address and port; no HTTPS/TLS initially.
- Implement a persistent multi-project catalogue, project UUID/name metadata, project create/list/read operations and repository lifecycle management.
- Implement branch listing, creation/forking and health/readiness endpoints in the new REST resource hierarchy.
- Add HTTP tests covering malformed requests, project isolation, persistence after restart and both native physical storage formats.
- Do **not** enforce the current provisional user/role policy in the new service. Authentication and authorization are deferred for an AOWIS-wide design.

### 2. Full model and version-control REST interface

- Expose branch heads, revision details/history, historical model/entity reads and diffs.
- Expose complete hydraulic network reads, indexed entity queries and validated atomic model edits.
- Expose merge previews, field-level and structural choices, semantic diagnostics, and validated merge publication.
- Require expected head/revision on all mutations to prevent unintended lost updates.
- Specify JSON models, binary payload endpoints, pagination, limits, stable errors and OpenAPI contracts; keep handlers thin.

### 3. HTTP-based distributed synchronization

- Reuse the portable branch bundle exporter/importer and incremental base-revision delta.
- Provide binary HTTP export/import endpoints; protect against partial/corrupted transfers and unbounded resource use.
- Define tracking-branch conventions and explicit conflict/merge workflow for divergent histories. Preserve SHA-256 object/revision IDs across replicas and physical formats.
- Add cross-server HTTP integration tests including cold initial sync, fast-forward, divergent branches, two-parent history, retries and transfer corruption.

### 4. Operational reliability and scalable model services

- Crash/power-loss fault injection, durable backup/restore, recovery verification, disk-full handling, graceful server shutdown and clear offline compaction lifecycle.
- Concurrency model for multiple projects and simultaneous HTTP requests, including the experimental block backend's single-open-handle restriction.
- Larger-scale benchmarks (imports, indexed edits, branch merges, object transfer and memory/CPU), profiling and performance regressions.
- Finish remaining semantic merge cases as required by AOWIS workflows; improve project and revision administration and management APIs.

### 5. AOWIS-wide security integration (deliberately deferred)

- Design identity, authentication, sessions, service-to-service trust and user/role/group permissions **for the whole AOWIS platform**.
- Integrate that unified security boundary with database project/branch operations; evaluate replacement/reuse of existing provisional access-policy code on its merits.
- Introduce HTTPS/TLS via an appropriate production reverse-proxy architecture as part of that design; add audit and administrative controls.

## Principles

- One canonical native database engine for library and REST service use.
- One canonical AOWIS model codec; do not create a serializer for REST.
- C++ library use and REST server use expose the same model/storage semantics.
- Database revisions and object integrity remain independent of transport.
- Fail clearly on merge conflicts and unsupported semantics; never weaken validation as a shortcut.
- Distinguish implemented capabilities from planned work in the README, documentation and tests. Define a stable persistence format before the first public release.
