# Native database HTTP/TCP service

`aowis-server-db` hosts independent AOWIS versioned project repositories in one process. The REST transport uses the vendored cpp-httplib (`external/cpp-httplib`, pinned v0.60.1) with a worker thread pool; the project catalogue, repository manager and native storage library are callable directly from C++ (via `AOWIS::NativeProjectService` and `AOWIS::NativeHydraulic`) with no network connection. The underlying native storage engine remains Qt-free.

## Build

With Qt6 Core installed (Qt Network/HttpServer are not needed), run:

```bash
./compile_linux.sh
./compile_linux_tests.sh --both
```

`AOWIS_NATIVE_WITH_HTTP_SERVER=ON` builds the executable and HTTP integration tests (the default when the model codec is enabled). A native-only build uses `-DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF -DAOWIS_NATIVE_WITH_HTTP_SERVER=OFF`.

## Run

```bash
./build-linux/aowis-server-db --config ./deploy/aowis-server-db.ini
# Or override the INI settings:
./build-linux/aowis-server-db --bind 127.0.0.1 --port 8755 --data-root ./db-data
```

`QSettings` reads `server/bind`, `server/port`, `storage/root` and optional `logging/file` from an INI file. Defaults if absent: `127.0.0.1`, `8755`, `./aowis-db-data`; errors go to stderr when no log file is configured. `--log-file` (or `logging/file` in INI) appends C++ service diagnostics to a file, which must be writable by the service account. Log rotation is the administrator's responsibility. Explicit CLI arguments override INI values. Paths are relative to the server process's working directory unless absolute. The example INI and systemd service unit in `deploy/` use `/var/lib/aowis-db`; an administrator must create the `aowis-db` service account and install the files at the paths specified by the unit. Run `--help` for supported CLI options.

The service listens on plain HTTP, **without TLS, authentication or authorization**. Expose it only on deliberately trusted networks. A cross-AOWIS security design is planned separately. Health: `GET /api/v1/health`; readiness: `GET /api/v1/ready`.

## Project catalogue and lifecycle

The service root contains a lifetime-locked `catalogue.lock` and a `projects/` directory. Each project is a UUID-named directory with a small durable `project.meta` manifest and a `repository/` subdirectory. The manifest records the UUID, display name and pack format; no client may specify a repository path. Projects are created under a temporary `.creating-<uuid>` directory with a fully initialized `main` branch; after file/directory synchronization an atomic rename publishes the project, followed by synchronization of the parent directory.

An interrupted creation may leave a `.creating-...` directory which startup ignores. Incomplete staging directories are not considered projects; administrators may remove them while the server is shut down after inspecting them. Once published, valid manifests are required: startup fails on damaged or unexpected catalogue entries rather than silently ignoring projects. The catalogue is locked exclusively across processes. The service retains at most one native `Repository` handle per project and closes them on shutdown, so all HTTP operations reuse a managed repository handle.

The native project manager serializes catalogue operations for correctness; concurrency/throughput improvements are deferred to performance work. Do not run offline compaction while the service has a repository open. All newly created projects use the same pack format. Pre-release block-format repositories must be exported using the older executable and imported into a new pack project.

If a disk error occurs immediately after catalogue publication, the service fails closed for subsequent operations; restart recovers the published directory if it exists and is complete. Project deletion and renaming are not exposed in this milestone.

## Current HTTP operations

See the machine-readable [OpenAPI contract](db-http-api.yaml) for request/response schemas and exact paths. The available endpoints are:

| Method | Path | Result |
|---|---|---|
| GET | `/api/v1/health`, `/api/v1/ready` | Process and catalogue checks |
| GET, POST | `/api/v1/projects` | List and create projects |
| GET | `/api/v1/projects/{projectId}` | Read project metadata |
| GET, POST | `/api/v1/projects/{projectId}/branches` | List and create/fork branches |
| GET | `/api/v1/projects/{projectId}/branches/{branch}/head` | Read current revision |
| GET | `/api/v1/projects/{projectId}/branches/{branch}/history` | First-parent revision history (limit 1–1000, optional exclusive `after` cursor) |
| GET | `/api/v1/projects/{projectId}/revisions/{revisionHash}` | Read immutable revision metadata |
| GET | `/api/v1/projects/{projectId}/diff?from={hash}&to={hash}` | Immutable object diff, up to 1000 changed UUIDs |
| GET | `/api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}` | Complete revision-pinned hydraulic network |
| POST | `/api/v1/projects/{projectId}/branches/{branch}/networks` | Validated, atomic full-network save |
| GET | `/api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/entities?type={ordinal}` | Ordered UUIDs of one collection (up to 10000) |
| GET | `/api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/entities/{entityId}` | Revision-pinned targeted entity |
| POST | `/api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/edits` | Atomic validated entity edits, up to 128 changes |
| POST | `/api/v1/projects/{projectId}/merges` | Semantic hydraulic three-way merge and explicit conflict choices |
| GET | `/api/v1/projects/{projectId}/branches/{branch}/bundle[?base={revisionHash}]` | Binary full or incremental snapshot export |
| POST | `/api/v1/projects/{projectId}/branches/{branch}/bundle` | Binary branch import; fast-forward or idempotent retry |

