#pragma once

// Optional Qt/V5 adapter for a complete NetworkHydraulic aggregate. The
// authoritative entity bytes are encoded by the native-owned canonical ModelCodec V5;
// ordered network membership is stored as bounded, individually versioned chunks.
#include "aowis/native/model_codec_adapter.h"

#include <QList>

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace aowis::native::v5 {

inline constexpr std::uint16_t hydraulicNetworkKind = 100;

// Replaces one complete hydraulic network inside the branch in ONE atomic
// model commit. Other networks in the branch are unaffected. All 18 top-level
// entity lists are stored as individually addressable canonical V5 records.
// Validation is the native canonical hydraulic-model validator, including typed refs.
// Throws std::runtime_error on validation, ownership, or storage failures.
Revision saveHydraulicNetwork(Repository& repository, std::string_view branch,
                              const NetworkHydraulic& network);

// Reconstructs the complete network in the original list order. All members,
// identities, kinds and declared reference sets are checked. A missing root
// returns nullopt; malformed/incomplete stored networks set error and fail.
std::optional<NetworkHydraulic> loadHydraulicNetwork(Repository& repository,
                                                      std::string_view branch,
                                                      const QUuid& networkUuid,
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
std::optional<QList<QUuid>> listHydraulicEntityIds(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    HydraulicEntityType type, QString* error = nullptr);

// Targeted, index-backed entity read. Membership is tested via the persistent
// reverse-reference index; the complete network is NOT reconstructed. This
// checks the canonical payload and entity type but intentionally does not run
// full-network semantic validation. Missing/unowned UUID -> nullopt with no
// error; malformed records and concurrent branch changes fail closed.
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
    std::size_t validatedEntities = 0; // changed payloads decoded on fast path
};

// Incremental *write* path for one existing network. Each change is encoded
// individually; unchanged entities and membership chunks are not rewritten.
// Existing scalar-only edits and eligible pipe rewires use bounded indexed
// validation. Addition of junctions, pipes, and map labels, or deletion of
// independent map labels, uses typed indexed validation and touches only the
// affected bounded membership chunk and, when its descriptor changes, root.
// Other edits use full canonical validation and cost O(network size).
// Both paths commit atomically with an expected-head check.
// Empty or semantically unchanged batches do not create a new revision.
Revision editHydraulicNetwork(Repository& repository, std::string_view branch,
                              const QUuid& networkUuid,
                              const std::vector<HydraulicEntityChange>& changes,
                              HydraulicEditDiagnostics* diagnostics = nullptr);

} // namespace aowis::native::v5
