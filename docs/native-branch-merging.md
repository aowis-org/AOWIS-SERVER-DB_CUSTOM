# Native three-way branch merging

AOWIS native repositories can merge two branches using their immutable,
SHA-256-addressed revision DAG and object-level Merkle roots. This is a
**generic storage merge** for raw or native `ModelRecord` objects. The SQL
repository is not involved.

## API

`Repository::mergeBranches(target, source)` reads both heads under the
repository-wide lock and returns a `MergeResult`:

- `UpToDate`: both heads are equal, or the source revision is already in the
  target's full ancestry. No ref is changed.
- `FastForward`: the target is an ancestor of the source. The target ref moves
  directly to the source revision; no extra revision is created.
- `Merged`: a divergent, conflict-free merge creates a new revision with
  `parents = {previousTargetHead, sourceHead}`. Only the target branch moves.
  Even if the resulting content root is already equal to the target root, the
  merge revision records the second parent.
- `Conflicts`: `conflicts` describes the rejected UUIDs and includes their
  optional base, target and source content hashes. Neither ref moves, and no
  additional objects are written for ordinary conflicting edits.

The result also carries the chosen merge-base hash and the original heads.
A caller can inspect a conflict, create another branch or resolve it in AOWIS
before attempting another merge. There is deliberately **no implicit
`ours`/`theirs` overwrite policy**.

## Merge algorithm

1. Find common ancestors by traversing **both parents** of merge revisions.
   Fast-forward and already-incorporated source detection use that complete
   ancestry graph, not the first-parent history view.
2. Select the unique lowest common ancestor. Multiple independent lowest
   common ancestors (criss-cross merging) are refused with an explicit error,
   rather than choosing a potentially incorrect base.
3. Diff the base against the target and source, comparing immutable object
   hashes for each changed UUID. Unchanged entries are reused from the target.
4. A changed UUID merges automatically if only one branch changed it, or both
   branches converged to the same object hash. Divergent changes to the **same
   UUID**, including delete-versus-update, are conflicts by default. Optional
   explicit whole-record selections can choose the target/source record for a
   genuine conflict, subject to the same reference/semantic preflight.
5. For model-indexed revisions, preflight every selected model record's target
   UUID references against the merged state and check incoming references on
   deletions. A dangling reference introduced only by combining independently
   valid branches is a conflict. The model's reverse-reference Merkle index
   is recomputed incrementally and published atomically with its main tree.
6. Fsync new objects before atomically publishing the target branch ref.
   The merge revision preserves both parent histories for historical reads
   and reachability-preserving compaction.

## Scope and limitations

- The generic object merger remains object-level by default. AOWIS should
  use `aowis::native::hydraulic::mergeHydraulicBranches()` for canonical field-level reconciliation,
  explicit field choices and semantic network validation. Edits to the same
  membership chunk or network root can still conflict even when edits affect
  different hydraulic entities.
- Generic model-reference existence is enforced during a model merge, but
  application-specific constraints (such as hydraulic pipe-attachment
  positions, MSX selections, and semantic type rules) still need a
  higher-level hydraulic merge/validation workflow before treating a merged
  network as valid for simulation. Do not expose the generic merge directly
  as an unchecked hydraulic-model merge operation in AOWIS.
- Mixed populated raw and model-indexed histories cannot be merged. Two
  model-indexed branches created from the same empty raw ancestor are valid.
- Multiple independent merge bases need recursive merge-base synthesis, which
  is not implemented here. The operation fails without publishing a ref.
- Explicit hydraulic field and whole-record choices with revision-head pinning
  are available; coordinated multi-object topology/membership resolution,
  user identity,
  authorization, distributed push/pull and a transport protocol remain open.
- Source branches remain unchanged. Manual conflict resolution can be
  performed with explicit native commits; retrying merges requires checking
  the latest heads as normal.

The new functionality uses the existing two-parent native revision format;
no migration or disk-format change is required.
