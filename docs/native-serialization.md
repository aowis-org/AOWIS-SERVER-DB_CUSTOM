# Native-owned model serialization

The AOWIS native database stores canonical serialized model bytes produced by `aowis::dvmdb::ModelCodec<T>`. The serializer, typed hydraulic reference validator and field-wise merge helpers live in this repository. Canonical type definitions come from the pinned `AOWIS-SERVER-MODEL` submodule and Qt6 Core. The low-level native object store works without Qt.

The model payload format contains internal binary-version markers, explicit field ordering and deterministic encoding. These markers do not belong in public namespaces or REST paths. Changing transport does not change canonical model bytes, SHA-256 object identities or revision IDs.

CMake runs `tools/check_model_codec.py` against `docs/model_codec_fields.json` and the actual Model definitions. It currently checks **68 persisted structures**; field omissions must fail schema validation rather than silently lose model data.

Key interfaces: `aowis/dvmdb/model_codec.h`, `aowis/dvmdb/model_validation.h`, `aowis/dvmdb/model_field_merge.h` and `aowis/dvmdb/hydraulic_network_persistence.h`.

A `ModelRecord` wraps payload bytes and required UUID references. The storage engine validates generic existence/deletion safety and publishes a reverse-reference index. The hydraulic adapter performs the domain-specific typed and whole-network validation. See [canonical codec](canonical-model-codec.md), [model record format](model-records.md) and [hydraulic persistence](native-hydraulic-network.md).

The implemented HTTP REST service uses this same codec and native library; canonical persistence bytes remain independent of transport.
