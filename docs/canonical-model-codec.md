# Canonical AOWIS model codec

The native database owns `aowis::dvmdb::ModelCodec<T>`, its hydraulic validator and field-aware merge helpers. Implementation is in `src/model_codec.cpp`, `src/model_validation.cpp` and `src/model_value_validation.cpp`, with public headers under `include/aowis/dvmdb/`. The codec never encodes or decodes an undeclared enum value or a non-finite number; the declared enumerators are generated from the Model headers at configure time. Physical value ranges are listed in [model value rules](model-value-rules.md). Canonical model type definitions come from the pinned `AOWIS-SERVER-MODEL` submodule. Qt6 Core is required for the typed layer; the low-level native store can build without Qt.

The codec is independent of the transport and physical object-store layout. Direct library calls and the planned REST service use the same canonical model payloads.

## Schema contract

`tools/check_model_codec.py` checks `src/model_codec.cpp` against `docs/model_codec_fields.json` and the pinned Model sources during CMake configuration. The manifest currently covers **68 persisted Model structures**. Adding, removing or changing fields requires deliberate codec and schema updates; silent omission should fail configuration.

The binary payload has an internal, explicit serialization version and deterministic field order. That internal version must **not** appear in public API namespaces, user-facing resource names or model identifiers. Public API names include `aowis::dvmdb::ModelCodec<T>` and `aowis::dvmdb::hydraulic`.

## Build

```bash
./git_submodule_init.sh
./compile_linux_tests.sh --both
./compile_linux.sh
```

The CMake option `AOWIS_DVMDB_WITH_MODEL_CODEC=OFF` allows building only the raw store. With Qt6 Core installed and the Model submodule available, the build provides `AOWIS::DvmdbModelCodec` and `AOWIS::DvmdbHydraulic` and runs model round-trip tests.

## Storage integration

A canonical payload is wrapped in a native `ModelRecord`, together with an entity kind and required UUID references. `commitModel()` publishes model and reverse-reference indexes atomically. `aowis/dvmdb/model_codec_adapter.h` contains helpers for typed junction/pipe storage and reading.

The complete hydraulic adapter stores all 18 top-level entity collections and validates cross-entity relationships; the generic envelope alone cannot enforce every hydraulic constraint. See [model records](model-records.md) and [hydraulic persistence](native-hydraulic-network.md).

## Transport independence

The planned REST service must call this codec and the existing model APIs. JSON may represent request/response metadata, but it must not become a separate authoritative model serialization format. Large model and synchronization payloads need binary transfer support. [REST design](rest-api-design.md).
