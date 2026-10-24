# Operating the native AOWIS database service

This is an operator procedure for the current native pack-based service, not a
release durability certification. The storage root contains `catalogue.lock`,
`projects/<uuid>/project.meta`, and each project's `repository/` (packs,
derived object index, published branch references, recovery evidence and tmp).
Never copy only the active pack: a repository snapshot also needs all matching
reference files and generation markers.

## Coordinated backup and restore

1. Stop the **database server and all direct-library users** of the storage
   root; confirm that they have exited. The catalogue lock coordinates server
   startup, but filesystem copying is **not** an online snapshot protocol.
2. Take a point-in-time filesystem snapshot, or copy the **entire storage root**
   to a new destination on a sufficiently provisioned filesystem. Preserve
   directory structure, permissions and symlinks; do not follow or replace
   entries with symlinks introduced by untrusted users. Keep the original root
   untouched. Where available, snapshot/copy after a filesystem sync.
3. On a **separate staging copy** of the backup, run full verification for
   every `projects/<uuid>/repository` with `aowis-native-verify --full PATH`.
   Verification may rebuild a derived sidecar or recover an unpublished tail,
   so do not use an immutable evidence copy for this step.
4. Retain the verified backup separately from live data. Restore only while
   all writers are stopped; preferably restore to a NEW root and configure the
   server to start from that root, then check `/api/v1/ready` and project list.
   Never combine files from two independently changing repository copies.
5. After restore, repeat `aowis-native-verify --full` for every repository and
   validate expected branch heads and one or more representative model reads.
   Storage verification does not establish hydraulic model semantics.

A successful verification is necessary but not sufficient proof of a fully
recoverable backup. Periodically perform full restore drills using an isolated
service instance and compare expected heads.

### Verified offline backup and restore utility

`tools/offline_backup.py` provides a **cold**, whole-service backup/restore
workflow for Linux. Stop the server and every direct-library user first. It
refuses a live catalogue or any independently open repository using the
existing lifetime locks; a filesystem snapshot is **not** created online.
Always choose a new, absent destination directory, on a filesystem with enough
space for the full root plus verification temporary files:

```bash
./compile_linux.sh
python3 tools/offline_backup.py backup /path/to/aowis-db-data /backup/new-snapshot \
  --verify-binary ./build-linux/aowis-native-verify
python3 tools/offline_backup.py restore /backup/new-snapshot /path/to/new-aowis-db-data \
  --verify-binary ./build-linux/aowis-native-verify
python3 tools/offline_backup.py verify /path/to/new-aowis-db-data \
  --verify-binary ./build-linux/aowis-native-verify
```

The utility locks the catalogue and **all** repository lifetime locks, copies
the complete storage root to a private sibling directory, compares project
metadata and every published branch head, verifies each copied repository with
`aowis-native-verify --full`, synchronizes files/directories, and atomically
publishes the destination directory with **no overwrite**. It refuses symbolic
links and nonregular entries in the source tree; inspect such entries rather
than blindly following them. `verify` likewise verifies an isolated private
copy while holding source locks, rather than trying to open an exclusively
locked source repository. An interrupted run may leave a hidden
`.aowis-offline-copy-*` staging directory. Only final destinations that
were successfully published should be treated as backups; clean old staging
folders after ensuring no operation is running.

The restore destination is a **new** root, not a merge with live data. Before
switching your service over, compare the expected project IDs and branch heads,
check representative hydraulic reads, and keep the original intact until the
restored service has been validated. Verification covers native physical and
logical consistency, **not** domain-level hydraulic semantics. Backup staging
requires free space and is subject to normal OS filesystem limits, not the
native engine's scratch budget. This is not an incremental or online backup
solution and does not replace a separate physically isolated backup policy.

## Full disk or scratch exhaustion

- HTTP 503 `staging_busy` means the **configured aggregate HTTP transfer
  staging reservation** is saturated; retry later. It does not mean disk-full.
