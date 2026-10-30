# UUIDv7 object identity

The canonical AOWIS persistence identity contract uses **UUIDv7** for
repository, project, branch, committed revision and working-copy identities.
MODEL entity identities follow the MODEL schema and validation rules. SHA-256
verifies immutable revision metadata and Merkle-rooted content; it does not
replace a logical UUIDv7 or identify a mutable branch ref.

A branch ref contains nullable committed HEAD and optional immutable fork
provenance. A project begins with an unborn `main`. The first successful
Commit creates a zero-parent revision; creating a branch at an existing
revision creates no revision. Branch identity is stable across independent
repository clones, while each repository retains its own mutable HEAD.

See [version-control semantics](version-control-semantics.md),
[revision integrity](uuidv7-identity-format-v2.md) and
[replication](uuidv7-revision-replication-v2.md). Before the first release,
old experimental layouts require no backward-compatibility support.
