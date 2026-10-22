#include "aowis/native/native_store.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>


namespace {

using aowis::native::BundleImportResult;
using aowis::native::BundleImportStatus;
using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::PhysicalStore;
using aowis::native::Repository;
using aowis::native::VerificationReport;

void require(bool valid, std::string_view reason) {
    if (!valid) throw std::runtime_error(std::string(reason));
}

std::uint32_t positiveNumber(const char* text, const char* label) {
    const std::string value(text);
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed, 10);
    require(consumed == value.size() && parsed > 0 &&
            parsed <= std::numeric_limits<std::uint32_t>::max(), label);
    return static_cast<std::uint32_t>(parsed);
}

PhysicalStore backend(std::string_view value) {
    if (value == "pack") return PhysicalStore::Stage7Pack;
    if (value == "block") return PhysicalStore::ExperimentalBlocks;
    throw std::runtime_error("backend must be pack or block");
}

Key key(std::uint32_t sequence) {
    // Scatter UUIDs across the trie rather than manufacturing a single deep
    // prefix. The last four bytes preserve uniqueness deterministically.
    std::uint64_t mixed = static_cast<std::uint64_t>(sequence) + 0x9e3779b97f4a7c15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31U;
    Key result{};
    for (unsigned position = 0; position < 8; ++position) {
        result[position] = static_cast<std::uint8_t>(mixed >> (8U * position));
    }
    for (unsigned position = 0; position < 4; ++position) {
        result[12U + position] = static_cast<std::uint8_t>(sequence >> (8U * position));
    }
    return result;
}

Bytes payload(std::uint32_t sequence, std::uint32_t size) {
    Bytes result(size);
    // Encode the identity to avoid accidental 256-way value deduplication.
    for (unsigned position = 0; position < 4; ++position) {
        result[position] = static_cast<std::uint8_t>(sequence >> (8U * position));
    }
    std::uint64_t state = sequence + 0xa0761d6478bd642fULL;
    for (std::size_t position = 4; position < result.size(); ++position) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        result[position] = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dULL) >> 56U);
    }
    return result;
}

void writeHeads(const std::filesystem::path& directory, const Hash& base,
                const Hash& full) {
    std::ofstream out(directory / "heads.txt", std::ios::trunc);
    require(out.good(), "cannot write fixture heads");
    out << aowis::native::toHex(base) << '\n' << aowis::native::toHex(full) << '\n';
    out.flush();
    require(out.good(), "failed to write fixture heads");
}

std::pair<Hash, Hash> readHeads(const std::filesystem::path& directory) {
    std::ifstream in(directory / "heads.txt");
    std::string base;
    std::string full;
    require(static_cast<bool>(std::getline(in, base)) &&
            static_cast<bool>(std::getline(in, full)), "fixture heads are missing");
    return {aowis::native::fromHex(base), aowis::native::fromHex(full)};
}

// Linux getrusage(RUSAGE_SELF).ru_maxrss can retain the spawning process's
// pre-exec RSS peak. Python's subprocess launcher is especially misleading:
// every import may appear to use exactly as much RAM as the Python driver.
// VmHWM belongs to this exec image, so it measures the import child itself.
std::uint64_t peakResidentKiB() {
    std::ifstream status("/proc/self/status");
    require(status.good(), "cannot read Linux process memory status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmHWM:")) continue;
        std::istringstream values(line.substr(6));
        std::uint64_t kib = 0;
        std::string units;
        std::string extra;
        require(static_cast<bool>(values >> kib >> units) && units == "kB" &&
                !(values >> extra) && kib > 0,
                "invalid Linux VmHWM memory measurement");
        return kib;
    }
    throw std::runtime_error("Linux VmHWM memory measurement is unavailable");
}

