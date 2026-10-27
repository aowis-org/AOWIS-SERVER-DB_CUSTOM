# Native database HTTP/TCP service

`aowis-server-dvmdb` hosts independent AOWIS versioned project repositories in one process. The REST transport uses the vendored cpp-httplib (`external/cpp-httplib`, pinned v0.60.1) with a worker thread pool; the project catalogue and native storage are callable directly from C++ (for example through `aowis::dvmdb::ProjectService` and `aowis::dvmdb::hydraulic`). CMake consumers link the `AOWIS::DvmdbProjectService` and `AOWIS::DvmdbHydraulic` targets; no network connection is required. The underlying native storage engine remains Qt-free.

## Build

With Qt6 Core installed (Qt Network/HttpServer are not needed), run:

```bash
./compile_linux.sh --build-only
./compile_linux_tests.sh --both
```

`AOWIS_DVMDB_WITH_HTTP_SERVER=ON` builds the executable and HTTP integration tests (the default when the model codec is enabled). A native-only build uses `-DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF -DAOWIS_DVMDB_WITH_HTTP_SERVER=OFF`.

The normal `./compile_linux.sh` command builds and **starts** the server only after all build steps succeed; `--build-only` does not start any process.

## Run and verify

For **development**, the normal path is simply:

```bash
./compile_linux.sh
```

This builds the Release executable and starts it in the foreground with the
default configuration. To stop it, press Ctrl+C. To build without starting,
run `./compile_linux.sh --build-only`.

From a **second terminal**, verify HTTP (a successful compilation alone
does not mean a server is listening):

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/health
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

These return `{"status":"ok"}`, `{"status":"ready"}` and an object
containing a `projects` array respectively. A new catalogue has
`{"projects":[]}`. `health` means the process responds; `ready` additionally
checks the project catalogue. GUI availability probes use the project-list
response and validate its JSON shape, not just the TCP socket.

To run the binary directly with an **explicit root**, or to use the
production-oriented example INI:

```bash
./build-linux/aowis-server-dvmdb --bind 127.0.0.1 --port 8755 --data-root ./db-data
./build-linux/aowis-server-dvmdb --config ./deploy/aowis-server-dvmdb.ini
```

**Important:** the example INI specifies `/var/lib/aowis-dvmdb`. This is
**not** the default development data directory. It requires suitable
permissions and is intended for an installed service; use it only when that
root is the one you intend to operate.

`QSettings` reads `server/bind`, `server/port`, `storage/root` and optional
`logging/file` from an INI file. Default bind/port: `127.0.0.1:8755`.
The selected storage root follows this precedence:

| Priority | Condition | Selected root |
|---|---|---|
| 1 | `--data-root` explicitly supplied | CLI directory |
| 2 | `[storage] root=` present in the selected/default INI | INI directory |
| 3 | `./aowis-dvmdb-data` already exists | Existing legacy directory, resolved from the **process working directory** |
| 4 | None of the above | Qt `QStandardPaths::GenericDataLocation` + `/aowis-dvmdb` (normally `$XDG_DATA_HOME/aowis-dvmdb`, otherwise `~/.local/share/aowis-dvmdb` on Linux) |

`./compile_linux.sh` changes to the repository root before launching the
binary, so its legacy-directory check is relative to that checkout. Running
the binary directly from a different working directory changes that check.
An existing legacy entry is selected even if it is malformed, rather than
silently creating an apparently empty database elsewhere. The process
prints the selected storage root. When both legacy and user-data roots exist,
the legacy one wins and the other is reported, **not merged**. Explicit
paths always win. No data is copied, deleted or migrated during startup.

To relocate storage, stop **all** writers (including embedded-library users),
take a verified whole-root offline backup, restore to the new location and
verify before changing the configured path. Do **not** delete an active
`catalogue.lock` to try to bypass a lock error; the file belongs inside the
storage root and coordinates access.

For an existing root you want to select without relying on working directory,
use `--data-root /absolute/path` (or an absolute INI value).

Errors go to stderr when no log file is configured. `--log-file` (or `logging/file` in INI) appends C++ service diagnostics to a file, which must be writable by the service account. Log rotation is the administrator's responsibility. Explicit CLI arguments override INI values. **Explicit** relative paths are relative to the server process's working directory unless absolute. The example INI and systemd unit in `deploy/` still use `/var/lib/aowis-dvmdb`. Create the `aowis-dvmdb` service account, install the binary as `/usr/local/bin/aowis-server-dvmdb` and the INI as `/etc/aowis/aowis-server-dvmdb.ini`. The unit is sandboxed: read-only system (`ProtectSystem=strict`), writable only `/var/lib/aowis-dvmdb` (state) and `/var/log/aowis-dvmdb` (an optional `[logging] file=`), private `/tmp`, no home directories, no capabilities and a `@system-service` system-call filter (`systemd-analyze security` exposure 1.7). Run `--help` for supported CLI options.

