# Native model records and reference indexes

The generic model-record layer stores a canonical model payload without interpreting domain-specific fields. It is used by the hydraulic persistence adapter and other potential AOWIS model domains.

`ModelRecord` contains a nonzero numeric kind, a canonical payload and required referenced entity UUIDs; `ModelChange` pairs the record (or deletion) with a 16-byte UUID key. The model codec owns the payload bytes; the native store owns immutable storage, SHA-256 objects and revision publication.

## Deterministic envelope

`encodeModelRecord()` uses the `AOMD` envelope: version byte, big-endian kind, sorted/deduplicated required references, and length-prefixed canonical payload. Canonical reference ordering avoids hash differences caused only by caller order. Numeric kind IDs are current native storage details, not a stable external REST entity enumeration.

## Atomic commits

`Repository::commitModel(branch, changes, expectedHead)` validates that required references exist in the resulting snapshot, including entities created in the same batch, and rejects deletion of still-referenced entities. It verifies the expected branch head under the repository lock before publication. On success it atomically publishes both the primary UUID-to-model index and the reverse-reference UUID-to-referrers index in a new immutable revision.

`Repository::getModel()`, `getModelAtRevision()`, `referencingModel()` and `referencingModelAtRevision()` provide direct and historical reads. Generic `Repository::commit()` is a lower-level raw-object operation and must not modify populated model-indexed branches.

## Scope of validation

Normal model commits enforce **UUID existence and reference-index integrity**. Portable model-bundle imports recompute all incoming edges from the canonical model records at each reachable historical revision, verify targets exist, and require an exact match with the separately stored reverse index. These validations run before any imported object is written. A separate comprehensive checker for repositories already on disk remains planned. It does not know whether a reference points to the correct type of hydraulic node or whether a demand-point assignment matches a pipe endpoint. Use `aowis::native::hydraulic` for real hydraulic network editing, validation and merging. Domain-specific relationships and network membership are described in [hydraulic persistence](native-hydraulic-network.md).

## Synthetic performance tool

`aowis-native-model-benchmark` uses synthetic hydraulic-shaped payloads to isolate model-envelope, reference-index and Merkle costs. It does **not** exercise full ModelCodec or complete hydraulic validation. For real typed junction/pipe data use `aowis-native-hydraulic-model-benchmark`; for network-wide editing use `aowis-native-hydraulic-edit-benchmark`.

The native record format is not yet frozen for a public release.
