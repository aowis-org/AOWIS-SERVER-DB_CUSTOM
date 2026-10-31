#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <aowis/dvmdb/model_validation.h>

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
namespace native = aowis::dvmdb;
namespace hydraulic = aowis::dvmdb::hydraulic;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QUuid uuid(int ordinal) {
    native::Key key{};
    key[0] = 0xA0;
    key[6] = 0x70;
    key[8] = 0x80;
    for (int i = 0; i < 4; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(ordinal >> (8 * i));
    }
    return hydraulic::uuidFromKey(key);
}

template<typename T>
T item(int ordinal) {
    T result;
    result.uuid = uuid(ordinal);
    result.id = QStringLiteral("Entity-%1").arg(ordinal);
    return result;
}

std::filesystem::path tempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "aowis-dvmdb-network-test-XXXXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* result = ::mkdtemp(writable.data());
    if (result == nullptr) throw std::runtime_error("mkdtemp failed");
    return result;
}

NetworkHydraulic completeNetwork() {
    NetworkHydraulic n;
    n.uuid = uuid(1);
    n.id = QStringLiteral("Network-1");
    n.title_line_1 = QStringLiteral("Réseau de Bitkine 水");
    n.duration_s = 123456;
    n.map_backdrop.file = QStringLiteral("background-map.png");
    MultiSpeciesSpecies species;
    species.id = QStringLiteral("chlorine");
    species.uuid = uuid(300);
    species.note = QStringLiteral("Dissolved Cl₂");
    n.multi_species.species.append(species);

    n.pipe_materials.append(item<HydraulicPipeMaterial>(10));
    n.patterns_time.append(item<HydraulicPatternTime>(11));
    n.patterns_time[0].multipliers = {1.0, 1.2, 0.75};
    n.curves_tank_volume.append(item<HydraulicCurveTankVolume>(12));
    n.curves_pump_head.append(item<HydraulicCurvePumpHead>(13));
    n.curves_pump_efficiency.append(item<HydraulicCurvePumpEfficiency>(14));
    n.curves_valve_headloss.append(item<HydraulicCurveValveHeadloss>(15));
    n.curves_valve_characteristic.append(item<HydraulicCurveValveCharacteristic>(16));
    n.curves_generic.append(item<HydraulicCurveGeneric>(17));
    n.nodes_reservoirs.append(item<HydraulicNodeReservoir>(18));
    n.nodes_junctions.append(item<HydraulicNodeJunction>(19));
    n.nodes_junctions.append(item<HydraulicNodeJunction>(20));
    n.nodes_junctions[0].metadata.description = QStringLiteral("Junction-α");
    n.nodes_tanks.append(item<HydraulicNodeTank>(21));
    n.nodes_tanks[0].volume_curve_uuid = uuid(12);
    n.demand_points.append(item<HydraulicDemandPoint>(22));
    n.demand_points[0].attachment.type = HydraulicDemandPointAttachmentType::Junction;
    n.demand_points[0].attachment.junction_uuid = uuid(19);
    n.links_pipes.append(item<HydraulicLinkPipe>(23));
    n.links_pipes[0].node_uuid_from = uuid(19);
    n.links_pipes[0].node_uuid_to = uuid(20);
    n.links_pipes[0].material_uuid = uuid(10);
    n.links_pipes[0].diameter_mm = 110.75;
    n.links_pumps.append(item<HydraulicLinkPump>(24));
    n.links_pumps[0].node_uuid_from = uuid(18);
    n.links_pumps[0].node_uuid_to = uuid(19);
    n.links_pumps[0].head_curve_uuid = uuid(13);
    n.links_pumps[0].speed_pattern_uuid = uuid(11);
    n.links_valves.append(item<HydraulicLinkValve>(25));
    n.links_valves[0].node_uuid_from = uuid(20);
    n.links_valves[0].node_uuid_to = uuid(21);
    n.links_valves[0].head_loss_curve_uuid = uuid(15);
    n.controls_simple.append(item<HydraulicControlSimple>(26));
    n.controls_simple[0].link_uuid = uuid(23);
    n.controls_simple[0].trigger_node_uuid = uuid(19);
    n.controls_rules.append(item<HydraulicControlRule>(27));
    HydraulicControlRuleAction action;
    action.link_uuid = uuid(24);
    n.controls_rules[0].actions_then.append(action);
    n.map_labels.append(item<HydraulicMapLabel>(28));
    n.map_labels[0].anchor_node_uuid = uuid(20);
    n.map_labels[0].text = QStringLiteral("Well Ω");
    return n;
}

void compareNetworks(const NetworkHydraulic& source, const NetworkHydraulic& result) {
    require(result.uuid == source.uuid && result.id == source.id &&
            result.title_line_1 == source.title_line_1 &&
            result.duration_s == source.duration_s &&
            result.map_backdrop.file == source.map_backdrop.file &&
            aowis::dvmdb::ModelCodec<NetworkMultiSpecies>::encode(result.multi_species) ==
            aowis::dvmdb::ModelCodec<NetworkMultiSpecies>::encode(source.multi_species),
            "network metadata changed on reload");
#define CHECK_LIST(name, type) \
    require(result.name.size() == source.name.size(), "network entity count changed: " #name); \
    for (qsizetype i = 0; i < source.name.size(); ++i) { \
        require(result.name[i].uuid == source.name[i].uuid, "network order changed: " #name); \
        require(aowis::dvmdb::ModelCodec<type>::encode(result.name[i]) == \
                aowis::dvmdb::ModelCodec<type>::encode(source.name[i]), \
                "canonical model payload changed: " #name); \
    }
    CHECK_LIST(pipe_materials, HydraulicPipeMaterial)
    CHECK_LIST(patterns_time, HydraulicPatternTime)
    CHECK_LIST(curves_tank_volume, HydraulicCurveTankVolume)
    CHECK_LIST(curves_pump_head, HydraulicCurvePumpHead)
    CHECK_LIST(curves_pump_efficiency, HydraulicCurvePumpEfficiency)
    CHECK_LIST(curves_valve_headloss, HydraulicCurveValveHeadloss)
    CHECK_LIST(curves_valve_characteristic, HydraulicCurveValveCharacteristic)
    CHECK_LIST(curves_generic, HydraulicCurveGeneric)
    CHECK_LIST(nodes_reservoirs, HydraulicNodeReservoir)
    CHECK_LIST(nodes_junctions, HydraulicNodeJunction)
    CHECK_LIST(nodes_tanks, HydraulicNodeTank)
    CHECK_LIST(demand_points, HydraulicDemandPoint)
    CHECK_LIST(links_pipes, HydraulicLinkPipe)
    CHECK_LIST(links_pumps, HydraulicLinkPump)
    CHECK_LIST(links_valves, HydraulicLinkValve)
    CHECK_LIST(controls_simple, HydraulicControlSimple)
    CHECK_LIST(controls_rules, HydraulicControlRule)
    CHECK_LIST(map_labels, HydraulicMapLabel)
#undef CHECK_LIST
}

// Load APIs report every failure as a typed StorageError (never as text).
bool throwsStorageError(native::StorageErrorKind expected, const std::function<void()>& action) {
    try {
        action();
    } catch (const native::StorageError& error) {
        if (error.kind() != expected)
            throw std::runtime_error(std::string("load failure has kind ") +
                                     native::StorageError::kindName(error.kind()) + ": " + error.what());
        return true;
    }
    return false;
}

NetworkHydraulic requiredLoad(native::Repository& repository, std::string_view branch,
                              const QUuid& id) {
    const std::optional<NetworkHydraulic> loaded = hydraulic::loadHydraulicNetwork(repository, branch, id);
    if (!loaded) throw std::runtime_error("network not found");
    return *loaded;
}

NetworkHydraulic requiredLoadAtRevision(native::Repository& repository,
                                         const native::Hash& revisionId, const QUuid& networkId) {
    const std::optional<NetworkHydraulic> loaded =
        hydraulic::loadHydraulicNetworkAtRevisionLegacy(repository, revisionId, networkId);
    if (!loaded) throw std::runtime_error("historical network not found");
    return *loaded;
}

// First model publication starts from an empty, non-model-indexed genesis
// revision. Historical model APIs must remain strict without preventing that
// first save or treating an untouched branch as a corrupted network.
void exerciseEmptyRawBranch() {
    const std::filesystem::path folder = tempDir();
    try {
        native::Repository repository(folder);
        repository.createBranch("main");
        const native::Revision initial = repository.head("main");
        require(!initial.modelIndexed && initial.root == native::Hash{},
                "new branch should start with an empty raw revision");
        require(!hydraulic::loadHydraulicNetwork(repository, "main", uuid(1)), "new branch network lookup did not return clean not-found");
        require(!hydraulic::listHydraulicEntityIds(
                    repository, "main", uuid(1), hydraulic::HydraulicEntityType::MapLabel),
                "new branch membership lookup did not return clean not-found");
        require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] { (void)hydraulic::listHydraulicEntityIds(
                    repository, "main", uuid(1),
                    static_cast<hydraulic::HydraulicEntityType>(999)); }),
                "invalid collection selector passed on an empty branch");
        require(!hydraulic::loadHydraulicEntity(repository, "main", uuid(1), uuid(28)), "new branch entity lookup did not return clean not-found");

        require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] { (void)hydraulic::loadHydraulicNetworkAtRevisionLegacy(
                    repository, initial.id, uuid(1)); }),
                "explicit historical model read unexpectedly accepted a raw revision");

        const NetworkHydraulic network = completeNetwork();
        const native::Revision saved = hydraulic::saveHydraulicNetworkLegacy(repository, "main", network);
        require(saved.modelIndexed && saved.id != initial.id,
                "first hydraulic save did not publish a model-indexed revision");
        compareNetworks(network, requiredLoad(repository, "main", network.uuid));
        compareNetworks(network, requiredLoadAtRevision(repository, saved.id, network.uuid));
        (void)repository.verifyRepository();

        repository.createBranch("raw");
        repository.commit("raw", {{hydraulic::keyFromUuid(uuid(9000)),
                                   native::Bytes{'r', 'a', 'w'}}});
        const native::Hash rawHead = repository.head("raw").id;
        bool rejected = false;
        try {
            (void)hydraulic::saveHydraulicNetworkLegacy(repository, "raw", network);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected && repository.head("raw").id == rawHead,
                "hydraulic save accepted populated raw branch or advanced its head");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

