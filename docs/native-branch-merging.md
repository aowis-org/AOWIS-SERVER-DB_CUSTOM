# Native three-way branch merging

`Repository::mergeBranches(target, source, options)` merges two named branches using their immutable revision ancestry and the UUID-keyed Merkle state. It is the **generic storage-level** merge, independent of hydraulic model semantics.

## Results

- `MergeStatus::UpToDate`: source already incorporated; no branch change.
- `MergeStatus::FastForward`: target is an ancestor of source; target head moves to source revision without creating a new revision.
- `MergeStatus::Merged`: divergent compatible changes produce one new two-parent revision (`previous target head`, `source head`), preserving both histories.
- `MergeStatus::Conflicts`: conflicting UUID updates/deletions are returned with hashes and reasons; no branch head is moved.

Both parents are traversed to determine ancestry. When there are multiple independent lowest common ancestors (criss-cross histories), deterministic in-memory recursive virtual merge bases are constructed; genuine conflicts in the virtual base are reported, not guessed. The virtual bases are not written as repository revisions. When `MergeOptions::validate` is provided, a model-indexed fast-forward candidate is validated before publishing the source tip as the target. A rejected validation returns `Conflicts` without writing or advancing either branch; a non-model-indexed fast-forward cannot satisfy a model validator. `UpToDate` does not invoke the validator because it does not publish a new snapshot. Optimistic head pins and unused-selection checks still apply in all cases.

## Generic merge rules

Changes to different UUIDs merge automatically, as do identical final changes to one UUID. Distinct changes to the same UUID conflict by default. Model-indexed merges also verify required UUID references in the complete proposed result and publish a matching reverse-reference index atomically. An optional `MergeOptions` callback can reconcile or explicitly select records; callbacks must obey the documented locking and purity requirements.

The source branch remains unchanged. If a divergent merge succeeds, publication flushes objects before atomically moving the target ref. Raw and model-indexed histories cannot be mixed indiscriminately.

## Domain-specific safety

Generic reference existence is **not** sufficient for hydraulic integrity. A pipe might refer to an existing UUID of the wrong kind, or a demand-point attachment may violate endpoint constraints. Use `aowis::native::hydraulic::mergeHydraulicBranches()` for hydraulic networks. That API can reconcile fields and coordinated membership, and validates the complete resulting network before publication. See [hydraulic merging](native-hydraulic-merging.md).

## Boundaries

No implicit overwrite policy resolves conflicts. Complex semantic conflicts, incompatible membership order and arbitrary multi-object topology may still require explicit review; recursive ancestry does not silently resolve conflicting edits. The implemented HTTP merge endpoint uses hydraulic-aware validation; see [service API](native-database-service.md) and [OpenAPI](dvmdb-http-api.yaml).
