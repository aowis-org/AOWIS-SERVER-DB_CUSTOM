# UUIDv7 branch reference and provenance contract

Each branch is a persistent, individually identified development line with
its own UUIDv7 and a name that may contain valid UTF-8 and spaces. Branch
files or database keys use UUIDv7, **never** the editable name.

## Logical fields

| Field | Semantics |
|---|---|
| `branchId` | Stable branch UUIDv7 |
| `projectId` | Owning project UUIDv7 |
| `name` | User-visible unique UTF-8 label |
| `headRevisionId` | Nullable UUIDv7 of **latest committed revision** |
| `originBranchId` | Immutable source branch UUIDv7 when known |
| `forkRevisionId` | Immutable exact source committed revision UUIDv7 when known |

A newly created project's `main` has a **null `headRevisionId`** and no
origin/fork revision. A branch created from committed Rn points at Rn but
does not create a new committed revision. Provenance continues to point at
the original source branch/fork revision even after HEAD moves or names
change. The branch name cannot be used to recover provenance.

## Publication

The branch HEAD may advance only after a successful commit or explicit
validated merge/fast-forward. Every update uses an exact expected HEAD
(compare-and-swap), with explicit null support. The new revision's immutable
content and full ancestry must be durable before publishing the ref.
Readers must never observe a partially rewritten record. Concurrent stale
writers receive a conflict rather than overwriting the ref.

## Validation and replication

Validate UTF-8 and prohibited control/path characters; reserve names under
an explicit project-level uniqueness rule. Preserve branch UUID, name,
nullable HEAD, origin and fork UUIDs on export/import and restart. Refuse
conflicting imports instead of silently assigning new UUIDs or discarding
provenance. A branch's committed HEAD is never an editable working copy.

See [version-control semantics](version-control-semantics.md) and
[replication contract](uuidv7-revision-replication-v2.md).
