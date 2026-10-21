# Block-packed native physical storage

The native engine supports **two physical storage backends** under one logical content-addressed repository API:

- `PhysicalStore::Stage7Pack` — default append-only object pack with persistent, recoverable hash/offset index.
- `PhysicalStore::ExperimentalBlocks` — optional block-packed backend storing multiple small immutable objects per physical block, with a derived index and offline compaction.

The names above are existing C++ enum values; **physical backend selection does not select a different logical database format**. Both use the same UUID Merkle trees, SHA-256 object identities, model records, revision DAG and branch publication semantics. They use **different on-disk directory layouts**; never open one repository's files with the wrong backend.

## Physical block design

A block records `AOBK`, a big-endian object count and block-body size, plus SHA-256 of its body. Its body stores length-framed object bytes. A recoverable index maps each canonical object SHA-256 to block offset, slot offset and length. Large objects can occupy a block alone. The block format groups objects to reduce per-object overhead; it does not compress or delta-encode canonical object bytes.

Object reads check the requested object's exact SHA-256. Full verification checks complete blocks, contained objects and index consistency. Writable batches use a synced commit marker; recovery discards uncommitted tails, and complete published corruption is rejected. The backend enforces a **single open repository handle** and offline maintenance.

## Choosing and testing a backend

```cpp
// Default physical store:
aowis::native::Repository packRepository("/path/to/pack-repo");

// Opt-in block-backed store:
aowis::native::Repository blockRepository(
    "/path/to/block-repo", aowis::native::PhysicalStore::ExperimentalBlocks);
```

```bash
./compile_linux_tests.sh --both
./compile_linux.sh
run_root="$(mktemp -d /tmp/aowis-db-custom-block.XXXXXXXX)"
./build-linux/aowis-native-hydraulic-model-benchmark \
  "$run_root/pack" 100000 20 10
./build-linux/aowis-native-hydraulic-model-benchmark \
  "$run_root/blocks" 100000 20 10 block
```

Compare total pack **and index** size, verified read throughput, reopen time, import/edit latency, peak RSS, full verification and compaction. Never benchmark by reusing an existing repository target path. Synthetic and full hydraulic workloads exercise different levels of validation.

The auxiliary `aowis-native-pack-experiment` and `aowis-native-block-store-benchmark` commands remain available for offline physical-layout comparisons. An offline candidate directory created by an experiment is **not** a complete native repository with branch refs; do not treat it as a server data directory.

## Offline compaction

The complete `Repository` layer supports offline reachability compaction for both physical backends. All objects reachable through every branch head, all parent revisions, both Merkle roots and their payloads are retained. Unpublished/unreachable objects may be reclaimed. Compaction publishes a synced replacement generation atomically and requires **exclusive repository lifetime access**. Shut down clients/services and close handles first.

## Limitations

Block storage remains an explicit experimental backend, not the silently selected default. Neither backend currently promises online compaction, production-grade multi-writer transaction isolation, comprehensive power-loss fault-injection results, or live backup snapshots. Those are reliability milestones in [the roadmap](roadmap.md).
