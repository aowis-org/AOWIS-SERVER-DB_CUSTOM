# DVMDB project conventions

DVMDB is the AOWIS native database. The C++ namespace is `aowis::dvmdb`,
the HTTP API prefix is `/dvmdb/v1`, and native headers live under
`include/aowis/dvmdb/`. The standalone HTTP executable is
`aowis-server-dvmdb` and the patch ZIP prefix is `aowis-dvmdb-`.

The native repository format is an AOWIS format, not a synonym for the
executable's name. See [architecture](architecture.md),
[canonical MODEL serialization](canonical-model-codec.md) and the
[first-release format policy](format-compatibility.md).

The project is pre-release: unsupported experimental formats are rejected
explicitly. There is no requirement for old API aliases, old-layout readers,
migration commands or legacy deployment workflows. Data relocation and
verified offline backup are documented in [operations](operations.md).
