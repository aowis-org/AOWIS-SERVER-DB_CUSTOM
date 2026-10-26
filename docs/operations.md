# Operating the native AOWIS database service

This is an operator procedure for the current native pack-based service, not a
release durability certification. The storage root contains `catalogue.lock`,
`projects/<uuid>/project.meta`, and each project's `repository/` (packs,
derived object index, published branch references, recovery evidence and tmp).
Never copy only the active pack: a repository snapshot also needs all matching
reference files and generation markers.

On Linux, an **unconfigured development server** reuses an existing
`./aowis-dvmdb-data/` checkout-local root; otherwise it uses
`$XDG_DATA_HOME/aowis-dvmdb/` (normally `~/.local/share/aowis-dvmdb/`).
The production `deploy/aowis-server-dvmdb.ini` still specifies
`/var/lib/aowis-dvmdb`. Existing roots are **not moved automatically**.
If both checkout-local and user-data roots exist, the checkout-local root
wins, and startup reports the path in use; use `--data-root` or
`[storage] root=` to select another. For a permanent relocation, stop every
writer, back up and restore the whole service root to a new verified location
before restarting. `catalogue.lock` belongs inside the storage root because
it coordinates all server processes sharing that root, irrespective of their
configuration files. It must not be relocated to `/etc`.

## Identify the active root before maintenance

`./compile_linux.sh` starts the HTTP process in the foreground after a
successful Release build. The default listener is `127.0.0.1:8755`. Before
backing up, relocating or investigating missing projects, **read the storage
root printed by that process** and confirm the HTTP catalogue:

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

The first command checks catalogue readiness; the second lists the projects
of **the root actually opened by the running process**. It does not look for
repositories elsewhere. A healthy, empty catalogue may simply mean you
selected the wrong root, not that your projects were erased.

