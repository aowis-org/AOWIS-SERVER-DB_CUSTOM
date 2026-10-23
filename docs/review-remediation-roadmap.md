# Review remediation roadmap (2026-10 code review)

This roadmap covers every finding of the October 2026 review of the native
database, ordered by risk and dependency. Each phase ships as **one patch
archive** (`aowis-db-custom-<phase>.zip`) that is applied with
`tools/apply_patch.sh` and must leave `./compile_linux_tests.sh --both` green.
Every behavioural fix comes with a regression test that fails before the fix.

Review baseline: the Qt-free build is warning-free under
`-Wall -Wextra -Wpedantic -Wconversion`, and all 18 Qt-free suites pass under
ASan/UBSan. The Qt-dependent code (model codec, hydraulic persistence, HTTP
routes) was reviewed by reading only. Patches touching it are first compiled
on the developer machine.

Style findings about `auto` are out of scope by decision.

## Overview

| Phase | Patch | Findings | Severity | Depends on | Status |
|---|---|---|---|---|---|
| 1 | `aowis-db-custom-pack-tail-recovery` | F1 torn pack tail, F7 double close, F11 orphan generations | High | – | done |
| 2 | `aowis-db-custom-typed-errors` | F2 error classification, F10b non-revision hash | Medium | – | done |
| 3 | `aowis-db-custom-request-bounds` | F5 unbounded bodies, F9 late caps, F10a branch scoping | Medium | 2 | done |
| 4 | `aowis-db-custom-scratch-storage` | F6 scratch files on tmpfs | Medium | – | done |
| 5 | `aowis-db-custom-httplib-server` | Replace QHttpServer (F5 on all Qt versions, streaming uploads) | Medium | 2, 3, 4 | done |
| 6 | `aowis-db-custom-incremental-sync` | F3 incremental import/export cost | Medium | 1 | done |
| 7 | `aowis-db-custom-concurrent-service` | F4 serialized server | Medium | 5 | open |
| 8 | `aowis-db-custom-model-value-validation` | F8 enums, NaN/Inf | Low | – | open |
| 9 | `aowis-db-custom-cleanup` | F12 tidy-ups | Low | – | open |

Phases 1, 2, 4, 8 and 9 are independent of each other. Phase 1 goes first
because it is the only finding that can make a repository unusable.

---

## Phase 1: Pack tail recovery and descriptor hygiene (High) — done

Implemented in `aowis-db-custom-pack-tail-recovery`:
- `scanPackTail` validates records by streaming SHA-256; `discardTornPackTail`
  applies the EOF rule and the published-head guard, preserves the bytes in
  `recovered-pack-tails/`, then truncates.
- `Repository::recoveredPackTailBytes()` is reported by `aowis-native-verify`
  and logged by the service.
- `exportBundleLocked` forgets its descriptor before `close()`.
- `compact()` removes stale generations first and reports
  `CompactionStats::staleFilesRemoved`.
- Tests are in `tests/native_recovery_tests.cpp`. Each guard was checked by
  mutation: removing it makes the suite fail, except the double close, which
  is not observable in a single-threaded test.

Original analysis:

**F1. A torn pack tail makes the repository unopenable.**
`Repository::scanPackTail` (`src/native_store.cpp`) only treats a *short*
trailing record as incomplete. After power loss a filesystem can persist a
larger file size without the data (zero-filled tail) or with a partially
written body. Both currently throw (`corrupt pack record header` /
`content pack checksum mismatch`), and `aowis-native-verify` cannot open the
repository either.

Fix:
- Every published ref points only at objects that were fsynced before the
  rename. So any invalid record whose header *or* body extends to the end of
  the pack is unreferenced by definition, and it is truncated like a short
  record.
- Rule: scan records sequentially. On the first record with an invalid magic,
  an out-of-bounds length or a checksum mismatch, look ahead. If no valid
  record header follows within the remaining bytes (i.e. the bad region runs
  to EOF), truncate at that record's start. Otherwise the corruption is
  mid-pack and still fails closed.
- Log the truncated byte count to stderr (service log) so the event is visible.
- `verifyPackLocked` and `compact` keep failing on mid-pack corruption.
- `aowis-native-verify` reports "recovered N bytes of torn pack tail" when the
  open performed a truncation.

