# Immutable revision record and integrity contract

The authoritative semantics are specified in
[version-control semantics](version-control-semantics.md). This document
defines the **logical canonical revision record**, independently of its
physical pack encoding and the implementation language.

## Committed revision

A committed revision has one stable UUIDv7. Its canonical integrity record
binds, at minimum:

| Field | Rule |
|---|---|
| `revisionId` | Fresh UUIDv7, never reassigned |
| `projectId` | Owning project UUIDv7 |
| `parents` | Ordered array of zero, one or two committed revision UUIDv7s |
| `parentRecordHashes` | SHA-256 bindings to the ordered immutable parent records |
| `snapshotRoot` | Merkle SHA-256 root of the complete model snapshot |
| `referenceIndexRoot` | Included when the model uses reverse references |
| `metadata` | Canonical immutable commit author/message/time and required format/version fields |
| `recordHash` | SHA-256 of the versioned, canonical bytes of this entire immutable revision record |

Canonical encoding must fix field order, endian/length rules, optional-field
representation and hash domain separation; parsing must reject malformed,
noncanonical or inconsistent encodings. A revision record is **immutable
from publication onward**. Renaming a branch or updating metadata outside
the revision cannot change `recordHash`.

The Merkle roots bind all model/entity records by recursively hashed child
references, allowing immutable object and subtree deduplication. SHA-256
is not a UUIDv7 ID, and identical model content in different commits does
not collapse separate revision identities.

## Unborn branches and working copies

A newly created project has an unborn `main` branch and no committed record.
There is **no empty root revision record** created just to represent the
project's existence. A private working copy holds mutable model state,
identity and an expected committed base UUID/null. It is not stored as an
immutable revision record or exposed through committed-history reads.

## Verification and publication

Check revision UUID uniqueness, project ownership, ordered parent closure,
parent-record bindings, Merkle root structure and model reference integrity.
Commit publication must durably write all immutable objects and the revision
record before atomically changing a branch HEAD under an exact expected-HEAD
check. Failures must not expose partially published histories.

Native and REST must expose equivalent ID/hash fields, including explicit
null branch HEADs. Import/export must preserve immutable UUIDs, ancestry
and hashes without remapping them.
