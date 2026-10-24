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