Tests (`tests/native_recovery_tests.cpp`):
- zero-filled tail after a committed revision → reopens, head unchanged,
  `verifyRepository()` clean, a new commit succeeds;
- valid header + garbage body of full length → same;
- garbage tail longer than one record → same;
- a corrupted record *followed by* a valid record → still throws;
- the existing short-partial-write cases stay green.

**F7. Double `close()` in `exportBundleLocked`.**
If `syncDirectory()` throws after a successful `::close(fd)`, the catch block
closes `fd` again. In the multi-threaded server that can close an unrelated
descriptor. Fix: track `fd` as `-1` after a close (same pattern as
`publishRef`), and close only `fd >= 0` in the handler. Linux closes the
descriptor even when `close()` reports an error, so set `-1` before checking
the result.

**F11. Orphaned compaction generations are never removed.**
A crash before or after publishing `objects.current` leaves
`objects.<token>.pack/.idx` files (and `.objects.current.*.tmp`) behind
forever. Fix: `compact()` (which holds the exclusive maintenance lock) deletes
every `objects.<token>.{pack,idx}` and `.objects.current.*.tmp` that is not
the active generation before it starts. Opening never deletes anything,
because other handles may exist.

Test: fault-inject at `compact.pack.before.pointer` and
`compact.pack.after.pointer`, reopen and compact again, then assert that
exactly one generation remains.

Acceptance: all recovery tests green under ASan; the torn-tail probe from the
review opens all three cases.

---

## Phase 2: Typed storage errors (Medium) — done

Implemented in `aowis-db-custom-typed-errors`:
- New header `include/aowis/native/storage_error.h`: `StorageError` and
  `StorageErrorKind`. Besides the planned kinds it has `Unavailable` (503) and
  `Internal`.
- `native_store.cpp`: all 198 `require` sites carry an explicit kind (the
  classification is listed in the patch); `systemCall` maps errno; context
  wrappers keep the kind.
- `clientRevision()` reports unknown and non-revision hashes as `NotFound`.
- Bundle import reclassifies verifier `Corruption` as `InvalidInput`, but
  failures while reading local objects keep their kind.
- The hydraulic layer, model records and the codec adapter throw typed errors.
- The HTTP routes contain no message matching any more. `ProjectService`
  translates client kinds to `ProjectError`.
- Behaviour changes:
  - non-network UUID on `/networks/{id}` → 404 (was 500);
  - editing a missing network → 404 (was 400);
  - "merge selection is not a conflicting field" → 400 (was 500);
  - ENOSPC → 507;
  - corruption → generic 500.
- Tests: new `aowis-native-error-kind-tests`; every forged bundle in the import
  integrity suite must now be `InvalidInput`; HTTP tests for tree-hash 404,
  generic corruption 500, non-network 404 and missing-network edit 404. Five
  mutations (removing each safeguard) were each caught.
- Known limits, carried to later phases:
  - The hydraulic *load* APIs (`load…AtRevision`, `list…AtRevision`) still
    report failures as text, so the service treats them as 500 even when the
    cause was I/O or ENOSPC (Phase 9).
  - A structurally invalid **local** object reached by an incremental import
    is reported as `InvalidInput`; resolved in Phase 6: local objects are no
    longer re-verified, damaged ones fail their SHA-256 check (`Corruption`),
    and a bundle that grafts a wrong-kind local object is `InvalidInput`.
  - The HTTP spool/export temp-file failures are `Io` because Qt hides errno
    (Phase 4; done there).

Original analysis:

**F2. HTTP status codes come from substring matching of exception text.**
`db_http_routes.cpp` (`hydraulicWrite`, bundle import/export) and
`project_service.cpp` classify `std::runtime_error` by message fragments.
Consequences:
- storage corruption (`invalid native revision object`, `invalid tree node
  tag`, …) is reported as **400 client error**;
- `ENOSPC` while creating import scratch files ("…replication index…") is
  reported as **400 invalid_bundle**;
- internal text (`strerror`, paths) is returned to clients.

Fix:
- Add `include/aowis/native/storage_error.h` with
  `class StorageError : public std::runtime_error` carrying
  `enum class StorageErrorKind { InvalidInput, NotFound, Conflict,
  LimitExceeded, Corruption, ResourceExhausted, Io }`.
