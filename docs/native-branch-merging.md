# Native three-way branch merging

`Repository::mergeBranches(target, source, options)` merges two named branches using their immutable revision ancestry and the UUID-keyed Merkle state. It is the **generic storage-level** merge, independent of hydraulic model semantics.

## Results

- `MergeStatus::UpToDate`: source already incorporated; no branch change.
- `MergeStatus::FastForward`: target is an ancestor of source; target head moves to source revision without creating a new revision.
- `MergeStatus::Merged`: divergent compatible changes produce one new two-parent revision (`previous target head`, `source head`), preserving both histories.
- `MergeStatus::Conflicts`: conflicting UUID updates/deletions are returned with hashes and reasons; no branch head is moved.

Both parents are traversed to determine ancestry. A unique lowest common ancestor is required; ambiguous criss-cross merge bases are rejected rather than guessed.

## Generic merge rules

Changes to different UUIDs merge automatically, as do identical final changes to one UUID. Distinct changes to the same UUID conflict by default. Model-indexed merges also verify required UUID references in the complete proposed result and publish a matching reverse-reference index atomically. An optional `MergeOptions` callback can reconcile or explicitly select records; callbacks must obey the documented locking and purity requirements.

The source branch remains unchanged. If a divergent merge succeeds, publication flushes objects before atomically moving the target ref. Raw and model-indexed histories cannot be mixed indiscriminately.

## Domain-specific safety

Generic reference existence is **not** sufficient for hydraulic integrity. A pipe might refer to an existing UUID of the wrong kind, or a demand-point attachment may violate endpoint constraints. Use `aowis::native::hydraulic::mergeHydraulicBranches()` for hydraulic networks. That API can reconcile fields and coordinated membership, and validates the complete resulting network before publication. See [hydraulic merging](native-hydraulic-merging.md).

## Boundaries

No implicit overwrite policy resolves conflicts. Complicated criss-cross ancestry, incompatible membership order and arbitrary multi-object topology still require explicit review or further algorithms. No HTTP service exists yet; [REST merge resources are planned](rest-api-design.md).
