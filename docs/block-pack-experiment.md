# Block-packed physical storage experiment (based on Stage 7)

The initial Stage-8 conversion described here is an **offline, non-authoritative experiment**. It reads a completed
Stage 7 content pack and generates `candidate.pack` and `candidate.idx` in a
**new directory**. It does not replace the repository's active pack, change
branch references, or alter model, tree, or revision hashes. Do not deploy the
candidate files as an AOWIS database.

The experiment answers a specific question: can we reduce the cost of one
45-byte physical header and one 52-byte index entry per tiny immutable object
without losing object identity or independent pack verification?

## Physical candidate

Each candidate block is: `AOBK` + big-endian `uint32 object_count` +
`uint32 block_body_size` + `SHA-256(block_body)` + its body. The body contains
repeated `uint32 item_size` + item bytes. A separate derived index has an
8-byte magic followed by 48-byte entries: content SHA-256 (32), block offset
(8), slot offset (4), and item length (4). Objects larger than the target
block size occupy a block by themselves.

The format deliberately **does not compress object bytes** or use delta
chains, so the size comparison isolates object-packing overhead. The block
hash authenticates all its contents and allows the index to be rebuilt by
walking the pack; the original SHA-256 of each object's exact canonical
bytes remains its identity. The verifier checks block hashes, object hashes,
record boundaries and the index. The result is separate from our authoritative
repository's Stage 7 pack and index.

## Repeatable comparison

After `./compile_linux.sh`, run the usual native V5 benchmark with a random
path and then run the experiment against the resulting repository:

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-block.XXXXXXXX)"
./build-linux/aowis-native-v5-model-benchmark \
  "$run_root/repository" 100000 20 10
./build-linux/aowis-native-pack-experiment \
  "$run_root/repository" "$run_root/candidate" 65536
```

The candidate directory must not already exist. The original benchmark path
must not be reused from an earlier run. A 64-KiB target is a starting point,
not a tuned optimal size. Run again with different block sizes and compare
**combined** pack+index disk bytes, build cost, verified random-read latency,
full verification time, and peak memory. Sample reads are warm-cache single
object lookups with per-object SHA-256 verification; the candidate does **not**
check an entire block digest on every individual lookup.

The candidate experiment does not yet test concurrent edits, publication,
rollback, power-loss safety, distributed replication, or sustained write
throughput. Those must pass before any block layout could be adopted as the
real storage format. The Stage 7 engine remains authoritative and unchanged.

## Preliminary local result (synthetic network, not a production comparison)

For a saved Stage 7 100,000-pipe synthetic fixture with 620,803 packed objects:

| Measurement | Stage 7 | Experimental blocks |
| --- | ---: | ---: |
| Pack bytes | 99,096,173 | 73,692,794 |
| Index bytes | 32,281,764 | 29,798,552 |
| Combined bytes | 131,377,937 | 103,491,346 |

That's **27,886,591 fewer bytes (21.2%)** in this one local run. The
experimental candidate took about 1.38 seconds to generate and about
0.50 seconds to verify. For 10,000 sampled reads, the same run took
12.5 ms from the Stage 7 pack and 15.5 ms from the block pack. The
read comparison needs repeated order-controlled trials and cold-cache
measurements before drawing conclusions. The candidate does not include
production recovery and write costs, so the space win alone is insufficient
to justify replacing Stage 7.

## Read-path optimization and controlled comparison (Stage 9 experiment)

The initial offline block candidate performed two `pread` operations for each
random object: one for its four-byte record framing, and another for its
payload. That was avoidable because the derived index already stores the
object's exact length and block-relative offset. The optimized path reads only
the payload and validates the *full* SHA-256 hash against the requested object
identity. Full block verification and index reconstruction continue checking
record framing, block hashes, and each indexed object's full SHA-256.

The experiment now measures **six alternating-order paired trials**, following
one warmup pair, and reports the median Stage 7 and block-candidate read time.
These are equal prelocated-entry, warm-cache, SHA-verified reads; they do **not**
include a persistent index load, hash-to-location lookup, cold-cache access,
concurrent writes, crash recovery, or commit publication. Those still require
separate evaluation before changing the authoritative storage format.

This optimization changes no pack data, index data, canonical model bytes or
revision hashes. Stage 7 remains authoritative and the candidate still writes
only to a separate experimental directory.

## Stage 10 — Writable block-store prototype (still not authoritative)

This stage adds `BlockStorePrototype`: it can **activate an existing verified
candidate** and then append transactional batches of immutable objects. The
original Stage 7 `Repository` continues using its unchanged pack, index and
branch-head format. This is an isolated physical-store experiment, not an
AOWIS repository conversion or a SQL/native comparison.

After `aowis-native-pack-experiment` creates a candidate, run:

```bash
./build-linux/aowis-native-block-store-benchmark "$run_root/candidate" 100
```

The first open verifies every existing block and object before writing a
72-byte `candidate.current` committed-length marker. Later opens batch-read the index into an
in-memory SHA-256-to-location map from `candidate.idx`; they verify returned
object payloads when read. `full_verify_ms` performs independent block and
object checking. Do **not** compare the one-time activation cost to steady-state
reopening, or compare already-located reads to indexed reads.

Batch commit protocol (single writable handle enforced by an exclusive flock):

1. Compute object hashes and deduplicate already-present or repeated objects.
2. Append complete blocks and index entries, then `fsync` both files.
3. Write and sync a pending marker containing committed lengths, object count
   and generation, protected by a SHA-256 checksum.
4. Atomically rename the marker to `candidate.current` and sync its directory.

Only the marker publishes the new objects. On reopening, trailing data beyond
its committed lengths is truncated; a missing/truncated derived index is
reconstructed from the committed, independently checked pack. Published pack
corruption is **not** treated as an uncommitted tail. A failed in-process commit
requires closing and reopening that handle. Full verification remains explicit.

**Still missing before adoption:** coordinated branch-head publication,
revision/working-model integration, comprehensive fault-injection and power-loss
validation, realistic concurrent-reader behavior, full indexed-vs-Stage-7
cold-cache comparisons, and long-term multi-pack compaction. The prototype
locks out a second handle rather than supplying multi-writer MVCC. The sidecar
is not independently authenticated in ordinary reopening; payload SHA-256
protects reads, and the independent full verifier detects index inconsistencies.
Existing candidate format v1 objects are kept unchanged in this experiment.

## Stage 11 — Experimental authoritative branch publication

An explicit `PhysicalStore::ExperimentalBlocks` mode now runs the same
`Repository` revision, copy-on-write UUID trees, typed model validation and
branch-head logic over `BlockStorePrototype`. **Stage-7 remains the default**;
opening a block repository with the Stage-7 constructor (or vice versa) is
rejected rather than writing to the wrong physical pack. This is still an
experimental single-handle backend. As of Stage 16, explicit offline
retention compaction is supported; remote replication is not. The canonical
model/revision bytes are unchanged.

Publication sequence for the block-backed mode:

1. Build the Merkle root, reverse-reference root and revision object in memory.
   Newly created objects are staged by SHA-256; no public branch ref moves.
2. `putBatch` durably writes immutable blocks, the derived index, and the
   committed-length marker (`candidate.current`). If publication fails, the
   handle fails closed until reopened.
3. Only after (2) succeeds, `publishRef` writes/fsyncs a temporary ref,
   atomically renames it and fsyncs `refs/heads`. A crash in between may retain
   unreachable objects, **never a branch ref to uncommitted block bytes**.

The initial empty store and `storage.format` marker are established when the
explicit block mode first opens a fresh directory. No prior prototype-format
migrations are provided; an existing offline converted candidate is **not** a
branch-enabled repository. Tests ensure hashes and results agree between
Stage-7 and block modes and that an intentionally obstructed ref publication
preserves the old branch head across reopening.

To compare canonical V5 payloads on a development machine:

```bash
run_root="$(mktemp -d /tmp/aowis-db-custom-publish.XXXXXXXX)"
./build-linux/aowis-native-v5-model-benchmark \
  "$run_root/stage7" 100000 20 10