**Upgrading an existing service:** The old binary, INI and systemd filenames,
Unix account and example `/var/lib/` and `/var/log/` paths were renamed to
`aowis-server-dvmdb` and `aowis-dvmdb`. The service does **not** move an
existing catalogue automatically. Stop all old service processes and back up
their data before changing service units. Either restore to the new root with
`tools/offline_backup.py` and verify the restored projects, or explicitly
configure `[storage] root=` / `--data-root` to the existing location and
allow that location in the systemd sandbox. Do not delete an old data root
until the new service has been verified against expected project IDs, branch
heads and representative historical reads. No HTTP API or physical pack
format IDs were renamed.

The service listens on plain HTTP, **without TLS, authentication or authorization**. Expose it only on deliberately trusted networks. A cross-AOWIS security design is planned separately. Health: `GET /dvmdb/v1/health`; readiness: `GET /dvmdb/v1/ready`.

## Startup failures and operator checks

| Symptom | Check / action |
|---|---|
| `curl` reports connection refused | Did `./compile_linux.sh` stay running after printing its startup messages? Is another process already using port `8755`? Check the listening address and the terminal's error output. |
| `GET /health` succeeds but `GET /ready` fails | The process is running but the catalogue is unavailable. Inspect service diagnostics and storage permissions; do not create an unrelated empty root as a workaround. |
| Unexpected empty `projects` list | Compare the **printed storage root** with the root holding your existing `projects/` directory. Look for an explicit INI or CLI override, and remember that direct execution uses the caller's working directory. |
| Cannot open/lock catalogue | Stop all competing server and embedded-library users, then investigate permissions and stale processes. Do not remove a lock file belonging to a live process. |
| `400` or `404` from GUI / `curl` | Use the `/dvmdb/v1` prefix exactly once. Test `/ready` and `/projects` directly, then verify the GUI profile URL. |
| GUI marks service offline despite HTTP `200` | The response must be a valid DVMDB projects catalogue (`{"projects":[...]}`); a proxy landing page or wrong server is not a valid health response. |
| Connection times out from another machine | Default bind is loopback only. The daemon does not provide TLS or access control; do **not** simply bind it to the public Internet. |

Read [operations](operations.md) before backup, data relocation, live lock
recovery or restore. For the GUI's automated polling, profile visibility
and supported operations, see its `docs/dvmdb-quickstart.md`.

## Project catalogue and lifecycle

The service root contains a lifetime-locked `catalogue.lock` and a `projects/` directory. Each project is a UUID-named directory with a small durable `project.meta` manifest and a `repository/` subdirectory. The manifest records the UUID, display name and pack format; no client may specify a repository path. Projects are created under a temporary `.creating-<uuid>` directory with a fully initialized `main` branch; after file/directory synchronization an atomic rename publishes the project, followed by synchronization of the parent directory.

An interrupted creation may leave a `.creating-...` directory which startup ignores. Incomplete staging directories are not considered projects; administrators may remove them while the server is shut down after inspecting them. Once published, valid manifests are required: startup fails on damaged or unexpected catalogue entries rather than silently ignoring projects. The catalogue is locked exclusively across processes. The service retains at most one native `Repository` handle per project and closes them on shutdown, so all HTTP operations reuse a managed repository handle.

**Concurrency.** Projects never wait for each other. The catalogue lock guards only the project list; each project's repository opens on first use under that project's own lock. Scoped operations of one project (saves, edits, merges, bundle imports and exports, branch creation) run one at a time; single reads (head, history, revision, branch list) take no operation lock. At most `[server] max_pending_per_project` scoped operations of one project may wait or run at once (default half of `worker_threads`, range 1..`worker_threads`, CLI `--max-pending-per-project`); further requests for that project get 503 `busy` with `Retry-After: 1`, a bundle upload before its body is read. This keeps workers free for other projects, `/dvmdb/v1/health` and `/dvmdb/v1/ready`. Do not run offline compaction while the service has a repository open. All projects use the pack format.

If a disk error occurs immediately after catalogue publication, the service fails closed for subsequent operations; restart recovers the published directory if it exists and is complete. Project deletion and renaming are not exposed in this milestone.

## Current HTTP operations

See the machine-readable [OpenAPI contract](dvmdb-http-api.yaml) for request/response schemas and exact paths. The available endpoints are:

