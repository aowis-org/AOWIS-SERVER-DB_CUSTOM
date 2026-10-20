# Native storage architecture (experimental)

## Goal

Answer one technical question: is a persistent native tree a better **primary
model store** for AOWIS than a mutable SQL working model plus a separate Merkle
snapshot? Do not assume a native binary store is faster without measurements.

## Current on-disk layout

```text
repository/
├── objects.pack           # append-only versioned binary records
├── objects.idx            # initial generation, recoverable sidecar
├── objects.current        # optional atomic active-pack generation pointer
├── objects.<id>.pack      # immutable replacement after compaction
├── objects.<id>.idx       # replacement index for the same generation
├── maintenance.lock      # shared lifetime lock; exclusive for compaction
├── repo.lock              # advisory interprocess lock
└── refs/
    └── heads/
        ├── main           # SHA-256 of a revision record
        └── experiment     # SHA-256 of a revision record
```

Each packed record contains a fixed header (magic `AOBJ`, version 1,
SHA-256, big-endian 64-bit payload size) followed by its bytes. Object keys
are SHA-256 of the complete payload. The in-memory index maps each 32-byte hash to its byte offset and length.
A separate `objects.idx` holds an append-only sequence of 52-byte entries:
SHA-256 (32 bytes), big-endian payload offset (8), payload length (8), and
CRC32 over those first 48 bytes (4). The eight-byte header is `AOIDX01\0`.
These are *physical* offsets, not content identities, and are never included
in revision hashes. Opening normally checks sidecar structure, offsets, bounds,
and entry checksums **without re-hashing every content payload**. Each object
read still checks the payload against its SHA-256.

`objects.idx` is derived and expendable: missing, truncated or internally
corrupt indexes are rebuilt from the pack, verifying every recovered payload.
A valid unindexed pack suffix is hash-checked and indexed on open. Incomplete
trailing pack records are truncated; fully present records whose hash or
header fails verification are rejected. Use `aowis-native-verify` (or
`Repository::verifyPack()`) to verify **every** physical object explicitly.
Because fast reopening does not read all payloads, damage to an otherwise
unread object is discovered on access, index reconstruction or explicit
full verification, not necessarily at startup. CRC32 is for accidental
sidecar damage detection, not an authentication mechanism.

Payloads have a leading type tag:

- `V`: canonical bytes of the stored model value.
- `L`: tree leaf, 16-byte UUID and 32-byte value hash.
- `B`: radix branch, nibble depth, 16-byte *canonical* prefix (nibbles
  beginning at the branch depth are zero), and 16 child hashes.
- `S`: revision metadata containing both tree roots, a model-indexed flag,
  parent count and ordered parent hashes (zero or one
  parent currently written; parser permits two for later merge support).

A zero hash represents an empty root/child, never an actual stored object.
The trie uses **path compression**: leaves carry full UUIDs and branches only
appear where two keys actually diverge. This avoids 32 single-child nodes for
every UUID. Inserting/deleting an object rewrites the minimal affected path;
unchanged nodes and values retain their hashes and are reused.

## Commit publication and recovery

1. Acquire an exclusive `flock` for the whole repository.
2. Refresh the pack index to observe objects appended by other processes.
3. Encode changed model values, new tree nodes and a new revision record.
4. `fsync(objects.pack)` so immutable dependencies are durable.
5. Append checksummed entries to `objects.idx` (no extra per-commit fsync:
   this cache is reconstructible from durable pack bytes).
6. Write a temporary branch ref, sync it, rename over the branch ref,
   and sync the `refs/heads` directory.
7. Release the lock.

A crash before step 5 leaves unreferenced immutable objects, which can later
be garbage-collected. The previous branch head remains valid. A crash after
publication must not expose references to unflushed objects on filesystems
providing the expected `fsync`/rename guarantees. Pack and ref backup must
be captured as a consistent repository state; backup and recovery APIs are
future work.

## Revision comparison

Matching node hashes immediately prune entire subtrees. Matching internal
branch depths can be compared recursively. When compressed paths do not
align, only those subtrees are decoded to compare sorted UUID/hash entries.
This is a correct first implementation, not yet an optimal diff algorithm.

## Typed model-record prototype

`Repository::commitModel` adds a stable binary envelope around opaque shared
model-codec bytes, required UUID-reference validation during atomic commits,
and safe deletion checks. It is a deliberately incomplete model adapter, **not**
a full hydraulic persistence layer. See [model-records.md](model-records.md).

## Next steps

1. Establish matched native and SQL benchmarks with identical **real model
   serialization**, reference validation and transaction semantics.
2. Add durable secondary model indexes, then domain-aware integrity checks.
3. Benchmark the indexed single-pack format, then investigate pack segments,
   compression, compaction and reachability-based garbage collection.
4. Add a staged working transaction model, merge parents and three-way merge.
5. Develop concurrent readers, finer write-locking, crash-injection testing,
   backup/recovery tooling and verified distributed object exchange.

Neither SQL compatibility nor migration support for experimental disk formats
is a goal at this pre-release stage.

## Stage 7: offline retention-aware compaction

Explicit `Repository::compact()` marks the transitive closure of every
non-temporary branch-head ref (following revision parents, both indexed roots,
all Merkle nodes and payloads), then copies verified reachable records into
a newly synced pack generation and sidecar index. It atomically publishes
`objects.current`, fsyncs the directory, reopens its own file descriptors,
and unlinks the retired generation. The marker is the single publication
point: a crash before publication keeps the old generation authoritative,
while a crash afterward selects the new complete generation. Unused staging
files from an interrupted compaction can remain and must not be mistaken for
reachable history or removed without verifying the active pointer.

The compactor takes an exclusive lifetime `flock`, refusing to run if any
other repository instance is alive, even in another process. Regular handles
hold a shared lifetime lock and continue using the existing `repo.lock` for
individual operations. The lock protocol prevents writers from continuing
on file descriptors belonging to a retired pack. Parent-history retention is
mandatory: there is no history-expiration mechanism in this prototype.
