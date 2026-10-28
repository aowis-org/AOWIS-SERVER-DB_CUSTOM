# UUIDv7 portable and incremental repository synchronization

The canonical DVMDB synchronization CLI and HTTP bundle routes transfer
**immutable UUIDv7 revisions**, including both ordered parents of merge
revisions. A revision UUIDv7 is its stable logical identity; SHA-256 hashes
authenticate its canonical record and Merkle/model data. Bundle imports do
not silently merge divergent histories or assign a new identity to an
existing revision.

## Native UUIDv7 bundle APIs

- `Repository::exportRevisionBundleV2(project, branch, fd)` exports a complete
  `AOWREV21` branch snapshot: project UUID, branch UUID, revision UUID,
  complete reachable ancestry and the associated content objects.
- `Repository::exportIncrementalRevisionBundleV2(project, branch, base, fd)`
  exports an `AOWREV22` delta based on an authenticated UUIDv7 revision and
  expected record SHA-256. Missing immutable objects must already be present
  in the recipient repository.
- `Repository::importRevisionBundleV2(project, fd, localBranch)` verifies the
  caller-supplied **expected project UUID** against the bundle, checks record
  and content hashes, verifies both parent links, and creates, fast-forwards
  or recognizes an already-imported branch HEAD. It rejects divergence and
  wrong-project histories. Publication occurs only after immutable objects
  and resolver bindings are synchronized.

The native importer uses disk-backed scratch indexes rather than retaining
all object payloads in memory. The caller can impose maximum byte, object and
per-object limits. Transfers are not a substitute for authenticated access.

## CLI usage

All canonical commands require a project UUIDv7; the revision and branch
identifiers in their outputs are UUIDv7, not SHA-256 strings.

```bash
project_uuid="<project-uuidv7>"
./build-linux/aowis-dvmdb-sync head \
  /path/to/source "$project_uuid" main
./build-linux/aowis-dvmdb-sync branch-id \
  /path/to/source "$project_uuid" main

./build-linux/aowis-dvmdb-sync export \
  /path/to/source "$project_uuid" main /tmp/main.aowbundle
./build-linux/aowis-dvmdb-sync import \
  /path/to/destination "$project_uuid" /tmp/main.aowbundle remote_main

base="$(./build-linux/aowis-dvmdb-sync head \
  /path/to/destination "$project_uuid" remote_main)"
./build-linux/aowis-dvmdb-sync export-incremental \
  /path/to/source "$project_uuid" main "$base" /tmp/update.aowbundle
./build-linux/aowis-dvmdb-sync import \
  /path/to/destination "$project_uuid" /tmp/update.aowbundle remote_main
```

Exports create new files exclusively; they refuse existing bundle filenames,
invalid project/revision UUIDs and symlink targets. The `-v2`, SHA-bundle,
identity-manifest and seed-branch-ID CLI commands have been removed. Neither
`AOWSYNC3` nor `AOWSYNC4` is accepted through this public CLI. There is no
compatibility migration for older pre-release repositories.

## HTTP synchronization

The canonical HTTP routes are project-scoped:

- `GET /dvmdb/v1/projects/{projectUuid}/branches/{branch}/bundle`
  exports a complete `AOWREV21` bundle.
- `GET .../bundle?base={revisionUuidV7}` exports an `AOWREV22` delta.
- `POST .../bundle` imports a binary bundle and reports branch creation,
  fast-forward or idempotent replay; divergent history fails with a conflict.

Uploads and downloads use bounded project scratch files and configured HTTP
transfer quotas. The HTTP server requires separate application-level
security measures: bundle SHA-256 checks prove integrity, **not** authorship,
authorization or permission to modify a project. The native verifier checks
physical and logical integrity, but generic bundle import is not a validator
of hydraulic engineering semantics.

## Branch collaboration and retention

Import into a tracking branch, inspect its immutable history and merge it into
the desired local branch explicitly. Branch IDs survive transfer and rename;
branch names are metadata inside records stored by UUIDv7 filenames. The
compactor retains every published UUIDv7 revision mapping, including revisions
not currently referenced by a HEAD, rather than making stable revision IDs
invalid. This is conservative retention, not a revision garbage collector.

For the complete frozen bundle wire format and resource limits, see
[UUIDv7 revision replication](uuidv7-revision-replication-v2.md).

### Branch creation provenance and unborn branches

Every full/incremental transfer preserves the UUIDv7 identity of a branch,
its nullable committed HEAD, immutable `originBranchId` and exact
`forkRevisionId` together with all committed revisions and SHA-256 integrity
bindings. A branch whose HEAD is `null` has no committed revision objects.
An imported branch must not acquire a guessed fork point or synthetic R0.

Private user working copies are not part of committed revision replication;
they require a distinct explicitly authorized draft backup/sync facility.
Conflicting target HEAD updates are rejected unless an explicit validated
fast-forward or merge policy applies. See
[replication contract](uuidv7-revision-replication-v2.md).
