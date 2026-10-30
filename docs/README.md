# DVMDB documentation

DVMDB is AOWIS's native, versioned model database. **Start with the product
contract and the implemented API, not an old development-stage log.**

## Primary references

| Read when… | Document |
|---|---|
| Understanding project/branch/commit/working-copy semantics | [Version-control contract](version-control-semantics.md) |
| Implementing native storage and revision identity | [Architecture](architecture.md), [native revision history](native-revision-history.md), [model records](model-records.md) |
| Building the HTTP server or a client | [Native service](native-database-service.md), [OpenAPI](dvmdb-http-api.yaml), [REST design](rest-api-design.md) |
| Implementing private drafts or editing events | [Working-copy storage](working-copy-storage-v1.md), [private repositories](private-working-repositories.md), [hydraulic persistence](native-hydraulic-network.md) |
| Implementing checkout, local Commit or publication | [Implementation roadmap](roadmap.md), [GUI integration requirements](gui-integration-requirements.md) |
| Working with canonical MODEL bytes | [Canonical MODEL codec](canonical-model-codec.md), [MODEL serialization](../external/AOWIS-SERVER-MODEL/docs/model-serialization.md) |
| Synchronizing or merging | [Offline synchronization](offline-repository-synchronization.md), [branch merge](native-branch-merging.md), [hydraulic merge](native-hydraulic-merging.md) |
| Operating, recovering or qualifying a repository | [Operations](operations.md), [repository verification](repository-verification.md), [durability qualification](durability-qualification.md), [format policy](format-compatibility.md) |
| Reproducing engineering results | [Engineering findings](engineering-findings.md), [benchmark methodology](benchmark-plan.md) |

## What is usable and what remains

The current source includes unborn branches, durable draft lifecycle and
atomic MODEL event journal, native hydraulic membership persistence, and
repository synchronization and revision/merge primitives. **An end-to-end
Projects-tab checkout → real editor autosave → reopen → local Commit is not
yet accepted**. The critical next work is GUI membership transmission,
full model hydration and session attachment, followed by working-copy Commit.
See the [roadmap status and next work](roadmap.md#current-implementation-state-and-immediate-delivery-priority).

The project is pre-release. Do **not** implement compatibility with earlier
experimental formats. Authentication, authorization and permissions are a
separate, unscheduled AOWIS-wide effort, documented for later review in
[security considerations](security-permissions-design.md). Repository
functional roles and fencing tokens are **not** authentication.

### Documentation maintenance

Document present product behavior in these topical pages; record future
implementation work and its acceptance criteria only in the roadmaps.
Keep benchmark evidence in its own reproducibility documents. Do not turn
product documentation into a chronology of ZIP patches. When a feature is
only partly implemented, say which layer exists and which end-to-end test
remains. An archive of an earlier test run does not establish current
release readiness.
