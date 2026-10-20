# Native-owned model serialization

The native database is the authoritative owner of its Model V5 serializer,
reference validation and typed field-merge helpers. They are built from
`src/model_codec.cpp` and `src/model_validation.cpp` with Qt Core and the pinned
AOWIS Model definitions. **No SQL database repository is required.**

This stage migrates the exact field-wise encoding and decoding logic previously
used by the standalone native adapter. It does not introduce a new on-disk
format or alter the serialized type envelope, magic, stream version, or field
order. Existing native revisions retain their content identities.

The schema check at CMake configure compares every persisted field in all 68
reachable Model structures against `docs/model_codec_fields.json` and checks
the corresponding writer and reader. Changes to Model types must update the
native serializer deliberately; otherwise configuration fails.

Public interfaces are `aowis/native/model_codec.h`,
`aowis/native/model_validation.h`, and `aowis/native/model_field_merge.h`.
Their implementations live in this repository. The Model submodule contains
only the types and other model utilities; it is not modified by this patch.

The native byte store can be built independently of Qt. To access typed
AOWIS model serialization and full hydraulic network persistence, Qt Core
must be installed and `AOWIS_NATIVE_WITH_MODEL_CODEC` must be enabled.

Runtime tests validate canonical round trips, full hydraulic model save/load,
incremental changes, and revisions through both native physical backends.
