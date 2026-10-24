# Repository and bundle format compatibility

AOWIS-SERVER-DB_CUSTOM has **one native pack storage implementation** and
portable, content-addressed `.aowbundle` synchronization. Pack files are the
local authoritative object storage; `objects.idx` is a derived sidecar and can
be rebuilt. Bundles transport immutable logical history independently of
physical pack offsets or sidecar details.

## Pre-release compatibility policy

The binary formats are **not yet frozen for a public stable release**. The
current release gate is narrower: a newer build must read and verify the frozen
pre-release reference repository, and must import the frozen complete and
incremental bundles. Any intentional incompatible change must be accompanied
by an explicit migration and compatibility decision rather than silently
replacing these fixtures.

Format stability is distinct from schema stability: the canonical hydraulic
model codec and its 68-structure/39-enum contract have their own validation.
Likewise, a valid native repository does not prove semantic correctness of all
hydraulic model configurations. The HTTP API has a separately versioned wire
contract.

## Immutable reference fixtures

`tests/fixtures/pack-20261024/` contains bytes generated with the single-pack
native engine on October 24, 2026. This is a *frozen pre-release specimen*,
not an on-disk format name or an assertion of compatibility with every older
experimental engine. It contains:

- An actual pack and two branch-reference files (`main` and `feature`),
  with ancestor revisions. Lock files and the derived `objects.idx` are
  intentionally **absent** to force index reconstruction.
- `base.aowbundle`: a complete branch bundle at the first nonempty revision.
- `delta.aowbundle`: an incremental update from that base to the later `main`
  head, including an overwritten value and a deletion.
- `feature.aowbundle` and `tip.aowbundle`: complete divergent-feature and
  latest-main histories.

All seven data files are byte-for-byte protected by SHA-256 digests embedded
in `native_format_compatibility_tests.cpp`. The fixture is read-only in normal
testing: the regression copies it to randomized private directories before
opening or modifying any repository. The test checks the original revision
hashes, divergent branch history, old values by revision ID, sidecar rebuild,
reopen, new commits on an old pack, frozen imports, idempotent incremental
import, current-to-frozen bundle interoperability and corrupted-bundle
rejection.

Run it independently:

```bash
ctest --test-dir build-linux-tests-release \
  -R '^aowis-native-format-compatibility-tests$' --output-on-failure
```

The test runs as part of normal Debug/Release CTest. It needs neither Qt,
Docker nor QEMU and does not contact any remote service.

## Whole-service upgrade and restoration drill

The `aowis-native-upgrade-restore-tests` regression composes the frozen
repository fixture with the actual multi-project catalogue and offline
backup/restore utility. Two out of four service projects contain unmodified
older-build pack bytes (with the derived object index omitted); the other
projects have independently edited branches, including more than 1 MiB of
current-format data. The test creates a cold backup without mutating the
original index, verifies the copied index was rebuilt, restores to a new root
and compares **every project identity, branch head, first-parent history and
branch payload**. It also checks historic reads, writes a new revision into a
restored historical pack and repeats backup/restore after that write.

This is a reproducible compatibility and deployment-regression gate, not a
promise of format downgrade, transparent online migration or preservation of
application-level model semantics. The source reference bytes remain fixed;
operator migration still requires a separately verified backup and a planned
rollback window (see [operations](operations.md)).

## Upgrades and format changes

1. **Back up** and independently verify important service roots before
   upgrading. Do not overwrite the only copy of a repository.
2. Run the frozen compatibility regression under the proposed new executable.
   Add a new independent frozen specimen when a new on-disk encoding is
   introduced; do not regenerate old fixtures with the new encoder.
3. If a format intentionally changes, document exactly which old versions
   remain readable, whether `objects.idx` can be rebuilt, and whether the
   portable bundle is still accepted. Provide a conversion path and test
   both historical import and post-upgrade verification.
4. Validate upgrade/restore against disposable copies of representative
   multi-project roots, not only tiny fixture repositories.

**Unsupported legacy format:** repositories bearing the obsolete experimental
`storage.format` block marker are deliberately rejected by the pack engine.
Export those repositories with an executable supporting that historical
format and import the portable bundle into a new pack repository. The frozen
fixture does not imply native block-format compatibility, compatibility with
SQLite/PostgreSQL adapters or unrestricted backward compatibility.

A stable release still requires a published, explicit support matrix for pack,
sidecar, bundles, model-codec schema and minimum reader/writer versions. The
reference fixtures establish a concrete regression baseline; they are **not**
a substitute for that policy.