double milliseconds(std::chrono::steady_clock::time_point start,
                    std::chrono::steady_clock::time_point finish) {
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

void prepare(const std::filesystem::path& directory, std::uint32_t objects,
             std::uint32_t commits, std::uint32_t payloadBytes,
             std::string_view backendName, std::string_view kind) {
    require(!std::filesystem::exists(directory), "fixture directory already exists");
    require(commits >= 2U && commits <= objects, "commits must be between 2 and objects");
    require(kind == "model" || kind == "raw", "kind must be model or raw");
    require(payloadBytes >= 4U && payloadBytes <= 1024U * 1024U,
            "payload must be between 4 bytes and 1 MiB");
    require(objects < std::numeric_limits<std::uint32_t>::max(),
            "object count exceeds benchmark UUID capacity");
    std::filesystem::create_directories(directory);
    Repository source(directory / "source", backend(backendName));
    source.createBranch("main");
    Hash base{};
    const std::uint32_t baseCommits = commits / 2U;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (std::uint32_t revision = 0; revision < commits; ++revision) {
        const std::uint32_t first = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(objects) * revision / commits) + 1U;
        const std::uint32_t end = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(objects) * (revision + 1U) / commits) + 1U;
        if (kind == "model") {
            std::vector<ModelChange> changes;
            changes.reserve(end - first);
            for (std::uint32_t sequence = first; sequence < end; ++sequence) {
                ModelRecord record;
                record.kind = 1;
                record.payload = payload(sequence, payloadBytes);
                if (sequence > 1U) record.references.push_back(key(sequence - 1U));
                changes.push_back(ModelChange{key(sequence), std::move(record)});
            }
            (void)source.commitModel("main", std::move(changes));
        } else {
            std::vector<Change> changes;
            changes.reserve(end - first);
            for (std::uint32_t sequence = first; sequence < end; ++sequence) {
                changes.push_back(Change{key(sequence), payload(sequence, payloadBytes)});
            }
            (void)source.commit("main", changes);
        }
        if (revision + 1U == baseCommits) {
            base = source.head("main").id;
            source.exportBranchBundle("main", directory / "base.aowbundle");
        }
    }
    const Hash full = source.head("main").id;
    source.exportBranchBundle("main", directory / "full.aowbundle");
    source.exportIncrementalBranchBundle("main", base, directory / "delta.aowbundle");
    writeHeads(directory, base, full);
    const VerificationReport verified = source.verifyRepository();
    require(verified.revisions >= commits, "source verification omitted history");
    const std::chrono::steady_clock::time_point finish = std::chrono::steady_clock::now();
    std::cout << std::fixed << std::setprecision(3)
              << "phase=prepare\nbackend=" << backendName << "\nkind=" << kind
              << "\nobjects=" << objects << "\ncommits=" << commits
              << "\nfixture_ms=" << milliseconds(start, finish)
              << "\nbundle_full_bytes=" << std::filesystem::file_size(directory / "full.aowbundle")
              << "\nbundle_delta_bytes=" << std::filesystem::file_size(directory / "delta.aowbundle")
              << "\nsource_head=" << aowis::native::toHex(full) << '\n';
}

void import(const std::filesystem::path& fixture, const std::filesystem::path& destination,
            std::string_view phase, std::string_view backendName) {
    require(std::filesystem::is_directory(fixture), "fixture directory is missing");
    const bool delta = phase == "import-delta";
    require(delta || phase == "import-full" || phase == "import-base", "invalid import phase");
    require(delta ? std::filesystem::is_directory(destination) :
            !std::filesystem::exists(destination),
            "destination must exist for delta and must not exist for full/base");
    const std::pair<Hash, Hash> heads = readHeads(fixture);
    const std::filesystem::path bundle = fixture /
        (delta ? "delta.aowbundle" : (phase == "import-base" ? "base.aowbundle" : "full.aowbundle"));
    const Hash expected = phase == "import-base" ? heads.first : heads.second;
    Repository receiver(destination, backend(backendName));
    if (delta) require(receiver.head("main").id == heads.first, "destination is not at base revision");
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const BundleImportResult result = receiver.importBranchBundle(bundle, "main");
    const std::chrono::steady_clock::time_point imported = std::chrono::steady_clock::now();
    require(receiver.head("main").id == expected, "imported branch head differs from source");
    const std::uint64_t afterImportKiB = peakResidentKiB();
    const VerificationReport report = receiver.verifyRepository();
    const std::chrono::steady_clock::time_point checked = std::chrono::steady_clock::now();
    const std::uint64_t afterAuditKiB = peakResidentKiB();
    require(afterAuditKiB >= afterImportKiB, "Linux RSS high-water mark decreased");
    require(result.status == (delta ? BundleImportStatus::FastForward : BundleImportStatus::Created),
            "import did not create/fast-forward as expected");
    std::cout << std::fixed << std::setprecision(3)
              << "phase=" << phase << "\nbackend=" << backendName
              << "\nimport_ms=" << milliseconds(start, imported)
              << "\nverify_ms=" << milliseconds(imported, checked)
              << "\nimport_peak_rss_kib=" << afterImportKiB
              << "\nprocess_peak_rss_kib=" << afterAuditKiB
              << "\nimported_objects=" << result.objectsImported
              << "\nverified_revisions=" << report.revisions
              << "\nverified_reachable_objects=" << report.reachableObjects
              << "\nbundle_bytes=" << std::filesystem::file_size(bundle)
              << "\nstorage_pack_bytes=" << receiver.packSize()
              << "\nhead=" << aowis::native::toHex(result.head) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 8 && std::string_view(argv[1]) == "prepare") {
            prepare(argv[2], positiveNumber(argv[3], "objects"),
                    positiveNumber(argv[4], "commits"), positiveNumber(argv[5], "payload bytes"),
                    argv[6], argv[7]);
        } else if (argc == 5 && std::string_view(argv[1]).starts_with("import-")) {
            import(argv[2], argv[3], argv[1], argv[4]);
        } else {
            std::cerr << "Usage:\n  " << argv[0]
                      << " prepare NEW_FIXTURE OBJECTS COMMITS PAYLOAD_BYTES pack|block raw|model\n  "
                      << argv[0] << " import-full|import-base|import-delta FIXTURE DESTINATION pack|block\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Replication benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