void exercise() {
    const std::filesystem::path folder = tempDir();
    try {
        const NetworkHydraulic initial = completeNetwork();
        require(aowis::dvmdb::validateHydraulicNetwork(initial).valid(), "test network invalid");
        // Compact persistence edges must match the detailed reference walker.
        const native::HydraulicDependencyValidation compact =
            native::validateHydraulicNetworkWithDependencies(initial);
        require(compact.report.valid(), "compact validation rejected valid network");
        std::set<std::pair<native::Key, native::Key>> compactEdges;
        for (const native::HydraulicDependency& edge : compact.dependencies) {
            compactEdges.emplace(hydraulic::keyFromUuid(edge.source_uuid),
                                 hydraulic::keyFromUuid(edge.target_uuid));
        }
        std::set<std::pair<native::Key, native::Key>> detailedEdges;
        for (const native::HydraulicReference& edge : native::hydraulicReferences(initial)) {
            detailedEdges.emplace(hydraulic::keyFromUuid(edge.source_uuid),
                                  hydraulic::keyFromUuid(edge.target_uuid));
        }
        require(compactEdges == detailedEdges && !compactEdges.empty(),
                "compact reference walker diverged from canonical detailed references");
        NetworkHydraulic invalidCompactFixture = initial;
        invalidCompactFixture.links_pipes[0].node_uuid_from = uuid(9999);
        const native::HydraulicDependencyValidation invalidCompact =
            native::validateHydraulicNetworkWithDependencies(invalidCompactFixture);
        const native::ModelValidationReport invalidDetailed =
            native::validateHydraulicNetwork(invalidCompactFixture);
        require(!invalidDetailed.valid() && !invalidCompact.report.valid() &&
                invalidCompact.report.summary() == invalidDetailed.summary(),
                "compact validation accepted an invalid endpoint or changed diagnostics");
        native::Hash original{};
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            original = hydraulic::saveHydraulicNetworkLegacy(repository, "main", initial).id;
            require(repository.head("main").modelIndexed, "model index was not published");
            const std::optional<NetworkHydraulic> restored =
                hydraulic::loadHydraulicNetwork(repository, "main", initial.uuid);
            require(restored.has_value(), "first network load failed");
            compareNetworks(initial, *restored);
            // Fast indexed reads must return the canonical entity without
            // reconstructing the aggregate. Check all 18 collection kinds.
            std::size_t checkedCollection = 0;
#define CHECK_FAST_READ(name, type) \
            { \
                const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIds( \
                    repository, "main", initial.uuid, \
                    static_cast<hydraulic::HydraulicEntityType>(checkedCollection)); \
                require(ids.has_value() && *ids == [&]() { \
                    QList<QUuid> expected; \
                    for (const type& item : initial.name) expected.append(item.uuid); \
                    return expected; \
                }(), "fast membership list changed: " #name); \
                for (const type& expected : initial.name) { \
                    const std::optional<hydraulic::HydraulicEntity> actual = hydraulic::loadHydraulicEntity( \
                        repository, "main", initial.uuid, expected.uuid); \
                    require(actual.has_value(), "fast entity lookup missing: " #name); \
                    const type* entity = std::get_if<type>(&*actual); \
                    require(entity != nullptr && \
                        aowis::dvmdb::ModelCodec<type>::encode(*entity) == \
                        aowis::dvmdb::ModelCodec<type>::encode(expected), \
                        "fast entity lookup returned wrong payload: " #name); \
                } \
                ++checkedCollection; \
            }
            CHECK_FAST_READ(pipe_materials, HydraulicPipeMaterial)
            CHECK_FAST_READ(patterns_time, HydraulicPatternTime)
            CHECK_FAST_READ(curves_tank_volume, HydraulicCurveTankVolume)
            CHECK_FAST_READ(curves_pump_head, HydraulicCurvePumpHead)
            CHECK_FAST_READ(curves_pump_efficiency, HydraulicCurvePumpEfficiency)
            CHECK_FAST_READ(curves_valve_headloss, HydraulicCurveValveHeadloss)
            CHECK_FAST_READ(curves_valve_characteristic, HydraulicCurveValveCharacteristic)
            CHECK_FAST_READ(curves_generic, HydraulicCurveGeneric)
            CHECK_FAST_READ(nodes_reservoirs, HydraulicNodeReservoir)
            CHECK_FAST_READ(nodes_junctions, HydraulicNodeJunction)
            CHECK_FAST_READ(nodes_tanks, HydraulicNodeTank)
            CHECK_FAST_READ(demand_points, HydraulicDemandPoint)
            CHECK_FAST_READ(links_pipes, HydraulicLinkPipe)
            CHECK_FAST_READ(links_pumps, HydraulicLinkPump)
            CHECK_FAST_READ(links_valves, HydraulicLinkValve)
            CHECK_FAST_READ(controls_simple, HydraulicControlSimple)
            CHECK_FAST_READ(controls_rules, HydraulicControlRule)
            CHECK_FAST_READ(map_labels, HydraulicMapLabel)
#undef CHECK_FAST_READ
            require(checkedCollection == 18, "incomplete indexed entity read coverage");
            require(!hydraulic::loadHydraulicEntity(repository, "main", initial.uuid, uuid(999)), "unknown UUID unexpectedly resolved");
            require(!hydraulic::loadHydraulicEntity(repository, "main", uuid(999), uuid(23)), "entity unexpectedly belongs to another network");
            require(!hydraulic::listHydraulicEntityIds(repository, "main", uuid(999),
                     hydraulic::HydraulicEntityType::Pipe),
                    "unknown network unexpectedly listed members");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] { (void)hydraulic::listHydraulicEntityIds(repository, "main", initial.uuid,
                     static_cast<hydraulic::HydraulicEntityType>(99)); }),
                    "invalid collection kind not rejected");
            require(repository.referencingModel("main", hydraulic::keyFromUuid(uuid(19))).size() >= 2,
                    "inbound model references were not indexed");
            bool rejectedRawDeletion = false;
            try { repository.commitModel("main", {{hydraulic::keyFromUuid(uuid(19)), std::nullopt}}); }
            catch (const std::runtime_error&) { rejectedRawDeletion = true; }
            require(rejectedRawDeletion && repository.head("main").id == original,
                    "network membership allowed raw deletion of a listed entity");
            repository.createBranch("feature", "main");
            NetworkHydraulic modified = initial;
            modified.links_pipes[0].diameter_mm = 211.5;
            modified.map_labels.clear();
            modified.title_line_1 = QStringLiteral("Updated network title");
            // A raw Merkle diff includes storage-level membership chunks.
            // Removing the only label retires its chunk as well as its entity;
            // it is not just the three semantic changes (root, pipe, label).
            const native::Key labelKey = hydraulic::keyFromUuid(uuid(28));
            const std::vector<native::Key> labelOwners = repository.referencingModel("main", labelKey);
            require(labelOwners.size() == 1, "label must belong to exactly one membership chunk");
            const native::Key labelChunkKey = labelOwners.front();
            const std::optional<native::ModelRecord> labelChunk =
                repository.getModel("main", labelChunkKey);
            require(labelChunk.has_value() && labelChunk->references.size() == 1 &&
                    labelChunk->references.front() == labelKey,
                    "original label membership chunk is invalid");
            hydraulic::saveHydraulicNetworkLegacy(repository, "feature", modified);
            const std::vector<native::Difference> differences = repository.diff("main", "feature");
            require(differences.size() == 4,
                    "whole-network update must affect root, pipe, label and retired membership chunk");
            const auto containsChangedKey = [&differences](const native::Key& key) {
                return std::any_of(differences.begin(), differences.end(),
                                   [&key](const native::Difference& difference) {
                                       return difference.key == key;
                                   });
            };
            require(containsChangedKey(hydraulic::keyFromUuid(initial.uuid)) &&
                    containsChangedKey(hydraulic::keyFromUuid(uuid(23))) &&
                    containsChangedKey(labelKey) && containsChangedKey(labelChunkKey),
                    "whole-network update changed unexpected model records");
            require(repository.getModel("main", labelChunkKey).has_value() &&
                    !repository.getModel("feature", labelChunkKey).has_value(),
                    "membership chunk deletion did not preserve branch isolation");
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));
            compareNetworks(modified, requiredLoad(repository, "feature", initial.uuid));
            require(!repository.getModel("feature", hydraulic::keyFromUuid(uuid(28))),
                    "removed label still has a model record");
            const std::optional<hydraulic::HydraulicEntity> featurePipe =
                hydraulic::loadHydraulicEntity(repository, "feature", initial.uuid, uuid(23));
            require(featurePipe.has_value() &&
                    std::get<HydraulicLinkPipe>(*featurePipe).diameter_mm == 211.5,
                    "indexed entity read did not follow branch changes");
            require(!hydraulic::loadHydraulicEntity(repository, "feature", initial.uuid, uuid(28)), "deleted entity still appears as a member");
            require(hydraulic::listHydraulicEntityIds(repository, "feature", initial.uuid,
                    hydraulic::HydraulicEntityType::MapLabel)->empty(),
                    "deleted label still appears in ordered membership");
            require(hydraulic::loadHydraulicEntity(repository, "main", initial.uuid, uuid(28)).has_value(),
                    "branch isolation failed for indexed lookups");
            // Bounded physical editing of all four non-pipe hydraulic kinds.
            // These test objects have live inbound and outgoing references;
            // preserving structural links is essential for correctness.
            repository.createBranch("physical-indexed", "main");
            NetworkHydraulic physicalExpected = initial;
            HydraulicNodeReservoir updatedReservoir = initial.nodes_reservoirs[0];
            updatedReservoir.hydraulic_head_m = 147.5;
            updatedReservoir.terrain_elevation_m = 135.0;
            HydraulicNodeTank updatedTank = initial.nodes_tanks[0];
            updatedTank.water_level_initial_m = 4.25;
            updatedTank.water_level_maximum_m = 9.5;
            HydraulicLinkPump updatedPump = initial.links_pumps[0];
            updatedPump.initial_speed_ratio = 1.4;
            updatedPump.constant_power_kw = 34.0;
            HydraulicLinkValve updatedValve = initial.links_valves[0];
            updatedValve.setting_pressure_head_m = 16.5;
            updatedValve.diameter_mm = 240.0;
            hydraulic::HydraulicEditDiagnostics physicalStats;
            const native::Revision physicalBase = repository.head("physical-indexed");
            const native::ModelRecord physicalRootBefore = *repository.getModel(
                "physical-indexed", hydraulic::keyFromUuid(initial.uuid));
            const native::Revision physicalChanged = hydraulic::editHydraulicNetworkLegacy(
                repository, "physical-indexed", initial.uuid,
                {{updatedReservoir.uuid, hydraulic::HydraulicEntity{updatedReservoir}},
                 {updatedTank.uuid, hydraulic::HydraulicEntity{updatedTank}},
                 {updatedPump.uuid, hydraulic::HydraulicEntity{updatedPump}},
                 {updatedValve.uuid, hydraulic::HydraulicEntity{updatedValve}}}, &physicalStats);
            require(physicalStats.indexedPhysicalFastPath && physicalStats.validatedEntities == 4 &&
                    physicalChanged.id != physicalBase.id,
                    "physical parameter batch did not use bounded validation");
            physicalExpected.nodes_reservoirs[0] = updatedReservoir;
            physicalExpected.nodes_tanks[0] = updatedTank;
            physicalExpected.links_pumps[0] = updatedPump;
            physicalExpected.links_valves[0] = updatedValve;
            compareNetworks(physicalExpected, requiredLoad(repository, "physical-indexed", initial.uuid));
            require(repository.getModel("physical-indexed", hydraulic::keyFromUuid(initial.uuid))->payload ==
                    physicalRootBefore.payload,
                    "physical-only update rewrote network membership");
            require(hydraulic::editHydraulicNetworkLegacy(repository, "physical-indexed", initial.uuid,
                {{updatedTank.uuid, hydraulic::HydraulicEntity{updatedTank}}}, &physicalStats).id ==
                    physicalChanged.id && physicalStats.indexedPhysicalFastPath,
                    "identical tank update advanced revision");
            const std::vector<native::Key> reservoirIncoming = repository.referencingModel(
                "physical-indexed", hydraulic::keyFromUuid(updatedReservoir.uuid));
            require(std::find(reservoirIncoming.begin(), reservoirIncoming.end(),
                    hydraulic::keyFromUuid(updatedPump.uuid)) != reservoirIncoming.end(),
                    "physical update lost pump-to-reservoir reference");
            const std::vector<native::Key> tankIncoming = repository.referencingModel(
                "physical-indexed", hydraulic::keyFromUuid(updatedTank.uuid));
            require(std::find(tankIncoming.begin(), tankIncoming.end(),
                    hydraulic::keyFromUuid(updatedValve.uuid)) != tankIncoming.end(),
                    "physical update lost valve-to-tank reference");
            HydraulicLinkPump wrongCurve = updatedPump;
            wrongCurve.head_curve_uuid = uuid(12); // Tank-volume curve, not pump-head curve.
            bool rejectedPhysical = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "physical-indexed", initial.uuid,
                    {{wrongCurve.uuid, hydraulic::HydraulicEntity{wrongCurve}}}, &physicalStats);
            } catch (const std::exception&) { rejectedPhysical = true; }
            require(rejectedPhysical && !physicalStats.indexedPhysicalFastPath &&
                    repository.head("physical-indexed").id == physicalChanged.id,
                    "indexed physical path accepted a wrong-type structural link");
            HydraulicNodeTank wrongCurveTank = updatedTank;
            wrongCurveTank.volume_curve_uuid = uuid(13); // Pump-head curve, not tank-volume curve.
            rejectedPhysical = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "physical-indexed", initial.uuid,
                    {{wrongCurveTank.uuid, hydraulic::HydraulicEntity{wrongCurveTank}}}, &physicalStats);
            } catch (const std::exception&) { rejectedPhysical = true; }
            require(rejectedPhysical && !physicalStats.indexedPhysicalFastPath &&
                    repository.head("physical-indexed").id == physicalChanged.id,
                    "indexed physical path accepted a wrong-type tank curve");
            // A mixed update must fail atomically, even if one physical edit
            // would have qualified for the fast path by itself.
            HydraulicLinkPump mixedPump = updatedPump;
            mixedPump.constant_power_kw += 11.0;
            rejectedPhysical = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "physical-indexed", initial.uuid,
                    {{mixedPump.uuid, hydraulic::HydraulicEntity{mixedPump}},
                     {wrongCurveTank.uuid, hydraulic::HydraulicEntity{wrongCurveTank}}}, &physicalStats);
            } catch (const std::exception&) { rejectedPhysical = true; }
            const std::optional<hydraulic::HydraulicEntity> pumpAfterRejected =
                hydraulic::loadHydraulicEntity(repository, "physical-indexed", initial.uuid, mixedPump.uuid);
            require(rejectedPhysical && !physicalStats.indexedPhysicalFastPath &&
                    repository.head("physical-indexed").id == physicalChanged.id &&
                    pumpAfterRejected.has_value() &&
                    std::get<HydraulicLinkPump>(*pumpAfterRejected).constant_power_kw ==
                        updatedPump.constant_power_kw,
                    "mixed physical/invalid reference batch partially committed");
            HydraulicLinkValve structuralValve = updatedValve;
            structuralValve.node_uuid_from = uuid(18);
            const native::Revision fallbackPhysical = hydraulic::editHydraulicNetworkLegacy(
                repository, "physical-indexed", initial.uuid,
                {{structuralValve.uuid, hydraulic::HydraulicEntity{structuralValve}}}, &physicalStats);
            require(!physicalStats.indexedPhysicalFastPath &&
                    fallbackPhysical.id != physicalChanged.id,
                    "valve endpoint rewire bypassed canonical validation");
            physicalExpected.links_valves[0] = structuralValve;
            compareNetworks(physicalExpected, requiredLoad(repository, "physical-indexed", initial.uuid));
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));

            // Native indexed structural insertion/deletion for reservoirs,
            // tanks, pumps, and valves. References may point to other members
            // newly inserted in the SAME atomic batch, independent of order.
            repository.createBranch("structural-indexed", "main");
            hydraulic::HydraulicEditDiagnostics structuralStats;
            HydraulicNodeReservoir newReservoir = item<HydraulicNodeReservoir>(33001);
            newReservoir.head_pattern_uuid = uuid(11);
            newReservoir.quality_source.pattern_uuid = uuid(11);
            HydraulicNodeTank newTank = item<HydraulicNodeTank>(33002);
            newTank.volume_curve_uuid = uuid(12);
            newTank.quality_source.pattern_uuid = uuid(11);
            HydraulicLinkPump newPump = item<HydraulicLinkPump>(33003);
            newPump.node_uuid_from = newReservoir.uuid;
            newPump.node_uuid_to = newTank.uuid;
            newPump.head_curve_uuid = uuid(13);
            newPump.speed_pattern_uuid = uuid(11);
            newPump.efficiency_curve_uuid = uuid(14);
            newPump.price_pattern_uuid = uuid(11);
            HydraulicLinkValve newValve = item<HydraulicLinkValve>(33004);
            newValve.node_uuid_from = newTank.uuid;
            newValve.node_uuid_to = uuid(19);
            newValve.head_loss_curve_uuid = uuid(15);
            newValve.characteristic_curve_uuid = uuid(16);
            const std::vector<hydraulic::HydraulicEntityChange> structuralInsertions = {
                {newPump.uuid, hydraulic::HydraulicEntity{newPump}},
                {newValve.uuid, hydraulic::HydraulicEntity{newValve}},
                {newTank.uuid, hydraulic::HydraulicEntity{newTank}},
                {newReservoir.uuid, hydraulic::HydraulicEntity{newReservoir}}
            };
            const native::Revision structuralInserted = hydraulic::editHydraulicNetworkLegacy(
                repository, "structural-indexed", initial.uuid, structuralInsertions, &structuralStats);
            require(structuralStats.indexedMembershipFastPath && structuralStats.validatedEntities == 4 &&
                    structuralInserted.id != repository.head("main").id,
                    "structural insertion did not use indexed membership validation");
            NetworkHydraulic structuralExpected = initial;
            structuralExpected.nodes_reservoirs.append(newReservoir);
            structuralExpected.nodes_tanks.append(newTank);
            structuralExpected.links_pumps.append(newPump);
            structuralExpected.links_valves.append(newValve);
            compareNetworks(structuralExpected,
                requiredLoad(repository, "structural-indexed", initial.uuid));
            require(aowis::dvmdb::validateHydraulicNetwork(structuralExpected).valid(),
                    "indexed structural insertion bypassed canonical validator");
            const std::vector<native::Key> pumpNodeIncoming = repository.referencingModel(
                "structural-indexed", hydraulic::keyFromUuid(newReservoir.uuid));
            require(std::find(pumpNodeIncoming.begin(), pumpNodeIncoming.end(),
                        hydraulic::keyFromUuid(newPump.uuid)) != pumpNodeIncoming.end(),
                    "structural insertion lost pump-to-reservoir reverse reference");
            const std::vector<native::Key> tankIncomingAfterInsert = repository.referencingModel(
                "structural-indexed", hydraulic::keyFromUuid(newTank.uuid));
            require(std::find(tankIncomingAfterInsert.begin(), tankIncomingAfterInsert.end(),
                        hydraulic::keyFromUuid(newPump.uuid)) != tankIncomingAfterInsert.end() &&
                    std::find(tankIncomingAfterInsert.begin(), tankIncomingAfterInsert.end(),
                        hydraulic::keyFromUuid(newValve.uuid)) != tankIncomingAfterInsert.end(),
                    "structural insertion lost cross-batch references");

            // A valve/pump endpoint and each curve/pattern is checked against
            // exact target kinds, not just UUID presence or incoming edges.
            HydraulicLinkPump invalidPump = item<HydraulicLinkPump>(33005);
            invalidPump.node_uuid_from = newReservoir.uuid;
            invalidPump.node_uuid_to = newTank.uuid;
            invalidPump.head_curve_uuid = uuid(12); // Wrong: tank-volume curve.
            const native::Revision structuralHead = repository.head("structural-indexed");
            bool rejectedStructural = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "structural-indexed", initial.uuid,
                    {{invalidPump.uuid, hydraulic::HydraulicEntity{invalidPump}}}, &structuralStats);
            } catch (const std::exception&) { rejectedStructural = true; }
            require(rejectedStructural && !structuralStats.indexedMembershipFastPath &&
                    repository.head("structural-indexed").id == structuralHead.id,
                    "wrong-type structural insertion bypassed validation");
            HydraulicNodeTank invalidTank = item<HydraulicNodeTank>(33006);
            invalidTank.volume_curve_uuid = uuid(13); // Wrong: pump-head curve.
            rejectedStructural = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "structural-indexed", initial.uuid,
                    {{invalidTank.uuid, hydraulic::HydraulicEntity{invalidTank}}}, &structuralStats);
            } catch (const std::exception&) { rejectedStructural = true; }
            require(rejectedStructural && !structuralStats.indexedMembershipFastPath &&
                    repository.head("structural-indexed").id == structuralHead.id,
                    "wrong-type tank curve bypassed validation");
            // Even a target created in the same atomic batch must have the
            // right kind; a new tank-volume curve is not a pump-head curve.
            HydraulicCurveTankVolume insertedWrongCurve =
                item<HydraulicCurveTankVolume>(33007);
            HydraulicLinkPump wrongBatchPump = item<HydraulicLinkPump>(33008);
            wrongBatchPump.node_uuid_from = newReservoir.uuid;
            wrongBatchPump.node_uuid_to = newTank.uuid;
            wrongBatchPump.head_curve_uuid = insertedWrongCurve.uuid;
            rejectedStructural = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "structural-indexed", initial.uuid,
                    {{wrongBatchPump.uuid, hydraulic::HydraulicEntity{wrongBatchPump}},
                     {insertedWrongCurve.uuid, hydraulic::HydraulicEntity{insertedWrongCurve}}},
                    &structuralStats);
            } catch (const std::exception&) { rejectedStructural = true; }
            require(rejectedStructural && !structuralStats.indexedMembershipFastPath &&
                    repository.head("structural-indexed").id == structuralHead.id &&
                    !repository.getModel("structural-indexed", hydraulic::keyFromUuid(insertedWrongCurve.uuid)),
                    "cross-batch wrong-kind reference partially committed");
            // Inbound references from live entities forbid deleting a node by
            // itself, and canonical control rules protect the original pump.
            rejectedStructural = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "structural-indexed", initial.uuid,
                    {{newTank.uuid, std::nullopt}}, &structuralStats);
            } catch (const std::exception&) { rejectedStructural = true; }
            require(rejectedStructural && repository.head("structural-indexed").id ==
                    structuralHead.id, "referenced tank was deleted");
            rejectedStructural = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "structural-indexed", initial.uuid,
                    {{uuid(24), std::nullopt}}, &structuralStats);
            } catch (const std::exception&) { rejectedStructural = true; }
            require(rejectedStructural && repository.head("structural-indexed").id ==
                    structuralHead.id, "control-rule referenced pump was deleted");

            // Network metadata is not represented in the ordinary incoming
            // model index. MSX references to a tank must still block deletion.
            repository.createBranch("structural-msx", "structural-indexed");
            NetworkHydraulic protectedNetwork = structuralExpected;
            MultiSpeciesParameterOverrideTank tankOverride;
            tankOverride.tank_uuid = newTank.uuid;
            protectedNetwork.multi_species.parameter_overrides_tanks.append(tankOverride);
            // Empty parameter UUID is legal in the canonical validator.
            require(aowis::dvmdb::validateHydraulicNetwork(protectedNetwork).valid(),
                    "structural MSX fixture invalid");
            hydraulic::saveHydraulicNetworkLegacy(repository, "structural-msx", protectedNetwork);
            rejectedStructural = false;
            const native::Revision protectedHead = repository.head("structural-msx");
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "structural-msx", initial.uuid,
                    {{newPump.uuid, std::nullopt}, {newValve.uuid, std::nullopt},
                     {newTank.uuid, std::nullopt}, {newReservoir.uuid, std::nullopt}},
                    &structuralStats);
            } catch (const std::exception&) { rejectedStructural = true; }
            require(rejectedStructural && !structuralStats.indexedMembershipFastPath &&
                    repository.head("structural-msx").id == protectedHead.id,
                    "MSX metadata dependency bypassed structural deletion guard");

            // All model-level dependencies are removed in one revision. The
            // original and MSX-protected branches remain unchanged.
            repository.createBranch("structural-removed", "structural-indexed");
            const native::Revision structuralRemoved = hydraulic::editHydraulicNetworkLegacy(
                repository, "structural-removed", initial.uuid,
                {{newPump.uuid, std::nullopt}, {newValve.uuid, std::nullopt},
                 {newTank.uuid, std::nullopt}, {newReservoir.uuid, std::nullopt}},
                &structuralStats);
            require(structuralStats.indexedMembershipFastPath &&
                    structuralStats.validatedEntities == 4 && structuralRemoved.id != structuralInserted.id,
                    "related structural deletion missed indexed path");
            compareNetworks(initial, requiredLoad(repository, "structural-removed", initial.uuid));
            compareNetworks(structuralExpected,
                requiredLoad(repository, "structural-indexed", initial.uuid));
            require(!repository.getModel("structural-removed", hydraulic::keyFromUuid(newTank.uuid)) &&
                    repository.getModel("structural-indexed", hydraulic::keyFromUuid(newTank.uuid)),
                    "branch-local structural deletion corrupted another branch");

            repository.createBranch("corrupt-query", "main");
            const std::optional<native::ModelRecord> validLabel =
                repository.getModel("corrupt-query", hydraulic::keyFromUuid(uuid(28)));
            require(validLabel.has_value(), "test label disappeared");
            native::ModelRecord malformedLabel = *validLabel;
            malformedLabel.kind = 999;
            repository.commitModel("corrupt-query", {
                {hydraulic::keyFromUuid(uuid(28)), std::move(malformedLabel)}});
            require(throwsStorageError(native::StorageErrorKind::Corruption, [&] { (void)hydraulic::loadHydraulicEntity(repository, "corrupt-query", initial.uuid,
                    uuid(28)); }),
                    "indexed query accepted an unknown entity kind");
            require(hydraulic::loadHydraulicEntity(repository, "main", initial.uuid, uuid(28)).has_value(),
                    "malformed branch damaged the original branch");

            // Fast indexed scalar edits must NOT reconstruct the entire
            // network, modify its membership, or lose typed reference checks.
            repository.createBranch("scalar-indexed", "main");
            const native::ModelRecord scalarManifest = *repository.getModel(
                "scalar-indexed", hydraulic::keyFromUuid(initial.uuid));
            NetworkHydraulic scalarExpected = initial;
            scalarExpected.links_pipes[0].diameter_mm = 143.25;
            scalarExpected.nodes_junctions[0].elevation_m = 17.0;
            scalarExpected.map_labels[0].text = QStringLiteral("Renamed label");
            hydraulic::HydraulicEditDiagnostics indexedStats;
            const native::Revision scalarRevision = hydraulic::editHydraulicNetworkLegacy(
                repository, "scalar-indexed", initial.uuid,
                {{uuid(23), hydraulic::HydraulicEntity{scalarExpected.links_pipes[0]}},
                 {uuid(19), hydraulic::HydraulicEntity{scalarExpected.nodes_junctions[0]}},
                 {uuid(28), hydraulic::HydraulicEntity{scalarExpected.map_labels[0]}}},
                &indexedStats);
            require(indexedStats.indexedScalarFastPath && indexedStats.validatedEntities == 3,
                    "safe scalar changes did not take indexed validation path");
            compareNetworks(scalarExpected, requiredLoad(repository, "scalar-indexed", initial.uuid));
            const std::optional<native::ModelRecord> scalarManifestAfter = repository.getModel(
                "scalar-indexed", hydraulic::keyFromUuid(initial.uuid));
            require(scalarManifestAfter &&
                    native::encodeModelRecord(*scalarManifestAfter) ==
                        native::encodeModelRecord(scalarManifest),
                    "indexed scalar edit changed the membership manifest");
            require(hydraulic::editHydraulicNetworkLegacy(repository, "scalar-indexed", initial.uuid,
                    {{uuid(23), hydraulic::HydraulicEntity{scalarExpected.links_pipes[0]}}},
                    &indexedStats).id == scalarRevision.id && indexedStats.indexedScalarFastPath,
                    "no-op scalar edit did not preserve revision identity");

            // An endpoint change is never an indexed-scalar edit, including
            // when a caller sneaks it into an otherwise scalar batch.
            HydraulicLinkPipe invalidRewire = scalarExpected.links_pipes[0];
            invalidRewire.node_uuid_from = uuid(9009);
            bool rejectedScalar = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "scalar-indexed", initial.uuid,
                    {{uuid(23), hydraulic::HydraulicEntity{invalidRewire}}}, &indexedStats);
            } catch (const std::exception&) { rejectedScalar = true; }
            require(rejectedScalar && !indexedStats.indexedScalarFastPath &&
                    repository.head("scalar-indexed").id == scalarRevision.id,
                    "invalid topology bypassed full network validation");
            // Metadata and graph-affecting edits use canonical full validation.
            HydraulicLinkPipe validRewire = scalarExpected.links_pipes[0];
            validRewire.node_uuid_from = uuid(18);
            const native::Revision rewiredRevision = hydraulic::editHydraulicNetworkLegacy(
                repository, "scalar-indexed", initial.uuid,
                {{uuid(23), hydraulic::HydraulicEntity{validRewire}}}, &indexedStats);
            require(!indexedStats.indexedScalarFastPath && rewiredRevision.id != scalarRevision.id,
                    "topology edit incorrectly used scalar validation");
            scalarExpected.links_pipes[0] = validRewire;
            compareNetworks(scalarExpected, requiredLoad(repository, "scalar-indexed", initial.uuid));
            // Stage 23: independent pipe endpoint rewires can be validated
            // using the reverse-reference and typed membership indexes.
            repository.createBranch("topology-indexed", "main");
            NetworkHydraulic unreferenced = initial;
            unreferenced.controls_simple.clear(); // Remove the inbound control reference.
            hydraulic::saveHydraulicNetworkLegacy(repository, "topology-indexed", unreferenced);
            const native::ModelRecord topologyManifest = *repository.getModel(
                "topology-indexed", hydraulic::keyFromUuid(unreferenced.uuid));
            const native::Revision topologyBase = repository.head("topology-indexed");
            HydraulicLinkPipe independentPipe = unreferenced.links_pipes[0];
            independentPipe.node_uuid_from = uuid(18); // Owned reservoir, valid node endpoint.
            const native::Revision topologyRevision = hydraulic::editHydraulicNetworkLegacy(
                repository, "topology-indexed", initial.uuid,
                {{independentPipe.uuid, hydraulic::HydraulicEntity{independentPipe}}}, &indexedStats);
            require(indexedStats.indexedTopologyFastPath &&
                    !indexedStats.indexedScalarFastPath && indexedStats.validatedEntities == 1 &&
                    topologyRevision.id != topologyBase.id,
                    "independent rewire did not take bounded topology validation");
            unreferenced.links_pipes[0] = independentPipe;
            compareNetworks(unreferenced, requiredLoad(repository, "topology-indexed", initial.uuid));
            require(native::encodeModelRecord(*repository.getModel(
                        "topology-indexed", hydraulic::keyFromUuid(initial.uuid))) ==
                    native::encodeModelRecord(topologyManifest),
                    "indexed topology change rewrote network membership");
            const std::vector<native::Key> oldEndpointRefs = repository.referencingModel(
                "topology-indexed", hydraulic::keyFromUuid(uuid(19)));
            require(std::find(oldEndpointRefs.begin(), oldEndpointRefs.end(),
                    hydraulic::keyFromUuid(uuid(23))) == oldEndpointRefs.end(),
                    "indexed rewire retained old endpoint edge");
            const std::vector<native::Key> newEndpointRefs = repository.referencingModel(
                "topology-indexed", hydraulic::keyFromUuid(uuid(18)));
            require(std::find(newEndpointRefs.begin(), newEndpointRefs.end(),
                    hydraulic::keyFromUuid(uuid(23))) != newEndpointRefs.end(),
                    "indexed rewire did not create new endpoint edge");
            bool rejectedTopology = false;
            HydraulicLinkPipe wrongNodeKind = independentPipe;
            wrongNodeKind.node_uuid_from = uuid(10); // A pipe material, not a node.
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "topology-indexed", initial.uuid,
                    {{wrongNodeKind.uuid, hydraulic::HydraulicEntity{wrongNodeKind}}}, &indexedStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !indexedStats.indexedTopologyFastPath &&
                    repository.head("topology-indexed").id == topologyRevision.id,
                    "wrong-type endpoint bypassed typed validation");
            HydraulicLinkPipe missingEndpoint = independentPipe;
            missingEndpoint.node_uuid_from = uuid(9009);
            rejectedTopology = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "topology-indexed", initial.uuid,
                    {{missingEndpoint.uuid, hydraulic::HydraulicEntity{missingEndpoint}}}, &indexedStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !indexedStats.indexedTopologyFastPath &&
                    repository.head("topology-indexed").id == topologyRevision.id,
                    "missing endpoint unexpectedly committed");
            HydraulicLinkPipe metadataRewire = independentPipe;
            metadataRewire.node_uuid_from = uuid(19);
            metadataRewire.id = QStringLiteral("Changed pipe ID");
            const native::Revision canonicalFallback = hydraulic::editHydraulicNetworkLegacy(
                repository, "topology-indexed", initial.uuid,
                {{metadataRewire.uuid, hydraulic::HydraulicEntity{metadataRewire}}}, &indexedStats);
            require(!indexedStats.indexedTopologyFastPath &&
                    !indexedStats.indexedScalarFastPath &&
                    canonicalFallback.id != topologyRevision.id,
                    "unsupported rewire metadata did not use canonical validation");
            unreferenced.links_pipes[0] = metadataRewire;
            compareNetworks(unreferenced, requiredLoad(repository, "topology-indexed", initial.uuid));
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));

            // A demand point's assigned junction must remain an endpoint of its
            // attached pipe: its inbound dependency forces full validation.
            repository.createBranch("attached-pipe", "main");
            NetworkHydraulic attached = initial;
            attached.demand_points[0].attachment.type = HydraulicDemandPointAttachmentType::Pipe;
            attached.demand_points[0].attachment.pipe_uuid = uuid(23);
            attached.demand_points[0].attachment.pipe_allocation_mode =
                HydraulicDemandPointPipeAllocationMode::AssignedJunction;
            attached.demand_points[0].attachment.pipe_assigned_junction_uuid = uuid(19);
            hydraulic::saveHydraulicNetworkLegacy(repository, "attached-pipe", attached);
            const native::Revision attachedHead = repository.head("attached-pipe");
            HydraulicLinkPipe invalidAttachedRewire = attached.links_pipes[0];
            invalidAttachedRewire.node_uuid_from = uuid(18);
            rejectedTopology = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "attached-pipe", initial.uuid,
                    {{invalidAttachedRewire.uuid, hydraulic::HydraulicEntity{invalidAttachedRewire}}},
                    &indexedStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !indexedStats.indexedTopologyFastPath &&
                    repository.head("attached-pipe").id == attachedHead.id,
                    "dependent demand-point constraint was bypassed");

            // The fast path must also reject any ownership ambiguity.
            const native::Revision otherNetworkRevision = repository.head("main");
            hydraulic::HydraulicEditDiagnostics ownershipStats;
            rejectedScalar = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "main", uuid(9000),
                    {{uuid(23), hydraulic::HydraulicEntity{scalarExpected.links_pipes[0]}}},
                    &ownershipStats);
            } catch (const std::exception&) { rejectedScalar = true; }
            require(rejectedScalar && repository.head("main").id == otherNetworkRevision.id,
                    "indexed scalar edit bypassed network ownership");

            // Membership edits use a bounded, typed reference check plus one
            // ordered UUID manifest rewrite. All three supported insert kinds
            // can reference members inserted in the same atomic batch.
            repository.createBranch("membership-indexed", "main");
            NetworkHydraulic membershipExpected = initial;
            HydraulicNodeJunction membershipNode = item<HydraulicNodeJunction>(30110);
            HydraulicLinkPipe membershipPipe = item<HydraulicLinkPipe>(30111);
            membershipPipe.node_uuid_from = uuid(19);
            membershipPipe.node_uuid_to = membershipNode.uuid;
            membershipPipe.material_uuid = uuid(10);
            HydraulicMapLabel membershipLabel = item<HydraulicMapLabel>(30112);
            membershipLabel.anchor_node_uuid = membershipNode.uuid;
            membershipLabel.text = QStringLiteral("New well");
            hydraulic::HydraulicEditDiagnostics membershipStats;
            const native::Revision membershipBase = repository.head("membership-indexed");
            const native::Revision membershipRevision = hydraulic::editHydraulicNetworkLegacy(
                repository, "membership-indexed", initial.uuid,
                {{membershipPipe.uuid, hydraulic::HydraulicEntity{membershipPipe}},
                 {membershipNode.uuid, hydraulic::HydraulicEntity{membershipNode}},
                 {membershipLabel.uuid, hydraulic::HydraulicEntity{membershipLabel}}}, &membershipStats);
            require(membershipStats.indexedMembershipFastPath &&
                    !membershipStats.indexedScalarFastPath &&
                    !membershipStats.indexedTopologyFastPath &&
                    membershipStats.validatedEntities == 3 &&
                    membershipRevision.id != membershipBase.id,
                    "new junction/pipe/label batch missed indexed membership validation");
            membershipExpected.nodes_junctions.append(membershipNode);
            membershipExpected.links_pipes.append(membershipPipe);
            membershipExpected.map_labels.append(membershipLabel);
            compareNetworks(membershipExpected,
                requiredLoad(repository, "membership-indexed", initial.uuid));
            const std::vector<native::Key> memberIncoming = repository.referencingModel(
                "membership-indexed", hydraulic::keyFromUuid(membershipNode.uuid));
            require(std::find(memberIncoming.begin(), memberIncoming.end(),
                    hydraulic::keyFromUuid(membershipPipe.uuid)) != memberIncoming.end() &&
                    std::find(memberIncoming.begin(), memberIncoming.end(),
                    hydraulic::keyFromUuid(membershipLabel.uuid)) != memberIncoming.end(),
                    "new member reference edges were not committed");
            const native::Revision afterRemoval = hydraulic::editHydraulicNetworkLegacy(
                repository, "membership-indexed", initial.uuid,
                {{membershipLabel.uuid, std::nullopt}}, &membershipStats);
            require(membershipStats.indexedMembershipFastPath &&
                    membershipStats.validatedEntities == 1 && afterRemoval.id != membershipRevision.id,
                    "independent label deletion missed indexed membership validation");
            membershipExpected.map_labels.removeLast();
            compareNetworks(membershipExpected,
                requiredLoad(repository, "membership-indexed", initial.uuid));
            require(!repository.getModel("membership-indexed", hydraulic::keyFromUuid(membershipLabel.uuid)),
                    "deleted indexed label remains stored");
            const native::Revision beforeBadMembership = repository.head("membership-indexed");
            HydraulicLinkPipe badMembershipPipe = item<HydraulicLinkPipe>(30113);
            badMembershipPipe.node_uuid_from = uuid(10); // Material, not a node.
            badMembershipPipe.node_uuid_to = membershipNode.uuid;
            rejectedTopology = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "membership-indexed", initial.uuid,
                    {{badMembershipPipe.uuid, hydraulic::HydraulicEntity{badMembershipPipe}}},
                    &membershipStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !membershipStats.indexedMembershipFastPath &&
                    repository.head("membership-indexed").id == beforeBadMembership.id,
                    "wrong-kind inserted endpoint passed indexed validation");
            HydraulicNodeJunction badPatternNode = item<HydraulicNodeJunction>(30114);
            HydraulicDemand invalidDemand;
            invalidDemand.pattern_uuid = uuid(30115);
            badPatternNode.demands.append(invalidDemand);
            rejectedTopology = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "membership-indexed", initial.uuid,
                    {{badPatternNode.uuid, hydraulic::HydraulicEntity{badPatternNode}}},
                    &membershipStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !membershipStats.indexedMembershipFastPath &&
                    repository.head("membership-indexed").id == beforeBadMembership.id,
                    "missing junction pattern passed indexed validation");
            rejectedTopology = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "membership-indexed", initial.uuid,
                    {{membershipNode.uuid, std::nullopt}}, &membershipStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !membershipStats.indexedMembershipFastPath &&
                    repository.head("membership-indexed").id == beforeBadMembership.id,
                    "referenced junction deletion bypassed canonical validation");
            // A pipe and its incident junction can be removed together without
            // reconstructing the entire network. The pipe's incoming edge to
            // the junction is safe only because BOTH are removed atomically.
            repository.createBranch("membership-cascade", "membership-indexed");
            hydraulic::HydraulicEditDiagnostics removalStats;
            const native::Revision beforeCascade = repository.head("membership-cascade");
            const native::Revision afterCascade = hydraulic::editHydraulicNetworkLegacy(
                repository, "membership-cascade", initial.uuid,
                {{membershipNode.uuid, std::nullopt}, {membershipPipe.uuid, std::nullopt}},
                &removalStats);
            require(removalStats.indexedMembershipFastPath &&
                    removalStats.validatedEntities == 2 && afterCascade.id != beforeCascade.id,
                    "dependent pipe/junction batch missed indexed deletion");
            compareNetworks(initial, requiredLoad(repository, "membership-cascade", initial.uuid));
            require(!repository.getModel("membership-cascade", hydraulic::keyFromUuid(membershipNode.uuid)) &&
                    !repository.getModel("membership-cascade", hydraulic::keyFromUuid(membershipPipe.uuid)),
                    "indexed deleted objects still exist in revision");
            require(repository.getModel("membership-indexed", hydraulic::keyFromUuid(membershipNode.uuid)) &&
                    repository.getModel("membership-indexed", hydraulic::keyFromUuid(membershipPipe.uuid)),
                    "indexed deletion changed original branch");

            // Embedded network metadata can refer to a pipe without a native
            // incoming-reference edge. The fast deletion MUST reject it.
            repository.createBranch("membership-metadata", "membership-indexed");
            NetworkHydraulic metadataProtected = membershipExpected;
            metadataProtected.options_report.selection_links.mode =
                HydraulicSimulationReportSelectionMode::Selected;
            metadataProtected.options_report.selection_links.uuids.append(membershipPipe.uuid);
            hydraulic::saveHydraulicNetworkLegacy(repository, "membership-metadata", metadataProtected);
            const native::Revision metadataProtectedHead = repository.head("membership-metadata");
            bool rejectedMetadataDeletion = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "membership-metadata", initial.uuid,
                    {{membershipPipe.uuid, std::nullopt}, {membershipNode.uuid, std::nullopt}},
                    &removalStats);
            } catch (const std::exception&) { rejectedMetadataDeletion = true; }
            require(rejectedMetadataDeletion && !removalStats.indexedMembershipFastPath &&
                    repository.head("membership-metadata").id == metadataProtectedHead.id,
                    "network metadata reference bypassed indexed deletion protection");

            // MSX node references are also embedded in network metadata;
            // they cannot be detected using ordinary incoming object edges.
            repository.createBranch("membership-msx", "membership-indexed");
            NetworkHydraulic msxProtected = membershipExpected;
            MultiSpeciesNodeInitialQuality initialQuality;
            initialQuality.node_uuid = membershipNode.uuid;
            initialQuality.species_uuid = msxProtected.multi_species.species[0].uuid;
            msxProtected.multi_species.initial_quality_nodes.append(initialQuality);
            hydraulic::saveHydraulicNetworkLegacy(repository, "membership-msx", msxProtected);
            const native::Revision msxHead = repository.head("membership-msx");
            bool rejectedMsxDeletion = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "membership-msx", initial.uuid,
                    {{membershipPipe.uuid, std::nullopt}, {membershipNode.uuid, std::nullopt}},
                    &removalStats);
            } catch (const std::exception&) { rejectedMsxDeletion = true; }
            require(rejectedMsxDeletion && !removalStats.indexedMembershipFastPath &&
                    repository.head("membership-msx").id == msxHead.id,
                    "MSX metadata reference bypassed indexed deletion protection");

            // A new pipe cannot claim a junction simultaneously being removed.
            repository.createBranch("membership-replace", "membership-indexed");
            HydraulicLinkPipe invalidReplacement = item<HydraulicLinkPipe>(30119);
            invalidReplacement.node_uuid_from = membershipNode.uuid;
            invalidReplacement.node_uuid_to = uuid(19);
            const native::Revision replaceHead = repository.head("membership-replace");
            bool rejectedMixedBatch = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "membership-replace", initial.uuid,
                    {{membershipPipe.uuid, std::nullopt},
                     {membershipNode.uuid, std::nullopt},
                     {invalidReplacement.uuid, hydraulic::HydraulicEntity{invalidReplacement}}},
                    &removalStats);
            } catch (const std::exception&) { rejectedMixedBatch = true; }
            require(rejectedMixedBatch && !removalStats.indexedMembershipFastPath &&
                    repository.head("membership-replace").id == replaceHead.id,
                    "inserted pipe referenced a node removed in the same commit");

            // Deleting a standalone pipe is independently eligible, without
            // forcing removal of a junction that is still a model member.
            repository.createBranch("membership-delete-pipe", "membership-indexed");
            const native::Revision pipeRemoved = hydraulic::editHydraulicNetworkLegacy(
                repository, "membership-delete-pipe", initial.uuid,
                {{membershipPipe.uuid, std::nullopt}}, &removalStats);
            require(removalStats.indexedMembershipFastPath && pipeRemoved.id != afterRemoval.id,
                    "standalone pipe removal did not use indexed membership validation");
            NetworkHydraulic noAddedPipe = membershipExpected;
            noAddedPipe.links_pipes.removeLast();
            compareNetworks(noAddedPipe, requiredLoad(repository, "membership-delete-pipe", initial.uuid));
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));

            // All eight catalogue collections have no outgoing model refs.
            // Changing their value preserves typed incoming edges; membership
            // changes use bounded indexed chunks and must remain branch-local.
            repository.createBranch("catalogue-indexed", "main");
            NetworkHydraulic catalogueExpected = initial;
            HydraulicPipeMaterial materialEdit = catalogueExpected.pipe_materials[0];
            materialEdit.id = QStringLiteral("catalogue-material-revised");
            HydraulicPatternTime patternEdit = catalogueExpected.patterns_time[0];
            patternEdit.multipliers = {0.8, 1.1, 1.3, 1.2};
            const std::optional<native::ModelRecord> catalogueManifestBefore =
                repository.getModel("catalogue-indexed", hydraulic::keyFromUuid(initial.uuid));
            require(catalogueManifestBefore.has_value(), "missing catalogue membership manifest");
            hydraulic::HydraulicEditDiagnostics catalogueStats;
            const native::Revision catalogueChanged = hydraulic::editHydraulicNetworkLegacy(
                repository, "catalogue-indexed", initial.uuid,
                {{materialEdit.uuid, hydraulic::HydraulicEntity{materialEdit}},
                 {patternEdit.uuid, hydraulic::HydraulicEntity{patternEdit}}}, &catalogueStats);
            require(catalogueStats.indexedCatalogueFastPath && catalogueStats.validatedEntities == 2,
                    "catalogue update reconstructed full network");
            catalogueExpected.pipe_materials[0] = materialEdit;
            catalogueExpected.patterns_time[0] = patternEdit;
            compareNetworks(catalogueExpected,
                requiredLoad(repository, "catalogue-indexed", initial.uuid));
            require(repository.getModel("catalogue-indexed", hydraulic::keyFromUuid(initial.uuid))->payload ==
                    catalogueManifestBefore->payload, "catalogue-only update rewrote membership");
            require(hydraulic::editHydraulicNetworkLegacy(repository, "catalogue-indexed", initial.uuid,
                        {{materialEdit.uuid, hydraulic::HydraulicEntity{materialEdit}}},
                        &catalogueStats).id == catalogueChanged.id &&
                    catalogueStats.indexedCatalogueFastPath,
                    "identical catalogue update advanced revision");
            const std::vector<native::Key> catalogueIncoming = repository.referencingModel(
                "catalogue-indexed", hydraulic::keyFromUuid(materialEdit.uuid));
            require(std::find(catalogueIncoming.begin(), catalogueIncoming.end(),
                    hydraulic::keyFromUuid(uuid(23))) != catalogueIncoming.end(),
                    "catalogue update discarded existing pipe reference");
            HydraulicPipeMaterial addedMaterial = item<HydraulicPipeMaterial>(31001);
            HydraulicPatternTime addedPattern = item<HydraulicPatternTime>(31002);
            HydraulicCurveTankVolume addedTankCurve = item<HydraulicCurveTankVolume>(31003);
            HydraulicCurvePumpHead addedPumpCurve = item<HydraulicCurvePumpHead>(31004);
            HydraulicCurvePumpEfficiency addedEfficiencyCurve = item<HydraulicCurvePumpEfficiency>(31005);
            HydraulicCurveValveHeadloss addedHeadlossCurve = item<HydraulicCurveValveHeadloss>(31006);
            HydraulicCurveValveCharacteristic addedCharacteristicCurve = item<HydraulicCurveValveCharacteristic>(31007);
            HydraulicCurveGeneric addedGenericCurve = item<HydraulicCurveGeneric>(31008);
            const std::vector<hydraulic::HydraulicEntityChange> catalogueInsertions = {
                {addedGenericCurve.uuid, hydraulic::HydraulicEntity{addedGenericCurve}},
                {addedMaterial.uuid, hydraulic::HydraulicEntity{addedMaterial}},
                {addedPattern.uuid, hydraulic::HydraulicEntity{addedPattern}},
                {addedCharacteristicCurve.uuid, hydraulic::HydraulicEntity{addedCharacteristicCurve}},
                {addedTankCurve.uuid, hydraulic::HydraulicEntity{addedTankCurve}},
                {addedPumpCurve.uuid, hydraulic::HydraulicEntity{addedPumpCurve}},
                {addedHeadlossCurve.uuid, hydraulic::HydraulicEntity{addedHeadlossCurve}},
                {addedEfficiencyCurve.uuid, hydraulic::HydraulicEntity{addedEfficiencyCurve}}
            };
            const native::Revision catalogueInserted = hydraulic::editHydraulicNetworkLegacy(
                repository, "catalogue-indexed", initial.uuid,
                catalogueInsertions, &catalogueStats);
            require(catalogueStats.indexedMembershipFastPath && catalogueStats.validatedEntities == 8 &&
                    catalogueInserted.id != catalogueChanged.id,
                    "catalogue insertion did not use indexed chunks");
            catalogueExpected.pipe_materials.append(addedMaterial);
            catalogueExpected.patterns_time.append(addedPattern);
            catalogueExpected.curves_tank_volume.append(addedTankCurve);
            catalogueExpected.curves_pump_head.append(addedPumpCurve);
            catalogueExpected.curves_pump_efficiency.append(addedEfficiencyCurve);
            catalogueExpected.curves_valve_headloss.append(addedHeadlossCurve);
            catalogueExpected.curves_valve_characteristic.append(addedCharacteristicCurve);
            catalogueExpected.curves_generic.append(addedGenericCurve);
            compareNetworks(catalogueExpected,
                requiredLoad(repository, "catalogue-indexed", initial.uuid));
            const native::Revision beforeInvalidCatalogue = repository.head("catalogue-indexed");
            bool rejectedCatalogueRemoval = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "catalogue-indexed", initial.uuid,
                    {{materialEdit.uuid, std::nullopt}}, &catalogueStats);
            } catch (const std::exception&) { rejectedCatalogueRemoval = true; }
            require(rejectedCatalogueRemoval && repository.head("catalogue-indexed").id ==
                    beforeInvalidCatalogue.id, "deleted a material referenced by a pipe");
            rejectedCatalogueRemoval = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "catalogue-indexed", initial.uuid,
                    {{patternEdit.uuid, std::nullopt}}, &catalogueStats);
            } catch (const std::exception&) { rejectedCatalogueRemoval = true; }
            require(rejectedCatalogueRemoval && repository.head("catalogue-indexed").id ==
                    beforeInvalidCatalogue.id, "deleted a pattern referenced by a pump");
            const std::vector<hydraulic::HydraulicEntityChange> catalogueRemovals = {
                {addedHeadlossCurve.uuid, std::nullopt}, {addedMaterial.uuid, std::nullopt},
                {addedGenericCurve.uuid, std::nullopt}, {addedTankCurve.uuid, std::nullopt},
                {addedPattern.uuid, std::nullopt}, {addedPumpCurve.uuid, std::nullopt},
                {addedCharacteristicCurve.uuid, std::nullopt}, {addedEfficiencyCurve.uuid, std::nullopt}
            };
            hydraulic::editHydraulicNetworkLegacy(repository, "catalogue-indexed", initial.uuid,
                catalogueRemovals, &catalogueStats);
            require(catalogueStats.indexedMembershipFastPath && catalogueStats.validatedEntities == 8,
                    "catalogue deletion batch did not use indexed membership validation");
            catalogueExpected.pipe_materials.removeLast();
            catalogueExpected.patterns_time.removeLast();
            catalogueExpected.curves_tank_volume.removeLast();
            catalogueExpected.curves_pump_head.removeLast();
            catalogueExpected.curves_pump_efficiency.removeLast();
            catalogueExpected.curves_valve_headloss.removeLast();
            catalogueExpected.curves_valve_characteristic.removeLast();
            catalogueExpected.curves_generic.removeLast();
            compareNetworks(catalogueExpected,
                requiredLoad(repository, "catalogue-indexed", initial.uuid));
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));

            // Incremental edits must share the same semantic validation as a
            // complete save without rewriting unchanged records/manifest.
            repository.createBranch("incremental", "feature");
            const native::Key rootKey = hydraulic::keyFromUuid(initial.uuid);
            const std::optional<native::ModelRecord> manifestBefore =
                repository.getModel("incremental", rootKey);
            require(manifestBefore.has_value(), "missing indexed network manifest");
            const native::Bytes manifestBytes = native::encodeModelRecord(*manifestBefore);
            NetworkHydraulic edited = modified;
            HydraulicLinkPipe changedPipe = edited.links_pipes[0];
            changedPipe.diameter_mm = 250.0;
            changedPipe.node_uuid_from = uuid(18);
            edited.links_pipes[0] = changedPipe;
            const native::Revision changed = hydraulic::editHydraulicNetworkLegacy(
                repository, "incremental", initial.uuid,
                {{changedPipe.uuid, hydraulic::HydraulicEntity{changedPipe}}});
            compareNetworks(edited, requiredLoad(repository, "incremental", initial.uuid));
            require(changed.id == repository.head("incremental").id,
                    "incremental revision not published");
            const std::optional<native::ModelRecord> manifestAfter =
                repository.getModel("incremental", rootKey);
            require(manifestAfter.has_value() &&
                    native::encodeModelRecord(*manifestAfter) == manifestBytes,
                    "payload-only edit rewrote network membership");
            const std::vector<native::Key> beforeRefs =
                repository.referencingModel("incremental", hydraulic::keyFromUuid(uuid(19)));
            require(std::find(beforeRefs.begin(), beforeRefs.end(), hydraulic::keyFromUuid(uuid(23))) ==
                        beforeRefs.end(), "old pipe endpoint reverse-reference was retained");
            const std::vector<native::Key> afterRefs =
                repository.referencingModel("incremental", hydraulic::keyFromUuid(uuid(18)));
            require(std::find(afterRefs.begin(), afterRefs.end(), hydraulic::keyFromUuid(uuid(23))) !=
                        afterRefs.end(), "new pipe endpoint reverse-reference missing");
            require(hydraulic::editHydraulicNetworkLegacy(repository, "incremental", initial.uuid,
                        {{changedPipe.uuid, hydraulic::HydraulicEntity{changedPipe}}}).id == changed.id,
                    "identical hydraulic upsert created an unnecessary revision");
            require(hydraulic::editHydraulicNetworkLegacy(repository, "incremental", initial.uuid, {}).id == changed.id,
                    "empty batch created a new revision");

            HydraulicNodeJunction extraNode = item<HydraulicNodeJunction>(202);
            HydraulicLinkPipe extraPipe = item<HydraulicLinkPipe>(203);
            extraPipe.node_uuid_from = uuid(20);
            extraPipe.node_uuid_to = extraNode.uuid;
            const native::Revision inserted = hydraulic::editHydraulicNetworkLegacy(
                repository, "incremental", initial.uuid,
                {{extraNode.uuid, hydraulic::HydraulicEntity{extraNode}},
                 {extraPipe.uuid, hydraulic::HydraulicEntity{extraPipe}}});
            edited.nodes_junctions.append(extraNode);
            edited.links_pipes.append(extraPipe);
            compareNetworks(edited, requiredLoad(repository, "incremental", initial.uuid));
            require(inserted.id != changed.id, "new members did not create revision");
            bool rejectedEdit = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "incremental", initial.uuid,
                                         {{extraNode.uuid, std::nullopt}});
            } catch (const std::exception&) { rejectedEdit = true; }
            require(rejectedEdit && repository.head("incremental").id == inserted.id,
                    "deleted referenced node through incremental editor");
            rejectedEdit = false;
            try {
                HydraulicLinkValve wrongType = item<HydraulicLinkValve>(23);
                hydraulic::editHydraulicNetworkLegacy(repository, "incremental", initial.uuid,
                                         {{wrongType.uuid, hydraulic::HydraulicEntity{wrongType}}});
            } catch (const std::exception&) { rejectedEdit = true; }
            require(rejectedEdit && repository.head("incremental").id == inserted.id,
                    "incremental editor changed an existing entity kind");
            rejectedEdit = false;
            try {
                hydraulic::editHydraulicNetworkLegacy(repository, "incremental", initial.uuid,
                                         {{extraPipe.uuid, std::nullopt},
                                          {extraPipe.uuid, std::nullopt}});
            } catch (const std::exception&) { rejectedEdit = true; }
            require(rejectedEdit && repository.head("incremental").id == inserted.id,
                    "duplicate UUID batch was accepted");
            hydraulic::editHydraulicNetworkLegacy(repository, "incremental", initial.uuid,
                                     {{extraPipe.uuid, std::nullopt},
                                      {extraNode.uuid, std::nullopt}});
            edited.links_pipes.removeLast();
            edited.nodes_junctions.removeLast();
            compareNetworks(edited, requiredLoad(repository, "incremental", initial.uuid));
            compareNetworks(modified, requiredLoad(repository, "feature", initial.uuid));
            require(!repository.getModel("incremental", hydraulic::keyFromUuid(extraNode.uuid)),
                    "deleted incremental member still stored in branch");
            repository.verifyPack();

            NetworkHydraulic invalid = modified;
            invalid.nodes_junctions.removeAt(0);
            bool rejected = false;
            try { hydraulic::saveHydraulicNetworkLegacy(repository, "feature", invalid); }
            catch (const std::exception&) { rejected = true; }
            require(rejected && repository.head("feature").id != original,
                    "dangling node change was accepted");

            // Independent second network is legal; duplicate ownership is not.
            NetworkHydraulic second;
            second.uuid = uuid(200);
            second.id = QStringLiteral("second-network");
            second.nodes_junctions.append(item<HydraulicNodeJunction>(201));
            hydraulic::saveHydraulicNetworkLegacy(repository, "main", second);
            compareNetworks(second, requiredLoad(repository, "main", second.uuid));
            second.nodes_junctions.append(initial.nodes_junctions[0]);
            rejected = false;
            try { hydraulic::saveHydraulicNetworkLegacy(repository, "main", second); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "cross-network UUID ownership collision was accepted");

            NetworkHydraulic duplicate = initial;
            duplicate.links_valves[0].uuid = duplicate.links_pipes[0].uuid;
            rejected = false;
            try { hydraulic::saveHydraulicNetworkLegacy(repository, "feature", duplicate); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "duplicate UUID among entity kinds was accepted");
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder);
            compareNetworks(initial, requiredLoad(reopened, "main", initial.uuid));
            const NetworkHydraulic physicalReloaded = requiredLoad(
                reopened, "physical-indexed", initial.uuid);
            require(physicalReloaded.nodes_reservoirs[0].hydraulic_head_m == 147.5 &&
                    physicalReloaded.nodes_tanks[0].water_level_initial_m == 4.25 &&
                    physicalReloaded.links_pumps[0].initial_speed_ratio == 1.4 &&
                    physicalReloaded.links_valves[0].node_uuid_from == uuid(18),
                    "physical indexed updates lost after reopening");
            compareNetworks(initial, requiredLoad(reopened, "structural-removed", initial.uuid));
            const NetworkHydraulic structuralReloaded = requiredLoad(
                reopened, "structural-indexed", initial.uuid);
            require(structuralReloaded.links_pumps.size() == initial.links_pumps.size() + 1 &&
                    structuralReloaded.nodes_tanks.size() == initial.nodes_tanks.size() + 1 &&
                    structuralReloaded.links_pumps.last().node_uuid_to == uuid(33002),
                    "indexed structural members lost after repository reopen");
            require(requiredLoad(reopened, "structural-msx", initial.uuid)
                        .multi_species.parameter_overrides_tanks.size() == 1,
                    "metadata-protected branch changed after structural deletion");
            const NetworkHydraulic catalogueReloaded = requiredLoad(
                reopened, "catalogue-indexed", initial.uuid);
            require(catalogueReloaded.pipe_materials[0].id ==
                    QStringLiteral("catalogue-material-revised") &&
                    catalogueReloaded.patterns_time[0].multipliers.size() == 4,
                    "catalogue indexed changes lost after reopen");
            const std::optional<hydraulic::HydraulicEntity> reopenedPipe =
                hydraulic::loadHydraulicEntity(reopened, "main", initial.uuid, uuid(23));
            require(reopenedPipe.has_value() &&
                    std::get<HydraulicLinkPipe>(*reopenedPipe).diameter_mm == 110.75,
                    "indexed hydraulic lookup failed after repository reopen");
            require(reopened.head("main").id != original, "new network commit missing");
            require(reopened.head("feature").id != reopened.head("main").id,
                    "branch divergence lost on reopening");
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
void exerciseChunkBoundaries() {
    const std::filesystem::path folder = tempDir();
    try {
        NetworkHydraulic network;
        network.uuid = uuid(800000);
        network.id = QStringLiteral("chunk-membership-regression");
        for (int index = 0; index < 514; ++index) {
            network.nodes_junctions.append(item<HydraulicNodeJunction>(801000 + index));
            if (index != 0) {
                HydraulicLinkPipe pipe = item<HydraulicLinkPipe>(802000 + index);
                pipe.node_uuid_from = network.nodes_junctions[index - 1].uuid;
                pipe.node_uuid_to = network.nodes_junctions[index].uuid;
                network.links_pipes.append(pipe);
            }
        }
        for (int index = 0; index < 260; ++index) {
            network.map_labels.append(item<HydraulicMapLabel>(803000 + index));
        }
        require(aowis::dvmdb::validateHydraulicNetwork(network).valid(),
                "chunk test fixture not valid");
        native::Hash original{};
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            original = hydraulic::saveHydraulicNetworkLegacy(repository, "main", network).id;
            const std::optional<native::ModelRecord> root = repository.getModel(
                "main", hydraulic::keyFromUuid(network.uuid));
            require(root.has_value() && root->references.size() == 8,
                    "chunk count was not bounded by 256 members");
            // Snapshot and branch isolation through different membership paths.
            repository.createBranch("feature", "main");
            HydraulicNodeJunction addedNode = item<HydraulicNodeJunction>(804000);
            HydraulicLinkPipe addedPipe = item<HydraulicLinkPipe>(804001);
            addedPipe.node_uuid_from = network.nodes_junctions[0].uuid;
            addedPipe.node_uuid_to = addedNode.uuid;
            hydraulic::HydraulicEditDiagnostics stats;
            hydraulic::editHydraulicNetworkLegacy(repository, "feature", network.uuid,
                {{addedNode.uuid, hydraulic::HydraulicEntity{addedNode}},
                 {addedPipe.uuid, hydraulic::HydraulicEntity{addedPipe}}}, &stats);
            require(stats.indexedMembershipFastPath, "chunked insertion missed fast path");
            for (int index = 0; index < 7; ++index) {
                hydraulic::editHydraulicNetworkLegacy(repository, "feature", network.uuid,
                    {{network.map_labels[index].uuid, std::nullopt}}, &stats);
                require(stats.indexedMembershipFastPath, "label deletion missed fast path");
            }
            NetworkHydraulic expected = network;
            expected.nodes_junctions.append(addedNode);
            expected.links_pipes.append(addedPipe);
            for (int index = 0; index < 7; ++index) expected.map_labels.removeFirst();
            // Remove all four labels from the final, partial membership chunk.
            // Its owner link and physical model record must be retired atomically.
            std::vector<hydraulic::HydraulicEntityChange> lastChunkRemoval;
            for (int index = 256; index < 260; ++index) {
                lastChunkRemoval.push_back({network.map_labels[index].uuid, std::nullopt});
            }
            hydraulic::editHydraulicNetworkLegacy(repository, "feature", network.uuid,
                                    lastChunkRemoval, &stats);
            require(stats.indexedMembershipFastPath,
                    "empty chunk retirement missed indexed validation");
            for (int index = 0; index < 4; ++index) expected.map_labels.removeLast();
            compareNetworks(expected, requiredLoad(repository, "feature", network.uuid));
            compareNetworks(network, requiredLoad(repository, "main", network.uuid));
            const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIds(repository, "feature",
                network.uuid, hydraulic::HydraulicEntityType::MapLabel);
            require(ids.has_value() && ids->size() == expected.map_labels.size() &&
                    ids->front() == expected.map_labels.front().uuid,
                    "chunk deletion lost ordered membership");
            HydraulicLinkPipe rewired = expected.links_pipes[0];
            rewired.node_uuid_to = expected.nodes_junctions[2].uuid;
            hydraulic::editHydraulicNetworkLegacy(repository, "feature", network.uuid,
                {{rewired.uuid, hydraulic::HydraulicEntity{rewired}}}, &stats);
            require(stats.indexedTopologyFastPath, "chunked owner index blocked independent rewire");
            expected.links_pipes[0] = rewired;
            compareNetworks(expected, requiredLoad(repository, "feature", network.uuid));
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head("main").id == original,
                    "original chunked branch head changed");
            compareNetworks(network, requiredLoad(reopened, "main", network.uuid));
            require(hydraulic::loadHydraulicEntity(reopened, "feature", network.uuid,
                            uuid(804000)).has_value(),
                    "added chunk member lost on reopen");
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Demand points and controls have typed references, and pipe-attached demand
// points additionally constrain assigned junctions to the pipe's endpoints.
// Verify that the bounded indexed path agrees with the canonical validator
// including a single-batch dependency graph.
void exerciseDependentMembership() {
    const std::filesystem::path folder = tempDir();
    try {
        NetworkHydraulic baseline = completeNetwork();
        baseline.nodes_junctions.append(item<HydraulicNodeJunction>(42000));
        require(aowis::dvmdb::validateHydraulicNetwork(baseline).valid(),
                "dependent membership fixture invalid");
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            hydraulic::saveHydraulicNetworkLegacy(repository, "main", baseline);
            repository.createBranch("dependent", "main");

            HydraulicDemandPoint point = item<HydraulicDemandPoint>(42001);
            point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
            point.attachment.pipe_uuid = uuid(23);
            point.attachment.pipe_position = 0.35;
            point.attachment.pipe_allocation_mode =
                HydraulicDemandPointPipeAllocationMode::AssignedJunction;
            point.attachment.pipe_assigned_junction_uuid = uuid(19);
            HydraulicDemand pointDemand;
            pointDemand.pattern_uuid = uuid(11);
            point.demands.append(pointDemand);

            HydraulicControlSimple control = item<HydraulicControlSimple>(42002);
            control.link_uuid = uuid(23);
            control.trigger_node_uuid = uuid(20);

            HydraulicControlRule rule = item<HydraulicControlRule>(42003);
            HydraulicControlRulePremise nodePremise;
            nodePremise.object = HydraulicControlRuleObject::Node;
            nodePremise.object_uuid = uuid(19);
            rule.premises.append(nodePremise);
            HydraulicControlRulePremise linkPremise;
            linkPremise.object = HydraulicControlRuleObject::Link;
            linkPremise.object_uuid = uuid(23);
            rule.premises.append(linkPremise);
            HydraulicControlRuleAction thenAction;
            thenAction.link_uuid = uuid(24);
            rule.actions_then.append(thenAction);
            HydraulicControlRuleAction elseAction;
            elseAction.link_uuid = uuid(25);
            rule.actions_else.append(elseAction);

            hydraulic::HydraulicEditDiagnostics stats;
            const native::Revision inserted = hydraulic::editHydraulicNetworkLegacy(repository, "dependent", baseline.uuid,
                {{control.uuid, hydraulic::HydraulicEntity{control}},
                 {point.uuid, hydraulic::HydraulicEntity{point}},
                 {rule.uuid, hydraulic::HydraulicEntity{rule}}}, &stats);
            require(stats.indexedMembershipFastPath && stats.validatedEntities == 3,
                    "demand and controls insertion missed indexed validation");
            NetworkHydraulic expected = baseline;
            expected.demand_points.append(point);
            expected.controls_simple.append(control);
            expected.controls_rules.append(rule);
            require(aowis::dvmdb::validateHydraulicNetwork(expected).valid(),
                    "indexed demand and controls did not satisfy canonical validation");
            compareNetworks(expected, requiredLoad(repository, "dependent", baseline.uuid));

            // Persisted outgoing reference sets must exactly match canonical
            // extraction, not just pass the edit path's local type checks.
            const QVector<native::HydraulicReference> canonical = native::hydraulicReferences(expected);
            for (const QUuid& source : {point.uuid, control.uuid, rule.uuid}) {
                std::set<native::Key> expectedReferences;
                for (const native::HydraulicReference& reference : canonical) {
                    if (reference.source_uuid == source && !reference.target_uuid.isNull())
                        expectedReferences.insert(hydraulic::keyFromUuid(reference.target_uuid));
                }
                const std::optional<native::ModelRecord> record =
                    repository.getModel("dependent", hydraulic::keyFromUuid(source));
                require(record.has_value() &&
                        record->references == std::vector<native::Key>(
                            expectedReferences.begin(), expectedReferences.end()),
                        "indexed dependent object references differ from canonical validator");
            }
            const std::vector<native::Key> pipeReferences = repository.referencingModel(
                "dependent", hydraulic::keyFromUuid(uuid(23)));
            require(std::find(pipeReferences.begin(), pipeReferences.end(),
                              hydraulic::keyFromUuid(point.uuid)) != pipeReferences.end() &&
                    std::find(pipeReferences.begin(), pipeReferences.end(),
                              hydraulic::keyFromUuid(control.uuid)) != pipeReferences.end() &&
                    std::find(pipeReferences.begin(), pipeReferences.end(),
                              hydraulic::keyFromUuid(rule.uuid)) != pipeReferences.end(),
                    "indexed dependent objects are absent from reverse-reference index");

            const auto reject = [&](const std::vector<hydraulic::HydraulicEntityChange>& changes,
                                    const char* message) {
                const native::Hash headBefore = repository.head("dependent").id;
                bool rejected = false;
                try { hydraulic::editHydraulicNetworkLegacy(repository, "dependent", baseline.uuid, changes); }
                catch (const std::exception&) { rejected = true; }
                require(rejected && repository.head("dependent").id == headBefore, message);
            };
            reject({{uuid(23), std::nullopt}}, "dependent pipe deletion was accepted");
            {
                HydraulicDemandPoint invalid = point;
                invalid.uuid = uuid(42010);
                invalid.attachment.pipe_assigned_junction_uuid = uuid(42000);
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "non-endpoint demand assignment was accepted");
                invalid.attachment.pipe_position = -0.1;
                invalid.attachment.pipe_assigned_junction_uuid = uuid(19);
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "negative pipe position was accepted");
                invalid.attachment.pipe_position = std::numeric_limits<double>::quiet_NaN();
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "non-finite pipe position was accepted");
                invalid.attachment.pipe_position = 0.5;
                invalid.demands[0].pattern_uuid = uuid(19);
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "wrong-type demand pattern was accepted");
            }
            {
                HydraulicControlSimple invalid = control;
                invalid.uuid = uuid(42011);
                invalid.link_uuid = uuid(19);
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "simple control targeting a node was accepted");
            }
            {
                HydraulicControlRule invalid = rule;
                invalid.uuid = uuid(42012);
                invalid.premises[0].object_uuid = uuid(23);
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "rule node premise targeting a pipe was accepted");
                invalid.premises[0].object_uuid = uuid(19);
                invalid.actions_else[0].link_uuid = uuid(20);
                reject({{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                       "rule action targeting a node was accepted");
            }
            require(repository.head("dependent").id == inserted.id,
                    "rejected dependent changes published a revision");

            hydraulic::editHydraulicNetworkLegacy(repository, "dependent", baseline.uuid,
                {{point.uuid, std::nullopt}, {control.uuid, std::nullopt},
                 {rule.uuid, std::nullopt}}, &stats);
            require(stats.indexedMembershipFastPath && stats.validatedEntities == 3,
                    "independent demand and controls deletion missed indexed validation");
            compareNetworks(baseline, requiredLoad(repository, "dependent", baseline.uuid));

            // The new pipe, junction, demand point and controls refer to one
            // another in one atomic batch. Every target may be initially absent.
            HydraulicNodeJunction newJunction = item<HydraulicNodeJunction>(42020);
            HydraulicLinkPipe newPipe = item<HydraulicLinkPipe>(42021);
            newPipe.node_uuid_from = uuid(19);
            newPipe.node_uuid_to = newJunction.uuid;
            HydraulicDemandPoint newPoint = point;
            newPoint.uuid = uuid(42022);
            newPoint.attachment.pipe_uuid = newPipe.uuid;
            newPoint.attachment.pipe_assigned_junction_uuid = newJunction.uuid;
            HydraulicControlSimple newControl = control;
            newControl.uuid = uuid(42023);
            newControl.link_uuid = newPipe.uuid;
            newControl.trigger_node_uuid = newJunction.uuid;
            HydraulicControlRule newRule = rule;
            newRule.uuid = uuid(42024);
            newRule.premises[0].object_uuid = newJunction.uuid;
            newRule.premises[1].object_uuid = newPipe.uuid;
            newRule.actions_then[0].link_uuid = newPipe.uuid;
            newRule.actions_else[0].link_uuid = newPipe.uuid;
            hydraulic::editHydraulicNetworkLegacy(repository, "dependent", baseline.uuid,
                {{newPoint.uuid, hydraulic::HydraulicEntity{newPoint}},
                 {newRule.uuid, hydraulic::HydraulicEntity{newRule}},
                 {newPipe.uuid, hydraulic::HydraulicEntity{newPipe}},
                 {newControl.uuid, hydraulic::HydraulicEntity{newControl}},
                 {newJunction.uuid, hydraulic::HydraulicEntity{newJunction}}}, &stats);
            require(stats.indexedMembershipFastPath && stats.validatedEntities == 5,
                    "same-batch demand/control references missed indexed validation");
            NetworkHydraulic together = baseline;
            together.nodes_junctions.append(newJunction);
            together.links_pipes.append(newPipe);
            together.demand_points.append(newPoint);
            together.controls_simple.append(newControl);
            together.controls_rules.append(newRule);
            require(native::validateHydraulicNetwork(together).valid(),
                    "same-batch dependent edits fail canonical validation");
            compareNetworks(together, requiredLoad(repository, "dependent", baseline.uuid));
            reject({{newPipe.uuid, std::nullopt}},
                   "new pipe deletion ignored same-batch demand/control references");
            hydraulic::editHydraulicNetworkLegacy(repository, "dependent", baseline.uuid,
                {{newJunction.uuid, std::nullopt},
                 {newPipe.uuid, std::nullopt},
                 {newPoint.uuid, std::nullopt},
                 {newControl.uuid, std::nullopt},
                 {newRule.uuid, std::nullopt}}, &stats);
            require(stats.indexedMembershipFastPath && stats.validatedEntities == 5,
                    "dependent batch deletion missed indexed validation");
            compareNetworks(baseline, requiredLoad(repository, "dependent", baseline.uuid));
            compareNetworks(baseline, requiredLoad(repository, "main", baseline.uuid));
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder);
            compareNetworks(baseline, requiredLoad(reopened, "dependent", baseline.uuid));
            compareNetworks(baseline, requiredLoad(reopened, "main", baseline.uuid));
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Existing demand/control edits may change referenced UUIDs without touching
// network membership. Exercise the real canonical validator and reference index
// including no-op, rejection and reopening.
void exerciseDependentUpdates() {
    const std::filesystem::path folder = tempDir();
    try {
        NetworkHydraulic baseline = completeNetwork();
        NetworkHydraulic expected = baseline;
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            hydraulic::saveHydraulicNetworkLegacy(repository, "main", baseline);
            repository.createBranch("dependent-updates", "main");
            const std::optional<native::ModelRecord> beforeRoot = repository.getModel(
                "dependent-updates", hydraulic::keyFromUuid(baseline.uuid));
            require(beforeRoot.has_value(), "missing root for dependent edit test");
            const native::Bytes rootBytes = native::encodeModelRecord(*beforeRoot);

            HydraulicDemandPoint point = baseline.demand_points[0];
            point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
            point.attachment.pipe_uuid = uuid(23);
            point.attachment.pipe_position = 0.65;
            point.attachment.pipe_allocation_mode =
                HydraulicDemandPointPipeAllocationMode::AssignedJunction;
            point.attachment.pipe_assigned_junction_uuid = uuid(20);
            HydraulicDemand demand;
            demand.pattern_uuid = uuid(11);
            point.demands.append(demand);
            HydraulicControlSimple control = baseline.controls_simple[0];
            control.link_uuid = uuid(25);
            control.trigger_node_uuid = uuid(21);
            HydraulicControlRule rule = baseline.controls_rules[0];
            HydraulicControlRulePremise premise;
            premise.object = HydraulicControlRuleObject::Node;
            premise.object_uuid = uuid(18);
            rule.premises.append(premise);
            rule.actions_then[0].link_uuid = uuid(23);
            HydraulicControlRuleAction elseAction;
            elseAction.link_uuid = uuid(25);
            rule.actions_else.append(elseAction);

            hydraulic::HydraulicEditDiagnostics stats;
            const std::vector<hydraulic::HydraulicEntityChange> edits = {
                {rule.uuid, hydraulic::HydraulicEntity{rule}},
                {point.uuid, hydraulic::HydraulicEntity{point}},
                {control.uuid, hydraulic::HydraulicEntity{control}}
            };
            const native::Revision updated = hydraulic::editHydraulicNetworkLegacy(
                repository, "dependent-updates", baseline.uuid, edits, &stats);
            require(stats.indexedDependentUpdateFastPath && stats.validatedEntities == 3,
                    "existing demand/control update missed bounded indexed validation");
            expected.demand_points[0] = point;
            expected.controls_simple[0] = control;
            expected.controls_rules[0] = rule;
            require(native::validateHydraulicNetwork(expected).valid(),
                    "indexed dependent update violated canonical validation");
            compareNetworks(expected, requiredLoad(repository, "dependent-updates", baseline.uuid));
            compareNetworks(baseline, requiredLoad(repository, "main", baseline.uuid));
            const std::optional<native::ModelRecord> afterRoot = repository.getModel(
                "dependent-updates", hydraulic::keyFromUuid(baseline.uuid));
            require(afterRoot.has_value() && native::encodeModelRecord(*afterRoot) == rootBytes,
                    "existing dependent update changed network membership root");

            // Incoming edges must reflect the NEW canonical reference set, not the
            // old attachments. Check every updated record and a removed edge.
            const QVector<native::HydraulicReference> canonical = native::hydraulicReferences(expected);
            for (const QUuid& source : {point.uuid, control.uuid, rule.uuid}) {
                std::set<native::Key> targets;
                for (const native::HydraulicReference& ref : canonical)
                    if (ref.source_uuid == source && !ref.target_uuid.isNull())
                        targets.insert(hydraulic::keyFromUuid(ref.target_uuid));
                const std::optional<native::ModelRecord> stored = repository.getModel(
                    "dependent-updates", hydraulic::keyFromUuid(source));
                require(stored.has_value() && stored->references ==
                        std::vector<native::Key>(targets.begin(), targets.end()),
                        "updated reference index differs from canonical validator");
            }
            const std::vector<native::Key> incomingOld = repository.referencingModel(
                "dependent-updates", hydraulic::keyFromUuid(uuid(19)));
            require(std::find(incomingOld.begin(), incomingOld.end(),
                              hydraulic::keyFromUuid(point.uuid)) == incomingOld.end() &&
                    std::find(incomingOld.begin(), incomingOld.end(),
                              hydraulic::keyFromUuid(control.uuid)) == incomingOld.end(),
                    "outdated reverse references survived dependent updates");
            const std::vector<native::Key> incomingNew = repository.referencingModel(
                "dependent-updates", hydraulic::keyFromUuid(uuid(25)));
            require(std::find(incomingNew.begin(), incomingNew.end(),
                              hydraulic::keyFromUuid(control.uuid)) != incomingNew.end() &&
                    std::find(incomingNew.begin(), incomingNew.end(),
                              hydraulic::keyFromUuid(rule.uuid)) != incomingNew.end(),
                    "new dependent reverse references were not persisted");
            require(hydraulic::editHydraulicNetworkLegacy(repository, "dependent-updates", baseline.uuid,
                        edits, &stats).id == updated.id && stats.indexedDependentUpdateFastPath,
                    "identical dependent updates created a new revision");

            const auto reject = [&](const hydraulic::HydraulicEntity& entity, const char* message) {
                const QUuid key = std::visit([](const auto& value) { return value.uuid; }, entity);
                const native::Hash head = repository.head("dependent-updates").id;
                bool rejected = false;
                try {
                    hydraulic::editHydraulicNetworkLegacy(repository, "dependent-updates", baseline.uuid,
                        {{key, entity}}, &stats);
                } catch (const std::exception&) { rejected = true; }
                require(rejected && repository.head("dependent-updates").id == head, message);
            };
            HydraulicDemandPoint wrong = point;
            wrong.attachment.pipe_assigned_junction_uuid = uuid(18);
            reject(hydraulic::HydraulicEntity{wrong}, "demand assigned non-endpoint was accepted");
            wrong = point;
            wrong.attachment.pipe_position = std::numeric_limits<double>::quiet_NaN();
            reject(hydraulic::HydraulicEntity{wrong}, "nonfinite demand position was accepted");
            wrong = point;
            wrong.attachment.pipe_position = 1.1;
            reject(hydraulic::HydraulicEntity{wrong}, "out-of-range demand position was accepted");
            wrong = point;
            wrong.demands[0].pattern_uuid = uuid(19);
            reject(hydraulic::HydraulicEntity{wrong}, "wrong-kind demand pattern was accepted");
            wrong = point;
            wrong.attachment.pipe_uuid = uuid(420099);
            reject(hydraulic::HydraulicEntity{wrong}, "missing demand pipe was accepted");
            HydraulicControlSimple badControl = control;
            badControl.link_uuid = uuid(20);
            reject(hydraulic::HydraulicEntity{badControl}, "simple control targeted node as link");
            badControl = control;
            badControl.trigger_node_uuid = uuid(11);
            reject(hydraulic::HydraulicEntity{badControl}, "simple control targeted pattern as node");
            HydraulicControlRule badRule = rule;
            badRule.premises.last().object_uuid = uuid(23);
            reject(hydraulic::HydraulicEntity{badRule}, "rule node premise targeted pipe");
            badRule = rule;
            badRule.actions_else[0].link_uuid = uuid(19);
            reject(hydraulic::HydraulicEntity{badRule}, "rule action targeted junction");
            {
                // One valid change must not sneak into an invalid atomic batch.
                HydraulicDemandPoint validPoint = point;
                validPoint.attachment.pipe_position = 0.4;
                const native::Hash beforeRejectedBatch = repository.head("dependent-updates").id;
                bool rejectedBatch = false;
                try {
                    hydraulic::editHydraulicNetworkLegacy(repository, "dependent-updates", baseline.uuid,
                        {{validPoint.uuid, hydraulic::HydraulicEntity{validPoint}},
                         {badRule.uuid, hydraulic::HydraulicEntity{badRule}}}, &stats);
                } catch (const std::exception&) { rejectedBatch = true; }
                require(rejectedBatch && repository.head("dependent-updates").id == beforeRejectedBatch,
                        "invalid dependent batch published the otherwise valid change");
            }

            // Mixed batches are deliberately *not* treated as independent bounded
            // dependent updates. Full validation must still handle them atomically.
            HydraulicMapLabel updatedLabel = baseline.map_labels[0];
            updatedLabel.text = QStringLiteral("changed through full validation");
            HydraulicControlSimple controlAgain = control;
            controlAgain.trigger_node_uuid = uuid(20);
            hydraulic::editHydraulicNetworkLegacy(repository, "dependent-updates", baseline.uuid,
                {{controlAgain.uuid, hydraulic::HydraulicEntity{controlAgain}},
                 {updatedLabel.uuid, hydraulic::HydraulicEntity{updatedLabel}}}, &stats);
            require(!stats.indexedDependentUpdateFastPath,
                    "mixed dependent and label edits bypassed full validation");
            expected.controls_simple[0] = controlAgain;
            expected.map_labels[0] = updatedLabel;
            compareNetworks(expected, requiredLoad(repository, "dependent-updates", baseline.uuid));
            repository.verifyPack();
        } // Release the writer lock before checking durability via a fresh handle.
        {
            native::Repository reopened(folder);
            compareNetworks(expected, requiredLoad(reopened, "dependent-updates", baseline.uuid));
            compareNetworks(baseline, requiredLoad(reopened, "main", baseline.uuid));
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

void exerciseHydraulicMerging() {
    const std::filesystem::path folder = tempDir();
    try {
        const NetworkHydraulic baseline = completeNetwork();
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            hydraulic::saveHydraulicNetworkLegacy(repository, "main", baseline);
            repository.createBranch("feature", "main");
            HydraulicLinkPipe left = baseline.links_pipes[0];
            HydraulicLinkPipe right = baseline.links_pipes[0];
            left.diameter_mm = 179.0;
            right.length_calculated_m = 37.5;
            hydraulic::editHydraulicNetworkLegacy(repository, "main", baseline.uuid,
                {{left.uuid, hydraulic::HydraulicEntity{left}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "feature", baseline.uuid,
                {{right.uuid, hydraulic::HydraulicEntity{right}}});
            const native::MergeResult merged = hydraulic::mergeHydraulicBranchesLegacy(repository, "main", "feature");
            require(merged.status == native::MergeStatus::Merged &&
                    merged.revision.parents.size() == 2,
                    "hydraulic field merge did not produce a two-parent revision");
            NetworkHydraulic expected = baseline;
            expected.links_pipes[0].diameter_mm = 179.0;
            expected.links_pipes[0].length_calculated_m = 37.5;
            compareNetworks(expected, requiredLoad(repository, "main", baseline.uuid));
            NetworkHydraulic featureExpected = baseline;
            featureExpected.links_pipes[0].length_calculated_m = 37.5;
            compareNetworks(featureExpected, requiredLoad(repository, "feature", baseline.uuid));
            require(repository.referencingModel("main", hydraulic::keyFromUuid(uuid(19))).size() > 0,
                    "field merge lost reverse references");
            // Repeated merge must not replay an incorporated source.
            require(hydraulic::mergeHydraulicBranchesLegacy(repository, "main", "feature").status ==
                    native::MergeStatus::UpToDate,
                    "repeated hydraulic merge changed branch history");

            repository.createBranch("conflict-left", "main");
            repository.createBranch("conflict-right", "main");
            HydraulicLinkPipe a = expected.links_pipes[0];
            HydraulicLinkPipe b = expected.links_pipes[0];
            a.diameter_mm = 181.0;
            b.diameter_mm = 182.0;
            hydraulic::editHydraulicNetworkLegacy(repository, "conflict-left", baseline.uuid,
                {{a.uuid, hydraulic::HydraulicEntity{a}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "conflict-right", baseline.uuid,
                {{b.uuid, hydraulic::HydraulicEntity{b}}});
            const native::Hash conflictHead = repository.head("conflict-left").id;
            const std::uint64_t conflictBytes = repository.packSize();
            const native::MergeResult conflict = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "conflict-left", "conflict-right");
            require(conflict.status == native::MergeStatus::Conflicts &&
                    !conflict.conflicts.empty() &&
                    conflict.conflicts[0].reason.find("diameter_mm") != std::string::npos &&
                    repository.head("conflict-left").id == conflictHead &&
                    repository.packSize() == conflictBytes,
                    "hydraulic same-field conflict did not fail without writing");

            // A GUI resolution must select only actual conflicting fields,
            // from the exact revision tips that were shown to the user.
            hydraulic::HydraulicMergeSelectionsLegacy chooseSource;
            chooseSource.expectedTargetHead = conflictHead;
            chooseSource.expectedSourceHead = repository.head("conflict-right").id;
            chooseSource.fields.push_back({uuid(23), QStringLiteral("diameter_mm"),
                                           hydraulic::HydraulicMergeSide::Source});
            const std::function<void(const hydraulic::HydraulicMergeSelectionsLegacy&, const char*)> rejectChoice =
                [&repository, &conflictHead, &conflictBytes](
                    const hydraulic::HydraulicMergeSelectionsLegacy& selections, const char* message) {
                    bool rejectedChoice = false;
                    try {
                        (void)hydraulic::mergeHydraulicBranchesLegacy(repository, "conflict-left", "conflict-right",
                                                         selections);
                    } catch (const std::exception&) { rejectedChoice = true; }
                    require(rejectedChoice && repository.head("conflict-left").id == conflictHead &&
                            repository.packSize() == conflictBytes, message);
                };
            hydraulic::HydraulicMergeSelectionsLegacy missingPins;
            missingPins.fields = chooseSource.fields;
            rejectChoice(missingPins, "unpinned field choice was accepted");
            hydraulic::HydraulicMergeSelectionsLegacy staleChoice = chooseSource;
            staleChoice.expectedSourceHead = repository.head("main").id;
            rejectChoice(staleChoice, "stale source revision was accepted");
            hydraulic::HydraulicMergeSelectionsLegacy unknownField = chooseSource;
            unknownField.fields[0].field = QStringLiteral("length_calculated_m");
            rejectChoice(unknownField, "choice for non-conflicting field was accepted");
            hydraulic::HydraulicMergeSelectionsLegacy duplicate = chooseSource;
            duplicate.fields.push_back(duplicate.fields[0]);
            rejectChoice(duplicate, "duplicate field choice was accepted");
            hydraulic::HydraulicMergeSelectionsLegacy unused = chooseSource;
            unused.fields.push_back({uuid(20), QStringLiteral("elevation_m"),
                                     hydraulic::HydraulicMergeSide::Target});
            rejectChoice(unused, "unused field choice was ignored");

            const native::MergeResult userMerged = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "conflict-left", "conflict-right", chooseSource);
            require(userMerged.status == native::MergeStatus::Merged &&
                    userMerged.revision.parents == std::vector<native::Hash>{
                        conflictHead, *chooseSource.expectedSourceHead} &&
                    repository.head("conflict-right").id == *chooseSource.expectedSourceHead,
                    "source-choice merge failed to preserve two-parent history");
            NetworkHydraulic chosenNetwork = expected;
            chosenNetwork.links_pipes[0].diameter_mm = 182.0;
            compareNetworks(chosenNetwork,
                requiredLoad(repository, "conflict-left", baseline.uuid));
            // After publication the same UI choices must be rejected as stale.
            bool staleAfterMerge = false;
            try {
                (void)hydraulic::mergeHydraulicBranchesLegacy(repository, "conflict-left", "conflict-right",
                                                 chooseSource);
            } catch (const std::runtime_error&) { staleAfterMerge = true; }
            require(staleAfterMerge, "already-applied field choices were silently replayed");

            repository.createBranch("target-choice-left", "main");
            repository.createBranch("target-choice-right", "main");
            HydraulicLinkPipe keepTarget = expected.links_pipes[0];
            HydraulicLinkPipe otherTarget = expected.links_pipes[0];
            keepTarget.diameter_mm = 191.0;
            otherTarget.diameter_mm = 192.0;
            otherTarget.length_calculated_m = 48.0;
            hydraulic::editHydraulicNetworkLegacy(repository, "target-choice-left", baseline.uuid,
                {{keepTarget.uuid, hydraulic::HydraulicEntity{keepTarget}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "target-choice-right", baseline.uuid,
                {{otherTarget.uuid, hydraulic::HydraulicEntity{otherTarget}}});
            hydraulic::HydraulicMergeSelectionsLegacy chooseTarget;
            chooseTarget.expectedTargetHead = repository.head("target-choice-left").id;
            chooseTarget.expectedSourceHead = repository.head("target-choice-right").id;
            chooseTarget.fields.push_back({uuid(23), QStringLiteral("diameter_mm"),
                                           hydraulic::HydraulicMergeSide::Target});
            const native::MergeResult targetMerged = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "target-choice-left", "target-choice-right", chooseTarget);
            require(targetMerged.status == native::MergeStatus::Merged,
                    "target-side field resolution was rejected");
            NetworkHydraulic targetChosenNetwork = expected;
            targetChosenNetwork.links_pipes[0].diameter_mm = 191.0;
            targetChosenNetwork.links_pipes[0].length_calculated_m = 48.0;
            compareNetworks(targetChosenNetwork,
                requiredLoad(repository, "target-choice-left", baseline.uuid));

            // Two individually valid branches can combine into invalid demand
            // allocation even when they edit DIFFERENT entities.
            repository.createBranch("topology-left", "feature");
            repository.createBranch("topology-right", "feature");
            HydraulicLinkPipe rewired = right;
            rewired.node_uuid_from = uuid(18);
            hydraulic::editHydraulicNetworkLegacy(repository, "topology-left", baseline.uuid,
                {{rewired.uuid, hydraulic::HydraulicEntity{rewired}}});
            HydraulicDemandPoint attached = baseline.demand_points[0];
            attached.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
            attached.attachment.pipe_uuid = uuid(23);
            attached.attachment.pipe_position = 0.4;
            attached.attachment.pipe_allocation_mode =
                HydraulicDemandPointPipeAllocationMode::AssignedJunction;
            attached.attachment.pipe_assigned_junction_uuid = uuid(19);
            hydraulic::editHydraulicNetworkLegacy(repository, "topology-right", baseline.uuid,
                {{attached.uuid, hydraulic::HydraulicEntity{attached}}});
            const native::Hash topologyHead = repository.head("topology-left").id;
            const std::uint64_t topologyBytes = repository.packSize();
            const native::MergeResult invalid = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "topology-left", "topology-right");
            require(invalid.status == native::MergeStatus::Conflicts &&
                    !invalid.conflicts.empty() &&
                    invalid.conflicts[0].reason.find("endpoint") != std::string::npos &&
                    repository.head("topology-left").id == topologyHead &&
                    repository.packSize() == topologyBytes,
                    "hydraulic merge published a semantically invalid combined network");
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder);
            NetworkHydraulic expected = baseline;
            expected.links_pipes[0].diameter_mm = 179.0;
            expected.links_pipes[0].length_calculated_m = 37.5;
            compareNetworks(expected, requiredLoad(reopened, "main", baseline.uuid));
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


void exerciseStructuralMergeChoices() {
    const std::filesystem::path folder = tempDir();
    try {
        const NetworkHydraulic baseline = completeNetwork();
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            hydraulic::saveHydraulicNetworkLegacy(repository, "main", baseline);
            repository.createBranch("delete-left", "main");
            repository.createBranch("modify-right", "main");
            repository.createBranch("delete-right", "main");
            repository.createBranch("modify-left", "main");
            HydraulicMapLabel label = baseline.map_labels[0];
            label.text = QStringLiteral("edited on surviving branch");
            hydraulic::editHydraulicNetworkLegacy(repository, "delete-left", baseline.uuid,
                                     {{label.uuid, std::nullopt}});
            hydraulic::editHydraulicNetworkLegacy(repository, "delete-right", baseline.uuid,
                                     {{label.uuid, std::nullopt}});
            hydraulic::editHydraulicNetworkLegacy(repository, "modify-left", baseline.uuid,
                                     {{label.uuid, hydraulic::HydraulicEntity{label}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "modify-right", baseline.uuid,
                                     {{label.uuid, hydraulic::HydraulicEntity{label}}});
            const native::Hash leftTip = repository.head("delete-left").id;
            const native::Hash rightTip = repository.head("modify-right").id;
            const std::uint64_t bytesBefore = repository.packSize();
            const native::MergeResult defaultConflict =
                hydraulic::mergeHydraulicBranchesLegacy(repository, "delete-left", "modify-right");
            require(defaultConflict.status == native::MergeStatus::Conflicts &&
                    repository.packSize() == bytesBefore &&
                    repository.head("delete-left").id == leftTip,
                    "delete-vs-modify failed to produce a non-writing conflict");
            hydraulic::HydraulicMergeSelectionsLegacy choice;
            choice.expectedTargetHead = leftTip;
            choice.expectedSourceHead = rightTip;
            choice.objects.push_back({label.uuid, hydraulic::HydraulicMergeSide::Target});
            const std::function<void(const hydraulic::HydraulicMergeSelectionsLegacy&, const char*)> rejectObjectChoice =
                [&repository, &leftTip, &bytesBefore](
                    const hydraulic::HydraulicMergeSelectionsLegacy& invalid, const char* description) {
                    bool rejected = false;
                    try {
                        (void)hydraulic::mergeHydraulicBranchesLegacy(
                            repository, "delete-left", "modify-right", invalid);
                    } catch (const std::exception&) { rejected = true; }
                    require(rejected && repository.head("delete-left").id == leftTip &&
                            repository.packSize() == bytesBefore, description);
                };
            hydraulic::HydraulicMergeSelectionsLegacy stale = choice;
            stale.expectedSourceHead = repository.head("main").id;
            rejectObjectChoice(stale, "stale structural choice changed a branch");
            hydraulic::HydraulicMergeSelectionsLegacy duplicate = choice;
            duplicate.objects.push_back(duplicate.objects[0]);
            rejectObjectChoice(duplicate, "duplicate structural choice accepted");
            hydraulic::HydraulicMergeSelectionsLegacy overlapping = choice;
            overlapping.fields.push_back({label.uuid, QStringLiteral("text"),
                                           hydraulic::HydraulicMergeSide::Target});
            rejectObjectChoice(overlapping, "field and whole-object choices overlapped");
            hydraulic::HydraulicMergeSelectionsLegacy unpinned = choice;
            unpinned.expectedTargetHead.reset();
            rejectObjectChoice(unpinned, "un-pinned structural resolution accepted");
            const native::MergeResult deleted = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "delete-left", "modify-right", choice);
            require(deleted.status == native::MergeStatus::Merged &&
                    deleted.revision.parents == std::vector<native::Hash>{leftTip, rightTip},
                    "explicit deletion did not produce a two-parent merge");
            NetworkHydraulic withoutLabel = baseline;
            withoutLabel.map_labels.clear();
            compareNetworks(withoutLabel, requiredLoad(repository, "delete-left", baseline.uuid));
            NetworkHydraulic withLabel = baseline;
            withLabel.map_labels[0] = label;
            compareNetworks(withLabel, requiredLoad(repository, "modify-right", baseline.uuid));

            // Keep a second branch at the unmerged tip to check that choosing
            // deletion still works independently of restoring the kept entity.
            repository.createBranch("modify-left-delete-choice", "modify-left");
            // Choosing the modified record now restores ONLY that entity's
            // network membership in the private merge candidate.
            const native::Hash oppositeHead = repository.head("modify-left").id;
            const native::Hash oppositeSource = repository.head("delete-right").id;
            const std::uint64_t oppositeBytes = repository.packSize();
            hydraulic::HydraulicMergeSelectionsLegacy keepOrphan;
            keepOrphan.expectedTargetHead = oppositeHead;
            keepOrphan.expectedSourceHead = oppositeSource;
            keepOrphan.objects.push_back({label.uuid, hydraulic::HydraulicMergeSide::Target});
            const native::MergeResult restored = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "modify-left", "delete-right", keepOrphan);
            require(restored.status == native::MergeStatus::Merged &&
                    restored.revision.parents == std::vector<native::Hash>{oppositeHead, oppositeSource} &&
                    repository.packSize() > oppositeBytes,
                    "keeping a deleted-on-source entity did not restore its membership");
            compareNetworks(withLabel, requiredLoad(repository, "modify-left", baseline.uuid));
            keepOrphan.expectedTargetHead = repository.head("modify-left-delete-choice").id;
            keepOrphan.objects[0].side = hydraulic::HydraulicMergeSide::Source;
            const native::MergeResult sourceDeletion = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "modify-left-delete-choice", "delete-right", keepOrphan);
            require(sourceDeletion.status == native::MergeStatus::Merged &&
                    !repository.getModel("modify-left-delete-choice", hydraulic::keyFromUuid(label.uuid)),
                    "source-side deletion was not selected");
            compareNetworks(withoutLabel, requiredLoad(repository, "modify-left-delete-choice", baseline.uuid));

            repository.createBranch("rewire-left", "main");
            repository.createBranch("rewire-right", "main");
            HydraulicLinkPipe left = baseline.links_pipes[0];
            HydraulicLinkPipe right = baseline.links_pipes[0];
            left.node_uuid_from = uuid(21);
            right.node_uuid_from = uuid(18);
            hydraulic::editHydraulicNetworkLegacy(repository, "rewire-left", baseline.uuid,
                                     {{left.uuid, hydraulic::HydraulicEntity{left}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "rewire-right", baseline.uuid,
                                     {{right.uuid, hydraulic::HydraulicEntity{right}}});
            const native::Hash topologyHead = repository.head("rewire-left").id;
            const native::Hash topologyOther = repository.head("rewire-right").id;
            const native::MergeResult unresolved =
                hydraulic::mergeHydraulicBranchesLegacy(repository, "rewire-left", "rewire-right");
            require(unresolved.status == native::MergeStatus::Conflicts,
                    "competing pipe rewires should require an object choice");
            hydraulic::HydraulicMergeSelectionsLegacy topologyChoice;
            topologyChoice.expectedTargetHead = topologyHead;
            topologyChoice.expectedSourceHead = topologyOther;
            topologyChoice.objects.push_back({right.uuid, hydraulic::HydraulicMergeSide::Source});
            const native::MergeResult rewire = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "rewire-left", "rewire-right", topologyChoice);
            require(rewire.status == native::MergeStatus::Merged,
                    "source-side topology choice was not accepted");
            NetworkHydraulic rewired = baseline;
            rewired.links_pipes[0] = right;
            compareNetworks(rewired, requiredLoad(repository, "rewire-left", baseline.uuid));
            const std::vector<native::Key> sourceReferrers =
                repository.referencingModel("rewire-left", hydraulic::keyFromUuid(uuid(18)));
            require(std::find(sourceReferrers.begin(), sourceReferrers.end(),
                              hydraulic::keyFromUuid(right.uuid)) != sourceReferrers.end(),
                    "source-side rewire lost reverse reference");
            // Reject selections for the wrong UUID or selections with stale
            // heads; neither may publish a revision nor write pack bytes.
            hydraulic::HydraulicMergeSelectionsLegacy unused;
            unused.expectedTargetHead = repository.head("delete-right").id;
            unused.expectedSourceHead = repository.head("modify-right").id;
            unused.objects.push_back({uuid(50001), hydraulic::HydraulicMergeSide::Target});
            const std::uint64_t rejectBytes = repository.packSize();
            bool unusedRejected = false;
            try { (void)hydraulic::mergeHydraulicBranchesLegacy(repository, "delete-right", "modify-right", unused); }
            catch (const std::exception&) { unusedRejected = true; }
            require(unusedRejected && repository.packSize() == rejectBytes,
                    "unused object choice was not rejected before publication");
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder);
            NetworkHydraulic expected = baseline;
            expected.map_labels.clear();
            compareNetworks(expected, requiredLoad(reopened, "delete-left", baseline.uuid));
            compareNetworks(expected, requiredLoad(reopened, "modify-left-delete-choice", baseline.uuid));
            NetworkHydraulic restoredLabel = baseline;
            restoredLabel.map_labels[0].text = QStringLiteral("edited on surviving branch");
            compareNetworks(restoredLabel, requiredLoad(reopened, "modify-left", baseline.uuid));
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

void exerciseCoordinatedMembershipMerges() {
    const std::filesystem::path folder = tempDir();
    try {
        const NetworkHydraulic baseline = completeNetwork();
        {
            native::Repository repository(folder);
            repository.createBranch("main");
            hydraulic::saveHydraulicNetworkLegacy(repository, "main", baseline);
            repository.createBranch("append-left", "main");
            repository.createBranch("append-right", "main");
            HydraulicMapLabel labelA = item<HydraulicMapLabel>(8101);
            HydraulicMapLabel labelB = item<HydraulicMapLabel>(8102);
            labelA.text = QStringLiteral("left append");
            labelB.text = QStringLiteral("right append");
            labelA.anchor_node_uuid = uuid(20);
            labelB.anchor_node_uuid = uuid(20);
            hydraulic::editHydraulicNetworkLegacy(repository, "append-left", baseline.uuid,
                {{labelA.uuid, hydraulic::HydraulicEntity{labelA}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "append-right", baseline.uuid,
                {{labelB.uuid, hydraulic::HydraulicEntity{labelB}}});
            const native::Hash leftHead = repository.head("append-left").id;
            const native::Hash rightHead = repository.head("append-right").id;
            const native::MergeResult appended = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "append-left", "append-right");
            require(appended.status == native::MergeStatus::Merged &&
                    appended.revision.parents == std::vector<native::Hash>{leftHead, rightHead},
                    "two independent appends to the same membership chunk did not merge");
            NetworkHydraulic combined = baseline;
            combined.map_labels.append(labelA);
            combined.map_labels.append(labelB);
            compareNetworks(combined, requiredLoad(repository, "append-left", baseline.uuid));
            // Reordering existing members must NOT be mistaken for an
            // append-only edit and silently discarded during reconciliation.
            repository.createBranch("order-left", "append-left");
            repository.createBranch("order-right", "append-left");
            NetworkHydraulic reordered = combined;
            reordered.map_labels.swapItemsAt(0, 1);
            hydraulic::saveHydraulicNetworkLegacy(repository, "order-left", reordered);
            HydraulicMapLabel later = item<HydraulicMapLabel>(8103);
            later.text = QStringLiteral("added after reorder");
            later.anchor_node_uuid = uuid(20);
            hydraulic::editHydraulicNetworkLegacy(repository, "order-right", baseline.uuid,
                {{later.uuid, hydraulic::HydraulicEntity{later}}});
            const native::Hash orderHead = repository.head("order-left").id;
            const std::uint64_t orderBytes = repository.packSize();
            const native::MergeResult orderConflict = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "order-left", "order-right");
            require(orderConflict.status == native::MergeStatus::Conflicts &&
                    repository.head("order-left").id == orderHead &&
                    repository.packSize() == orderBytes,
                    "membership reordering was silently discarded");
            repository.createBranch("remove-left", "append-left");
            repository.createBranch("remove-right", "append-left");
            hydraulic::editHydraulicNetworkLegacy(repository, "remove-left", baseline.uuid,
                {{labelA.uuid, std::nullopt}});
            hydraulic::editHydraulicNetworkLegacy(repository, "remove-right", baseline.uuid,
                {{labelB.uuid, std::nullopt}});
            const native::MergeResult removed = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "remove-left", "remove-right");
            require(removed.status == native::MergeStatus::Merged,
                    "independent deletions from a shared chunk conflicted");
            compareNetworks(baseline, requiredLoad(repository, "remove-left", baseline.uuid));

            // Two new branches create their first membership chunk at the
            // same deterministic serial/UUID, with different contents.
            NetworkHydraulic noLabels = baseline;
            noLabels.map_labels.clear();
            repository.createBranch("empty-labels", "main");
            hydraulic::saveHydraulicNetworkLegacy(repository, "empty-labels", noLabels);
            repository.createBranch("first-left", "empty-labels");
            repository.createBranch("first-right", "empty-labels");
            hydraulic::editHydraulicNetworkLegacy(repository, "first-left", baseline.uuid,
                {{labelA.uuid, hydraulic::HydraulicEntity{labelA}}});
            hydraulic::editHydraulicNetworkLegacy(repository, "first-right", baseline.uuid,
                {{labelB.uuid, hydraulic::HydraulicEntity{labelB}}});
            const native::MergeResult newChunk = hydraulic::mergeHydraulicBranchesLegacy(
                repository, "first-left", "first-right");
            require(newChunk.status == native::MergeStatus::Merged,
                    "independent initial chunks with deterministic IDs did not reconcile");
            noLabels.map_labels.append(labelA);
            noLabels.map_labels.append(labelB);
            compareNetworks(noLabels, requiredLoad(repository, "first-left", baseline.uuid));
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder);
            NetworkHydraulic combined = baseline;
            HydraulicMapLabel labelA = item<HydraulicMapLabel>(8101);
            HydraulicMapLabel labelB = item<HydraulicMapLabel>(8102);
            labelA.text = QStringLiteral("left append");
            labelB.text = QStringLiteral("right append");
            labelA.anchor_node_uuid = uuid(20);
            labelB.anchor_node_uuid = uuid(20);
            combined.map_labels.append(labelA);
            combined.map_labels.append(labelB);
            compareNetworks(combined, requiredLoad(reopened, "append-left", baseline.uuid));
            compareNetworks(baseline, requiredLoad(reopened, "remove-left", baseline.uuid));
            combined.map_labels.clear();
            combined.map_labels.append(labelA);
            combined.map_labels.append(labelB);
            compareNetworks(combined, requiredLoad(reopened, "first-left", baseline.uuid));
            reopened.verifyPack();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// A revision is an immutable read boundary: branch movement must never
// combine metadata, membership, and model payloads from different commits.
void exerciseSnapshotReads() {
    const std::filesystem::path folder = tempDir();
    try {
        native::Repository repository(folder);
        repository.createBranch("main");
        NetworkHydraulic initial = completeNetwork();
        initial.title_line_1 = QStringLiteral("0");
        initial.nodes_junctions[0].metadata.description = QStringLiteral("0");
        initial.map_labels[0].text = QStringLiteral("0");
        const native::Revision original = hydraulic::saveHydraulicNetworkLegacy(repository, "main", initial);
        const std::optional<NetworkHydraulic> originalCopy =
            hydraulic::loadHydraulicNetworkAtRevisionLegacy(repository, original.id, initial.uuid);
        require(originalCopy.has_value(), "cannot read original revision");
        compareNetworks(initial, *originalCopy);
        const std::optional<QList<QUuid>> originalLabels =
            hydraulic::listHydraulicEntityIdsAtRevision(repository, original.id, initial.uuid,
                                                       hydraulic::HydraulicEntityType::MapLabel);
        require(originalLabels && originalLabels->size() == 1 &&
                originalLabels->first() == uuid(28),
                "original membership list is incorrect");
        const std::optional<hydraulic::HydraulicEntity> originalLabel =
            hydraulic::loadHydraulicEntityAtRevisionLegacy(repository, original.id, initial.uuid, uuid(28));
        require(originalLabel && std::get_if<HydraulicMapLabel>(&*originalLabel) != nullptr &&
                std::get<HydraulicMapLabel>(*originalLabel).text == QStringLiteral("0"),
                "original targeted read is incorrect");

        NetworkHydraulic updated = initial;
        updated.title_line_1 = QStringLiteral("2");
        updated.nodes_junctions[0].metadata.description = QStringLiteral("2");
        updated.map_labels[0].text = QStringLiteral("2");
        HydraulicMapLabel additional = item<HydraulicMapLabel>(299);
        additional.anchor_node_uuid = uuid(20);
        additional.text = QStringLiteral("extra");
        updated.map_labels.append(additional);
        const native::Revision second = hydraulic::saveHydraulicNetworkLegacy(repository, "main", updated);
        require(second.id != original.id, "updated network did not advance branch");

        compareNetworks(initial, requiredLoadAtRevision(repository, original.id, initial.uuid));
        compareNetworks(updated, requiredLoadAtRevision(repository, second.id, initial.uuid));
        const std::optional<QList<QUuid>> before =
            hydraulic::listHydraulicEntityIdsAtRevision(repository, original.id, initial.uuid,
                                                       hydraulic::HydraulicEntityType::MapLabel);
        const std::optional<QList<QUuid>> after =
            hydraulic::listHydraulicEntityIdsAtRevision(repository, second.id, initial.uuid,
                                                       hydraulic::HydraulicEntityType::MapLabel);
        require(before && before->size() == 1 && after && after->size() == 2,
                "membership lists did not honor requested revision");
        require(!hydraulic::loadHydraulicEntityAtRevisionLegacy(
                    repository, original.id, initial.uuid, additional.uuid),
                "future entity was visible in historical revision");
        const std::optional<hydraulic::HydraulicEntity> inserted =
            hydraulic::loadHydraulicEntityAtRevisionLegacy(
                repository, second.id, initial.uuid, additional.uuid);
        require(inserted && std::get_if<HydraulicMapLabel>(&*inserted) != nullptr,
                "new member missing in new revision");
        require(!hydraulic::loadHydraulicNetworkAtRevisionLegacy(
                    repository, original.id, uuid(98765)),
                "missing historical network was not a clean not-found");
        require(!hydraulic::listHydraulicEntityIdsAtRevision(
                    repository, original.id, uuid(98765),
                    hydraulic::HydraulicEntityType::MapLabel),
                "missing historical network list was not a clean not-found");
        require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] { (void)hydraulic::loadHydraulicNetworkAtRevisionLegacy(
                    repository, native::Hash{}, initial.uuid); }),
                "invalid revision was not rejected");
        require(throwsStorageError(native::StorageErrorKind::NotFound, [&] {
                    (void)hydraulic::loadHydraulicNetworkAtRevisionLegacy(
                        repository, native::sha256(native::Bytes{9, 9}), initial.uuid); }),
                "unknown revision was not a typed NotFound");
        require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] { (void)hydraulic::listHydraulicEntityIdsAtRevision(
                    repository, original.id, initial.uuid,
                    static_cast<hydraulic::HydraulicEntityType>(999)); }),
                "invalid collection was not rejected");

        // Concurrent writers repeatedly replace every snapshot marker and
        // sometimes rewrite collection membership. Each returned aggregate
        // must belong wholly to one generation, including its member count.
        std::atomic<bool> begin{false};
        std::atomic<int> readerChecks{0};
        std::exception_ptr failure;
        std::mutex failureMutex;
        const std::function<void(std::exception_ptr)> reportFailure =
            [&failure, &failureMutex](std::exception_ptr errorPtr) {
                std::lock_guard<std::mutex> guarded(failureMutex);
                if (!failure) failure = errorPtr;
            };
        const std::function<void()> reader = [&]() {
            try {
                while (!begin.load()) std::this_thread::yield();
                for (int iteration = 0; iteration < 100; ++iteration) {
                    const std::optional<NetworkHydraulic> observed =
                        hydraulic::loadHydraulicNetwork(repository, "main", initial.uuid);
                    require(observed.has_value(), "concurrent network read failed");
                    const QString marker = observed->title_line_1;
                    bool numeric = false;
                    const int generation = marker.toInt(&numeric);
                    require(numeric && generation >= 2 && generation <= 40,
                            "invalid generation marker in concurrent read");
                    require(observed->nodes_junctions[0].metadata.description == marker &&
                            observed->map_labels[0].text == marker,
                            "mixed network entity generations in one read");
                    require(observed->map_labels.size() == (generation % 2 == 0 ? 2 : 1),
                            "mixed network manifest and metadata generations");
                    if (iteration % 5 == 0) {
                        // A second branch read may select a newer revision,
                        // but must still return an internally valid member.
                        const std::optional<hydraulic::HydraulicEntity> targeted =
                            hydraulic::loadHydraulicEntity(
                                repository, "main", initial.uuid, uuid(28));
                        require(targeted && std::get_if<HydraulicMapLabel>(&*targeted) != nullptr, "concurrent targeted lookup failed");
                        const QString entityMarker = std::get<HydraulicMapLabel>(*targeted).text;
                        bool validMarker = false;
                        const int entityGeneration = entityMarker.toInt(&validMarker);
                        require(validMarker && entityGeneration >= 2 && entityGeneration <= 40,
                                "concurrent targeted lookup returned an invalid generation");
                        const std::optional<QList<QUuid>> listed =
                            hydraulic::listHydraulicEntityIds(
                                repository, "main", initial.uuid,
                                hydraulic::HydraulicEntityType::MapLabel);
                        require(listed && (listed->size() == 1 || listed->size() == 2) &&
                                listed->first() == uuid(28),
                                "concurrent membership listing failed");
                        compareNetworks(initial, requiredLoadAtRevision(
                            repository, original.id, initial.uuid));
                    }
                    ++readerChecks;
                    std::this_thread::yield();
                }
            } catch (...) {
                reportFailure(std::current_exception());
            }
        };
        // 'updated' corresponds to generation 2; start at generation 3.
        std::thread firstReader(reader);
        std::thread secondReader(reader);
        std::thread writer([&]() {
            try {
                begin.store(true);
                for (int generation = 3; generation <= 40; ++generation) {
                    updated.title_line_1 = QString::number(generation);
                    updated.nodes_junctions[0].metadata.description = QString::number(generation);
                    updated.map_labels[0].text = QString::number(generation);
                    if (generation % 2 != 0) updated.map_labels.removeLast();
                    else if (updated.map_labels.size() == 1) updated.map_labels.append(additional);
                    (void)hydraulic::saveHydraulicNetworkLegacy(repository, "main", updated);
                    std::this_thread::yield();
                }
            } catch (...) {
                reportFailure(std::current_exception());
            }
        });
        firstReader.join();
        secondReader.join();
        writer.join();
        if (failure) std::rethrow_exception(failure);
        require(readerChecks == 200, "concurrent snapshot readers did not finish");
        compareNetworks(initial, requiredLoadAtRevision(repository, original.id, initial.uuid));
        compareNetworks(updated, requiredLoad(repository, "main", initial.uuid));
        require(repository.verifyRepository().revisions >= 40,
                "concurrent writes did not preserve valid historical revisions");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Explicit caller-provided optimistic pins must survive the entire adapter
// preparation path and reach the store's atomic compare-and-publish boundary.
void exerciseCallerPinnedWrites() {
    const std::filesystem::path folder = tempDir();
    try {
        native::Repository repository(folder);
        repository.createBranch("main");
        NetworkHydraulic network = completeNetwork();
        const native::Hash emptyHead = repository.head("main").id;
        const native::Revision initial = hydraulic::saveHydraulicNetworkLegacy(
            repository, "main", network, emptyHead);
        require(initial.id != emptyHead, "caller-pinned first save did not commit");
        bool staleRejected = false;
        try {
            (void)hydraulic::saveHydraulicNetworkLegacy(repository, "main", network, emptyHead);
        } catch (const std::runtime_error&) { staleRejected = true; }
        require(staleRejected && repository.head("main").id == initial.id,
                "stale full-network save was accepted");

        HydraulicNodeJunction updated = network.nodes_junctions[0];
        updated.elevation_m += 2.0;
        const std::vector<hydraulic::HydraulicEntityChange> changes{
            {updated.uuid, hydraulic::HydraulicEntity{updated}}};
        const native::Revision edited = hydraulic::editHydraulicNetworkLegacy(
            repository, "main", network.uuid, changes, nullptr, initial.id);
        require(edited.id != initial.id, "caller-pinned edit did not commit");
        staleRejected = false;
        try {
            (void)hydraulic::editHydraulicNetworkLegacy(repository, "main", network.uuid,
                                                  changes, nullptr, initial.id);
        } catch (const std::runtime_error&) { staleRejected = true; }
        require(staleRejected && repository.head("main").id == edited.id,
                "stale indexed edit was accepted");
        staleRejected = false;
        try {
            (void)hydraulic::editHydraulicNetworkLegacy(repository, "main", network.uuid,
                                                  {}, nullptr, initial.id);
        } catch (const std::runtime_error&) { staleRejected = true; }
        require(staleRejected && repository.head("main").id == edited.id,
                "stale empty edit was accepted");
        const native::Revision noop = hydraulic::editHydraulicNetworkLegacy(
            repository, "main", network.uuid, {}, nullptr, edited.id);
        require(noop.id == edited.id && repository.verifyRepository().revisions >= 3,
                "valid pinned no-op changed history or damaged repository");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// A fast-forward's source may have been written by the generic storage API
// rather than by the hydraulic adapter. The adapter must reject that history
// if its model records cannot form a valid complete hydraulic network.
void exerciseHydraulicFastForwardValidation() {
    const std::filesystem::path folder = tempDir();
    try {
        native::Repository repository(folder);
        repository.createBranch("main");
        const NetworkHydraulic baseline = completeNetwork();
        const native::Revision initial = hydraulic::saveHydraulicNetworkLegacy(repository, "main", baseline);
        repository.createBranch("ff-target", "main");
        repository.createBranch("ff-source", "ff-target");
        HydraulicNodeJunction junction = baseline.nodes_junctions[0];
        junction.elevation_m += 5.0;
        const native::Revision validTip = hydraulic::editHydraulicNetworkLegacy(repository,
            "ff-source", baseline.uuid, {{junction.uuid, hydraulic::HydraulicEntity{junction}}});
        const std::uint64_t validBytes = repository.packSize();
        hydraulic::HydraulicMergeSelectionsLegacy validPins;
        validPins.expectedTargetHead = initial.id;
        validPins.expectedSourceHead = validTip.id;
        const native::MergeResult valid = hydraulic::mergeHydraulicBranchesLegacy(
            repository, "ff-target", "ff-source", validPins);
        require(valid.status == native::MergeStatus::FastForward &&
                valid.revision.id == validTip.id &&
                repository.head("ff-target").id == validTip.id &&
                repository.head("ff-source").id == validTip.id &&
                repository.packSize() == validBytes,
                "valid hydraulic fast-forward did not reuse source revision");
        NetworkHydraulic expected = baseline;
        expected.nodes_junctions[0] = junction;
        compareNetworks(expected, requiredLoad(repository, "ff-target", baseline.uuid));
        validPins.expectedTargetHead = validTip.id;
        require(hydraulic::mergeHydraulicBranchesLegacy(repository, "ff-target", "ff-source",
                validPins).status == native::MergeStatus::UpToDate,
                "equal-tip hydraulic no-op unexpectedly failed");

        repository.createBranch("bad-target", "main");
        repository.createBranch("bad-source", "bad-target");
        const native::Key member = hydraulic::keyFromUuid(baseline.nodes_junctions[0].uuid);
        const native::Key orphan = hydraulic::keyFromUuid(uuid(9999));
        const std::optional<native::ModelRecord> original = repository.getModel("bad-source", member);
        require(original.has_value(), "missing canonical hydraulic fixture record");
        // This is accepted by the generic native index, but not by hydraulic
        // membership/ownership validation. A bundle import can similarly
        // supply a generic model-indexed source snapshot.
        const native::Revision badTip = repository.commitModel("bad-source", {{orphan, *original}});
        const std::uint64_t badBytes = repository.packSize();
        hydraulic::HydraulicMergeSelectionsLegacy badPins;
        badPins.expectedTargetHead = initial.id;
        badPins.expectedSourceHead = badTip.id;
        const native::MergeResult invalid = hydraulic::mergeHydraulicBranchesLegacy(
            repository, "bad-target", "bad-source", badPins);
        require(invalid.status == native::MergeStatus::Conflicts &&
                !invalid.conflicts.empty() &&
                repository.head("bad-target").id == initial.id &&
                repository.head("bad-source").id == badTip.id &&
                repository.packSize() == badBytes,
                "hydraulic fast-forward accepted orphan model record or modified the pack");
        badPins.expectedSourceHead = initial.id;
        bool staleRejected = false;
        try { (void)hydraulic::mergeHydraulicBranchesLegacy(
            repository, "bad-target", "bad-source", badPins); }
        catch (const native::StorageError& error) {
            staleRejected = error.kind() == native::StorageErrorKind::Conflict;
        }
        require(staleRejected && repository.head("bad-target").id == initial.id &&
                repository.packSize() == badBytes,
                "stale hydraulic fast-forward source pin was not rejected");
        badPins.expectedSourceHead = badTip.id;
        badPins.expectedTargetHead = badTip.id;
        staleRejected = false;
        try { (void)hydraulic::mergeHydraulicBranchesLegacy(
            repository, "bad-target", "bad-source", badPins); }
        catch (const native::StorageError& error) {
            staleRejected = error.kind() == native::StorageErrorKind::Conflict;
        }
        require(staleRejected && repository.head("bad-target").id == initial.id &&
                repository.packSize() == badBytes,
                "stale hydraulic fast-forward target pin was not rejected");
        repository.verifyPack();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// End-to-end UUIDv7 hydraulic workflow: one pinned model snapshot powers
// every network, membership and indexed-ownership read. Model edits publish
// a distinct UUID revision; historical identities are never regenerated.
void exerciseUuidV7Hydraulic() {
    const std::filesystem::path folder = tempDir();
    try {
        const native::ProjectId project = native::ProjectId::generate();
        const NetworkHydraulic baseline = completeNetwork();
        native::RevisionId initial = native::RevisionId::generate();
        native::RevisionId saved = initial;
        native::RevisionId edited = initial;
        {
            native::Repository repository(folder);
            initial = repository.createBranchV2(project, "main");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::loadHydraulicNetworkAtRevision(repository, project, initial, baseline.uuid);
            }), "initial raw UUIDv7 revision should reject model lookup");
            saved = hydraulic::saveHydraulicNetwork(repository, project, "main", baseline, initial);
            require(saved != initial && repository.headV2(project, "main") == saved,
                    "UUIDv7 hydraulic save did not publish a fresh revision");
            const std::optional<NetworkHydraulic> loaded =
                hydraulic::loadHydraulicNetworkAtRevision(repository, project, saved, baseline.uuid);
            require(loaded.has_value(), "UUIDv7 hydraulic network missing after save");
            compareNetworks(baseline, *loaded);
            const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                repository, project, saved, baseline.uuid, hydraulic::HydraulicEntityType::Junction, 100);
            require(ids && ids->size() == baseline.nodes_junctions.size(),
                    "UUIDv7 membership list missing junctions");
            require(throwsStorageError(native::StorageErrorKind::LimitExceeded, [&] {
                (void)hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                    repository, project, saved, baseline.uuid, hydraulic::HydraulicEntityType::Junction, 1);
            }), "UUIDv7 bounded membership did not reject excess members");
            const std::optional<hydraulic::HydraulicEntity> targeted =
                hydraulic::loadHydraulicEntityAtRevision(repository, project, saved,
                    baseline.uuid, baseline.nodes_junctions.front().uuid);
            require(targeted && std::holds_alternative<HydraulicNodeJunction>(*targeted),
                    "UUIDv7 indexed ownership lookup failed");
            require(!hydraulic::loadHydraulicEntityAtRevision(
                repository, project, saved, baseline.uuid, uuid(10000)),
                "UUIDv7 indexed lookup found an unowned entity");
            HydraulicNodeJunction updated = baseline.nodes_junctions.front();
            updated.metadata.description = QStringLiteral("UUIDv7 updated junction");
            const std::vector<hydraulic::HydraulicEntityChange> changes{
                {updated.uuid, hydraulic::HydraulicEntity{updated}}};
            edited = hydraulic::editHydraulicNetwork(
                repository, project, "main", baseline.uuid, changes, saved);
            require(edited != saved && repository.headV2(project, "main") == edited,
                    "UUIDv7 hydraulic edit failed to advance HEAD");
            require(hydraulic::editHydraulicNetwork(
                repository, project, "main", baseline.uuid, changes, edited) == edited,
                    "UUIDv7 no-op edit created a new revision");
            NetworkHydraulic expected = baseline;
            expected.nodes_junctions.front() = updated;
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, edited, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(
                    repository, project, "main", baseline.uuid, changes, saved);
            }), "UUIDv7 stale HEAD edit was accepted");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::saveHydraulicNetwork(repository, project, "main", baseline, saved);
            }), "UUIDv7 stale full save was accepted");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "main", baseline.uuid,
                    {{uuid(9900), std::nullopt}}, edited);
            }), "UUIDv7 unowned deletion was accepted");
            const native::RevisionId fork = repository.createBranchAtRevisionV2(
                project, "history", "main", saved);
            require(fork == saved && repository.headV2(project, "history") == saved,
                    "historical UUIDv7 branch was not pinned to immutable revision");
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.headV2(project, "main") == edited &&
                    reopened.headV2(project, "history") == saved,
                    "UUIDv7 hydraulic HEAD identities changed after reopen/compaction");
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                reopened, project, saved, baseline.uuid));
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