./build-linux/aowis-native-v5-model-benchmark \
  "$run_root/blocks" 100000 20 10 block
```

These are comparable **native-engine physical-store** runs. They do not yet
constitute a complete optimized SQL-vs-native comparison. Compare both timing
and peak memory, disk bytes, reopens and cold-cache reads. This experimental
block mode holds an exclusive file lock for its lifetime and does not permit
multiple open repository handles; do not use for live server deployments.

## Stage 13 — Reduce staging allocations (same format)

Large commits already use a `SHA-256 -> Bytes` staging map, so internal batches
are unique by hash. The block writer now accepts that invariant instead of
constructing a second deduplication hash set and result vector. The **public**
`putBatch` API still hashes inputs, returns a result for every input, and
removes duplicates before handing values to the writer. Newly encoded tree and
revision objects can be moved into staging rather than copied. Reusable block
buffers reduce allocation churn.

No pack, index, marker, tree, or revision encoding was changed. Measure with
identical canonical V5 workloads and alternate benchmark order to limit cache
and background-load bias. The synthetic import comparison is useful for
isolating physical-store behavior, but it is not a SQL-versus-native result.

## Stage 15 — Bounded staging for large commits

The block-backed repository keeps unpublished object bytes in a staging map so
that tree construction can read them before the branch head is published. Large
initial imports previously retained the entire object set until finalization.

Stage 15 durably flushes object batches after approximately 8 MiB of staged
payload bytes. The commit's branch head **never** advances during these early
flushes; it moves only after all referenced objects and the final revision
manifest are durable. If validation fails before tree construction, no pack
objects are written. If construction or ref publication fails after early
flushes, unreachable objects may remain for later compaction, but the previous
revision stays authoritative. This is the same unreachable-content tradeoff
already allowed by append-only storage.

Different flush boundaries can change physical block counts and pack bytes.
Canonical hashes, Merkle roots and revision IDs must remain identical.

Linux benchmarks now report `peak_rss_kib` directly using `getrusage`, with no
external timing utility or Python wrapper. It measures the entire benchmark
process, including codec preparation, indexing, scanning and reopening, not just
the block writer's allocation peak. Compare matched workloads and alternate
backend order.

## Stage 16 — Generation-based compaction of block repositories

`Repository::compact()` now supports the explicit block backend as well as
Stage 7. It traverses all branch heads and full parent ancestry, including
the main UUID tree, the incoming-reference tree and both trees' payloads.
It writes reachable objects in bounded batches to a separate block-packed
generation, verifies the complete replacement, syncs its directory, and
atomically publishes an active-generation pointer. Only then is the former
physical generation removed. The logical revision hashes are unchanged.

Normal reads and writes resolve `blocks/candidate.active` once when opening
the store; without a pointer, original block repositories remain readable.
An invalid pointer fails closed. Index loss can still be repaired from the
committed block pack. Compaction refuses to run without exclusive lifetime
access, and stale/unpublished objects can be reclaimed on the next pass.

Unpublished generation directories may remain after interrupted maintenance
and should not be mistaken for live data. Do not manually remove the active
generation. There is no automated cleanup of all stale generation dirs yet.
This remains experimental single-handle storage; long-running server
concurrency, exhaustive power-loss simulation and online GC are not claimed.
