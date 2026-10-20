#include "aowis/native/model_codec_adapter.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#endif

namespace {
using aowis::native::Key;
using aowis::native::Bytes;
using aowis::native::ModelRecord;
using aowis::native::ModelChange;
using aowis::native::Repository;
namespace hydraulic = aowis::native::hydraulic;

QUuid uuidFor(std::uint8_t kind, std::uint64_t ordinal) {
    Key key{};
    key[0] = kind;
    key[6] = 0x70;
    key[8] = 0x80;
    for (unsigned i = 0; i < 6; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(ordinal >> (8U * i));
    }
    return hydraulic::uuidFromKey(key);
}

HydraulicNodeJunction junctionFor(std::uint64_t ordinal) {
    HydraulicNodeJunction result;
    result.uuid = uuidFor(1, ordinal);
    result.id = QStringLiteral("J-%1").arg(ordinal);
    result.elevation_m = 400.0 + static_cast<double>(ordinal % 400);
    result.coordinate_wgs84.latitude_deg = 11.5 + static_cast<double>(ordinal % 500) / 100000.0;
    result.coordinate_wgs84.longitude_deg = 18.0 + static_cast<double>(ordinal / 500) / 100000.0;
    return result;
}

HydraulicLinkPipe pipeFor(std::uint64_t ordinal, std::uint64_t revision) {
    HydraulicLinkPipe result;
    result.uuid = uuidFor(2, ordinal);
    result.node_uuid_from = uuidFor(1, ordinal);
    result.node_uuid_to = uuidFor(1, ordinal + 1);
    result.id = QStringLiteral("P-%1").arg(ordinal);
    result.length_measured_m = 80.0 + static_cast<double>(ordinal % 300);
    result.diameter_mm = 80.0 + static_cast<double>(ordinal % 10) + static_cast<double>(revision);
    result.roughness_darcy_weisbach_mm = 0.01;
    return result;
}

std::uint64_t positive(const char* arg) {
    const std::string text(arg);
    std::size_t parsed = 0;
    const std::uint64_t result = std::stoull(text, &parsed);
    if (parsed != text.size() || result == 0) throw std::runtime_error("positive integer required");
    return result;
}

double elapsed(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc < 2 || argc > 6) {
        std::cerr << "Usage: aowis-native-hydraulic-model-benchmark <NEW_DIRECTORY> [pipes=1000] [revisions=10] [edits=10] [block]\n"
                  << "Real native model codec bytes and content-addressed native model storage.\n";
        return 2;
    }
    try {
        const std::filesystem::path directory(argv[1]);
        if (std::filesystem::exists(directory)) throw std::runtime_error("benchmark path already exists");
        const std::uint64_t pipes = argc > 2 ? positive(argv[2]) : 1000;
        const std::uint64_t revisions = argc > 3 ? positive(argv[3]) : 10;
        const std::uint64_t edits = argc > 4 ? positive(argv[4]) : 10;
        const bool blocks = argc == 6 && std::string(argv[5]) == "block";
        if (argc == 6 && !blocks) throw std::runtime_error("unknown storage format (expected block)");
        const aowis::native::PhysicalStore engine = blocks
            ? aowis::native::PhysicalStore::ExperimentalBlocks
            : aowis::native::PhysicalStore::Stage7Pack;
        if (pipes > 1000000 || revisions > 10000 || edits > pipes) {
            throw std::runtime_error("invalid size or edits exceed pipe count");
        }
        std::cout << std::fixed << std::setprecision(3);
        const std::chrono::steady_clock::time_point starting = std::chrono::steady_clock::now();
        std::unique_ptr<Repository> store = std::make_unique<Repository>(directory, engine);
        store->createBranch("main");
        const double openMs = elapsed(starting);

        // Measure canonical serialization separately from durable native storage.
        const std::chrono::steady_clock::time_point preparing = std::chrono::steady_clock::now();
        std::vector<ModelChange> initial;
        initial.reserve(static_cast<std::size_t>(2 * pipes + 1));
        for (std::uint64_t i = 0; i <= pipes; ++i) {
            initial.push_back(hydraulic::changeFor(junctionFor(i)));
        }
        for (std::uint64_t i = 0; i < pipes; ++i) {
            initial.push_back(hydraulic::changeFor(pipeFor(i, 0)));
        }
        const double prepareMs = elapsed(preparing);
        const std::chrono::steady_clock::time_point importing = std::chrono::steady_clock::now();
        store->commitModel("main", std::move(initial));
        const double importMs = elapsed(importing);
        const std::chrono::steady_clock::time_point checkingReferences =
            std::chrono::steady_clock::now();
        const Key indexedNode = hydraulic::keyFromUuid(junctionFor(0).uuid);
        const std::size_t incomingReferences = store->referencingModel("main", indexedNode).size();
        const double referenceLookupMs = elapsed(checkingReferences);
        const std::chrono::steady_clock::time_point checkingDeletion =
            std::chrono::steady_clock::now();
        bool deletionRejected = false;
        try { store->commitModel("main", {{indexedNode, std::nullopt}}); }
        catch (const std::runtime_error&) { deletionRejected = true; }
        const double deletionCheckMs = elapsed(checkingDeletion);
        if (!deletionRejected || incomingReferences == 0) {
            throw std::runtime_error("expected indexed endpoint deletion rejection");
        }
        store->createBranch("baseline", "main");

        const std::chrono::steady_clock::time_point revising = std::chrono::steady_clock::now();
        for (std::uint64_t revision = 1; revision <= revisions; ++revision) {
            std::vector<ModelChange> changes;
            changes.reserve(static_cast<std::size_t>(edits));
            for (std::uint64_t j = 0; j < edits; ++j) {
                const std::uint64_t index = (j * 101ULL + revision * 17ULL) % pipes;
                changes.push_back(hydraulic::changeFor(pipeFor(index, revision)));
            }
            store->commitModel("main", std::move(changes));
        }
        const double revisionMs = elapsed(revising);
        const std::chrono::steady_clock::time_point comparing = std::chrono::steady_clock::now();
        const std::size_t differenceCount = store->diff("baseline", "main").size();
        const double diffMs = elapsed(comparing);
        const std::chrono::steady_clock::time_point scanning = std::chrono::steady_clock::now();
        std::uint64_t count = 0;
        for (const std::pair<Key, Bytes>& pair : store->scan("main")) {
            const ModelRecord record = aowis::native::decodeModelRecord(pair.second);
            const QByteArray bytes = hydraulic::qByteArrayFromBytes(record.payload);
            if (record.kind == hydraulic::junctionKind) {
                const std::optional<HydraulicNodeJunction> junction =
                    aowis::native::ModelCodec<HydraulicNodeJunction>::decode(bytes);
                if (!junction || hydraulic::keyFromUuid(junction->uuid) != pair.first) {
                    throw std::runtime_error("junction model payload/key mismatch");
                }
            } else if (record.kind == hydraulic::pipeKind) {
                const std::optional<HydraulicLinkPipe> pipe =
                    aowis::native::ModelCodec<HydraulicLinkPipe>::decode(bytes);
                if (!pipe || hydraulic::keyFromUuid(pipe->uuid) != pair.first || record.references.size() != 2) {
                    throw std::runtime_error("pipe model payload/key/reference mismatch");
                }
                std::vector<Key> required{hydraulic::keyFromUuid(pipe->node_uuid_from),
                                          hydraulic::keyFromUuid(pipe->node_uuid_to)};
                std::sort(required.begin(), required.end());
                if (record.references != required) {
                    throw std::runtime_error("pipe dependency list diverged from encoded pipe endpoints");
                }
            } else {
                throw std::runtime_error("unknown record kind");
            }
            ++count;
        }
        if (count != 2 * pipes + 1) throw std::runtime_error("incorrect record count");
        const double scanMs = elapsed(scanning);
        const aowis::native::Revision final = store->head("main");
        const std::uint64_t packBytes = store->packSize();
        const std::uint64_t packedObjects = store->objectCount();
        store.reset();  // Block store intentionally permits one writable handle.
        const std::chrono::steady_clock::time_point reopening = std::chrono::steady_clock::now();
        Repository reopened(directory, engine);
        if (reopened.head("main").root != final.root) throw std::runtime_error("reopen root mismatch");
        const double reopenMs = elapsed(reopening);
        const std::chrono::steady_clock::time_point verifying = std::chrono::steady_clock::now();
        reopened.verifyPack();
        const double verifyMs = elapsed(verifying);

        std::cout << (blocks ? "engine=native-experimental-blocks\n" : "engine=native-v0\n")
                  << "fixture=model-hydraulic-topology\ncodec=native-model-canonical\n"
                  << "pipes=" << pipes << "\nnodes=" << pipes + 1
                  << "\nobjects=" << 2 * pipes + 1 << "\nrevisions=" << revisions
                  << "\nedits_per_revision=" << edits
                  << "\nopen_ms=" << openMs
                  << "\nencode_prepare_ms=" << prepareMs
                  << "\nincoming_refs=" << incomingReferences
                  << "\nreference_lookup_ms=" << referenceLookupMs
                  << "\nindexed_delete_reject_ms=" << deletionCheckMs
                  << "\nimport_ms=" << importMs
                  << "\nrevision_total_ms=" << revisionMs
                  << "\nrevision_mean_ms=" << revisionMs / static_cast<double>(revisions)
                  << "\ndiff_ms=" << diffMs << "\ndiff_objects=" << differenceCount
                  << "\nscan_decode_ms=" << scanMs
                  << "\npack_bytes=" << packBytes
                  << "\npack_objects=" << packedObjects
                  << "\nindex_bytes=" << std::filesystem::file_size(blocks
                         ? directory / "blocks" / "candidate.idx" : directory / "objects.idx")
                  << "\nreopen_ms=" << reopenMs << "\nverify_ms=" << verifyMs << '\n';
#ifdef __linux__
        // Linux reports ru_maxrss in KiB. Capture the whole benchmark process
        // (including canonical codec allocations, scanning and reopening).
        struct rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) == 0) {
            std::cout << "peak_rss_kib=" << usage.ru_maxrss << '\n';
        }
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native hydraulic model benchmark error: " << error.what() << '\n';
        return 1;
    }
}
