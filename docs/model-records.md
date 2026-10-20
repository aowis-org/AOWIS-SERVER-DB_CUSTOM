# Model-record integration (native prototype stage 3)

This is a **storage contract**, not a replacement for the canonical AOWIS
ModelCodec. `Repository::commitModel` accepts:

- A 16-byte entity UUID (`ModelChange::key`). All entity UUIDs in a branch
  must be unique, regardless of entity kind.
- A nonzero type tag (`ModelRecord::kind`). Numeric tags in tests are local
  examples; the stable mapping to the AOWIS entity catalog is not defined yet.
- The **unchanged** canonical bytes supplied by the AOWIS model codec
  (`ModelRecord::payload`). The native store does not interpret these bytes.
- Referenced entity UUIDs (`ModelRecord::references`). These are explicit
  *required existence* references, not yet the complete typed/reference
  relationships of every AOWIS Model entity.

## Encoding

The stored value (before the core store's `V` tag) is a deterministic binary
`AOMD` envelope: one-byte format version (`1`), big-endian 16-bit kind,
big-endian 32-bit reference count, sorted, de-duplicated 16-byte UUIDs, big-endian
32-bit canonical-payload length, and opaque payload bytes. No platform-sized
integers, struct padding, Qt streaming defaults or native byte order are used.
Changing the reference *set* changes the content hash, but reference
ordering and duplicates do not. The original codec bytes remain untouched. The envelope is an experimental
format and can be replaced before the first public release; there is no
pre-release migration mechanism.

## Atomic model commits

`commitModel` validates under the same process and interprocess lock used to
publish a new branch revision. References may point to records created in the
same batch; deleting a referenced entity is rejected unless every referencing
record is also deleted or updated in that batch. Rejected commits do not append
content or publish revisions. Concurrent repository handles cannot invalidate
this validation between checking and publishing.

**Limitations:**

- This checks only UUID existence, not expected entity kinds, hydraulic
  validity, cross-network boundaries, specific domain rules, or AOWIS
  authorization. Those require the shared model's reference catalog.
- The Stage 5 reverse-reference index is stored as a second immutable tree
  in each model revision. Indexed deletion checks touch incoming references
  to affected UUIDs rather than scanning every record. This index adds import
  time and physical storage overhead; it is not a full typed hydraulic catalog.
- Generic `Repository::commit` remains an unchecked low-level microbenchmark
  API. It rejects writes to indexed model branches. `commitModel` also rejects
  populated raw branches so unindexed content cannot contaminate the index.
- The payload/metadata pair is only one model record; there are no network
  membership indexes, object-group indexes, audit journals or merge rules yet.
- The current repository-wide `flock` serializes commits. The pack index is
  still rebuilt when opening the repository.

## Synthetic hydraulic-shaped benchmark

Build and run:

```sh
./compile_linux.sh
run_root="$(mktemp -d /tmp/aowis-db-custom-model.XXXXXXXX)"
./build-linux/aowis-native-model-benchmark "$run_root/repository" 100000 20 10
```

The target directory must not exist. This creates 100,001 junction records,
100,000 pipe records, and two required junction references per pipe. Sparse
revisions modify pipe payloads while retaining their endpoint references.
Unlike the original microbenchmark it measures `commitModel` validation and
record envelopes. It reports `fixture=synthetic-hydraulic-topology` and
`codec=placeholder-not-sql-v5` deliberately: the payload is unique synthetic
binary data, **not** serialized `HydraulicNodeJunction` or `HydraulicLinkPipe`
instances and **not** comparable to full SQL `saveNetworkHydraulic` operations.

## Native-owned canonical V5 codec

The native database compiles its own canonical `ModelCodec<T>` implementation
from `src/model_codec.cpp` and `src/model_validation.cpp`. All 68 persisted
model structures are checked against the pinned Model fields at configuration;
no second database checkout is needed. Real junction/pipe and full-network
operations use the native codec and native reference validator.
See [native serialization](native-serialization.md).

The synthetic workload above remains only a storage microbenchmark; use
`aowis-native-v5-model-benchmark` for typed hydraulic entities. Full network
save/load/edit behavior is tested in `aowis-native-hydraulic-network-tests`.

## Persistent model reference index (Stage 5)

Every indexed model revision publishes two content-addressed roots atomically:

1. Its existing UUID -> canonical model-record tree.
2. A reverse-reference UUID -> sorted referrer UUIDs tree.

The second tree reuses the native UUID radix structure and stores canonical
`INR1` index-value objects. Incoming references are maintained incrementally
for changes to old and new outgoing edges, including pipe rewiring, additions
and deletions. An entity cannot be deleted while surviving entities refer to
it; deletions of the referring entities within the same commit are supported.
If validation rejects a commit, nothing is appended or published. Referrer
lookup via `Repository::referencingModel()` returns sorted UUIDs and is
revision/branch isolated. Model branches cannot be altered through generic
unchecked writes.

The revision object was changed to the experimental `S` format containing both
roots and a model-indexed flag. **No migration for prior development formats is
provided.** Use freshly created repositories for new benchmark runs.

**Not yet implemented:** typed endpoint constraints (pipe links must reference
hydraulic node entity kinds), network membership/object-group indexes,
complete model reference catalog and full SQL-equivalent persistence rules.
