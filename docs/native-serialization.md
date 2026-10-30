# Model serialization and DVMDB storage

AOWIS-SERVER-MODEL owns `aowis::model::ModelCodec<T>`, hydraulic reference
validation, field-aware model merging and numeric value rules. DVMDB consumes
MODEL as a dependency and does not build a competing codec. The Model target
checks its 68-struct field manifest and enum/value rules at configure time.

DVMDB owns `ModelRecord` envelopes, storage validation, reverse-reference
indexes, immutable revisions, durable working copies and HTTP transport.
`aowis/dvmdb/hydraulic_network_persistence.h` bridges typed hydraulic networks
to native records; its adapter aliases refer to the MODEL implementation and
do not introduce format compatibility or independent code paths.

The current encoding is deterministic binary, with strict enumerator/value
checks. Pre-release formats may change without backward readers; this source
refactoring must preserve the canonical invariants of the current format,
not maintain a reader for superseded pre-release formats.

The REST API uses the same canonical MODEL payload wrapped in JSON metadata.