| Method | Path | Result |
|---|---|---|
| GET | `/dvmdb/v1/health`, `/dvmdb/v1/ready` | Process and catalogue checks |
| GET, POST | `/dvmdb/v1/projects` | List and create projects |
| GET | `/dvmdb/v1/projects/{projectId}` | Read project metadata |
| GET, POST | `/dvmdb/v1/projects/{projectId}/branches` | List and create/fork branches |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/head` | Read current revision |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/history` | **First-parent only** revision history (limit 1–1000, optional exclusive `after` cursor; **not** a complete multi-branch DAG) |
| GET | `/dvmdb/v1/projects/{projectId}/revisions/{revisionHash}` | Read immutable revision metadata |
| GET | `/dvmdb/v1/projects/{projectId}/diff?from={hash}&to={hash}` | Immutable object diff, up to 1000 changed UUIDs |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/networks/{networkId}` | Complete revision-pinned hydraulic network |
| POST | `/dvmdb/v1/projects/{projectId}/branches/{branch}/networks` | Validated, atomic full-network save |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/entities?type={ordinal}` | Ordered UUIDs of one collection (up to 10000) |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/entities/{entityId}` | Revision-pinned targeted entity |
| POST | `/dvmdb/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/edits` | Atomic validated entity edits, up to 128 changes |
| POST | `/dvmdb/v1/projects/{projectId}/merges` | Semantic hydraulic three-way merge and explicit conflict choices |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/bundle[?base={revisionHash}]` | UUIDv7 identity-bearing full or incremental export (AOWSYNC3/4; optional identity=1 is redundant) |
| GET | `/dvmdb/v1/projects/{projectId}/branches/{branch}/revision-bundle[?base={revisionUuidV7}]` | Explicit `AOWREV21` full / `AOWREV22` incremental transfer of UUIDv7 revision history; no legacy fallback |
| POST | `/dvmdb/v1/projects/{projectId}/branches/{branch}/revision-bundle` | Verified UUIDv7 revision import; project UUID and stable branch UUID enforced, full/incremental replay and fast-forward |
| POST | `/dvmdb/v1/projects/{projectId}/branches/{branch}/bundle` | Binary branch import; fast-forward or idempotent retry |

Example:

```bash
curl -sS http://127.0.0.1:8755/dvmdb/v1/health
curl -sS -H 'Content-Type: application/json' -d '{"name":"Water Distribution"}' \
  http://127.0.0.1:8755/dvmdb/v1/projects
```

**Graph/HEAD semantics:** each branch has its own current HEAD (the revision
returned by `/head`), and multiple branch refs can point to the same revision.
The server has no global checked-out HEAD. A HEAD need not be a leaf of the
union of all branch histories. The GUI intends to display that union; a
snapshot-consistent full-DAG native/HTTP graph endpoint is **planned**, not
available. See [version-control semantics](version-control-semantics.md) and
[GUI requirements](gui-integration-requirements.md).

Project creation accepts `{"name":"..."}`; any other field is rejected (400). Creating a branch accepts `{"name":"scenario"}` (new independent raw history), `{"name":"scenario","fromBranch":"main"}` (fork current head) or `{"name":"scenario","fromBranch":"main","revision":"<64-character-hash>"}` (fork a particular reachable revision). A historical fork never changes the source branch. Branch names are `[A-Za-z0-9_-]`, maximum 96 characters; project IDs are lowercase UUIDs. JSON mutation bodies are limited per route (see request limits below) before they are read. Responses use JSON and include machine-readable error codes.

**Error model.** The engine raises `aowis::dvmdb::StorageError` (a `std::runtime_error`) carrying a `StorageErrorKind`; the service maps the kind, never message text: `InvalidInput` 400 `invalid_request`, `NotFound` 404 `not_found`, `Conflict` 409 `conflict`, `LimitExceeded` 413 `limit_exceeded`, `Unavailable` 503 `unavailable` (a full project queue is 503 `busy` with `Retry-After`), `ResourceExhausted` (ENOSPC/EDQUOT) 507 `insufficient_storage`, and `Corruption`/`Io`/`Internal` 500 `internal_error`. Client errors return the engine message; 5xx responses return a generic message and the details are logged with the kind (`AOWIS database request failed [corruption]: ...`). A structural defect in an uploaded bundle is `InvalidInput`; the same defect found in the local repository is `Corruption`. `ProjectService` methods keep their contract of reporting client mistakes as `ProjectError`.

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
- **Global transfer staging:** `[sync] max_inflight_mib` (default at least 128 MiB or twice `max_transfer_mib`, whichever is larger; range `max_transfer_mib`..4096) caps the sum of reservations for in-progress HTTP bundle uploads/downloads across **all projects**. Each transfer reserves its full configured maximum before creating a spool; a full budget returns 503 `staging_busy` with `Retry-After: 1`, and an unread upload connection closes. A disconnect or completed response releases the reservation. This is an admission limit for **HTTP transfer spools only**, not a quota on internal graph-verification/export-sort scratch files, pack growth or physical free disk space. Internal scratch has its own process-wide logical-extent quota, described below. True disk exhaustion remains 507.
- **Workers:** requests run on `[server] worker_threads` worker threads (default 8, range 1–256, CLI `--worker-threads`). At most 16 further connections per worker are queued; more are closed. One project may occupy at most `max_pending_per_project` of them (see Concurrency).
- **Error bodies:** all rejections, including protocol-level ones, carry the usual JSON error body.

