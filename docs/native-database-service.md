# AOWIS database service — planned HTTP architecture

**Status: not implemented.** The repository provides a native C++ engine and model/merge/synchronization APIs, but currently builds **no database HTTP service executable**. REST/TCP endpoints described here are a design contract, not callable endpoints. The canonical route proposal is [REST API design](rest-api-design.md).

## Server role

- A single executable named `aowis-server-db` will serve multiple independent project repositories through **Qt QHttpServer** over ordinary HTTP/TCP.
- A configurable bind address and port will control where the server listens. TLS/HTTPS and authentication/authorization will not be implemented in the initial service; those belong to the future AOWIS-wide security architecture.
- The server will own a durable catalogue of project UUIDs, names and server-controlled repository directories. Clients select projects by UUID, never by raw filesystem path.
- Native C++ APIs will remain independently callable from AOWIS standalone builds. REST handlers are transport adapters, not replacements for the engine.

## Service behavior

The initial REST foundation should implement health, project create/list/read, branch create/list/fork and read-only revision browsing. Subsequent API work will expose hydraulic entity lookup, validated atomic edit batches, merge previews and explicit conflict resolution, and binary full/incremental bundle transfer.

Every edit or merge publication must carry an expected branch revision and be checked atomically. Imports must continue rejecting divergent branch overwrites, incomplete graphs and invalid object hashes. No handler may substitute superficial HTTP-level checks for canonical model validation.

## Repository lifecycle

The service should manage open repository handles centrally, including the block backend's single-open-handle constraint. Project metadata and repository creation must have defined failure/rollback behavior. Offline compaction requires the affected repository to be closed and exclusive. Graceful shutdown, bounded HTTP payloads, project isolation, restart persistence and failure diagnostics require end-to-end tests.

## Configuration and deployment

Define Qt QSettings INI configuration for bind address, port, project root/catalogue and logs, with Linux service defaults and explicit overrides. Provide systemd deployment files **only once an actual HTTP server is implemented**; no obsolete server unit is shipped. Publish a maintained OpenAPI contract and integration tests along with the executable.

**Security caveat:** The initially planned plain-HTTP server is deliberately unauthenticated and unencrypted. Anyone able to reach it will be able to use its API. It is for controlled development environments, not untrusted or sensitive production networks. Authentication and TLS will be designed across AOWIS later.