- `native_store.cpp`:
  - `require(...)` call sites get an explicit kind;
  - `systemCall` maps `ENOSPC`/`EDQUOT` to `ResourceExhausted` and
    everything else to `Io`;
  - object, trie and revision decode failures on **local** data are
    `Corruption`; the same failures on **bundle** data are `InvalidInput`.
    The verifier gets a flag telling it which source it is validating.
- The hydraulic layer throws `StorageError(InvalidInput)` for semantic
  validation failures instead of relying on message prefixes.
- `ProjectError` maps from `StorageError` in one place. HTTP mapping:

  | Kind | Status | Client message |
  |---|---|---|
  | InvalidInput | 400 | detailed |
  | NotFound | 404 | detailed |
  | Conflict | 409 | detailed |
  | LimitExceeded | 413 | detailed |
  | ResourceExhausted | 507 | generic |
  | Corruption, Io | 500 | generic, details only in the log |

- Delete every `find(...)` / `starts_with(...)` message classifier.

**F10b. Non-revision hashes give 500.** `GET /revisions/{hash}` with the hash
of a tree or value object currently fails inside `readRevision`. With typed
errors, "object is not a revision" becomes `NotFound` (404), so callers cannot
probe object types.

Tests:
- unit: each error kind is raised at representative sites;
- HTTP (`tests/native_http_tests.cpp`): corrupted local object → 500 without
  internal text; tree hash on `/revisions` → 404; a malformed bundle → 400;
  an ENOSPC import simulated through the fault-test build → 507.

---

## Phase 3: Request and response bounds (Medium, needs Phase 2) — done

Implemented in `aowis-db-custom-request-bounds`. (The F5 startup policy was
superseded by Phase 5: the new server limits requests itself on every
platform, so the policy, its INI key and its test were removed again.)
- **F5:**
  - Qt >= 6.11 (Arch) limits bodies to `max_transfer_mib` and headers to 64 KiB.
  - Qt < 6.11 (Debian 13 ships 6.8) gets a startup policy from
    `include/aowis/native/http_limit_policy.h`: loopback starts with a
    warning; non-loopback is refused (exit 2) unless
    `server/proxy_enforces_body_limit=true` or `--proxy-enforces-body-limit`.
  - `deploy/nginx-aowis-server-db.conf` is the matching proxy example.
  - No changes to or around QHttpServer itself.
- **F9:**
  - `diffHashes` is now a structural trie diff whose work is proportional to
    the changed paths, also across prefix splits. It no longer lists unaligned
    subtrees; this also speeds up merges.
  - `diffRevisions(from, to, max)` raises `LimitExceeded` before
    materializing more results.
  - `listHydraulicEntityIdsAtRevisionLimited` stops after one membership
    chunk past the cap.
  - HTTP returns 413 `limit_exceeded` for > 1000 diff records or > 10000 UUIDs.
- **F10a:** went with option 1, but with **404** instead of 409 (the revision
  does not exist *on that branch*). `Repository::revisionOnBranch` covers
  ancestors and merged second parents.
- **Tests:**
  - new `aowis-native-diff-scope-tests`: randomized diff against an
    independent reference over 120 commits, a damaged-leaf probe proving the
    diff never reads unchanged subtrees, limit boundaries 1000/1001, and branch
    scoping incl. merges;
  - new `aowis-http-limit-policy-tests`;
  - HTTP tests run the real binary for every policy case (CLI and INI), plus
    diff 1000/1001, entity list 10000/10001 (current and historical), and
    branch scoping on all three read routes.
  - Nine mutations, including the previous listing-based diff, were each
    caught.

Original analysis:

**F5. Request bodies are unbounded on Qt < 6.11.**
`setMaximumBodySize` is compiled in only for Qt ≥ 6.11 (Ubuntu 24.04 ships
6.4). Every POST route checks its size after Qt has buffered the whole body.

Fix (layered):
- CMake: detect the Qt version. Below 6.11 the service logs a startup
  **warning** that body limits must be enforced by a reverse proxy, and
  `deploy/` gains an nginx example (`client_max_body_size` equal to
  `max_transfer_mib`, `proxy_request_buffering on`).
- Add `[server] require_body_limit=true|false` (default true). With true,
  a Qt < 6.11 build refuses to start unless the bind address is loopback, so
  that only a local reverse proxy can reach it.