For development startup without an explicit root, priority goes to an
existing `./aowis-dvmdb-data` (relative to the server's working directory),
then the per-user directory. A configured INI `storage/root` overrides both;
`--data-root` overrides the INI. The bundled `deploy/` INI explicitly uses
`/var/lib/aowis-dvmdb`, not the per-user root.

Before changing paths, stop the HTTP server **and any standalone application
or other process using the same native root**. Different DVMDB storage roots
are independent databases; pointing multiple processes at one root is not a
synchronization mechanism. Never delete a live `catalogue.lock`, manually
combine catalogue directories, or replace a running repository pack. Use
the verified offline backup/restore procedure below.

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
   every `projects/<uuid>/repository` with `aowis-dvmdb-verify --full PATH`.
   Verification may rebuild a derived sidecar or recover an unpublished tail,
   so do not use an immutable evidence copy for this step.
4. Retain the verified backup separately from live data. Restore only while
   all writers are stopped; preferably restore to a NEW root and configure the
   server to start from that root, then check `/dvmdb/v1/ready` and project list.
   Never combine files from two independently changing repository copies.
5. After restore, repeat `aowis-dvmdb-verify --full` for every repository and
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
./compile_linux.sh --build-only
python3 tools/offline_backup.py backup /path/to/aowis-dvmdb-data /backup/new-snapshot \
  --verify-binary ./build-linux/aowis-dvmdb-verify
python3 tools/offline_backup.py restore /backup/new-snapshot /path/to/new-aowis-dvmdb-data \
  --verify-binary ./build-linux/aowis-dvmdb-verify
python3 tools/offline_backup.py verify /path/to/new-aowis-dvmdb-data \
  --verify-binary ./build-linux/aowis-dvmdb-verify
```

The utility locks the catalogue and **all** repository lifetime locks, copies
the complete storage root to a private sibling directory, compares project
metadata and every published branch head, verifies each copied repository with
`aowis-dvmdb-verify --full`, synchronizes files/directories, and atomically
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

## Upgrading or relocating a complete service root

An engine upgrade is a **cold, whole-root** operation, not an in-place rewrite
of pack objects. For a new executable or a change of host/filesystem:

1. Stop the HTTP service and every direct C++ user. Record the current binary
   version, project catalogue (IDs and names), **all branch heads**, and a few
   representative historical reads. Take a verified offline backup with
   `tools/offline_backup.py`; store it separately from the live root.
2. Restore that backup into a **new, absent directory** using the prospective
   build's `aowis-dvmdb-verify`. This verifies its ability to read the old
   persisted bytes; never try an upgrade on the sole source copy.
3. Point a disposable instance of the prospective build at the restored root.
   Check project identities, branch names, revision ancestry, model payloads
   and historical reads, not merely that the service starts. Ensure all
   repository handles close before an offline verification or backup command.
4. Make a representative new commit on the restored root, reopen it and verify
   historical reads again. Back up and restore **that newly written state** to
   another new directory to confirm the new format remains recoverable.
5. Only after validation, switch the service to the new root, retaining the
   original untouched until a rollback window has expired. A rollback to an
   older executable is **not** guaranteed to read commits made by a newer
   executable; restore the original verified backup rather than pointing an
   older binary at a root modified by the newer version.

The `aowis-dvmdb-upgrade-restore-tests` CTest automates an isolated version of
this drill with four projects, two immutable earlier-build pack specimens,
divergent branches, more than 1 MiB of current-format model values, sidecar
reconstruction and a second post-write backup/restore. It checks all project
IDs, branch histories and payloads. The test demonstrates *this* historical
format is readable by the present build; it does not exercise a separately
running old binary, an actual filesystem migration, online upgrades or an
unbounded scale of projects.

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

The test `aowis-dvmdb-http-transfer-stress-tests` runs the real server on an
isolated loopback port with a disposable multi-project root. It first tests
HTTP transfer staging admission at a one-transfer budget, including an
incomplete upload and interrupted download, then restarts the server with a
larger staging budget and runs six concurrent clients performing repeated
multi-megabyte imports, downloads and head checks across three projects.
It verifies the repositories after stopping the server. Run it through CTest:

```bash
./compile_linux_tests.sh --both
ctest --test-dir build-linux-tests-release -R '^aowis-dvmdb-http-transfer-stress-tests$' --output-on-failure
```

The test creates temporary data under `/tmp` and keeps its private directory
and server log if it fails. Successful completion demonstrates basic admission,
cleanup, response correctness and short-run cross-project stability. It does
**not** measure throughput under sustained production loads, test true disk-full
conditions, or establish CPU, memory, or service latency SLAs.

## Real filesystem-full regression

`aowis-dvmdb-physical-enospc-tests` uses the **production native store** and
an isolated **32 MiB Linux tmpfs** mounted in a new user/mount namespace. The
fixture allocates real filesystem blocks until a kernel write produces ENOSPC,
then checks a failed commit, atomic named export and incremental import. After
space reclamation it independently reopens, fully verifies and retries the
repositories. No existing host filesystem or database is filled.

```bash
./compile_linux_tests.sh --both
ctest --test-dir build-linux-tests-release \
  -R '^aowis-dvmdb-physical-enospc-tests$' --output-on-failure -V
```

The test returns **SKIP** (not pass) when unprivileged user/mount namespaces or
private tmpfs mounts are prohibited by the host or CI sandbox. A skipped test
is not evidence of ENOSPC safety; run it on a supported isolated Linux host.
The test covers genuine tmpfs space exhaustion, **not** filesystem metadata
journaling, ext4/XFS behavior, controller write caches, actual storage-device
power loss or sustained HTTP staging exhaustion. Those require isolated
block-device/VM qualification before broader durability claims.

## Abrupt process termination qualification

`aowis-dvmdb-sigkill-restart-tests` exercises the **production native library**,
not the fault-instrumented library. An independent child process repeatedly
commits to a repository and reports each completed commit through a pipe. The
parent terminates it with `SIGKILL` at varying points, reopens the repository,
fully verifies its history, and checks that every acknowledged commit remains
visible. A commit that was durably published but not yet acknowledged may also
be present; its counter must be exactly one ahead of the last acknowledgement.
The test then proves that further commits still succeed.

A second fixture repeatedly terminates a process during filesystem-path
bundle exports, verifies every final `*.bundle` name by importing it into an
independent repository, and checks that source history remains unchanged.
Private orphan staging files are **not** treated as published bundles.
The entire test uses disposable directories with randomized names.

```bash
./compile_linux_tests.sh --both
ctest --test-dir build-linux-tests-release \
  -R '^aowis-dvmdb-sigkill-restart-tests$' --output-on-failure -V
```

**Limits of evidence:** `SIGKILL` does not drop the kernel page cache, emulate
lost disk-controller write caches, or interrupt block-device flushes the way a
machine power cut might. Passing this test establishes process-death recovery
on the tested host, **not** filesystem or hardware power-loss durability.
Controlled ext4/XFS-on-virtual-disk tests must abruptly terminate the VM or
virtual storage device without orderly guest shutdown, then remount/check and
verify the recovered native repository and expected committed branch heads.
Record the filesystem, mount options, virtual-disk cache mode, flush behavior,
cut timing, and results. Never run destructive power-cut experiments against
live AOWIS repositories or host block devices.

## Recovery and maintenance

- On reopening after an interrupted append, the engine may preserve an
  unpublished torn pack tail under `recovered-pack-tails/`. Preserve this
  evidence until investigated; do not treat every preserved tail as corruption
  of a published revision.
- For unexplained corruption, first stop writers and retain a complete copy of
  the root. Run physical and full verification on a separate copy and preserve
  logs. Never 'repair' an object by hand or delete a published head.
- Offline compaction: stop all other repository users, take and verify a
  backup, run `aowis-dvmdb-compact PATH` on the selected repository and then
  run `aowis-dvmdb-verify --full PATH`. The engine refuses compaction if
  another handle owns a lifetime lock; always verify the output afterwards.
- If ref publication reports an I/O error, treat publication as **ambiguous**:
  restart/reopen and inspect the actual durable branch head before retrying a
  commit or merge. Retrying blindly may create duplicate logical work.

Deployment on particular filesystems, power-loss guarantees, automated online
snapshots, multi-process scratch enforcement and long-duration fault tests are
still outstanding deployment qualification.

## VM power-cut and sustained HTTP qualification

See [durability qualification](durability-qualification.md) for the opt-in
QEMU ext4/XFS guest-cache-loss procedure and the one-server sustained HTTP
soak. Neither is included in the short CTest matrix. Killing a QEMU guest is
not proof of physical SSD/controller power-loss durability.
