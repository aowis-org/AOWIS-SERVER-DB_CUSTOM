#include "aowis/native/hydraulic_network_persistence.h"

#include <aowis/native/model_validation.h>

#include <QCoreApplication>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
namespace native = aowis::native;
namespace v5 = aowis::native::v5;

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
    return v5::uuidFromKey(key);
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
                           "aowis-db-custom-network-test-XXXXXXXX").string();
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
            aowis::native::ModelCodec<NetworkMultiSpecies>::encode(result.multi_species) ==
            aowis::native::ModelCodec<NetworkMultiSpecies>::encode(source.multi_species),
            "network metadata changed on reload");
#define CHECK_LIST(name, type) \
    require(result.name.size() == source.name.size(), "network entity count changed: " #name); \
    for (qsizetype i = 0; i < source.name.size(); ++i) { \
        require(result.name[i].uuid == source.name[i].uuid, "network order changed: " #name); \
        require(aowis::native::ModelCodec<type>::encode(result.name[i]) == \
                aowis::native::ModelCodec<type>::encode(source.name[i]), \
                "canonical V5 payload changed: " #name); \
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

NetworkHydraulic requiredLoad(native::Repository& repository, std::string_view branch,
                              const QUuid& id) {
    QString error;
    const std::optional<NetworkHydraulic> loaded =
        v5::loadHydraulicNetwork(repository, branch, id, &error);
    if (!loaded) throw std::runtime_error("network load failed: " + error.toStdString());
    return *loaded;
}

void exercise(native::PhysicalStore physical) {
    const std::filesystem::path folder = tempDir();
    try {
        const NetworkHydraulic initial = completeNetwork();
        require(aowis::native::validateHydraulicNetwork(initial).valid(), "test network invalid");
        native::Hash original{};
        {
            native::Repository repository(folder, physical);
            repository.createBranch("main");
            original = v5::saveHydraulicNetwork(repository, "main", initial).id;
            require(repository.head("main").modelIndexed, "model index was not published");
            QString error;
            const std::optional<NetworkHydraulic> restored =
                v5::loadHydraulicNetwork(repository, "main", initial.uuid, &error);
            require(restored.has_value(), "first network load failed");
            compareNetworks(initial, *restored);
            // Fast indexed reads must return the canonical entity without
            // reconstructing the aggregate. Check all 18 collection kinds.
            std::size_t checkedCollection = 0;
#define CHECK_FAST_READ(name, type) \
            { \
                const std::optional<QList<QUuid>> ids = v5::listHydraulicEntityIds( \
                    repository, "main", initial.uuid, \
                    static_cast<v5::HydraulicEntityType>(checkedCollection), &error); \
                require(ids.has_value() && *ids == [&]() { \
                    QList<QUuid> expected; \
                    for (const type& item : initial.name) expected.append(item.uuid); \
                    return expected; \
                }(), "fast membership list changed: " #name); \
                for (const type& expected : initial.name) { \
                    const std::optional<v5::HydraulicEntity> actual = v5::loadHydraulicEntity( \
                        repository, "main", initial.uuid, expected.uuid, &error); \
                    require(actual.has_value(), "fast entity lookup missing: " #name); \
                    const type* entity = std::get_if<type>(&*actual); \
                    require(entity != nullptr && \
                        aowis::native::ModelCodec<type>::encode(*entity) == \
                        aowis::native::ModelCodec<type>::encode(expected), \
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
            require(!v5::loadHydraulicEntity(repository, "main", initial.uuid, uuid(999), &error) &&
                    error.isEmpty(), "unknown UUID unexpectedly resolved");
            require(!v5::loadHydraulicEntity(repository, "main", uuid(999), uuid(23), &error) &&
                    error.isEmpty(), "entity unexpectedly belongs to another network");
            require(!v5::listHydraulicEntityIds(repository, "main", uuid(999),
                     v5::HydraulicEntityType::Pipe, &error) && error.isEmpty(),
                    "unknown network unexpectedly listed members");
            require(!v5::listHydraulicEntityIds(repository, "main", initial.uuid,
                     static_cast<v5::HydraulicEntityType>(99), &error) && !error.isEmpty(),
                    "invalid collection kind not rejected");
            require(repository.referencingModel("main", v5::keyFromUuid(uuid(19))).size() >= 2,
                    "inbound model references were not indexed");
            bool rejectedRawDeletion = false;
            try { repository.commitModel("main", {{v5::keyFromUuid(uuid(19)), std::nullopt}}); }
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
            const native::Key labelKey = v5::keyFromUuid(uuid(28));
            const std::vector<native::Key> labelOwners = repository.referencingModel("main", labelKey);
            require(labelOwners.size() == 1, "label must belong to exactly one membership chunk");
            const native::Key labelChunkKey = labelOwners.front();
            const std::optional<native::ModelRecord> labelChunk =
                repository.getModel("main", labelChunkKey);
            require(labelChunk.has_value() && labelChunk->references.size() == 1 &&
                    labelChunk->references.front() == labelKey,
                    "original label membership chunk is invalid");
            v5::saveHydraulicNetwork(repository, "feature", modified);
            const std::vector<native::Difference> differences = repository.diff("main", "feature");
            require(differences.size() == 4,
                    "whole-network update must affect root, pipe, label and retired membership chunk");
            const auto containsChangedKey = [&differences](const native::Key& key) {
                return std::any_of(differences.begin(), differences.end(),
                                   [&key](const native::Difference& difference) {
                                       return difference.key == key;
                                   });
            };
            require(containsChangedKey(v5::keyFromUuid(initial.uuid)) &&
                    containsChangedKey(v5::keyFromUuid(uuid(23))) &&
                    containsChangedKey(labelKey) && containsChangedKey(labelChunkKey),
                    "whole-network update changed unexpected model records");
            require(repository.getModel("main", labelChunkKey).has_value() &&
                    !repository.getModel("feature", labelChunkKey).has_value(),
                    "membership chunk deletion did not preserve branch isolation");
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));
            compareNetworks(modified, requiredLoad(repository, "feature", initial.uuid));
            require(!repository.getModel("feature", v5::keyFromUuid(uuid(28))),
                    "removed label still has a model record");
            const std::optional<v5::HydraulicEntity> featurePipe =
                v5::loadHydraulicEntity(repository, "feature", initial.uuid, uuid(23), &error);
            require(featurePipe.has_value() &&
                    std::get<HydraulicLinkPipe>(*featurePipe).diameter_mm == 211.5,
                    "indexed entity read did not follow branch changes");
            require(!v5::loadHydraulicEntity(repository, "feature", initial.uuid, uuid(28), &error) &&
                    error.isEmpty(), "deleted entity still appears as a member");
            require(v5::listHydraulicEntityIds(repository, "feature", initial.uuid,
                    v5::HydraulicEntityType::MapLabel, &error)->empty(),
                    "deleted label still appears in ordered membership");
            require(v5::loadHydraulicEntity(repository, "main", initial.uuid, uuid(28), &error).has_value(),
                    "branch isolation failed for indexed lookups");
            repository.createBranch("corrupt-query", "main");
            const std::optional<native::ModelRecord> validLabel =
                repository.getModel("corrupt-query", v5::keyFromUuid(uuid(28)));
            require(validLabel.has_value(), "test label disappeared");
            native::ModelRecord malformedLabel = *validLabel;
            malformedLabel.kind = 999;
            repository.commitModel("corrupt-query", {
                {v5::keyFromUuid(uuid(28)), std::move(malformedLabel)}});
            require(!v5::loadHydraulicEntity(repository, "corrupt-query", initial.uuid,
                    uuid(28), &error) && !error.isEmpty(),
                    "indexed query accepted an unknown entity kind");
            require(v5::loadHydraulicEntity(repository, "main", initial.uuid, uuid(28), &error).has_value(),
                    "malformed branch damaged the original branch");

            // Fast indexed scalar edits must NOT reconstruct the entire
            // network, modify its membership, or lose typed reference checks.
            repository.createBranch("scalar-indexed", "main");
            const native::ModelRecord scalarManifest = *repository.getModel(
                "scalar-indexed", v5::keyFromUuid(initial.uuid));
            NetworkHydraulic scalarExpected = initial;
            scalarExpected.links_pipes[0].diameter_mm = 143.25;
            scalarExpected.nodes_junctions[0].elevation_m = 17.0;
            scalarExpected.map_labels[0].text = QStringLiteral("Renamed label");
            v5::HydraulicEditDiagnostics indexedStats;
            const native::Revision scalarRevision = v5::editHydraulicNetwork(
                repository, "scalar-indexed", initial.uuid,
                {{uuid(23), v5::HydraulicEntity{scalarExpected.links_pipes[0]}},
                 {uuid(19), v5::HydraulicEntity{scalarExpected.nodes_junctions[0]}},
                 {uuid(28), v5::HydraulicEntity{scalarExpected.map_labels[0]}}},
                &indexedStats);
            require(indexedStats.indexedScalarFastPath && indexedStats.validatedEntities == 3,
                    "safe scalar changes did not take indexed validation path");
            compareNetworks(scalarExpected, requiredLoad(repository, "scalar-indexed", initial.uuid));
            const std::optional<native::ModelRecord> scalarManifestAfter = repository.getModel(
                "scalar-indexed", v5::keyFromUuid(initial.uuid));
            require(scalarManifestAfter &&
                    native::encodeModelRecord(*scalarManifestAfter) ==
                        native::encodeModelRecord(scalarManifest),
                    "indexed scalar edit changed the membership manifest");
            require(v5::editHydraulicNetwork(repository, "scalar-indexed", initial.uuid,
                    {{uuid(23), v5::HydraulicEntity{scalarExpected.links_pipes[0]}}},
                    &indexedStats).id == scalarRevision.id && indexedStats.indexedScalarFastPath,
                    "no-op scalar edit did not preserve revision identity");

            // An endpoint change is never an indexed-scalar edit, including
            // when a caller sneaks it into an otherwise scalar batch.
            HydraulicLinkPipe invalidRewire = scalarExpected.links_pipes[0];
            invalidRewire.node_uuid_from = uuid(9009);
            bool rejectedScalar = false;
            try {
                v5::editHydraulicNetwork(repository, "scalar-indexed", initial.uuid,
                    {{uuid(23), v5::HydraulicEntity{invalidRewire}}}, &indexedStats);
            } catch (const std::exception&) { rejectedScalar = true; }
            require(rejectedScalar && !indexedStats.indexedScalarFastPath &&
                    repository.head("scalar-indexed").id == scalarRevision.id,
                    "invalid topology bypassed full network validation");
            // Metadata and graph-affecting edits use canonical full validation.
            HydraulicLinkPipe validRewire = scalarExpected.links_pipes[0];
            validRewire.node_uuid_from = uuid(18);
            const native::Revision rewiredRevision = v5::editHydraulicNetwork(
                repository, "scalar-indexed", initial.uuid,
                {{uuid(23), v5::HydraulicEntity{validRewire}}}, &indexedStats);
            require(!indexedStats.indexedScalarFastPath && rewiredRevision.id != scalarRevision.id,
                    "topology edit incorrectly used scalar validation");
            scalarExpected.links_pipes[0] = validRewire;
            compareNetworks(scalarExpected, requiredLoad(repository, "scalar-indexed", initial.uuid));
            // Stage 23: independent pipe endpoint rewires can be validated
            // using the reverse-reference and typed membership indexes.
            repository.createBranch("topology-indexed", "main");
            NetworkHydraulic unreferenced = initial;
            unreferenced.controls_simple.clear(); // Remove the inbound control reference.
            v5::saveHydraulicNetwork(repository, "topology-indexed", unreferenced);
            const native::ModelRecord topologyManifest = *repository.getModel(
                "topology-indexed", v5::keyFromUuid(unreferenced.uuid));
            const native::Revision topologyBase = repository.head("topology-indexed");
            HydraulicLinkPipe independentPipe = unreferenced.links_pipes[0];
            independentPipe.node_uuid_from = uuid(18); // Owned reservoir, valid node endpoint.
            const native::Revision topologyRevision = v5::editHydraulicNetwork(
                repository, "topology-indexed", initial.uuid,
                {{independentPipe.uuid, v5::HydraulicEntity{independentPipe}}}, &indexedStats);
            require(indexedStats.indexedTopologyFastPath &&
                    !indexedStats.indexedScalarFastPath && indexedStats.validatedEntities == 1 &&
                    topologyRevision.id != topologyBase.id,
                    "independent rewire did not take bounded topology validation");
            unreferenced.links_pipes[0] = independentPipe;
            compareNetworks(unreferenced, requiredLoad(repository, "topology-indexed", initial.uuid));
            require(native::encodeModelRecord(*repository.getModel(
                        "topology-indexed", v5::keyFromUuid(initial.uuid))) ==
                    native::encodeModelRecord(topologyManifest),
                    "indexed topology change rewrote network membership");
            const std::vector<native::Key> oldEndpointRefs = repository.referencingModel(
                "topology-indexed", v5::keyFromUuid(uuid(19)));
            require(std::find(oldEndpointRefs.begin(), oldEndpointRefs.end(),
                    v5::keyFromUuid(uuid(23))) == oldEndpointRefs.end(),
                    "indexed rewire retained old endpoint edge");
            const std::vector<native::Key> newEndpointRefs = repository.referencingModel(
                "topology-indexed", v5::keyFromUuid(uuid(18)));
            require(std::find(newEndpointRefs.begin(), newEndpointRefs.end(),
                    v5::keyFromUuid(uuid(23))) != newEndpointRefs.end(),
                    "indexed rewire did not create new endpoint edge");
            bool rejectedTopology = false;
            HydraulicLinkPipe wrongNodeKind = independentPipe;
            wrongNodeKind.node_uuid_from = uuid(10); // A pipe material, not a node.
            try {
                v5::editHydraulicNetwork(repository, "topology-indexed", initial.uuid,
                    {{wrongNodeKind.uuid, v5::HydraulicEntity{wrongNodeKind}}}, &indexedStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !indexedStats.indexedTopologyFastPath &&
                    repository.head("topology-indexed").id == topologyRevision.id,
                    "wrong-type endpoint bypassed typed validation");
            HydraulicLinkPipe missingEndpoint = independentPipe;
            missingEndpoint.node_uuid_from = uuid(9009);
            rejectedTopology = false;
            try {
                v5::editHydraulicNetwork(repository, "topology-indexed", initial.uuid,
                    {{missingEndpoint.uuid, v5::HydraulicEntity{missingEndpoint}}}, &indexedStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !indexedStats.indexedTopologyFastPath &&
                    repository.head("topology-indexed").id == topologyRevision.id,
                    "missing endpoint unexpectedly committed");
            HydraulicLinkPipe metadataRewire = independentPipe;
            metadataRewire.node_uuid_from = uuid(19);
            metadataRewire.id = QStringLiteral("Changed pipe ID");
            const native::Revision canonicalFallback = v5::editHydraulicNetwork(
                repository, "topology-indexed", initial.uuid,
                {{metadataRewire.uuid, v5::HydraulicEntity{metadataRewire}}}, &indexedStats);
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
            v5::saveHydraulicNetwork(repository, "attached-pipe", attached);
            const native::Revision attachedHead = repository.head("attached-pipe");
            HydraulicLinkPipe invalidAttachedRewire = attached.links_pipes[0];
            invalidAttachedRewire.node_uuid_from = uuid(18);
            rejectedTopology = false;
            try {
                v5::editHydraulicNetwork(repository, "attached-pipe", initial.uuid,
                    {{invalidAttachedRewire.uuid, v5::HydraulicEntity{invalidAttachedRewire}}},
                    &indexedStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !indexedStats.indexedTopologyFastPath &&
                    repository.head("attached-pipe").id == attachedHead.id,
                    "dependent demand-point constraint was bypassed");

            // The fast path must also reject any ownership ambiguity.
            const native::Revision otherNetworkRevision = repository.head("main");
            v5::HydraulicEditDiagnostics ownershipStats;
            rejectedScalar = false;
            try {
                v5::editHydraulicNetwork(repository, "main", uuid(9000),
                    {{uuid(23), v5::HydraulicEntity{scalarExpected.links_pipes[0]}}},
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
            v5::HydraulicEditDiagnostics membershipStats;
            const native::Revision membershipBase = repository.head("membership-indexed");
            const native::Revision membershipRevision = v5::editHydraulicNetwork(
                repository, "membership-indexed", initial.uuid,
                {{membershipPipe.uuid, v5::HydraulicEntity{membershipPipe}},
                 {membershipNode.uuid, v5::HydraulicEntity{membershipNode}},
                 {membershipLabel.uuid, v5::HydraulicEntity{membershipLabel}}}, &membershipStats);
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
                "membership-indexed", v5::keyFromUuid(membershipNode.uuid));
            require(std::find(memberIncoming.begin(), memberIncoming.end(),
                    v5::keyFromUuid(membershipPipe.uuid)) != memberIncoming.end() &&
                    std::find(memberIncoming.begin(), memberIncoming.end(),
                    v5::keyFromUuid(membershipLabel.uuid)) != memberIncoming.end(),
                    "new member reference edges were not committed");
            const native::Revision afterRemoval = v5::editHydraulicNetwork(
                repository, "membership-indexed", initial.uuid,
                {{membershipLabel.uuid, std::nullopt}}, &membershipStats);
            require(membershipStats.indexedMembershipFastPath &&
                    membershipStats.validatedEntities == 1 && afterRemoval.id != membershipRevision.id,
                    "independent label deletion missed indexed membership validation");
            membershipExpected.map_labels.removeLast();
            compareNetworks(membershipExpected,
                requiredLoad(repository, "membership-indexed", initial.uuid));
            require(!repository.getModel("membership-indexed", v5::keyFromUuid(membershipLabel.uuid)),
                    "deleted indexed label remains stored");
            const native::Revision beforeBadMembership = repository.head("membership-indexed");
            HydraulicLinkPipe badMembershipPipe = item<HydraulicLinkPipe>(30113);
            badMembershipPipe.node_uuid_from = uuid(10); // Material, not a node.
            badMembershipPipe.node_uuid_to = membershipNode.uuid;
            rejectedTopology = false;
            try {
                v5::editHydraulicNetwork(repository, "membership-indexed", initial.uuid,
                    {{badMembershipPipe.uuid, v5::HydraulicEntity{badMembershipPipe}}},
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
                v5::editHydraulicNetwork(repository, "membership-indexed", initial.uuid,
                    {{badPatternNode.uuid, v5::HydraulicEntity{badPatternNode}}},
                    &membershipStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !membershipStats.indexedMembershipFastPath &&
                    repository.head("membership-indexed").id == beforeBadMembership.id,
                    "missing junction pattern passed indexed validation");
            rejectedTopology = false;
            try {
                v5::editHydraulicNetwork(repository, "membership-indexed", initial.uuid,
                    {{membershipNode.uuid, std::nullopt}}, &membershipStats);
            } catch (const std::exception&) { rejectedTopology = true; }
            require(rejectedTopology && !membershipStats.indexedMembershipFastPath &&
                    repository.head("membership-indexed").id == beforeBadMembership.id,
                    "referenced junction deletion bypassed canonical validation");
            compareNetworks(initial, requiredLoad(repository, "main", initial.uuid));

            // Incremental edits must share the same semantic validation as a
            // complete save without rewriting unchanged records/manifest.
            repository.createBranch("incremental", "feature");
            const native::Key rootKey = v5::keyFromUuid(initial.uuid);
            const std::optional<native::ModelRecord> manifestBefore =
                repository.getModel("incremental", rootKey);
            require(manifestBefore.has_value(), "missing indexed network manifest");
            const native::Bytes manifestBytes = native::encodeModelRecord(*manifestBefore);
            NetworkHydraulic edited = modified;
            HydraulicLinkPipe changedPipe = edited.links_pipes[0];
            changedPipe.diameter_mm = 250.0;
            changedPipe.node_uuid_from = uuid(18);
            edited.links_pipes[0] = changedPipe;
            const native::Revision changed = v5::editHydraulicNetwork(
                repository, "incremental", initial.uuid,
                {{changedPipe.uuid, v5::HydraulicEntity{changedPipe}}});
            compareNetworks(edited, requiredLoad(repository, "incremental", initial.uuid));
            require(changed.id == repository.head("incremental").id,
                    "incremental revision not published");
            const std::optional<native::ModelRecord> manifestAfter =
                repository.getModel("incremental", rootKey);
            require(manifestAfter.has_value() &&
                    native::encodeModelRecord(*manifestAfter) == manifestBytes,
                    "payload-only edit rewrote network membership");
            const std::vector<native::Key> beforeRefs =
                repository.referencingModel("incremental", v5::keyFromUuid(uuid(19)));
            require(std::find(beforeRefs.begin(), beforeRefs.end(), v5::keyFromUuid(uuid(23))) ==
                        beforeRefs.end(), "old pipe endpoint reverse-reference was retained");
            const std::vector<native::Key> afterRefs =
                repository.referencingModel("incremental", v5::keyFromUuid(uuid(18)));
            require(std::find(afterRefs.begin(), afterRefs.end(), v5::keyFromUuid(uuid(23))) !=
                        afterRefs.end(), "new pipe endpoint reverse-reference missing");
            require(v5::editHydraulicNetwork(repository, "incremental", initial.uuid,
                        {{changedPipe.uuid, v5::HydraulicEntity{changedPipe}}}).id == changed.id,
                    "identical hydraulic upsert created an unnecessary revision");
            require(v5::editHydraulicNetwork(repository, "incremental", initial.uuid, {}).id == changed.id,
                    "empty batch created a new revision");

            HydraulicNodeJunction extraNode = item<HydraulicNodeJunction>(202);
            HydraulicLinkPipe extraPipe = item<HydraulicLinkPipe>(203);
            extraPipe.node_uuid_from = uuid(20);
            extraPipe.node_uuid_to = extraNode.uuid;
            const native::Revision inserted = v5::editHydraulicNetwork(
                repository, "incremental", initial.uuid,
                {{extraNode.uuid, v5::HydraulicEntity{extraNode}},
                 {extraPipe.uuid, v5::HydraulicEntity{extraPipe}}});
            edited.nodes_junctions.append(extraNode);
            edited.links_pipes.append(extraPipe);
            compareNetworks(edited, requiredLoad(repository, "incremental", initial.uuid));
            require(inserted.id != changed.id, "new members did not create revision");
            bool rejectedEdit = false;
            try {
                v5::editHydraulicNetwork(repository, "incremental", initial.uuid,
                                         {{extraNode.uuid, std::nullopt}});
            } catch (const std::exception&) { rejectedEdit = true; }
            require(rejectedEdit && repository.head("incremental").id == inserted.id,
                    "deleted referenced node through incremental editor");
            rejectedEdit = false;
            try {
                HydraulicLinkValve wrongType = item<HydraulicLinkValve>(23);
                v5::editHydraulicNetwork(repository, "incremental", initial.uuid,
                                         {{wrongType.uuid, v5::HydraulicEntity{wrongType}}});
            } catch (const std::exception&) { rejectedEdit = true; }
            require(rejectedEdit && repository.head("incremental").id == inserted.id,
                    "incremental editor changed an existing entity kind");
            rejectedEdit = false;
            try {
                v5::editHydraulicNetwork(repository, "incremental", initial.uuid,
                                         {{extraPipe.uuid, std::nullopt},
                                          {extraPipe.uuid, std::nullopt}});
            } catch (const std::exception&) { rejectedEdit = true; }
            require(rejectedEdit && repository.head("incremental").id == inserted.id,
                    "duplicate UUID batch was accepted");
            v5::editHydraulicNetwork(repository, "incremental", initial.uuid,
                                     {{extraPipe.uuid, std::nullopt},
                                      {extraNode.uuid, std::nullopt}});
            edited.links_pipes.removeLast();
            edited.nodes_junctions.removeLast();
            compareNetworks(edited, requiredLoad(repository, "incremental", initial.uuid));
            compareNetworks(modified, requiredLoad(repository, "feature", initial.uuid));
            require(!repository.getModel("incremental", v5::keyFromUuid(extraNode.uuid)),
                    "deleted incremental member still stored in branch");
            repository.verifyPack();

            NetworkHydraulic invalid = modified;
            invalid.nodes_junctions.removeAt(0);
            bool rejected = false;
            try { v5::saveHydraulicNetwork(repository, "feature", invalid); }
            catch (const std::exception&) { rejected = true; }
            require(rejected && repository.head("feature").id != original,
                    "dangling node change was accepted");

            // Independent second network is legal; duplicate ownership is not.
            NetworkHydraulic second;
            second.uuid = uuid(200);
            second.id = QStringLiteral("second-network");
            second.nodes_junctions.append(item<HydraulicNodeJunction>(201));
            v5::saveHydraulicNetwork(repository, "main", second);
            compareNetworks(second, requiredLoad(repository, "main", second.uuid));
            second.nodes_junctions.append(initial.nodes_junctions[0]);
            rejected = false;
            try { v5::saveHydraulicNetwork(repository, "main", second); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "cross-network UUID ownership collision was accepted");

            NetworkHydraulic duplicate = initial;
            duplicate.links_valves[0].uuid = duplicate.links_pipes[0].uuid;
            rejected = false;
            try { v5::saveHydraulicNetwork(repository, "feature", duplicate); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "duplicate UUID among entity kinds was accepted");
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder, physical);
            compareNetworks(initial, requiredLoad(reopened, "main", initial.uuid));
            QString indexedError;
            const std::optional<v5::HydraulicEntity> reopenedPipe =
                v5::loadHydraulicEntity(reopened, "main", initial.uuid, uuid(23), &indexedError);
            require(reopenedPipe.has_value() && indexedError.isEmpty() &&
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
void exerciseChunkBoundaries(native::PhysicalStore physical) {
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
        require(aowis::native::validateHydraulicNetwork(network).valid(),
                "chunk test fixture not valid");
        native::Hash original{};
        {
            native::Repository repository(folder, physical);
            repository.createBranch("main");
            original = v5::saveHydraulicNetwork(repository, "main", network).id;
            const std::optional<native::ModelRecord> root = repository.getModel(
                "main", v5::keyFromUuid(network.uuid));
            require(root.has_value() && root->references.size() == 8,
                    "chunk count was not bounded by 256 members");
            // Snapshot and branch isolation through different membership paths.
            repository.createBranch("feature", "main");
            HydraulicNodeJunction addedNode = item<HydraulicNodeJunction>(804000);
            HydraulicLinkPipe addedPipe = item<HydraulicLinkPipe>(804001);
            addedPipe.node_uuid_from = network.nodes_junctions[0].uuid;
            addedPipe.node_uuid_to = addedNode.uuid;
            v5::HydraulicEditDiagnostics stats;
            v5::editHydraulicNetwork(repository, "feature", network.uuid,
                {{addedNode.uuid, v5::HydraulicEntity{addedNode}},
                 {addedPipe.uuid, v5::HydraulicEntity{addedPipe}}}, &stats);
            require(stats.indexedMembershipFastPath, "chunked insertion missed fast path");
            for (int index = 0; index < 7; ++index) {
                v5::editHydraulicNetwork(repository, "feature", network.uuid,
                    {{network.map_labels[index].uuid, std::nullopt}}, &stats);
                require(stats.indexedMembershipFastPath, "label deletion missed fast path");
            }
            NetworkHydraulic expected = network;
            expected.nodes_junctions.append(addedNode);
            expected.links_pipes.append(addedPipe);
            for (int index = 0; index < 7; ++index) expected.map_labels.removeFirst();
            // Remove all four labels from the final, partial membership chunk.
            // Its owner link and physical model record must be retired atomically.
            std::vector<v5::HydraulicEntityChange> lastChunkRemoval;
            for (int index = 256; index < 260; ++index) {
                lastChunkRemoval.push_back({network.map_labels[index].uuid, std::nullopt});
            }
            v5::editHydraulicNetwork(repository, "feature", network.uuid,
                                    lastChunkRemoval, &stats);
            require(stats.indexedMembershipFastPath,
                    "empty chunk retirement missed indexed validation");
            for (int index = 0; index < 4; ++index) expected.map_labels.removeLast();
            compareNetworks(expected, requiredLoad(repository, "feature", network.uuid));
            compareNetworks(network, requiredLoad(repository, "main", network.uuid));
            const std::optional<QList<QUuid>> ids = v5::listHydraulicEntityIds(repository, "feature",
                network.uuid, v5::HydraulicEntityType::MapLabel);
            require(ids.has_value() && ids->size() == expected.map_labels.size() &&
                    ids->front() == expected.map_labels.front().uuid,
                    "chunk deletion lost ordered membership");
            HydraulicLinkPipe rewired = expected.links_pipes[0];
            rewired.node_uuid_to = expected.nodes_junctions[2].uuid;
            v5::editHydraulicNetwork(repository, "feature", network.uuid,
                {{rewired.uuid, v5::HydraulicEntity{rewired}}}, &stats);
            require(stats.indexedTopologyFastPath, "chunked owner index blocked independent rewire");
            expected.links_pipes[0] = rewired;
            compareNetworks(expected, requiredLoad(repository, "feature", network.uuid));
            repository.verifyPack();
        }
        {
            native::Repository reopened(folder, physical);
            require(reopened.head("main").id == original,
                    "original chunked branch head changed");
            compareNetworks(network, requiredLoad(reopened, "main", network.uuid));
            QString error;
            require(v5::loadHydraulicEntity(reopened, "feature", network.uuid,
                            uuid(804000), &error).has_value() && error.isEmpty(),
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

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        exercise(native::PhysicalStore::Stage7Pack);
        exercise(native::PhysicalStore::ExperimentalBlocks);
        exerciseChunkBoundaries(native::PhysicalStore::Stage7Pack);
        exerciseChunkBoundaries(native::PhysicalStore::ExperimentalBlocks);
        std::cout << "Complete V5 hydraulic network persistence tests passed on both backends\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Hydraulic network persistence test failed: " << failure.what() << '\n';
        return 1;
    }
}