TLS and access control are not built in; terminate TLS in a reverse proxy, see `deploy/nginx-aowis-server-dvmdb.conf`.

**Branch-scoped revisions.** `?revision=` on `/branches/{branch}/...` routes must name the branch head or one of its ancestors (including merged second parents); any other revision of the project returns 404 `not_found`. Use project-level `/revisions/{id}` and `/diff` for revisions independent of a branch. Large network retrievals currently encode the full result in memory; paginated/streaming alternatives are part of the subsequent resource-hardening stage.

The service does not yet provide network enumeration, a dry-run merge preview, project deletion or runtime compaction. These follow in subsequent milestones. Bundle bodies have configurable byte/object ceilings enforced before reading; downloads and uploads are streamed through project scratch files. Incremental imports verify only the delta (see offline-repository-synchronization.md).

## Verification and integration tests

`aowis-dvmdb-project-service-tests` covers the pack format, catalogue restart, project isolation, staged creation debris, malformed manifests, invalid path/ID inputs, multiple project/branch operations, concurrent creation and cross-process locking; `aowis-dvmdb-project-concurrency-tests` covers project independence, per-project serialization, the queue limit and racing writers. When the HTTP server is enabled, `aowis-dvmdb-http-tests` exercises the routes over real localhost TCP with Qt Network, including error codes, CRUD, pagination and restart. Both are part of `./compile_linux_tests.sh --both`.

### Portable bundle replication over HTTP

`GET /dvmdb/v1/projects/{id}/branches/{branch}/bundle` exports the same
verified identity-bearing `.aowbundle` as the native API and CLI. All full
exports use `AOWSYNC3`; adding `?base=<64-character-SHA-256>` produces the
`AOWSYNC4` delta format. Both carry the source branch's UUIDv7 by default;
`identity=1` remains a redundant accepted query value, **not** an opt-in to
new semantics. Old `AOWSYNC1/2` imports are rejected. POST validates SHA
content, reachable ancestry and UUIDv7 identity; it rejects identity
collisions/rebindings, divergent heads, incomplete objects and resource limit
violations. HTTP has no authentication or authorization: incoming identity
claims must come from trusted sources. Response fields remain
`{"status":"created|fast_forward|up_to_date","head":"...","objectsImported":N}`.
The HEAD is still SHA-256 until the separately planned UUIDv7 revision-v2
format is implemented.

The `[storage]` INI section also sets `max_internal_scratch_mib` (default 1024 MiB, range 1–16384; command-line `--max-internal-scratch-mib`). It limits the combined logical extent of the native engine's export visited tables, export/reference sort runs, import location indices, verification marks and trie summaries across all projects served by this process. Saturation returns 503 `scratch_busy` with `Retry-After: 1`. Unlike physical ENOSPC (507), quota saturation is transient and releases its reservations when operations finish. This cap does not cover HTTP spools, pack growth, compaction, or other processes and is not a filesystem quota. See [operations](operations.md).

The `[sync]` INI section sets `max_inflight_mib` (global HTTP transfer staging reservation ceiling, also `--sync-max-inflight-mib`) in addition to `max_transfer_mib` (default 32, range 1–256), `max_objects` (default 100000, range 1–1000000), and `max_object_mib` (default 8, range 1–64). The same values can be supplied as `--sync-max-mib`, `--sync-max-objects` and `--sync-max-object-mib`; an individual object may not exceed the transfer ceiling. The HTTP server also rejects larger uploads with 413 before reading them. The native importer/exporter independently enforces all ceilings even when called from C++.

Bundle **transfers are streamed**: a download is exported into a nameless scratch file in the project repository and sent in 64 KiB chunks; the file disappears when the response ends, also on client disconnect. An upload is written into such a scratch file while it arrives, without holding any catalogue or project lock, so slow clients do not block other requests; the import (verification and publication) starts only after the complete body has arrived. A full disk while spooling returns 507. Both directions retain the same transfer limits. Export preparation and import verification remain synchronous, and the service is not yet an end-to-end streaming transfer implementation. Graph verification is still O(reachable history) in CPU/metadata and transfers are performed synchronously; use the offline synchronization CLI for larger data until the streaming follow-up is implemented. No authentication or TLS has been added.
