# Native database architecture

AOWIS uses a content-addressed C++ model database. Its core is a reusable library and the storage foundation of the planned multi-project HTTP service. This repository owns model serialization and uses `AOWIS-SERVER-MODEL` for canonical model types.

## Layers

1. **Model definitions and codec:** `AOWIS-SERVER-MODEL` (Git submodule) defines model types; this repository's `ModelCodec<T>` serializes them and checks a 68-structure schema contract.
2. **Domain persistence:** the hydraulic adapter validates `NetworkHydraulic`, persists network metadata, up to 256 ordered UUIDs per membership chunk, and one independently addressed record per top-level entity.
3. **Logical object store:** content-addressed immutable values, compressed 16-way UUID Merkle trie, persistent reverse-reference index, revision DAG and named branch refs.
4. **Physical store:** default append-only object pack with derived index; optional block-packed backend. Both represent the same logical objects and revisions, but their directories cannot be mixed.
5. **Service boundary (planned):** a Qt QHttpServer REST application over HTTP/TCP managing multiple project repositories. No server executable exists yet; native storage and model APIs are already usable directly from C++. The service will not change stored model identities.

## Logical identities and history

- Entity keys are 16-byte UUIDs. Object and revision identities are 32-byte SHA-256 hashes of canonical bytes.
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
└── refs/heads/             # branch -> immutable revision hash
```

Object records use an `AOBJ` header and typed canonical bytes. Index entries contain object hash, physical offset/length and a CRC32 checksum. The index is derived: on damage or absence it can be rebuilt from the authoritative pack. A normal reopen checks index structure but does **not** hash every object; payload reads and explicit verification check SHA-256. Corrupted complete pack records are rejected. Incomplete trailing records can be truncated during recovery.

`aowis-native-verify` performs physical verification of all stored objects and, by default, traverses every published branch's complete history to validate Merkle structure, typed native records and exact reverse-reference consistency. `--physical` limits verification to physical storage only. The same immutable logical verifier is used during bundle import and on-disk audits. Application-specific hydraulic semantic validation is outside this native storage check; see [repository verification](repository-verification.md). CRC32 is an accidental-corruption check for index entries, **not a signature or authentication mechanism**.

## Block-packed storage

`PhysicalStore::ExperimentalBlocks` groups small content-addressed objects into blocks to reduce physical overhead while preserving the same logical hashes and revision roots. Block-level verification, an independent derived object-location index, writable atomic batches, and offline compaction exist. It is still explicitly experimental. Use `PhysicalStore::Stage7Pack` for the default backend and `ExperimentalBlocks` only when deliberately selected. See [block storage](block-pack-experiment.md).

## Publication, durability and failure handling

- Repository operations coordinate using POSIX locks. Commits preflight references and optimistic expected-head checks under the same lock as branch publication.
- Newly encoded immutable objects are appended, flushed with `fsync`, and only then does the engine atomically publish a synced branch ref. An interrupted commit may leave unreachable objects but should not expose an incompletely flushed revision through a published ref on supported filesystems.
- Offline compaction retains **all** objects reachable from every branch's revision ancestry (including merge parents and both Merkle indexes). The replacement generation is synchronized before the active-generation pointer is published.
- Compaction requires exclusive lifetime access; do not compact a live service repository. Independent backup, restore verification, fault-injection and online maintenance are not yet complete.

## Concurrency and physical limits

Hydraulic read operations capture one immutable revision and resolve their entire network, membership or targeted entity query against it; multi-call clients can pass the same revision ID explicitly. Full-save/edit preparation also uses revision-pinned reads, with atomic expected-head publication protecting against concurrent writers. The code currently relies on repository-level locking rather than general multi-writer, fine-grained transactional concurrency. The block backend permits one open repository handle at a time. An HTTP service managing multiple projects must own/manage their repository handles centrally, rather than repeatedly opening competing handles. The database and server are Linux/POSIX-oriented and not yet production hardened.

## Replication independence

Portable `.aowbundle` files contain a complete reachable branch graph or a delta relative to a shared base revision. Import verifies object hashes, graph completeness and canonical Merkle trie structure (including path/UUID correspondence and branch-depth ordering) before creating or fast-forwarding a branch. Every imported model-indexed revision is checked against the exact reverse-reference mapping derived from its canonical model records, including historical revisions and locally borrowed incremental ancestors. Missing, extra and dangling references and noncanonical model reference lists are rejected before writes. Divergent branch heads are refused. The transport format is independent of both disk layouts; HTTP synchronization is planned on top of this existing core. See [synchronization](offline-repository-synchronization.md).

## API ownership

- `aowis::native::Repository` — low-level storage, history, compaction, bundles and generic merges.
- `aowis::native::ModelCodec<T>` — typed serialization.
- `aowis::native::hydraulic` — complete networks, indexed editing, typed validation and hydraulic-aware merging.
- Planned `aowis-server-db` REST handlers — thin adapters calling these libraries, **not an independent persistence system**.

The binary formats are not yet frozen for a public release. Stability and long-term format compatibility must be defined before that release. See the [roadmap](roadmap.md).
