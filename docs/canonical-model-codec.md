# Canonical AOWIS model codec

The native database owns `aowis::native::ModelCodec<T>`, its hydraulic validator and field-aware merge helpers. Implementation is in `src/model_codec.cpp` and `src/model_validation.cpp`, with public headers under `include/aowis/native/`. Canonical model type definitions come from the pinned `AOWIS-SERVER-MODEL` submodule. Qt6 Core is required for the typed layer; the low-level native store can build without Qt.

The codec is **not a SQL adapter** and is not tied to HTTP or a physical storage format. It serializes the same canonical model payload regardless of whether the library is called directly or through the future REST service.

## Schema contract

`tools/check_model_codec.py` checks `src/model_codec.cpp` against `docs/model_codec_fields.json` and the pinned Model sources during CMake configuration. The manifest currently covers **68 persisted Model structures**. Adding, removing or changing fields requires deliberate codec and schema updates; silent omission should fail configuration.

The binary payload has an internal, explicit serialization version and deterministic field order. That internal version must **not** appear in public API namespaces, user-facing resource names or model identifiers. Public API names include `aowis::native::ModelCodec<T>` and `aowis::native::hydraulic`.

## Build

```bash
./git_submodule_init.sh
./compile_linux_tests.sh --both
./compile_linux.sh
```

The CMake option `AOWIS_NATIVE_WITH_MODEL_CODEC=OFF` allows building only the raw store. With Qt6 Core installed and the Model submodule available, the build provides `AOWIS::NativeModelCodec` and `AOWIS::NativeHydraulic` and runs model round-trip tests.

## Storage integration

A canonical payload is wrapped in a native `ModelRecord`, together with an entity kind and required UUID references. `commitModel()` publishes model and reverse-reference indexes atomically. `aowis/native/model_codec_adapter.h` contains helpers for typed junction/pipe storage and reading.

The complete hydraulic adapter stores all 18 top-level entity collections and validates cross-entity relationships; the generic envelope alone cannot enforce every hydraulic constraint. See [model records](model-records.md) and [hydraulic persistence](native-hydraulic-network.md).

## Transport independence

The planned REST service must call this codec and the existing model APIs. JSON may represent request/response metadata, but it must not become a separate authoritative model serialization format. Large model and synchronization payloads need binary transfer support. [REST design](rest-api-design.md).