- HTTP 507 `insufficient_storage` or native `ResourceExhausted` means the
  filesystem reported ENOSPC/EDQUOT. Stop new writes/transfers and inspect
  free bytes, inodes, quotas and ongoing pack/scratch usage. Do not delete live
  pack generations, current pointer markers or reference files manually.
- Scratch files are repository-local and normally unlinked. Space held by an
  open descriptor is freed when that operation/process exits. A restart
  removes the named scratch fallback leftovers on repository open.
- **Interrupted filesystem-path bundle exports:** `exportBranchBundle(path)`
  and its incremental variant write and fsync a private file in the final
  destination directory, then atomically link the completed file at the
  requested name **without replacing existing files or symlinks**. The
  directory is fsynced before a successful return. A crash before publication
  leaves the final name absent; a crash afterward can leave the final name
  present with a **complete** bundle. After a publication-stage error, the
  caller may not know which outcome occurred: inspect the destination name
  and verify/import its contents before retrying under a new name. On
  filesystems without `O_TMPFILE`, a killed exporter may leave a hidden
  `.aowis-export-*` staging file in the destination directory. Remove only
  stale staging files after confirming all exporters have stopped; never
  treat staging filenames as completed bundles. Filesystem-path staging is
  outside the repository's internal scratch budget. Caller-owned descriptor
  exports do **not** get automatic name publication or crash cleanup: their
  owner must coordinate visibility and durability.
- Internal export/import verification scratch is bounded independently by
  `[storage] max_internal_scratch_mib` (default 1024 MiB in the HTTP service,
  or `--max-internal-scratch-mib`), across all native projects in that server
  process. The quota tracks the sum of **logical file extents**, including
  overlapping export-table resize files, and rejects expansions with
  `Busy` before they write (HTTP 503 `scratch_busy` with `Retry-After: 1`). Accounting is released on close, even
  after a failed operation. The standalone native library defaults to no cap;
  callers may set one with `setInternalScratchLimit()` before starting work.
  It does not include HTTP transfer spools, pack growth, offline compaction,
  named recovery evidence or other processes using the same filesystem.
  Keep filesystem headroom and an OS quota if a physical per-volume guarantee
  is required. A sparse file extent reservation is not a disk-block guarantee.

## Concurrent HTTP transfer qualification

The test `aowis-native-http-transfer-stress-tests` runs the real server on an
isolated loopback port with a disposable multi-project root. It first tests
HTTP transfer staging admission at a one-transfer budget, including an
incomplete upload and interrupted download, then restarts the server with a
larger staging budget and runs six concurrent clients performing repeated
multi-megabyte imports, downloads and head checks across three projects.
It verifies the repositories after stopping the server. Run it through CTest:

```bash
./compile_linux_tests.sh --both
ctest --test-dir build-linux-tests-release -R '^aowis-native-http-transfer-stress-tests$' --output-on-failure
```

The test creates temporary data under `/tmp` and keeps its private directory
and server log if it fails. Successful completion demonstrates basic admission,
cleanup, response correctness and short-run cross-project stability. It does
**not** measure throughput under sustained production loads, test true disk-full
conditions, or establish CPU, memory, or service latency SLAs.

## Recovery and maintenance

- On reopening after an interrupted append, the engine may preserve an
  unpublished torn pack tail under `recovered-pack-tails/`. Preserve this
  evidence until investigated; do not treat every preserved tail as corruption
  of a published revision.
- For unexplained corruption, first stop writers and retain a complete copy of
  the root. Run physical and full verification on a separate copy and preserve
  logs. Never 'repair' an object by hand or delete a published head.
- Offline compaction: stop all other repository users, take and verify a
  backup, run `aowis-native-compact PATH` on the selected repository and then
  run `aowis-native-verify --full PATH`. The engine refuses compaction if
  another handle owns a lifetime lock; always verify the output afterwards.
- If ref publication reports an I/O error, treat publication as **ambiguous**:
  restart/reopen and inspect the actual durable branch head before retrying a
  commit or merge. Retrying blindly may create duplicate logical work.

Deployment on particular filesystems, power-loss guarantees, automated online
snapshots, multi-process scratch enforcement and long-duration fault tests are
still Phase 6/7 qualification work.
