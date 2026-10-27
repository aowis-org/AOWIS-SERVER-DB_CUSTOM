# UUIDv7 revision replication — native full and incremental bundles

**Implemented native checkpoint, Phase 2e-2b (2026-10-27).** Native UUIDv7
revision replication supports full (`AOWREV21`) and incremental (`AOWREV22`)
transfers. CLI and HTTP now expose explicit UUIDv7 revision-bundle operations;
the existing SHA-era default project/revision endpoints and the GUI have not switched to
UUIDv7 revision identifiers. No legacy revision-store migration is supplied.

## Native API and identity contract

- `Repository::exportRevisionBundleV2(project, branch, fd, limits)` produces a
  **complete** branch history, including both parents of merges and all
  referenced Merkle objects. The caller supplies an **empty regular file**.
- `Repository::exportIncrementalRevisionBundleV2(project, branch,
  recipientBase, fd, limits)` produces only objects not reachable from a
  specified ancestor revision UUIDv7. The base must be a published revision
  belonging to the same project and reachable from the source branch HEAD.
- `Repository::importRevisionBundleV2(expectedProject, fd, localBranch,
  limits)` auto-detects **only** `AOWREV21` and `AOWREV22`, verifies the
  explicitly expected project identity, and preserves the source branch ID,
  revision UUIDs, parent UUID/SHA bindings and model snapshots.
- An incremental import requires an existing branch with the **same UUIDv7
  branch identity** at the **exact base UUID and SHA-256**. Identical replay
  of a previously imported tip is a no-op. A different target HEAD is a
  conflict, never a silent reset. The source branch is not moved by import.
- Names used by callers are *display names*, never repository filenames.
  Branch records and immutable identity mappings use UUIDv7 filenames.

## Wire formats

Both formats use big-endian unsigned 64-bit counts/sizes, UUIDv7 in binary
RFC order, SHA-256 as raw 32-byte digests, and monotonically increasing
object SHA-256 order. Each object entry is
`[objectSHA(32)][length(u64)][payload(length bytes)]`.

| Offset | Size | `AOWREV21` full bundle | `AOWREV22` incremental bundle |
|---:|---:|---|---|
| 0 | 8 | ASCII `AOWREV21` | ASCII `AOWREV22` |
| 8 | 16 | Project UUIDv7 | Project UUIDv7 |
| 24 | 16 | Branch UUIDv7 | Branch UUIDv7 |
| 40 | 16 | New HEAD revision UUIDv7 | New HEAD revision UUIDv7 |
| 56 | 32 | New HEAD revision SHA-256 | New HEAD revision SHA-256 |
| 88 | 8 | Object count | Object count |
| 96 | 16 | — | Pinned recipient base revision UUIDv7 |
| 112 | 32 | — | Pinned recipient base record SHA-256 |
| 96 or 144 | variable | Full immutable closure | Only delta objects |

`AOWREV22` can carry **zero objects** only for a same-revision replay of its
base. No manifest or branch-name field is embedded in a user-controlled path.

The hash of a v2 revision is the SHA-256 digest of its complete immutable
`T/02` record. Its UUIDv7 is a separate domain identity. A replicated revision
must preserve **both**. Content-addressed Merkle objects can be shared safely.

## Memory, filesystem and resource constraints

- Bundle payloads remain in caller-owned regular files. The importer's
  preflight validates their SHA-256 with **64 KiB read chunks** before
  publication; an indexed on-disk offset table provides random lookup. Large
  payloads are decoded **one object at a time**, not into a whole-bundle map.
- Export uses repository-local, private, accounted scratch files for the
  visited SHA hash tables and external sorted runs. Incremental export builds
  the base's reachable-object set independently and subtracts it from the
  source HEAD closure.
- Import uses a scratch-backed location index, visitation marks and tree
  summaries. A bounded in-memory revision UUID→offset table is still needed;
  the module is **not constant memory** in total number of revisions.
- Hard safety ceilings: **64 GiB** bundle bytes, **1,000,000 objects** and
  **64 MiB per object**. `BundleLimits` may set *smaller* byte, object and
  per-object limits. Scratch accounting and filesystem quotas can stop a
  transfer earlier. This removes the prior 64 MiB *whole-bundle* limit;
  it does not permit unlimited memory or disk usage.
- Native transfers use caller-owned regular file descriptors. The explicit
  HTTP `revision-bundle` endpoint now streams 64 KiB chunks from repository
  scratch files and spools uploads to disk with request size limits and the
  existing global `TransferStagingBudget` reservation. Disconnected downloads
  release reservations; aborted or oversized uploads never invoke import.
  **Resumable network chunks, authentication, and bandwidth limits remain
  unimplemented.** This is *not* a resumable transport.

## Authentication, integrity and publication

1. Verify the expected project UUID, format and header, and the local branch
   UUID/base HEAD (for incremental). Reject unsupported formats.
2. Validate each bundle object's order, bounds and SHA-256 before writing to
   the destination object pack. Verify UUID/expected-SHA ancestry (including
   both merge parents), loading omitted old ancestors through the authenticated
   local resolver for incremental transfers.
3. Verify the combined immutable graph, canonical trie structure, model
   records and reverse-reference indexes, with per-bundle disk-backed
   reachability marks. Reject unreferenced/malformed bundle entries.
4. Only after all checks: append missing immutable pack objects, `fsync`
   the pack, publish new UUID→SHA bindings **parent-first**, then atomically
   replace the branch HEAD **last**. An interrupted import can leave unused
   immutable objects/identities, but no partially verified published HEAD.
5. Conflicting project, branch identity, revision digest or divergent HEAD
   causes rejection. A replay never regenerates UUIDs or rewinds a branch.

**SHA-256 integrity is not remote authentication.** No unsigned bundle may
establish trust in an untrusted external sender. HTTP use requires AOWIS-wide
TLS, authorization, and resource-budget integration.

## Regression coverage

- Complete DAG transfer with shared history and two-parent merges.
- Incremental base → new HEAD, replay, stale base rejection, corrupt delta
  rejection, unknown base rejection and incoming merge with a new side branch.
- Identity and project isolation; preserving the branch UUID across restart
  and compaction; model/reverse-index integrity.
- A full bundle **larger than 64 MiB** imports successfully through
  disk-backed staging; a caller-supplied smaller ceiling rejects it without
  publishing any branch.

## Canonical CLI cutover — Phase 2g-4b

The public `aowis-dvmdb-sync` commands are `head`, `branch-id`, `export`,
`export-incremental`, and `import`. Every command requires a canonical project
UUIDv7; incremental export additionally requires a base revision UUIDv7. Full
and incremental transfers use only `AOWREV21` and `AOWREV22`, respectively.
`head-v2`, `export-v2`, `import-v2`, `manifest-*`, `seed-branch-id`, and all
SHA-revision transfer aliases have been removed from this CLI. Exports refuse
existing destination files; failures cannot publish partial bundle files.

The canonical `ProjectService` methods now create UUIDv7 revisions by default
and use typed `RevisionId` results for branches, HEADs, history, graphs,
merges, model commits and revision reads. Existing `*V2` calls remain typed
aliases for the HTTP implementation. SHA-only project operations are named
`*ShaFixture` and used only by storage-era native regression tests; those
fixtures are not data-format compatibility or an application API.

Remaining work: migrate the old native SHA-fixture tests off the public service
header, remove those temporary fixture entry points, eliminate SHA operations
from `Repository` itself, and integrate the GUI. The native-only tests cannot
verify Qt HTTP/GUI binary behavior.
