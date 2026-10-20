#include <aowis/native/hydraulic_network_persistence.h>

#include <QCoreApplication>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/resource.h>

namespace native = aowis::native;
namespace v5 = aowis::native::v5;

namespace {
QUuid uuid(std::uint32_t value) {
    native::Key key{};
    key[6] = 0x70;
    key[8] = 0x80;
    for (int i = 0; i < 4; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    return v5::uuidFromKey(key);
}

double elapsedMs(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3 || argc > 6) {
        std::cerr << "usage: aowis-native-hydraulic-edit-benchmark NEW_REPOSITORY PIPES [SCALAR_EDITS=20] [TOPOLOGY_EDITS=3] [block]\n";
        return 2;
    }
    try {
        const std::filesystem::path directory(argv[1]);
        if (std::filesystem::exists(directory))
            throw std::runtime_error("benchmark repository already exists");
        const int pipeCount = std::stoi(argv[2]);
        const int scalarEdits = argc >= 4 ? std::stoi(argv[3]) : 20;
        const int topologyEdits = argc >= 5 ? std::stoi(argv[4]) : 3;
        if (pipeCount < 2 || pipeCount > 1000000 || scalarEdits < 1 || topologyEdits < 1)
            throw std::runtime_error("invalid benchmark counts");
        const native::PhysicalStore physical = argc >= 6 && std::string(argv[5]) == "block"
            ? native::PhysicalStore::ExperimentalBlocks : native::PhysicalStore::Stage7Pack;

        NetworkHydraulic network;
        network.uuid = uuid(1);
        network.id = QStringLiteral("benchmark");
        network.nodes_junctions.reserve(pipeCount + 1);
        network.links_pipes.reserve(pipeCount);
        for (int i = 0; i <= pipeCount; ++i) {
            HydraulicNodeJunction node;
            node.uuid = uuid(static_cast<std::uint32_t>(100 + i));
            network.nodes_junctions.append(std::move(node));
        }
        for (int i = 0; i < pipeCount; ++i) {
            HydraulicLinkPipe pipe;
            pipe.uuid = uuid(static_cast<std::uint32_t>(1000100 + i));
            pipe.node_uuid_from = network.nodes_junctions[i].uuid;
            pipe.node_uuid_to = network.nodes_junctions[i+1].uuid;
            network.links_pipes.append(std::move(pipe));
        }
        native::Repository repository(directory, physical);
        repository.createBranch("main");
        const std::chrono::steady_clock::time_point importStart = std::chrono::steady_clock::now();
        v5::saveHydraulicNetwork(repository, "main", network);
        const double importMs = elapsedMs(importStart);

        HydraulicLinkPipe edited = network.links_pipes[0];
        const std::chrono::steady_clock::time_point scalarStart = std::chrono::steady_clock::now();
        for (int i = 0; i < scalarEdits; ++i) {
            edited.diameter_mm += 1.0;
            v5::HydraulicEditDiagnostics diagnostic;
            v5::editHydraulicNetwork(repository, "main", network.uuid,
                {{edited.uuid, v5::HydraulicEntity{edited}}}, &diagnostic);
            if (!diagnostic.indexedScalarFastPath || diagnostic.validatedEntities != 1)
                throw std::runtime_error("expected bounded indexed validation");
        }
        const double scalarMs = elapsedMs(scalarStart);

        const std::chrono::steady_clock::time_point topologyStart = std::chrono::steady_clock::now();
        for (int i = 0; i < topologyEdits; ++i) {
            edited.node_uuid_from = network.nodes_junctions[1 + (i % 2)].uuid;
            v5::HydraulicEditDiagnostics diagnostic;
            v5::editHydraulicNetwork(repository, "main", network.uuid,
                {{edited.uuid, v5::HydraulicEntity{edited}}}, &diagnostic);
            if (!diagnostic.indexedTopologyFastPath || diagnostic.indexedScalarFastPath ||
                diagnostic.validatedEntities != 1)
                throw std::runtime_error("independent pipe rewire did not use indexed validation");
        }
        const double topologyMs = elapsedMs(topologyStart);
        // A single membership batch must not decode all pre-existing pipes.
        // Its UUID manifest is still O(network members) and is deliberately
        // measured separately from the O(log N) payload-only edit paths.
        HydraulicNodeJunction newNode;
        newNode.uuid = uuid(4000010);
        HydraulicLinkPipe newPipe;
        newPipe.uuid = uuid(4000011);
        newPipe.node_uuid_from = network.nodes_junctions[0].uuid;
        newPipe.node_uuid_to = newNode.uuid;
        HydraulicMapLabel newLabel;
        newLabel.uuid = uuid(4000012);
        newLabel.anchor_node_uuid = newNode.uuid;
        const std::chrono::steady_clock::time_point insertStart = std::chrono::steady_clock::now();
        v5::HydraulicEditDiagnostics membershipStats;
        v5::editHydraulicNetwork(repository, "main", network.uuid,
            {{newPipe.uuid, v5::HydraulicEntity{newPipe}},
             {newNode.uuid, v5::HydraulicEntity{newNode}},
             {newLabel.uuid, v5::HydraulicEntity{newLabel}}}, &membershipStats);
        if (!membershipStats.indexedMembershipFastPath || membershipStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed membership insertion");
        const double insertionMs = elapsedMs(insertStart);
        const std::chrono::steady_clock::time_point removalStart = std::chrono::steady_clock::now();
        v5::editHydraulicNetwork(repository, "main", network.uuid,
            {{newLabel.uuid, std::nullopt}}, &membershipStats);
        if (!membershipStats.indexedMembershipFastPath || membershipStats.validatedEntities != 1)
            throw std::runtime_error("expected indexed label removal");
        const double removalMs = elapsedMs(removalStart);
        repository.verifyPack();
        const std::optional<native::ModelRecord> networkRoot = repository.getModel(
            "main", v5::keyFromUuid(network.uuid));
        if (!networkRoot || networkRoot->kind != v5::hydraulicNetworkKind)
            throw std::runtime_error("missing indexed network root");
        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
        std::cout << std::fixed << std::setprecision(3)
                  << "engine=" << (physical == native::PhysicalStore::ExperimentalBlocks ? "blocks" : "stage7") << '\n'
                  << "fixture=canonical-v5-whole-network\n"
                  << "pipes=" << pipeCount << '\n'
                  << "scalar_edits=" << scalarEdits << '\n'
                  << "topology_edits=" << topologyEdits << '\n'
                  << "import_ms=" << importMs << '\n'
                  << "indexed_scalar_mean_ms=" << scalarMs / scalarEdits << '\n'
                  << "indexed_topology_mean_ms=" << topologyMs / topologyEdits << '\n'
                  << "indexed_membership_insert_batch_ms=" << insertionMs << '\n'
                  << "indexed_membership_delete_label_ms=" << removalMs << '\n'
                  << "network_membership_chunk_count=" << networkRoot->references.size() << '\n'
                  << "peak_rss_kib=" << usage.ru_maxrss << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hydraulic edit benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
