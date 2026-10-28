# Revision and branch replication contract

Full and incremental transfers preserve the logical objects defined by
[version-control semantics](version-control-semantics.md). A transfer is a
verifiable collection of **committed** snapshots, revision ancestry and
branch refs, not a copy of another editor's private mutable working copy.

## Transfer content

- Project UUIDv7 and required canonical format/version information.
- Branch UUIDv7, UTF-8 label, nullable committed HEAD, immutable
  `originBranchId` and exact `forkRevisionId`.
- Committed revision UUIDv7, ordered parent UUIDv7s, parent SHA-256 bindings,
  canonical immutable metadata, record SHA-256 and snapshot Merkle roots.
- Content-addressed immutable objects/subtrees needed to verify all included
  snapshots, including reference-index data and hydraulic model records.
- Explicit shared-base and completeness information for incremental bundles.

## Import correctness

Before publishing imported refs, validate canonical encoding, UUID
uniqueness, all required parent and object closure, SHA-256 integrity, branch
provenance, project ownership, and applicable domain constraints. Branch
HEAD conflicts cannot be resolved by overwriting unrelated local history.
Compare expected current HEADs and move refs atomically only after imported
objects are durable. Preserve UUIDs as-is across all transports.

An unborn branch is represented as a **branch with null HEAD**—not as a
fabricated empty revision. Private working copies are not committed history;
backing up an editor's uncommitted changes requires a different, explicitly
authorized session mechanism.

## UUIDv7 revision-bundle provenance wire formats (VC-02)

A revision bundle stores branch provenance in its **header metadata**, not
in revision records or the commit-parent DAG. Importers accept the original
`AOWREV21` (96-byte full header) and `AOWREV22` (144-byte incremental
header). These legacy headers have no provenance fields: importing one into
a new branch must **not guess** an origin or fork revision. Importing into an
existing branch must preserve the local recorded provenance.

The provenance-aware formats are `AOWREV23` (128-byte full header) and
`AOWREV24` (176-byte incremental header). Their initial 96/144 bytes are
identical to the corresponding legacy header. An additional 32 bytes contain
`originBranchId` (16-byte binary UUIDv7) followed by `forkRevisionId`
(16-byte binary UUIDv7), using 16 zero bytes for an absent optional value.
The exporter uses these formats when either piece of provenance exists.
Provenance-free root branches continue to export the legacy wire format.

The importer checks UUIDv7 validity, requires an origin whenever a fork
revision exists, and verifies that the fork revision is a genuine ancestor
of the incoming HEAD. On subsequent imports into an existing branch,
non-legacy provenance must match the immutable local record; neither
fast-forward nor idempotent replay may overwrite it. All validation occurs
before publishing the imported branch reference. Older importers do **not**
understand `AOWREV23/24`; upgrade recipients before transferring a branch
with provenance. This format change does not add any DAG vertices or edges.