- Streaming upload ingestion stays on the main roadmap; this phase only
  closes the unbounded-memory gap.

**F9. Size caps are applied after full materialization.**
`/diff` builds the complete difference list before rejecting more than 1000
entries; entity listing does the same for 10000 UUIDs.

Fix: add `diffRevisions(from, to, std::size_t limit)` and a bounded
entity-ID enumeration that stop with `LimitExceeded` as soon as
`limit + 1` results exist. The existing unbounded C++ APIs stay for library
callers.

**F10a. `?revision=` ignores the branch in the URL.**
Branch-scoped read routes accept any revision of the project. Decide and
document one of:
1. require the revision to be reachable from the named branch (409
   otherwise), using a cached reachability check; or
2. keep the behaviour, but document that it is project-scoped, and add
   project-level `/revisions/{id}/networks/...` aliases.

Recommendation: option 1, because it keeps future branch-level
authorization meaningful.

Tests: oversized diff/entity requests return 413 without a large
allocation (checked through RSS in the stress suite); a foreign-branch
revision behaves as documented.

---

## Phase 4: Scratch storage location (Medium) — done

Implemented in `aowis-db-custom-scratch-storage`:
- Every scratch table (bundle location index, visit marks, trie summaries,
  external reference sort) lives in `<repository>/tmp/`:
  - mode 0700;
  - created with `O_TMPFILE` (no name), or with a `mkostemp` plus immediate
    `unlink` fallback;
  - leftovers of that fallback are removed on open, under `repo.lock`;
  - a symlinked or non-directory `tmp` is refused as `Corruption`.
- No code uses `TMPDIR` any more. A `[storage] scratch=` option was not added:
  the pack's own filesystem is exactly the right place.
- New C++ API: `ScratchFile`, `Repository::createScratchFile()`,
  `scratchDirectory()`, and descriptor overloads of `importBranchBundle`,
  `exportBranchBundle` and `exportIncrementalBranchBundle` (a failed export
  leaves the descriptor empty).
- HTTP: bundle upload spools and exports use project scratch files instead of
  `QTemporaryDir`. A full disk while spooling is now 507, which removes the
  `Io` limitation noted in Phase 2.
- No statfs/tmpfs warning: scratch is on the pack's filesystem, so it is
  pointless.
- Tests:
  - new `aowis-native-scratch-tests`: location, permissions, operation
    without `TMPDIR` (O_TMPFILE and forced named fallback), leftover cleanup,
    unsafe paths, `ScratchFile` semantics, descriptor import/export incl.
    failure cleanup and pipes;
  - the import integrity test now runs with a nonexistent `TMPDIR` and blocks
    the repository's own `tmp` to prove fail-closed behaviour;
  - the HTTP bundle API test runs without a usable `TMPDIR` and checks that
    nothing is left in the project scratch directory.
  - Seven mutations were each caught.

Original analysis:

**F6. "Disk-backed" scratch files live in `TMPDIR`.**
The bundle location index, visit marks, trie summaries and reference-edge
runs, plus the HTTP upload spool (`QTemporaryDir`), go to
`std::filesystem::temp_directory_path()`. On tmpfs (`/tmp` default on Fedora,
Arch and Debian 13) they consume RAM or swap, which defeats the design.

Fix:
- Scratch directory = `<repository>/tmp/` by default, so it is on the same
  filesystem as the pack. Create it with mode 0700 at open time.
- Use `open(O_TMPFILE)` where supported; otherwise fall back to the current
  `mkstemp` + immediate `unlink`.
- At open time, remove leftover named files in `<repository>/tmp/`
  (only possible from the fallback path after a crash).
- Optional `BundleLimits`-adjacent `ScratchOptions{path}` for library
  callers; INI `[storage] scratch=` for the service.
- HTTP import spool: write into the target project's scratch directory, not
  `QTemporaryDir()` defaults.
- At startup, check the scratch filesystem with `statfs` and log a warning
  if it is `TMPFS_MAGIC`.

Tests: an import with `TMPDIR` set to a nonexistent path still succeeds;
scratch files are created under the repository; no files remain after
success or after an injected failure.

---

## Phase 5: Replace QHttpServer with cpp-httplib (Medium, needs Phases 2–4) — done