Example:

```bash
curl -sS http://127.0.0.1:8755/api/v1/health
curl -sS -H 'Content-Type: application/json' -d '{"name":"Water Distribution"}' \
  http://127.0.0.1:8755/api/v1/projects
```

Project creation accepts `{"name":"..."}`; the optional `"storage":"pack"` field is accepted, but no alternate backend is available. Creating a branch accepts `{"name":"scenario"}` (new independent raw history), `{"name":"scenario","fromBranch":"main"}` (fork current head) or `{"name":"scenario","fromBranch":"main","revision":"<64-character-hash>"}` (fork a particular reachable revision). A historical fork never changes the source branch. Branch names are `[A-Za-z0-9_-]`, maximum 96 characters; project IDs are lowercase UUIDs. JSON mutation bodies are limited per route (see request limits below) before they are read. Responses use JSON and include machine-readable error codes.

**Error model.** The engine raises `aowis::native::StorageError` (a `std::runtime_error`) carrying a `StorageErrorKind`; the service maps the kind, never message text: `InvalidInput` 400 `invalid_request`, `NotFound` 404 `not_found`, `Conflict` 409 `conflict`, `LimitExceeded` 413 `limit_exceeded`, `Unavailable` 503 `unavailable`, `ResourceExhausted` (ENOSPC/EDQUOT) 507 `insufficient_storage`, and `Corruption`/`Io`/`Internal` 500 `internal_error`. Client errors return the engine message; 5xx responses return a generic message and the details are logged with the kind (`AOWIS database request failed [corruption]: ...`). A structural defect in an uploaded bundle is `InvalidInput`; the same defect found in the local repository is `Corruption`. `ProjectService` methods keep their contract of reporting client mistakes as `ProjectError`.

### Canonical hydraulic API

The native model codec must be enabled for hydraulic HTTP routes. Network and entity payloads are **canonical `ModelCodec<T>` bytes in standard Base64**, not a second hand-maintained JSON model serializer. A client uses `ModelCodec<NetworkHydraulic>::encode/decode` for complete network payloads and `ModelCodec<EntityType>::encode/decode` for individual entities. The JSON envelope also contains UUID, entity type (where applicable) and the exact immutable revision used for the read. Network/entity reads accept optional `?revision=<hash>`; without it they pin the branch head at the beginning of that call. Separate calls requiring a consistent historical view must supply the same revision.

Full saves: `POST .../branches/{branch}/networks` with `{"expectedHead":"<64 lowercase hex>","payloadBase64":"<canonical NetworkHydraulic bytes>"}`. This creates or replaces **one** complete network after domain validation. It does not replace unrelated networks. HTTP edits: `POST .../networks/{networkId}/edits` with an `expectedHead` and `changes` array. Each upsert is `{"uuid":"<uuid>","type":9,"payloadBase64":"<canonical entity bytes>"}`; deletion is `{"uuid":"<uuid>","delete":true}`. Every edit in the array is one atomic validated native commit, with a single expected-head comparison guarding the whole batch. Stale heads return HTTP 409, not silently retried. The API does not expose arbitrary unvalidated `commitModel()` operations.

Entity `type` is the numeric ordinal in `HydraulicEntityType` (0–17): pipe material, time pattern, tank volume curve, pump head curve, pump efficiency curve, valve headloss curve, valve characteristic curve, generic curve, reservoir, **junction (9)**, tank, demand point, pipe, pump, valve, simple control, rule control, map label. The corresponding canonical payload includes the entity UUID, which must agree with the JSON UUID.

Semantic merges use `POST .../projects/{projectId}/merges` with `target`, `source`, `expectedTargetHead`, `expectedSourceHead`, and optional `fieldChoices` / `objectChoices`. Each choice identifies an actual conflict and selects `target` or `source`; all choices must match pinned tips. Successful merges return the resulting revision and status; unresolved conflicts return HTTP 409 with UUIDs, hashes and reasons **without changing either branch**. This endpoint executes a merge and is not a dry-run preview API.

