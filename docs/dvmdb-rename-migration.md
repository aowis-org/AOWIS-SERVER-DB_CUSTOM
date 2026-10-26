# AOWIS-SERVER-DVMDB API rename and rollout

AOWIS remains the owning platform and model ecosystem. DVMDB replaces the
former database project identifier; the canonical deployment and tools use
**AOWIS-SERVER-DVMDB** and `aowis-dvmdb-*` names.

## Breaking source and runtime names

| Surface | Canonical name |
|---|---|
| Source directory | `AOWIS-SERVER-DVMDB/` |
| CMake project | `AOWIS_Server_DVMDB` |
| CMake option prefix | `AOWIS_DVMDB_` |
| Libraries / test executables | `aowis-dvmdb-*` |
| Server executable | `aowis-server-dvmdb` |
| C++ public namespace | `aowis::dvmdb` |
| Public headers | `include/aowis/dvmdb/` |
| Public CMake aliases | `AOWIS::DvmdbProjectService`, `AOWIS::DvmdbModelCodec`, `AOWIS::DvmdbHydraulic` |
| HTTP route prefix | `/dvmdb/v1` |
| OpenAPI contract | `docs/dvmdb-http-api.yaml` |
| Source snapshot | `dvmdb.zip`, created by `tools/zip_dvmdb.sh` |
| Patch names | `aowis-dvmdb-*.zip` |

No compatibility aliases, deprecated header subtree, old routing prefix or
old ZIP wrapper are present. The HTTP base change intentionally requires
client URL updates and a coordinated deployment. Existing URL requests to
`/api/v1` receive 404 instead of being silently forwarded.

## Persistence is still AOWIS

The pack record markers, index framing, project manifest markers, canonical
AOWIS-SERVER-MODEL codec/type identifiers, Merkle content hashes, replication
bundle markers and `.aowbundle` extension are **AOWIS storage protocols**.
They never contained the former database project name and remain byte-identical.
This is intentional: changing them would invalidate stored identities and
break recovery, cross-version bundle exchange and historical repositories
for no product-name benefit. The frozen pack/bundle fixture test verifies
these bytes are still readable after the public API rename.

## Migration procedure

1. Stop active database writers and the old service.
2. Take and independently verify an offline repository backup before deploying.
3. Update integrations to compile against `aowis::dvmdb`, `aowis/dvmdb/`
   and the new exported CMake aliases. Rebuild dependent binaries.
4. Point HTTP clients and reverse proxies at `/dvmdb/v1`; update server
   executable names, service units and any hardcoded build target names.
5. Choose the data root deliberately: the new service's default can be a new,
   empty location. Restore your verified backup or set the existing root
   explicitly; do not confuse a fresh empty repository with migrated data.
6. Run `./compile_linux_tests.sh --both` and a server readiness/integration
   check before allowing writes.

This change does not rewrite or erase stored projects or hydraulic model data.
