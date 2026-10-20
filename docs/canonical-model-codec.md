# Canonical AOWIS model serialization (native-owned)

`AOWIS-SERVER-DB_CUSTOM` owns its canonical model codec, full hydraulic
reference validator and field-wise merge helpers. They live in `src/model_codec.cpp`,
`src/model_validation.cpp`, and public headers under `include/aowis/native/`.
They use only `AOWIS-SERVER-MODEL` and Qt Core, not SQLite, PostgreSQL, or
another AOWIS database repository. `AOWIS::NativeModelCodec` is the shared
CMake target for model-facing native code.

Serialization uses an explicit schema version in its binary envelope. Renaming
the C++ API does not change field encoding, byte order, schema version or
the encoded payloads. Its public namespace is
`aowis::native`; there is no `aowis::db` API dependency. The 68-structure
manifest in `docs/model_codec_fields.json` and the local schema checker in
`tools/check_model_codec.py` make field omissions fail configuration.
The Model submodule remains a separate source of AOWIS model definitions and
requires no modifications to its gitlink for this change.

## Build

```sh
./git_submodule_init.sh
./compile_linux_tests.sh --both
./compile_linux.sh
```

Qt6 Core is required for the typed model persistence adapter. If Qt6 isn't available,
the standalone byte store still builds; CMake reports that the typed adapter
was skipped. To deliberately build only the byte store, configure with
`-DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF`. No SQL checkout or custom SQL path is
required. The build has no configurable SQL source-tree dependency.

## Benchmark with a unique temporary repository

Each run needs a fresh repository directory. Use `mktemp -d` as a parent and
place the benchmark repository inside it, so the benchmark's target path has
never existed. For example:

```sh
run_root="$(mktemp -d /tmp/aowis-db-custom-codec.XXXXXXXX)"
./build-linux/aowis-native-hydraulic-model-benchmark \
  "$run_root/repository" 100000 20 10
```

The parent directory and results are intentionally retained for inspection.
Remove them only after collecting the results.

The hydraulic-model benchmark uses real `HydraulicNodeJunction` and `HydraulicLinkPipe`
structures, their canonical ModelCodec encoded bytes, canonical UUIDs and
pipe endpoint dependencies. It reports `encode_prepare_ms` separately from
`import_ms`. `scan_decode_ms` **decodes** every record using the native-owned codec.

The junction/pipe benchmark exercises canonical typed encoding and storage,
not complete network aggregates. The native API also supports full-network
save/load and incremental entity edits; these operations have their own tests.
Revision history, branching, and validated hydraulic merge resolution already
exist. Authorization, remote replication, and the HTTP server remain future work.

## Adapter contract

`aowis/native/model_codec_adapter.h` is an optional bridge. `changeFor` maps
an actual junction or pipe to a native ModelChange. `readJunction` and
`readPipe` decode through the **native model codec**, validate the model UUID
against its tree key, and for pipes verify that their declared required UUID
references match their decoded endpoints.

For now the Stage 3 experimental numeric type tags are reused (junction 1,
pipe 4). These are not claimed to constitute a stable global entity catalog.
The native repository's basic existence-only reference checking is unchanged.