Implemented in `aowis-db-custom-httplib-server`:
- **Dependency:** `external/cpp-httplib/` vendors v0.60.1 (commit `00afaae`,
  MIT). CMake no longer needs Qt Network/HttpServer, only Qt Core and Threads.
- **Routes:** same paths, JSON shapes and error codes. `registerRoutes()` now
  takes an `httplib::Server&`.
- **Limits (before a body byte is read):**
  - global payload limit;
  - bundle uploads checked by declared length;
  - per-route JSON limits from `Content-Length` in the pre-request handler;
  - JSON without `Content-Length` returns 411;
  - URI over 8 KiB returns 414; header line over 8 KiB or more than 100
    headers returns 400;
  - all rejections close the connection. httplib 0.60.1 also does this itself
    for every error, so our explicit `Connection: close` is defence in depth.
- **Streaming:**
  - uploads go into a `ScratchFile` through a content receiver; no catalogue
    or project lock is held while the body arrives;
  - downloads use a content provider reading 64 KiB chunks from the export
    `ScratchFile`.
- **Server:**
  - `[server] worker_threads` / `--worker-threads` (default 8);
  - at most 16 queued connections per worker;
  - read/write/keep-alive timeouts of 30/60/10 s;
  - JSON error bodies also for httplib's own rejections;
  - SIGINT/SIGTERM handled by a `sigtimedwait` watcher thread (clean exit 0);
  - `SIGPIPE` ignored;
  - bind address must be a numeric IP.
- **Removed:** the Qt < 6.11 policy (`http_limit_policy.h`, its test, the INI
  key and the CLI flag). `deploy/nginx-aowis-server-db.conf` is now an optional
  TLS front end.
- **Tests:**
  - the HTTP suite now uses `httplib::Client`, an in-process
    `httplib::Server` and raw sockets; all previous assertions are kept;
  - limits are proven before the body: oversized declared upload, every JSON
    route limit, chunked JSON, chunked upload over the limit, URI/header
    limits;
  - streaming: a slow upload does not block health/catalogue/project
    requests; an abandoned upload leaves no branch and no file;
  - concurrency: 8 × 20 parallel requests;
  - the executable: clean SIGTERM shutdown, non-loopback bind, invalid
    CLI/INI configuration.
  - Seven of eight mutations were caught; the eighth, removing the explicit
    connection close, is behaviour-neutral with httplib 0.60.1.
  - Whole suite incl. the threaded HTTP server is clean under ASan/UBSan.
  - The model submodule's `unit_conversion_test` does not compile with GCC's
    UBSan (`static_assert` not constant). That is in AOWIS-SERVER-MODEL, not
    this repository.

Original plan:

Decision (2026-10-23): QHttpServer cannot limit requests before Qt 6.11
(Debian 13 ships 6.8) and only exposes fully buffered bodies. Replace it with
**cpp-httplib**, vendored as a pinned single header (MIT) in
`external/cpp-httplib/`. The distro packages are not used because their
versions differ too much (Debian 13: 0.18.7, Arch: 0.48+).

Scope:
- `src/db_http_routes.cpp` → httplib handlers with the same routes, JSON
  shapes and error mapping (`StorageErrorKind` → status).
- `set_payload_max_length` (= `max_transfer_mib`), header and URI limits, and
  read/write timeouts on every Qt version. The Phase 3 proxy policy is kept
  only as documentation for TLS termination.
- Streaming bundle upload through a content receiver directly into a
  `ScratchFile` (no full body in RAM); streaming download from the export
  `ScratchFile` via a content provider.
- The thread pool replaces the single event loop (prepares Phase 7).
- Server dependencies: Qt Network and Qt HttpServer are dropped; Qt Core stays
  only for the model codec. JSON uses Qt Core's `QJsonDocument`, as before.
- HTTP tests use `httplib::Client`. All existing assertions are kept, and
  additional tests cover oversized bodies (413 before buffering), slow and
  aborted uploads, and concurrent requests.

---

## Phase 6: Incremental synchronization cost (Medium, needs Phase 1) — done

