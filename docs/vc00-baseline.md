# VC-00 — Verified baseline and implementation gaps (2026-10-28)

Scope: supplied DVMDB and GUI snapshots of 2026-10-28. This is a source inspection, **not** evidence that VC-01 has been implemented or that tests passed. The storage engine is DVMDB's native filesystem/object-pack repository; **no relational database or query engine is involved**.

## Exact API/format inventory

| Surface | Current entry point / type | Gap blocking the approved lifecycle |
|---|---|---|
| Project creation | `src/project_service.cpp`, `ProjectService::createProjectImpl` | Creates `main` with `Repository::createBranchV2`; not unborn |
| Native branch creation | `src/native_store.cpp`, `Repository::createBranchV2` | No-source branch writes a root revision (`writeRevisionV2Locked`) |
| Native HEAD | `include/aowis/dvmdb/native_store.h`, `Repository::headV2` | Returns non-optional `RevisionId` |
| Service HEAD | `include/aowis/dvmdb/project_service.h`, `ProjectService::head` / `headV2` | Returns non-optional `RevisionId` |
| HTTP HEAD | `src/dvmdb_http_routes.cpp`, `GET .../branches/:branch/head` | Serializes non-null `id` |
| HTTP graph | `src/dvmdb_http_routes.cpp`, `GET .../revision-graph` | Each head serializes a non-null `revision` and `recordHash` |
| Graph reader | `src/native_store.cpp`, `Repository::revisionGraphV2` | Needs explicit empty-graph and nullable-ref representation |
| Identity | `include/aowis/dvmdb/revision_record_v2.h`, `include/aowis/dvmdb/native_store.h` | Preserve distinct UUIDv7 project/branch/revision IDs and SHA-256 record digests |
| Bundles / CLI | `tests/native_revision_bundle_v2_tests.cpp`, `tools/native_sync.cpp`, `tools/native_verify.cpp` | Audit nullable ref transfer, verification and restores before changing wire/persistent shapes |
| Existing repos | `tests/fixtures/pack-20261024/repository/refs/heads/*` | Preserve committed R0 and every recorded identifier; no implicit migration |

## Cross-project findings

GUI source reader `src/projects/project_source_reader.h` uses `SourceBranch::head` as `QString`; `src/projects/project_source_reader.cpp::validateCompleteGraph` requires every branch HEAD to be UUIDv7 and resolve to an immutable record hash. GUI graph/widget inputs and `src/tabs/project_revision_graph.*` must accept a **real branch UUID with absent HEAD and zero revision nodes**, never a synthetic commit. The GUI checkout also includes a Standalone DVMDB integration path: native headers and library versions must be coordinated before a GUI edit is released.

## Acceptance matrix (target, currently *not* established)

| Case | Native/REST expected | GUI expected | Current status |
|---|---|---|---|
| New project | `main` real branch UUID; null HEAD; zero commits | Show unborn `main`, empty graph | Gap |
| First commit | CAS from explicit expected-null; create first immutable commit | One revision node | Gap |
| Branch from existing commit | Distinct branch UUID; shared commit HEAD | One node with multiple ref decorations | Revalidate |
| Simultaneous sessions | Independent private durable drafts and writer leases | No editing shared HEAD directly | Gap |
| Reload/crash recovery | Persist unborn status and drafts | Reload without fake R0 | Gap |
| Restore older repository | Preserve recorded existing R0 and IDs | Show historical commits unchanged | Migration qualification pending |
| Cross-repository sync | Repository-qualified HEAD and conditional publication | Separate local/tracking/shared HEADs | Gap |

## Reproduction and gates

Read-only source probe from DVMDB checkout: `python3 tools/check_vc00_contract.py --gui-root ../AOWIS-SERVER-GUI`. It reports known source assumptions and exits nonzero only when an expected source file is missing; a changed assumption must be reviewed, not interpreted as automatically passing VC-01.

Existing test entry points include `tests/native_revision_graph_v2_tests.cpp`, `tests/native_branch_identity_tests.cpp`, `tests/native_repository_revision_head_v2_tests.cpp`, `tests/native_revision_bundle_v2_tests.cpp`, `tests/native_upgrade_restore_tests.cpp`, `tests/native_http_tests.cpp`, and GUI `src/projects/project_source_reader.*` with project-source-reader and project-revision-graph CTest targets in its `CMakeLists.txt`. Run each project's configured CTest suite after implementing changed contracts; the attached snapshots do not supply a verified build environment or passing-test result.

**Next implementation checkpoint:** introduce explicitly nullable branch ref storage and native HEAD/graph contracts, make first commit conditional on expected-null, preserve existing branches transactionally, then update REST serialization and GUI parsing/tests as a coordinated VC-01 patch. No storage migration or existing data changes are authorized by this VC-00 audit.
