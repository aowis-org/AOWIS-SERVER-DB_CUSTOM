# VC-03 — Repository role and remote-tracking metadata (version 1)

This document defines a **metadata-only** prerequisite for independent personal
and shared DVMDB repositories. It does not implement Fetch, Clone, Checkout,
Commit, Publish, authentication, or authorization; those remain in subsequent
VC milestones. In particular, a configured remote URL or UUID **cannot** prove
that a remote endpoint is authentic. Identity must be verified at connection
and again at fetch/publish time. Deployment isolation is required for private
HTTP roots until VC-12 permissions exist.

## Logical identity and namespaces

- `repositoryId`: immutable UUIDv7 of a storage root (`repository.id`).
- `projectId` and `branchId`: immutable logical UUIDv7s, copied *unchanged*
  when the corresponding project/branch is cloned or fetched.
- Each root owns its own mutable native branch HEAD; branch HEADs are **never**
  shared by moving or copying tracking entries.
- The remote registry is keyed by `remoteRepositoryId`, independent of a GUI
  connection profile name or the configured endpoint string.
- A tracking entry is keyed by the tuple
  `(remoteRepositoryId, projectId, branchId)` and holds the last observed
  `RevisionId` or an **explicit null** observed HEAD. A **missing entry** means
  the branch has not been observed (or is absent); it is **not** the same as
  an observed unborn branch with a null HEAD. The observer must distinguish
  a remote 404 from a successful read with a null HEAD.
- The role is initially `unconfigured`, and may be explicitly set once to
  `private` or `shared`. These values describe deployment intent, not access
  policy, user identity, or security capabilities.

## Durable wire format

The root-level file `repository.tracking`, when present, uses UTF-8/ASCII
newline-delimited **`AOWIS-TRACKING-1`** format:

```text
AOWIS-TRACKING-1
role\tprivate
remote\t<remote-repository-uuidv7>\t<lowercase-hex-encoded-ASCII-endpoint>
ref\t<remote-repository-uuidv7>\t<project-uuidv7>\t<branch-uuidv7>\t<revision-uuidv7-or-dash>
```

The angle-bracket placeholders in the example are explanatory. The on-disk
values contain no angle brackets. The marker and `role` line are mandatory;
`remote` and `ref` lines are optional, sorted by their stable UUID keys.
A dash (`-`) is an observed null HEAD. Missing `ref` is a distinct state.
The format allows at most 64 remotes, 10,000 tracking entries and 8 MiB of
payload. Endpoints are ASCII HTTP(S) URLs or absolute `file:///` locations;
userinfo, URL fragments and controls are rejected. No credentials are stored.
Existing roots without this file load as `unconfigured`, with an empty registry
and no observations. Corrupt or unknown-version files **fail closed**.

Writes are serialized under the root's existing lifetime catalogue lock and
service mutex. The whole file is staged via `O_EXCL`, `fsync`ed, renamed
atomically over the previous file, and the root directory is `fsync`ed. A
leftover unpublished `repository.tracking.tmp` is discarded on restart.
If publication becomes uncertain, further operations refuse until restart.
Offline backup/restore verifies the exact tracking file bytes as well as all
native branch refs, and refuses malformed metadata.

## Native/REST usage and limitations

`ProjectService` offers explicit one-time role configuration, idempotent remote
registration by UUID, and tracking observation updates with a compare-and-swap
precondition. The expected observation has three states: absent; present and
null; or present with a concrete revision UUID. A mismatch fails with Conflict.
An existing remote UUID cannot be silently rebound to another endpoint.
Native callers must only record observations obtained from a verified remote
read; these operations are **not themselves a verification protocol**.

`GET /dvmdb/v1/repository` remains backward compatible.
`GET /dvmdb/v1/repository/tracking` supplies repository UUID, role, remote
registry and qualified observed refs (including explicit JSON null for unborn
HEADs). It is read-only: no HTTP route can change this metadata yet. Publishing
these endpoints without authentication can reveal deployment metadata; until
VC-12, deploy them behind appropriately isolated and protected transports.

VC-03 is **not complete** until the GUI can explicitly configure a personal
working repository and can safely verify remote identity/targeting; verified
fetch/clone and transfer are addressed in VC-06. No branch ref or revision
ancestry is modified by this metadata schema.
