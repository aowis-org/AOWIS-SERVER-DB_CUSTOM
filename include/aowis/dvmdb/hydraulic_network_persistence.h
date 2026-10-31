#pragma once

#include <cstddef>
#include <functional>

// Optional Qt/hydraulic persistence adapter for a complete NetworkHydraulic aggregate. The
// authoritative entity bytes are encoded by the native-owned canonical ModelCodec;
// ordered network membership is stored as bounded, individually versioned chunks.
#include "aowis/dvmdb/model_codec_adapter.h"

#include <QList>

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace aowis::dvmdb::hydraulic {

inline constexpr std::uint16_t hydraulicNetworkKind = 100;

// VC-04 draft transaction membership operation. The GUI transmits logical
// collection identity; only this DVMDB adapter knows chunk and manifest bytes.
// The owning network must already exist in the pinned base or candidate.
struct DraftMembershipIntent final {
    Key network;
    Key member;
    std::uint16_t kind = 0;
    std::uint16_t collection = 0;
    bool inserted = false;
};

// Construct the server-owned empty membership manifest for a newly created
// hydraulic network. This is an ordinary draft ModelChange, not a revision.
// No external caller should manufacture kind-100 manifest bytes itself.
ModelChange emptyDraftHydraulicNetworkRoot(const QUuid& networkId, const QString& name);

// Read one consistent working-copy view (immutable committed base overlaid by
// the draft's complete candidate record map). Never reads a mutable HEAD.
// Validates native chunks, canonical entity payloads and typed dependencies.
std::optional<NetworkHydraulic> loadDraftHydraulicNetwork(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& networkId);


// Add native root/chunk ModelChanges for a bounded batch of validated logical
// membership intents. `readCurrent` returns the draft BEFORE the incoming
// batch (candidate overlay first, immutable base second); `changes` initially
// contains the caller's ordinary entity updates. This function never writes.
// The resulting changes are published together with the events by the
// existing generation-checked, fsynced ProjectService draft replacement.
void materializeDraftMembership(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const std::vector<DraftMembershipIntent>& intents,
    std::vector<ModelChange>& changes);


// Test-only SHA-revision interface pending migration of the indexed edit
// qualification suite to project-scoped UUIDv7 revisions. Not for new callers.
// Replaces one complete hydraulic network inside the branch in ONE atomic
// model commit. Other networks in the branch are unaffected. All 18 top-level
// entity lists are stored as individually addressable canonical model records.
// Validation is the native canonical hydraulic-model validator, including typed refs.
// Throws StorageError (InvalidInput, Conflict, ...) on validation, ownership or storage failures.
Revision saveHydraulicNetworkLegacy(Repository& repository, std::string_view branch,
                              const NetworkHydraulic& network,
                              std::optional<Hash> expectedHead = std::nullopt);

// SHA-revision historical/branch readers retained for existing qualification tests.
// Load APIs: a missing network or entity returns nullopt. Every failure
// throws StorageError with its kind (Corruption for malformed stored data,
// InvalidInput for a bad selector or revision, Io/ResourceExhausted for
// storage failures), so callers map it like any other storage error.
//
// Reconstructs the complete network in the original list order. All members,
// identities, kinds and declared reference sets are checked. The branch head
// is captured once; subsequent writes cannot mix revisions.
std::optional<NetworkHydraulic> loadHydraulicNetwork(Repository& repository,
                                                      std::string_view branch,
                                                      const QUuid& networkUuid);

// Load a historical immutable snapshot, independent of any branch movement.
// The revision must identify a model-indexed revision stored in this repository.
std::optional<NetworkHydraulic> loadHydraulicNetworkAtRevisionLegacy(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid);

// Exactly the 18 top-level types persisted by saveHydraulicNetworkLegacy().
// The alternative order follows the manifest's collection order.
using HydraulicEntity = std::variant<
    HydraulicPipeMaterial, HydraulicPatternTime, HydraulicCurveTankVolume,
    HydraulicCurvePumpHead, HydraulicCurvePumpEfficiency, HydraulicCurveValveHeadloss,
    HydraulicCurveValveCharacteristic, HydraulicCurveGeneric, HydraulicNodeReservoir,
    HydraulicNodeJunction, HydraulicNodeTank, HydraulicDemandPoint, HydraulicLinkPipe,
    HydraulicLinkPump, HydraulicLinkValve, HydraulicControlSimple,
    HydraulicControlRule, HydraulicMapLabel>;

// Ordinal matches the corresponding alternative in HydraulicEntity, and the
// canonical NetworkHydraulic collection order. This is an API enum, not a
// serialized/on-disk kind identifier.
enum class HydraulicEntityType : std::size_t {
    PipeMaterial = 0,
    TimePattern,
    TankVolumeCurve,
    PumpHeadCurve,
    PumpEfficiencyCurve,
    ValveHeadlossCurve,
    ValveCharacteristicCurve,
    GenericCurve,
    Reservoir,
    Junction,
    Tank,
    DemandPoint,
    Pipe,
    Pump,
    Valve,
    SimpleControl,
    RuleControl,
    MapLabel
};

// Returns UUIDs in network collection order without decoding model entities.
// Missing network -> nullopt. Corrupt/mismatched chunks throw. Complexity is O(members in the requested collection),
// not the entire network.
// Historical counterpart: membership chunks are read at exactly revisionId.
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevision(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    HydraulicEntityType type);

std::optional<QList<QUuid>> listHydraulicEntityIds(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    HydraulicEntityType type);

// Bounded listing for untrusted callers. Missing network -> nullopt. Failures
// throw StorageError; a collection with more than maxCount members throws
// LimitExceeded after decoding at most one membership chunk beyond the limit.
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimitedLegacy(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    HydraulicEntityType type, std::size_t maxCount);

// Targeted, index-backed entity read. Membership is tested via the persistent
// reverse-reference index; the complete network is NOT reconstructed. This
// checks the canonical payload and entity type but intentionally does not run
// full-network semantic validation. Missing/unowned UUID -> nullopt;
// malformed records throw. Branch reads pin a single revision.
// The historical counterpart also pins ownership and reverse-reference reads.
std::optional<HydraulicEntity> loadHydraulicEntityAtRevisionLegacy(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    const QUuid& entityUuid);

std::optional<HydraulicEntity> loadHydraulicEntity(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const QUuid& entityUuid);

struct HydraulicEntityChange {
    QUuid uuid;
    // nullopt removes the UUID from its owning collection; a value replaces
    // the existing same-kind member or appends a new member to its collection.
    std::optional<HydraulicEntity> value;
};

// Diagnostics for choosing between bounded indexed validation and the
// conservative complete-network validator. Populated on success only.
struct HydraulicEditDiagnostics {
    bool indexedScalarFastPath = false;
    bool indexedTopologyFastPath = false;
    bool indexedMembershipFastPath = false;
    bool indexedCatalogueFastPath = false;
    bool indexedPhysicalFastPath = false;
    bool indexedDependentUpdateFastPath = false;
    std::size_t validatedEntities = 0; // changed payloads decoded on fast path
};

// A user may select a side for a genuinely conflicting canonical model field.
// `field` is an exact name from the merge conflict diagnostic, including
// nested metadata fields (e.g. "metadata.description"). Nonconflicting or
// unknown names are rejected, not silently ignored.
enum class HydraulicMergeSide { Target, Source };
struct HydraulicFieldChoice {
    QUuid entityUuid;
    QString field;
    HydraulicMergeSide side = HydraulicMergeSide::Target;
};
struct HydraulicObjectChoice {
    QUuid entityUuid;
    // Selects one complete existing/deleted record from a genuinely divergent
    // UUID conflict. A deleted side means deletion; a kept side may fail the
    // whole-network validation if membership changes are incompatible.
    HydraulicMergeSide side = HydraulicMergeSide::Target;
};
struct HydraulicMergeSelectionsLegacy {
    // Required together when choices are supplied. They pin the previewed
    // branch tips, preventing a stale GUI dialog from merging newer changes.
    std::optional<Hash> expectedTargetHead;
    std::optional<Hash> expectedSourceHead;
    std::vector<HydraulicFieldChoice> fields;
    std::vector<HydraulicObjectChoice> objects;
};

// SHA-revision merge retained temporarily to preserve the existing
// reconciliation qualification suite until its UUIDv7 migration.
// Three-way merge with canonical model field reconciliation and full candidate
// hydraulic validation before publication. Field choices resolve only actual
// same-field conflicts of unchanged-reference existing entities. Whole-object
// choices may select either version of a divergent entity (including deletion),
// subject to complete network and membership validation. Kept entities whose
// membership the other branch deleted are restored to bounded chunks using
// historical ownership. Compatible append/deletes in the same chunk merge;
// reordered membership and unsafe combinations remain conflicts.
// The default native object merge is unaffected. Invalid or stale choices
// throw without changing either branch or appending pack objects.
MergeResult mergeHydraulicBranchesLegacy(Repository& repository, std::string_view target,
                                   std::string_view source,
                                   const HydraulicMergeSelectionsLegacy& selections = {});

// Canonical project-scoped UUIDv7 hydraulic merge. Both immutable branch-tip
// UUIDs pin every interactive selection; full-network validation is mandatory.
struct HydraulicMergeSelections {
    RevisionId expectedTargetHead;
    RevisionId expectedSourceHead;
    std::vector<HydraulicFieldChoice> fields;
    std::vector<HydraulicObjectChoice> objects;
};
MergeResultV2 mergeHydraulicBranches(Repository& repository, const ProjectId& project,
                                       std::string_view target, std::string_view source,
                                       const HydraulicMergeSelections& selections);

// SHA-only indexed edit implementation retained to migrate its qualification
// and benchmark coverage onto the canonical project-scoped API.
// Incremental *write* path for one existing network. Each change is encoded
// individually; unchanged entities and membership chunks are not rewritten.
// Existing scalar-only edits, eligible pipe rewires, whitelisted
// reservoir/tank/pump/valve physical parameters, and existing demand-point /
// simple-control / rule-control relationship updates use bounded indexed validation.
// Addition and deletion of junctions, pipes, reservoirs, tanks, pumps, valves,
// demand points, simple/rule controls, catalogues and map labels uses typed
// indexed validation when references are locally provable. Demand point pipe
// allocation is checked against actual pipe endpoints. New nodes and links may refer to one another in a batch.
// Deleting a pipe and its dependent junction together is supported when no
// surviving entity or network metadata refers to either removed UUID.
// These edits touch only affected bounded membership chunks and, when a
// chunk descriptor changes, the root.
// Other edits use full canonical validation and cost O(network size).
// Both paths commit atomically with an expected-head check.
// Empty or semantically unchanged batches do not create a new revision.
Revision editHydraulicNetworkLegacy(Repository& repository, std::string_view branch,
                              const QUuid& networkUuid,
                              const std::vector<HydraulicEntityChange>& changes,
                              HydraulicEditDiagnostics* diagnostics = nullptr,
                              std::optional<Hash> expectedHead = std::nullopt);

// Canonical project-scoped UUIDv7 hydraulic operations. All reads pin an
// immutable revision; saves/edits compare expected HEAD during atomic commit.
// Whitelisted scalar-only edits use indexed revision records and preserve the
// unchanged network membership manifest; other edits use canonical full-network
// validation and replacement until additional indexed paths are migrated.
// No SHA-revision fallback is permitted.
RevisionId saveHydraulicNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const NetworkHydraulic& network,
                                  const RevisionId& expectedHead);
RevisionId editHydraulicNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const QUuid& networkUuid,
                                  const std::vector<HydraulicEntityChange>& changes,
                                  const RevisionId& expectedHead,
                                  HydraulicEditDiagnostics* diagnostics = nullptr);
std::optional<NetworkHydraulic> loadHydraulicNetworkAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid);
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimited(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, HydraulicEntityType type, std::size_t maxCount);
std::optional<HydraulicEntity> loadHydraulicEntityAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, const QUuid& entityUuid);

} // namespace aowis::dvmdb::hydraulic
