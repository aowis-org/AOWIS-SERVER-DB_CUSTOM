# Native database HTTP/TCP service

`aowis-server-db` hosts independent AOWIS versioned project repositories in one process. The REST transport uses Qt QHttpServer; the project catalogue, repository manager and native storage library are callable directly from C++ (via `AOWIS::NativeProjectService` and `AOWIS::NativeHydraulic`) with no network connection. The underlying native storage engine remains Qt-free.

## Build

With Qt6 Core, Network and HttpServer installed, run:

```bash
./compile_linux.sh
./compile_linux_tests.sh --both
```

`AOWIS_NATIVE_WITH_HTTP_SERVER=ON` builds the executable and HTTP integration tests (the default when the model codec is enabled). A missing Qt HttpServer module causes an explicit CMake error. A native-only build uses `-DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF -DAOWIS_NATIVE_WITH_HTTP_SERVER=OFF`.

## Run

```bash
./build-linux/aowis-server-db --config ./deploy/aowis-server-db.ini
# Or override the INI settings:
./build-linux/aowis-server-db --bind 127.0.0.1 --port 8755 --data-root ./db-data --backend pack
```

`QSettings` reads `server/bind`, `server/port`, `storage/root`, `storage/default_backend` and optional `logging/file` from an INI file. Defaults if absent: `127.0.0.1`, `8755`, `./aowis-db-data`, and `pack`; errors go to stderr when no log file is configured. `--log-file` (or `logging/file` in INI) appends C++ service diagnostics to a file, which must be writable by the service account. Log rotation is the administrator's responsibility. Explicit CLI arguments override INI values. Paths are relative to the server process's working directory unless absolute. The example INI and systemd service unit in `deploy/` use `/var/lib/aowis-db`; an administrator must create the `aowis-db` service account and install the files at the paths specified by the unit. Run `--help` for supported CLI options.

The service listens on plain HTTP, **without TLS, authentication or authorization**. Expose it only on deliberately trusted networks. A cross-AOWIS security design is planned separately. Health: `GET /api/v1/health`; readiness: `GET /api/v1/ready`.

## Project catalogue and lifecycle

The service root contains a lifetime-locked `catalogue.lock` and a `projects/` directory. Each project is a UUID-named directory with a small durable `project.meta` manifest and a `repository/` subdirectory. The manifest records the UUID, display name and physical backend; no client may specify a repository path. Projects are created under a temporary `.creating-<uuid>` directory with a fully initialized `main` branch; after file/directory synchronization an atomic rename publishes the project, followed by synchronization of the parent directory.

An interrupted creation may leave a `.creating-...` directory which startup ignores. Incomplete staging directories are not considered projects; administrators may remove them while the server is shut down after inspecting them. Once published, valid manifests are required: startup fails on damaged or unexpected catalogue entries rather than silently ignoring projects. The catalogue is locked exclusively across processes. The service retains at most one native `Repository` handle per project and closes them on shutdown, preserving the block backend's single-handle restriction.

The native project manager serializes catalogue operations for correctness; concurrency/throughput improvements are deferred to performance work. Do not run offline compaction while the service has a repository open. Repository directories are physical-backend-specific; a project cannot switch formats in place.

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

Example:

```bash
curl -sS http://127.0.0.1:8755/api/v1/health
curl -sS -H 'Content-Type: application/json' -d '{"name":"Water Distribution"}' \
  http://127.0.0.1:8755/api/v1/projects
```

Project creation accepts `{"name":"...", "storage":"pack"}` (backend optional, default configured). Creating a branch accepts `{"name":"scenario"}` (new independent raw history), `{"name":"scenario","fromBranch":"main"}` (fork current head) or `{"name":"scenario","fromBranch":"main","revision":"<64-character-hash>"}` (fork a particular reachable revision). A historical fork never changes the source branch. Branch names are `[A-Za-z0-9_-]`, maximum 96 characters; project IDs are lowercase UUIDs. JSON mutation bodies are currently limited to 16 KiB after QHttpServer has received them. Responses use JSON and include machine-readable error codes.

### Canonical hydraulic API

The native model codec must be enabled for hydraulic HTTP routes. Network and entity payloads are **canonical `ModelCodec<T>` bytes in standard Base64**, not a second hand-maintained JSON model serializer. A client uses `ModelCodec<NetworkHydraulic>::encode/decode` for complete network payloads and `ModelCodec<EntityType>::encode/decode` for individual entities. The JSON envelope also contains UUID, entity type (where applicable) and the exact immutable revision used for the read. Network/entity reads accept optional `?revision=<hash>`; without it they pin the branch head at the beginning of that call. Separate calls requiring a consistent historical view must supply the same revision.

Full saves: `POST .../branches/{branch}/networks` with `{"expectedHead":"<64 lowercase hex>","payloadBase64":"<canonical NetworkHydraulic bytes>"}`. This creates or replaces **one** complete network after domain validation. It does not replace unrelated networks. HTTP edits: `POST .../networks/{networkId}/edits` with an `expectedHead` and `changes` array. Each upsert is `{"uuid":"<uuid>","type":9,"payloadBase64":"<canonical entity bytes>"}`; deletion is `{"uuid":"<uuid>","delete":true}`. Every edit in the array is one atomic validated native commit, with a single expected-head comparison guarding the whole batch. Stale heads return HTTP 409, not silently retried. The API does not expose arbitrary unvalidated `commitModel()` operations.

Entity `type` is the numeric ordinal in `HydraulicEntityType` (0–17): pipe material, time pattern, tank volume curve, pump head curve, pump efficiency curve, valve headloss curve, valve characteristic curve, generic curve, reservoir, **junction (9)**, tank, demand point, pipe, pump, valve, simple control, rule control, map label. The corresponding canonical payload includes the entity UUID, which must agree with the JSON UUID.

Semantic merges use `POST .../projects/{projectId}/merges` with `target`, `source`, `expectedTargetHead`, `expectedSourceHead`, and optional `fieldChoices` / `objectChoices`. Each choice identifies an actual conflict and selects `target` or `source`; all choices must match pinned tips. Successful merges return the resulting revision and status; unresolved conflicts return HTTP 409 with UUIDs, hashes and reasons **without changing either branch**. This endpoint executes a merge and is not a dry-run preview API.

JSON payloads are limited to 8 MiB for network saves and entity edits (256 KiB for merges). Entity batches are limited to 128 changes; UUID lists to 10000 entries and diffs to 1000 changed UUIDs. Limits are enforced after Qt has already buffered HTTP requests, so they are **not** sufficient protection for an untrusted/public listener. Large network retrievals currently encode the full result in memory; paginated/streaming alternatives are part of the subsequent resource-hardening stage.

The service does not yet provide network enumeration, a dry-run merge preview, HTTP bundle synchronization, project deletion or runtime compaction. These follow in subsequent milestones. Bounds on HTTP buffering and concurrent requests require further hardening before untrusted or production traffic; current payload checks occur after Qt has buffered the request.

## Verification and integration tests

`aowis-native-project-service-tests` covers both physical storage formats, catalogue restart, project isolation, staged creation debris, malformed manifests, invalid path/ID inputs, multiple project/branch operations, concurrent creation and cross-process locking. When Qt HttpServer is installed, `aowis-native-http-tests` exercises the routes over real localhost TCP with Qt Network, including error codes, CRUD, pagination and restart. Both are part of `./compile_linux_tests.sh --both`.