JSON payloads are limited to 8 MiB for network saves and entity edits (256 KiB for merges). Entity batches are limited to 128 changes. UUID lists (10000 entries) and diffs (1000 changed UUIDs) are bounded **while** they are computed: a larger result returns 413 `limit_exceeded` after reading at most one extra membership chunk or changed trie path, never after building the full list. The diff walks only changed trie paths, also when an insertion splits a compressed prefix above a large unchanged subtree.

**Request limits (all platforms).** Enforced by the server before a body byte is read:

- **Bodies:** a bundle upload may be at most `[sync] max_transfer_mib`. JSON bodies must declare `Content-Length` (411 otherwise) and are limited per route: 16 KiB for projects/branches, 256 KiB for merges, 8 MiB for network saves and edits. Oversized requests return 413, and the connection is closed without reading the body.
- **Request line and headers:** at most 8 KiB per header line (400 above that), 8 KiB per URI (414) and 100 header fields (400).
- **Timeouts:** read 30 s, write 60 s, keep-alive 10 s.
- **Workers:** requests run on `[server] worker_threads` worker threads (default 8, range 1–256, CLI `--worker-threads`). At most 16 further connections per worker are queued; more are closed.
- **Error bodies:** all rejections, including protocol-level ones, carry the usual JSON error body.

TLS and access control are not built in; terminate TLS in a reverse proxy, see `deploy/nginx-aowis-server-db.conf`.

**Branch-scoped revisions.** `?revision=` on `/branches/{branch}/...` routes must name the branch head or one of its ancestors (including merged second parents); any other revision of the project returns 404 `not_found`. Use project-level `/revisions/{id}` and `/diff` for revisions independent of a branch. Large network retrievals currently encode the full result in memory; paginated/streaming alternatives are part of the subsequent resource-hardening stage.

The service does not yet provide network enumeration, a dry-run merge preview, project deletion or runtime compaction. These follow in subsequent milestones. Bundle bodies have configurable byte/object ceilings enforced before reading; downloads and uploads are streamed through project scratch files. Cheaper incremental verification remains future work.

## Verification and integration tests

`aowis-native-project-service-tests` covers the pack format, catalogue restart, project isolation, staged creation debris, malformed manifests, invalid path/ID inputs, multiple project/branch operations, concurrent creation and cross-process locking. When the HTTP server is enabled, `aowis-native-http-tests` exercises the routes over real localhost TCP with Qt Network, including error codes, CRUD, pagination and restart. Both are part of `./compile_linux_tests.sh --both`.

### Portable bundle replication over HTTP

`GET /api/v1/projects/{id}/branches/{branch}/bundle` exports the same validated `.aowbundle` format used by `aowis-native-sync`. Add `?base=<64-character-revision-hash>` to omit immutable objects already reachable from that ancestor. `POST` the binary bundle to the corresponding endpoint on a destination project, choosing a tracking branch if you wish to preserve independent work; import never overwrites divergent history. The response is `{"status":"created|fast_forward|up_to_date","head":"...","objectsImported":N}`. Errors are JSON with 400 for malformed transfer, 409 for divergent history or invalid incremental base, and 413 for transfer ceilings.

The `[sync]` INI section sets `max_transfer_mib` (default 32, range 1–256), `max_objects` (default 100000, range 1–1000000), and `max_object_mib` (default 8, range 1–64). The same values can be supplied as `--sync-max-mib`, `--sync-max-objects` and `--sync-max-object-mib`; an individual object may not exceed the transfer ceiling. The HTTP server also rejects larger uploads with 413 before reading them. The native importer/exporter independently enforces all ceilings even when called from C++.

Bundle **transfers are streamed**: a download is exported into a nameless scratch file in the project repository and sent in 64 KiB chunks; the file disappears when the response ends, also on client disconnect. An upload is written into such a scratch file while it arrives, without holding any catalogue or project lock, so slow clients do not block other requests; the import (verification and publication) starts only after the complete body has arrived. A full disk while spooling returns 507. Both directions retain the same transfer limits. Export preparation and import verification remain synchronous, and the service is not yet an end-to-end streaming transfer implementation. Graph verification is still O(reachable history) in CPU/metadata and transfers are performed synchronously; use the offline synchronization CLI for larger data until the streaming follow-up is implemented. No authentication or TLS has been added.
