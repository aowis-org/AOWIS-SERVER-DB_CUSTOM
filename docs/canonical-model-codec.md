# Canonical MODEL serialization consumed by DVMDB

The canonical entity codec, model validation, field-merge logic, schema
manifest, value rules and enum generation are owned by **AOWIS-SERVER-MODEL**.
DVMDB links `AOWIS::Model` and never builds a competing serializer. See
`AOWIS-SERVER-MODEL/docs/model-serialization.md` for the authoritative
contract. DVMDB's typed adapters, reverse-reference indexes, storage envelopes
and revisions remain here.

In this pre-first-release codebase obsolete persisted representations need not
remain readable. New formats should be verified through strict current-format
round-trip, corruption, persistence and restart tests, not legacy fixtures.

Native-only DVMDB builds may still use `-DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF`.
Qt Core is required only for the typed model and hydraulic adapter layer.