void exerciseUuidV7HydraulicMerges() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    try {
        native::RevisionId resolved = native::RevisionId::generate();
        const NetworkHydraulic baseline = completeNetwork();
        {
            native::Repository repository(folder);
            const native::RevisionId root = repository.createBranchV2(project, "main");
            const native::RevisionId base = hydraulic::saveHydraulicNetwork(
                repository, project, "main", baseline, root);
            require(repository.createBranchV2(project, "feature", "main") == base,
                    "UUIDv7 hydraulic fork generated an extra revision");
            HydraulicNodeJunction targetJunction = baseline.nodes_junctions[0];
            targetJunction.metadata.description = QStringLiteral("hydraulic-target");
            const native::RevisionId target = hydraulic::editHydraulicNetwork(
                repository, project, "main", baseline.uuid,
                {{targetJunction.uuid, hydraulic::HydraulicEntity{targetJunction}}}, base);
            HydraulicNodeJunction sourceJunction = baseline.nodes_junctions[0];
            sourceJunction.metadata.description = QStringLiteral("hydraulic-source");
            const native::RevisionId source = hydraulic::editHydraulicNetwork(
                repository, project, "feature", baseline.uuid,
                {{sourceJunction.uuid, hydraulic::HydraulicEntity{sourceJunction}}}, base);
            hydraulic::HydraulicMergeSelections selections{target, source, {}, {}};
            const std::uint64_t before = repository.packSize();
            const native::MergeResultV2 conflict = hydraulic::mergeHydraulicBranches(
                repository, project, "main", "feature", selections);
            require(conflict.status == native::MergeStatus::Conflicts &&
                    !conflict.conflicts.empty() && repository.headV2(project, "main") == target &&
                    repository.headV2(project, "feature") == source &&
                    repository.packSize() == before,
                    "UUIDv7 hydraulic conflict changed immutable revisions");
            selections.fields.push_back(hydraulic::HydraulicFieldChoice{
                targetJunction.uuid, QStringLiteral("metadata.description"),
                hydraulic::HydraulicMergeSide::Source});
            const native::MergeResultV2 merged = hydraulic::mergeHydraulicBranches(
                repository, project, "main", "feature", selections);
            require(merged.status == native::MergeStatus::Merged && merged.resultHead &&
                    repository.headV2(project, "feature") == source,
                    "UUIDv7 hydraulic explicit field choice did not produce a merge");
            resolved = *merged.resultHead;
            const native::RevisionRecordV2 record = native::decodeRevisionRecordV2(
                *repository.readRevisionV2(project, resolved));
            require(record.parents.size() == 2 && record.parents[0].id == target &&
                    record.parents[1].id == source,
                    "UUIDv7 resolved hydraulic merge lost its two parent identities");
            NetworkHydraulic expected = baseline;
            expected.nodes_junctions[0] = sourceJunction;
            const std::optional<NetworkHydraulic> actual = hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, resolved, baseline.uuid);
            require(actual.has_value(), "resolved UUIDv7 network missing");
            compareNetworks(expected, *actual);
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::mergeHydraulicBranches(repository, project,
                    "main", "feature", selections);
            }), "stale UUIDv7 hydraulic merge selections were accepted");

            // A chosen UUID must actually participate in the merge; silent
            // ignored choices would apply outdated GUI decisions to new data.
            hydraulic::HydraulicMergeSelections unused{resolved, source, {}, {}};
            unused.objects.push_back(hydraulic::HydraulicObjectChoice{
                uuid(9700), hydraulic::HydraulicMergeSide::Target});
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::mergeHydraulicBranches(repository, project,
                    "main", "feature", unused);
            }), "unused UUIDv7 hydraulic object choice was silently accepted");

            // A generic model record can be valid for the native model index
            // but invalid as a hydraulic-network member. Fast-forward must
            // run the whole-network validator before moving the target HEAD.
            require(repository.createBranchAtRevisionV2(project,
                "invalid-left", "main", base) == base, "cannot create v2 invalid merge target");
            require(repository.createBranchAtRevisionV2(project,
                "invalid-right", "main", base) == base, "cannot create v2 invalid merge source");
            const native::Key existingKey = hydraulic::keyFromUuid(baseline.nodes_junctions[0].uuid);
            const std::optional<native::ModelRecord> member = repository.getModelAtRevisionV2(
                project, base, existingKey);
            require(member.has_value(), "missing v2 hydraulic member fixture");
            const native::RevisionId badTip = repository.commitModelV2(
                project, "invalid-right", {{hydraulic::keyFromUuid(uuid(9701)), *member}}, base);
            const std::uint64_t sizeBefore = repository.packSize();
            const native::MergeResultV2 invalid = hydraulic::mergeHydraulicBranches(
                repository, project, "invalid-left", "invalid-right",
                hydraulic::HydraulicMergeSelections{base, badTip, {}, {}});
            require(invalid.status == native::MergeStatus::Conflicts &&
                    repository.headV2(project, "invalid-left") == base &&
                    repository.headV2(project, "invalid-right") == badTip &&
                    repository.packSize() == sizeBefore,
                    "invalid UUIDv7 hydraulic fast-forward published a corrupt network");
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.headV2(project, "main") == resolved &&
                    reopened.revisionReachableFromV2(project, resolved,
                        reopened.headV2(project, "feature")),
                    "resolved hydraulic UUIDv7 ancestry lost after compaction/reopen");
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        exerciseEmptyRawBranch();
        exercise();
        exerciseChunkBoundaries();
        exerciseDependentMembership();
        exerciseDependentUpdates();
        exerciseHydraulicMerging();
        exerciseHydraulicFastForwardValidation();
        exerciseStructuralMergeChoices();
        exerciseCoordinatedMembershipMerges();
        exerciseSnapshotReads();
        exerciseCallerPinnedWrites();
        exerciseUuidV7Hydraulic();
        exerciseUuidV7HydraulicMerges();
        std::cout << "Complete hydraulic network persistence tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Hydraulic network persistence test failed: " << failure.what() << '\n';
        return 1;
    }
}
