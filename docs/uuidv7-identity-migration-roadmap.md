# UUIDv7 object identity and SHA-256 integrity

**Canonical ID contract.** Project, branch, committed revision and working
copy are separate logical objects and each receives a stable UUIDv7. A
branch's editable name is not its identifier. A revision's content hash is
not its identifier either.

See [version-control semantics](version-control-semantics.md),
[identity record contract](uuidv7-identity-format-v2.md),
[branch ref contract](uuidv7-branch-ref-format.md), and
[replication](uuidv7-revision-replication-v2.md).

- Project creation generates project UUIDv7 and `main` branch UUIDv7 with a
  **null committed HEAD**. No revision ID exists until the first commit.
- Branch creation generates a new branch UUIDv7, records stable origin branch
  and exact immutable fork revision UUIDs, and does not itself create a
  revision.
- Each successful commit generates a fresh revision UUIDv7 and a separate
  canonical SHA-256 record digest binding all immutable content and ancestry.
- Every user/session works in an independently identified mutable working
  copy, not in a mutable HEAD revision.
- Native and REST APIs, branch records, graph pagination, import/export,
  verification, GC and GUI selection all carry logical UUIDs unchanged.
- Concurrency uses atomic expected-HEAD publication. A stale working copy
  must reconcile before it can commit; it must never overwrite another
  editor's committed snapshot.

UUIDv7 is for identity and practical uniqueness, not for discovering
revision ancestors. Ancestry is represented exclusively by parent UUIDs.
SHA-256 remains content integrity and Merkle deduplication, not object
identity. Every code path and test must uphold that separation.
