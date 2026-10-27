# Native database architecture

AOWIS uses a content-addressed C++ model database. Its core is a reusable library and the storage foundation of the implemented multi-project HTTP service. This repository owns model serialization and uses `AOWIS-SERVER-MODEL` for canonical model types.

## Layers

1. **Model definitions and codec:** `AOWIS-SERVER-MODEL` (Git submodule) defines model types; this repository's `ModelCodec<T>` serializes them and checks a 68-structure schema contract.
2. **Domain persistence:** the hydraulic adapter validates `NetworkHydraulic`, persists network metadata, up to 256 ordered UUIDs per membership chunk, and one independently addressed record per top-level entity.
3. **Logical object store:** content-addressed immutable values, compressed 16-way UUID Merkle trie, persistent reverse-reference index, revision DAG and named branch refs.
4. **Physical store:** append-only content-addressed object pack with a derived/rebuildable index.
5. **Service boundary:** `aowis-server-dvmdb` uses the vendored cpp-httplib over HTTP/TCP (worker thread pool) with a durable UUID-based multi-project catalogue. Native storage/model APIs and the Qt-free project-service library remain directly callable from C++; the HTTP adapters never change stored model identities. Implemented HTTP routes cover catalogue, branches, immutable revisions and diffs, canonical hydraulic reads/edits/merges, and bounded full/incremental bundle transfers.

## Logical identities and history

The agreed meaning of *revision/commit*, *branch*, *per-branch HEAD* and the
GUI's complete multi-branch graph is recorded in
[version-control semantics](version-control-semantics.md). In particular, a
branch is a named ref, not a revision ancestry node. The repository stores
no global checkout HEAD or original branch-creation event. Existing
first-parent history APIs do not enumerate all merge-parent ancestry;
a separate full-DAG reader is already implemented in Repository,
ProjectService and HTTP. The [UUIDv7 identity migration
roadmap](uuidv7-identity-migration-roadmap.md) and [v2 format
contract](uuidv7-identity-format-v2.md) specify a future revision-ID
format change, not a change to revision DAG semantics. New project creation
now uses UUIDv7, while existing UUIDv4 projects remain readable; revision
and branch identity/storage still use legacy SHA-based references.

- **Current implementation:** new project IDs are UUIDv7; old project UUIDv4 IDs remain readable. Entity keys are 16-byte UUIDs; pack object and revision IDs remain 32-byte SHA-256 hashes of canonical bytes. **Proposed target:** branch and revision identities also become distinct UUIDv7 values, with immutable object hashes retained for integrity and Merkle deduplication.
- A persistent, path-compressed 16-way radix Merkle tree maps UUIDs to immutable object hashes. Unchanged values/subtrees are reused across revisions.
- Model-indexed revisions also publish a reverse-reference Merkle tree mapping a UUID to its incoming referencing UUIDs. Both roots belong to one revision.
- Each revision holds zero, one or two parent revision hashes. Branch names resolve to mutable refs pointing at immutable revision objects. Changes to one branch do not modify other branches.
- Generic model commits validate required UUID references atomically. The hydraulic layer adds typed reference and model-level validation. Generic merges do **not** replace hydraulic semantic validation.

## Default pack layout

```text
repository/
├── objects.pack            # initial append-only pack
├── objects.idx             # recoverable object-location sidecar
├── objects.current         # optional active compaction generation
├── objects.<id>.pack       # generated replacement pack
├── objects.<id>.idx        # its corresponding index
├── maintenance.lock        # repository lifetime/compaction locking
├── repo.lock               # repository operation locking
├── recovered-pack-tails/   # copies of discarded torn pack tails (if any)
├── tmp/                    # scratch space, mode 0700 (see below)
└── refs/heads/             # current: branch name -> immutable revision hash; planned: branch UUID + revision UUID mapping
```

**Scratch space.** Import/verification tables (bundle location index, visit marks, trie summaries, external reference sort) and HTTP bundle spools/exports are nameless files in `tmp/`, created with `O_TMPFILE`; on filesystems without it, `aowis-scratch-*` files are created and unlinked immediately, and any such leftover from a crash is removed on open. Scratch space is therefore on the pack's own filesystem: never in `TMPDIR`, which is often a RAM-backed tmpfs (Fedora, Arch, Debian 13), so "file-backed, evictable" really means disk. A symlinked or non-directory `tmp` is refused. C++ callers can use `Repository::createScratchFile()` together with the descriptor overloads of `importBranchBundle` / `export…BranchBundle`.