Implemented in `aowis-db-custom-incremental-sync`:
- Import: a hash referenced by the bundle but not carried in it, and stored
  locally, is a trusted **boundary** (`ExternalVerificationMarks::trustedLocal`):
  - a local trie node gives its parent summary from one read (leaf UUID, or
    branch depth plus canonical prefix); depth ordering, slot and prefix are
    checked against the bundled parent as before;
  - model and reverse-index value kinds of grafted subtrees are enforced by
    the semantic diff against the verified parent snapshot (every changed
    leaf is decoded); a local subtree used as a raw tree is checked by
    descending to one leaf (stored subtrees are homogeneous: `V` or `INR1`);
  - a local revision must be a valid revision object; its snapshot counts as
    verified and its ancestry is not walked;
  - base and fast-forward checks: in the bundle, a boundary, or (fallback,
    only then reads local history) an ancestor of a boundary.
- Export: `collectDelta` reads the base's revision ancestry only. Each new
  revision's trees are walked against one parent's trees (a parent the
  recipient holds is preferred), aligning compressed tries where a branch
  split or collapsed. No pruning across revisions: that was found unsound by
  the random-history test (two revisions can justify each other's skips).
- Result: a 1-record delta on a 2,000-record model reads 106 local objects at
  both 50 and 400 commits of history (was ~9,400 / ~100,000); the bundle has
  17 objects.
- Trade-off: the bundle is a superset of the minimal one when a revision
  restores content held by an older revision (e.g. an undo of a large
  deletion). The importer does not store such objects twice.
- Tests: new `aowis-native-incremental-sync-tests`:
  - local reads equal at 50 and 400 commits and bounded; small bundles;
  - trie shape changes (deep splits, collapses, a 40-key subtree moving up
    and down, empty tree, restored snapshot, multi-revision and empty deltas);
  - six random model histories with merges and an eager and a lagging
    recipient, each delta reproducing the source exactly, then full verify;
  - forged bundles grafting local objects: reverse-index trie as raw or model
    tree, model trie as reverse index, bundled leaf with a local `INR1` value,
    local leaf in the wrong slot, local branch not deeper than its parent,
    local tree node as parent revision, base not an ancestor; plus a valid
    graft (local model trie as raw tree) as positive control;
  - fast-forward where the old head lies below a boundary, repeated and older
    deltas (up to date), divergence (`Conflict`), older held base.
  - `native_error_kind_tests` local-damage case now damages the base revision
    (always read) instead of an arbitrary object.
  - Nine mutations (raw-kind descent, trust off, ancestry fallback, value
    emission, boundary snapshot, boundary marked done, cross-revision pruning,
    trie alignment both ways) were each caught.

Original analysis:

**F3. Incremental import and export scale with total history.**
Measured with a 4-object delta (ASan build):

| Local history (commits) | Local objects re-read | Bytes re-read |
|---|---|---|
| 50 | 9,442 | 3 MB |
| 200 | 38,542 | 13.7 MB |
| 800 | 202,398 | 75 MB |

Causes:
- import: `verifyLogicalGraph` follows parents into local history and
  re-verifies every local revision, tree and value;
- export: `collectObjects(recipientBase)` materializes the base's complete
  object graph in a `std::set<Hash>`.

Fix (import):
- Objects already present in the local pack were written either by
  `commit`/`commitModel` (canonical by construction) or by a fully verified
  import. Treat them as **trusted boundary objects**: the structural walk stops
  at any hash that exists locally and is not in the bundle.
- The model reference check needs the parent snapshot to be "verified".
  Treat local indexed revisions as verified baselines, so a new revision is
  checked by diff against its local parent snapshot.
- Keep the bundle-only invariants: every bundled object is reachable, the
  recipient base is an ancestor, and fast-forward only.
- `aowis-native-verify` remains the full audit of local history.

Fix (export):
- Replace the full base walk with a boundary walk:
  1. collect the revision IDs reachable from the base (revision objects only);
  2. walk new revisions from the tip, stopping at base revisions;
  3. for each new revision, diff its trees against its first parent's trees
     and emit only subtrees whose hash differs (shared Merkle subtrees are
     skipped);
  4. dedupe with a hash set that is proportional to the delta.
- Step 1 is still O(revisions); add a revision-ancestry cache, or require
  the base to be the most recent shared head, if this shows up in profiles.

Tests and benchmark:
- extend `native_bundle_import_profile_tests`: for a 1-commit delta,
  `localObjectReads` must be bounded by a constant multiple of the delta
  size, independent of history length (run at 50 and 800 commits);
- import still rejects: a missing referenced object, a bundle whose reverse
  index disagrees with its models, and divergent history;
- add a phase to `tools/benchmark_replication_scaling.py` that records
  incremental import/export time against history length.

---

## Phase 7: Concurrent service (Medium, needs Phase 5)

**F4. The service processes one request at a time.**
Route handlers return synchronously on the main event-loop thread, and
`ProjectService` holds one mutex for **all** projects across import, export,
merge and save. A long transfer blocks `/health` and every other project.

Fix:
- `ProjectService`:
  - the catalogue mutex protects only the map (lookup, create, lazy open);
  - each `Entry` holds a `std::shared_ptr<Repository>` and its own operation
    mutex. `Repository` is already thread-safe internally, so read routes can
    skip the per-project mutex entirely.
- HTTP (cpp-httplib already runs handlers on a thread pool, and
  `[server] worker_threads` exists since Phase 5):
  - per-project queue limit: return 503 with `Retry-After` when a project
    already has N pending writes;
  - `/health` and `/ready` never wait for a project lock.

Tests: `/health` answers within 200 ms while a large import runs; two
projects import concurrently; concurrent edits on one project still give
exactly one success and one 409 per expected-head race; the stress suite
runs with the thread pool enabled.

---

## Phase 8: Model value validation (Low)

**F8. Out-of-range enums and non-finite numbers are stored.**
- `model_codec.cpp` decodes enums with an unchecked `static_cast`.
- `model_validation.cpp` checks finiteness only for `pipe_position`.
- Downstream EPANET runs would receive invalid enum values, NaN or Inf.

Fix:
- `tools/check_model_codec.py` already parses the model headers. Extend it
  to emit a generated `model_enum_ranges.inc` with the valid enumerator set
  per enum.
- `readValue` rejects (`ReadCorruptData`) values outside that set, so a
  decode failure becomes 400 at HTTP and a bundle rejection at import.
- `model_validation.cpp`: reject NaN/Inf in every `double` field. Use
  explicit ranges (> 0, ≥ 0, [0,1]) for physical quantities, defined in one
  table (diameter, length, roughness, minor loss, elevation, tank levels and
  diameter, demand multipliers, pattern factors, curve points, time values).
  The table is to be agreed with the hydraulics side before implementation.
- Existing repositories: `aowis-native-verify --semantic` reports violating
  entities instead of failing silently later.

Tests: a codec round trip with an out-of-range enum is rejected; validation
rejects NaN/Inf and out-of-range values per field class; HTTP edit with
NaN → 400.

---

## Phase 9: Cleanup (Low)

- `Repository::Repository`: fix the leftover indentation (former `if` branch)
  and remove the unused `format` variable.
- Remove the duplicated comment text in `syncPack()` and `compact()`.
- Remove the unused `loadMembers()` in `hydraulic_network_persistence.cpp`
  and the unused `backend` parameters in `tests/native_repository_verify_tests.cpp`
  and `tests/native_http_tests.cpp` (the only `-Wall -Wextra` warnings).
- Give the hydraulic load APIs a typed failure channel (e.g. rethrow
  non-Corruption `StorageError`s) so I/O and ENOSPC map to 500/507 correctly.
- Remove no-op `try { … } catch (...) { throw; }` wrappers in
  `mergeBranches`, `commit` and `commitModel`.
- `ProjectService::createProject`: the `remove_all(staging)` in the catch
  block must not throw (use the `std::error_code` overload), so it cannot
  replace the original exception.
- `deploy/aowis-server-db.service`: add `ProtectSystem=strict`,
  `ReadWritePaths=/var/lib/aowis-db`, `PrivateTmp=true` and
  `ProtectHome=true`. Align the INI file name with `ExecStart`
  (`/etc/aowis/server-db.ini` vs `deploy/aowis-server-db.ini`).

No functional tests are needed beyond the existing suites.

---

## Applying each phase

```
./tools/apply_patch.sh ~/Downloads/aowis-db-custom-<phase>.zip
./compile_linux_tests.sh --both
```

Phases that change the service (2, 3, 4, 5, 7) additionally need a release
build and a restart of `aowis-server-db`:

```
./compile_linux.sh
```
