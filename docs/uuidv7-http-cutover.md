# Canonical UUIDv7 HTTP revision API — Phase 2g-4a

This is the **pre-release breaking HTTP cutover**. Revision identity is an
RFC 9562 UUIDv7; SHA-256 hashes authenticate immutable record bytes and Merkle
content but are never used as revision IDs, parent IDs, or branch HEAD IDs.
Project and branch metadata names are not filenames.

- `POST /dvmdb/v1/projects` accepts **only** `{ "name": "..." }`. A new
  project has a unique UUIDv7 initial revision and a UUIDv7 `main` branch.
  The former `revisionFormat` selector is rejected rather than being used to
  choose between incompatible storage formats.
- `POST /dvmdb/v1/projects/{id}/branches`, `PATCH /.../branches/{branch}`,
  `GET /.../branches/{branch}/head` and `/history` use UUIDv7 revision IDs.
  Branch creation from a revision does not copy or synthesize that revision.
- `GET /.../revisions/{revision}` and `/revision-graph` expose record SHA-256
  digests separately from UUIDv7 identities. The graph includes every branch
  HEAD and both ordered parents, and its signed pagination cursor pins the
  original HEAD snapshot.
- `POST /.../branches/{branch}/model-commits`, `POST /.../merges`, and
  `GET /.../diff` require/use immutable revision UUIDv7 identifiers. The
  generic merge route is not a hydraulic semantic validator.
- `POST /.../hydraulic-merges` enforces the hydraulic domain validator.
  Canonical `/.../networks`, `/.../networks/{network}/entities`, and
  `/.../networks/{network}/edits` paths use the existing UUIDv7 hydraulic
  adapter; writes require exact expected branch HEAD IDs.
- `GET/POST /.../branches/{branch}/bundle` transfers **only** AOWREV21 (full)
  or AOWREV22 (incremental) bundles, with verified project, branch and revision
  UUID identities, upload limits and staging quotas. Network authentication and
  resumable transfers are not supplied by this change.

All former `-v2` public route suffixes and HTTP SHA-revision routes are removed.
The authoritative request/response schema is
[`dvmdb-http-api.yaml`](dvmdb-http-api.yaml).

## Scope and remaining work

This patch changes **HTTP handlers and OpenAPI**, not the native API ABI.
`ProjectService::createProject()` and older native/CLI SHA-revision callers
still exist and are **not** converted by this patch. Using those native
SHA-revision APIs on repositories created by the canonical HTTP API will
fail closed rather than silently recast a SHA digest as a UUID. The later
native/CLI and GUI cutovers must eliminate those entry points in coordination.

`tests/native_http_tests.cpp` now runs its UUIDv7 transport, staging-budget,
graph/model/merge and executable smoke checks. Its obsolete SHA-only HTTP
fixtures are not invoked because their HTTP endpoints have been removed;
separate lower-level SHA storage tests are unchanged. HTTP behavioral tests
must be built/run with Qt6 development headers. The native-only route contract
test verifies *path/spec consistency*, not runtime handler behavior.

Pre-release storage from older SHA-based project runs is not migrated.

## Native and CLI follow-up — Phase 2g-4b

The canonical native `ProjectService::createProject()` now creates a UUIDv7
initial revision. Unsuffixed branch/HEAD/history/graph/revision/model/merge
methods expose UUIDv7 `RevisionId` values, and the canonical synchronization
CLI accepts only UUIDv7 project-scoped `AOWREV21/22` commands. Explicit V2
service methods remain as implementation aliases for the HTTP handlers.
Old SHA-only service operations were renamed `*ShaFixture` for existing
native storage tests; they are not supported application interfaces. See
[replication](uuidv7-revision-replication-v2.md) for the canonical commands.

## Offline backup and restore — Phase 2g-4d

`tools/offline_backup.py` now requires canonical UUIDv7 project directory
identities and published `AOWIS-BRANCH-2` records. It pins and compares the
exact bytes of all branch records while acquiring the project/repository
maintenance locks and validating every repository with the native full
verifier on a private copy. Each branch has a canonical UUIDv7 filename,
display-name metadata, a revision UUIDv7 and an expected immutable record
SHA-256. Forged record checksums, malformed/hidden ref entries and old
SHA-head `AOWIS-BRANCH-1` records fail before snapshot publication.

The former historical backup regression now exercises current-format
UUIDv7 project forks, divergent model-indexed histories, immutable past
revisions, a large model record, a post-restore commit and a second restore.
The pre-release frozen SHA fixture is not migrated or accepted as a backup.
