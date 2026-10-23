#pragma once

#include <cstddef>

// Optional Qt/hydraulic persistence adapter for a complete NetworkHydraulic aggregate. The
// authoritative entity bytes are encoded by the native-owned canonical ModelCodec;
// ordered network membership is stored as bounded, individually versioned chunks.
#include "aowis/native/model_codec_adapter.h"

#include <QList>

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace aowis::native::hydraulic {

inline constexpr std::uint16_t hydraulicNetworkKind = 100;

// Replaces one complete hydraulic network inside the branch in ONE atomic
// model commit. Other networks in the branch are unaffected. All 18 top-level
// entity lists are stored as individually addressable canonical model records.
// Validation is the native canonical hydraulic-model validator, including typed refs.
// Throws std::runtime_error on validation, ownership, or storage failures.
Revision saveHydraulicNetwork(Repository& repository, std::string_view branch,
                              const NetworkHydraulic& network,
                              std::optional<Hash> expectedHead = std::nullopt);

// Reconstructs the complete network in the original list order. All members,
// identities, kinds and declared reference sets are checked. A missing root
// returns nullopt; malformed/incomplete stored networks set error and fail.
// The branch head is captured once; subsequent writes cannot mix revisions.
// A missing network clears error.
std::optional<NetworkHydraulic> loadHydraulicNetwork(Repository& repository,
                                                      std::string_view branch,
                                                      const QUuid& networkUuid,
                                                      QString* error = nullptr);

// Load a historical immutable snapshot, independent of any branch movement.
// The revision must identify a model-indexed revision stored in this repository.
std::optional<NetworkHydraulic> loadHydraulicNetworkAtRevision(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    QString* error = nullptr);

// Exactly the 18 top-level types persisted by saveHydraulicNetwork().
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
// Missing network -> nullopt with no error. Corrupt/mismatched chunks fail
// closed and set error. Complexity is O(members in the requested collection),
// not the entire network.
// Historical counterpart: membership chunks are read at exactly revisionId.
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevision(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    HydraulicEntityType type, QString* error = nullptr);

std::optional<QList<QUuid>> listHydraulicEntityIds(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    HydraulicEntityType type, QString* error = nullptr);

// Bounded listing for untrusted callers. Missing network -> nullopt. Failures
// throw StorageError; a collection with more than maxCount members throws
// LimitExceeded after decoding at most one membership chunk beyond the limit.
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimited(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    HydraulicEntityType type, std::size_t maxCount);

// Targeted, index-backed entity read. Membership is tested via the persistent
// reverse-reference index; the complete network is NOT reconstructed. This
// checks the canonical payload and entity type but intentionally does not run
// full-network semantic validation. Missing/unowned UUID -> nullopt with no
// error; malformed records fail closed. Branch reads pin a single revision.
// The historical counterpart also pins ownership and reverse-reference reads.
std::optional<HydraulicEntity> loadHydraulicEntityAtRevision(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    const QUuid& entityUuid, QString* error = nullptr);

std::optional<HydraulicEntity> loadHydraulicEntity(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const QUuid& entityUuid, QString* error = nullptr);

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
struct HydraulicMergeSelections {
    // Required together when choices are supplied. They pin the previewed
    // branch tips, preventing a stale GUI dialog from merging newer changes.
    std::optional<Hash> expectedTargetHead;
    std::optional<Hash> expectedSourceHead;
    std::vector<HydraulicFieldChoice> fields;
    std::vector<HydraulicObjectChoice> objects;
};

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
MergeResult mergeHydraulicBranches(Repository& repository, std::string_view target,
                                   std::string_view source,
                                   const HydraulicMergeSelections& selections = {});

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
Revision editHydraulicNetwork(Repository& repository, std::string_view branch,
                              const QUuid& networkUuid,
                              const std::vector<HydraulicEntityChange>& changes,
                              HydraulicEditDiagnostics* diagnostics = nullptr,
                              std::optional<Hash> expectedHead = std::nullopt);

} // namespace aowis::native::hydraulic