Object records use an `AOBJ` header and typed canonical bytes. Index entries contain object hash, physical offset/length and a CRC32 checksum. The index is derived: on damage or absence it can be rebuilt from the authoritative pack. A normal reopen checks index structure but does **not** hash every object; payload reads and explicit verification check SHA-256. Corrupted complete pack records are rejected. A torn pack tail is repaired on open: if the first invalid record (short, zero-filled, or a full-length body whose SHA-256 does not match) is followed by no valid record up to EOF, and every published branch head is still present before it, the region is unpublished by construction. It is copied to `recovered-pack-tails/` (fsynced), then truncated, and `Repository::recoveredPackTailBytes()` reports the amount. An invalid record followed by a valid one, or damage reaching a published head, is mid-pack corruption and fails closed without modifying the pack. A torn region whose payload happens to contain a complete, valid embedded pack record is also refused (fail-closed direction).

`aowis-dvmdb-verify` performs physical verification of all stored objects and, by default, traverses every published branch's complete history to validate Merkle structure, typed native records and exact reverse-reference consistency. `--physical` limits verification to physical storage only. The same immutable logical verifier is used during bundle import and on-disk audits. Application-specific hydraulic semantic validation is outside this native storage check; see [repository verification](repository-verification.md). CRC32 is an accidental-corruption check for index entries, **not a signature or authentication mechanism**.

## Physical storage

Every repository uses the same append-only content pack. Object hashes identify
immutable logical content, while the physical index maps hashes to pack offsets.
An index suffix lost during a crash is reconstructed by scanning the verified
pack tail. Compaction writes and verifies a replacement pack before atomically
publishing its generation pointer. Existing repository handles become unusable
if that pointer switches and the operation subsequently fails; reopen them
before retrying writes.

Directories with the obsolete pre-release `storage.format` block marker are
rejected explicitly. Use an older build to export such a repository to a
portable bundle before upgrading, then import into a new pack repository.

## Publication, durability and failure handling

- Repository operations coordinate using POSIX locks. Commits preflight references and optimistic expected-head checks under the same lock as branch publication.
- Newly encoded immutable objects are appended, flushed with `fsync`, and only then does the engine atomically publish a synced branch ref. An interrupted commit may leave unreachable objects but should not expose an incompletely flushed revision through a published ref on supported filesystems.
- Offline compaction retains **all** objects reachable from every branch's revision ancestry (including merge parents and both Merkle indexes). The replacement generation is synchronized before the active-generation pointer is published. If compaction fails after switching this pointer, the in-memory repository handle is invalidated and must be reopened before any further commit; continuing to write the old pack is prohibited.
- Compaction requires exclusive lifetime access; do not compact a live service repository. Deterministic process-crash and simulated write-failure recovery tests cover the pack and offline backup/restore. An operator-run QEMU/KVM drill passed four guest-cache-loss cuts each on ext4 and XFS; physical-controller power-loss qualification and online maintenance remain outstanding. Coordinated offline backup/restore is implemented.

## Concurrency and physical limits

Hydraulic read operations capture one immutable revision and resolve their entire network, membership or targeted entity query against it; multi-call clients can pass the same revision ID explicitly. Full-save/edit preparation also uses revision-pinned reads, with atomic expected-head publication protecting against concurrent writers. The code currently relies on repository-level locking rather than general multi-writer, fine-grained transactional concurrency. Pack repositories permit coordinated concurrent handles; compaction requires exclusive repository lifetime ownership. The HTTP service manages repository handles centrally, with one operation lock and a bounded operation queue per project; projects do not block each other. The database and server are Linux/POSIX-oriented and not yet production hardened.

## Replication independence

Portable `.aowbundle` files contain a complete reachable branch graph or a delta relative to a shared base revision. Import verifies object hashes, graph completeness and canonical Merkle trie structure (including path/UUID correspondence and branch-depth ordering) before creating or fast-forwarding a branch. Every imported model-indexed revision is checked against the exact reverse-reference mapping derived from its canonical model records, including historical revisions and locally borrowed incremental ancestors. Missing, extra and dangling references and noncanonical model reference lists are rejected before writes. Divergent branch heads are refused. The transport format is independent of both disk layouts; Bounded HTTP bundle synchronization and offline CLI transfer both use this core. See [synchronization](offline-repository-synchronization.md).

## API ownership

- `aowis::dvmdb::Repository` — low-level storage, history, compaction, bundles and generic merges.
- `aowis::dvmdb::ModelCodec<T>` — typed serialization.
- `aowis::dvmdb::hydraulic` — complete networks, indexed editing, typed validation and hydraulic-aware merging.
- Implemented `aowis-server-dvmdb` HTTP route handlers — thin REST adapters calling `AOWIS::DvmdbProjectService` and the native database, **not an independent persistence system**. The service also exposes hydraulic edits/merges and bounded full/incremental synchronization.

The binary formats are not yet frozen for a public release. Stability and long-term format compatibility must be defined before that release. See the [format compatibility policy](format-compatibility.md) and [roadmap](roadmap.md).
