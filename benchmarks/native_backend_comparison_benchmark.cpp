#include <aowis/native/native_store.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace native = aowis::native;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double elapsed(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

std::uint32_t number(std::string_view input) {
    std::size_t end = 0;
    const unsigned long parsed = std::stoul(std::string(input), &end);
    require(end == input.size() && parsed > 0 && parsed < 1000001, "invalid positive benchmark integer");
    return static_cast<std::uint32_t>(parsed);
}

native::PhysicalStore backend(std::string_view name) {
    if (name == "pack") return native::PhysicalStore::Stage7Pack;
    if (name == "block") return native::PhysicalStore::ExperimentalBlocks;
    throw std::runtime_error("backend must be pack or block");
}

native::Key key(std::uint32_t category, std::uint32_t item) {
    native::Key result{};
    // Deliberately scatter keys through the trie; category in the last bytes.
    std::uint64_t mixed = static_cast<std::uint64_t>(item) + 0x9e3779b97f4a7c15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31U;
    for (unsigned i = 0; i < 8; ++i) result[i] = static_cast<std::uint8_t>(mixed >> (i * 8U));
    result[12] = static_cast<std::uint8_t>(category);
    for (unsigned i = 0; i < 3; ++i) result[13 + i] = static_cast<std::uint8_t>(item >> (i * 8U));
    return result;
}

native::Bytes payload(std::uint32_t category, std::uint32_t item, std::uint32_t revision) {
    native::Bytes result(128);
    std::uint64_t value = 0x243f6a8885a308d3ULL ^ (static_cast<std::uint64_t>(item) << 13U) ^
                          (static_cast<std::uint64_t>(category) << 41U) ^ revision;
    for (std::size_t i = 0; i < result.size(); ++i) {
        value ^= value >> 12U;
        value ^= value << 25U;
        value ^= value >> 27U;
        result[i] = static_cast<std::uint8_t>((value * 0x2545f4914f6cdd1dULL) >> 56U);
    }
    return result;
}

native::ModelRecord junction(std::uint32_t item) {
    return native::ModelRecord{1, {}, payload(1, item, 0)};
}

native::ModelRecord pipe(std::uint32_t item, std::uint32_t revision) {
    std::vector<native::Key> references{key(1, item), key(1, item + 1U)};
    std::sort(references.begin(), references.end());
    return native::ModelRecord{4, std::move(references), payload(4, item, revision)};
}

std::uint64_t directoryBytes(const std::filesystem::path& root) {
    std::uint64_t sum = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) sum += entry.file_size();
    }
    return sum;
}

std::uint64_t peakRssKiB() {
#ifdef __linux__
    std::ifstream file("/proc/self/status");
    std::string line;
    while (std::getline(file, line)) {
        if (line.starts_with("VmHWM:")) {
            std::size_t end = 0;
            const std::string value = line.substr(6);
            const std::uint64_t peak = std::stoull(value, &end);
            require(peak > 0, "invalid VmHWM");
            return peak;
        }
    }
#endif
    return 0;
}

void createModel(const std::filesystem::path& path, std::string_view backendName,
                 std::uint32_t pipeCount, std::uint32_t edits) {
    require(!std::filesystem::exists(path), "repository already exists");
    require(edits > 0 && edits * 2U <= pipeCount, "edits must be <= pipes/2");
    native::Repository repository(path, backend(backendName));
    repository.createBranch("main");
    std::vector<native::ModelChange> changes;
    changes.reserve(2ULL * pipeCount + 1ULL);
    for (std::uint32_t i = 0; i <= pipeCount; ++i) {
        changes.push_back(native::ModelChange{key(1, i), junction(i)});
    }
    for (std::uint32_t i = 0; i < pipeCount; ++i) {
        changes.push_back(native::ModelChange{key(4, i), pipe(i, 0)});
    }
    const Clock::time_point initialStart = Clock::now();
    const native::Revision base = repository.commitModel("main", std::move(changes));
    const double initialMs = elapsed(initialStart);
    repository.createBranch("feature", "main");
    repository.createBranch("baseline", "main");
    const Clock::time_point editsStart = Clock::now();
    for (std::uint32_t i = 0; i < edits; ++i) {
        repository.commitModel("main", {{key(4, i * 2U), pipe(i * 2U, i + 1U)}});
        repository.commitModel("feature", {{key(4, i * 2U + 1U), pipe(i * 2U + 1U, i + 1U)}});
    }
    const double editMs = elapsed(editsStart);
    const Clock::time_point mergeStart = Clock::now();
    const native::MergeResult merged = repository.mergeBranches("main", "feature");
    const double mergeMs = elapsed(mergeStart);
    require(merged.status == native::MergeStatus::Merged && merged.conflicts.empty(),
            "disjoint model merge did not succeed");
    require(merged.revision.parents.size() == 2 && merged.base == base.id,
            "merge parents/base incorrect");
    const Clock::time_point readStart = Clock::now();
    for (std::uint32_t i = 0; i < edits * 2U; ++i) {
        const std::optional<native::ModelRecord> stored = repository.getModel("main", key(4, i));
        require(stored.has_value() && stored->kind == 4 && stored->references.size() == 2,
                "merged pipe missing or references corrupted");
        const std::uint32_t expectedRevision = i / 2U + 1U;
        require(stored->payload == pipe(i, expectedRevision).payload,
                "merged payload does not contain both branches' edits");
    }
    const double indexedReadMs = elapsed(readStart);
    const Clock::time_point diffStart = Clock::now();
    const std::vector<native::Difference> differences = repository.diff("baseline", "main");
    const double diffMs = elapsed(diffStart);
    require(differences.size() == edits * 2U, "unexpected merged difference count");
    std::cout << "scenario=model\nbackend=" << backendName
              << "\npipes=" << pipeCount << "\nedits=" << edits
              << "\ninitial_ms=" << initialMs
              << "\nedit_total_ms=" << editMs
              << "\nedit_mean_ms=" << editMs / (2.0 * edits)
              << "\nmerge_ms=" << mergeMs
              << "\nindexed_read_ms=" << indexedReadMs
              << "\ndiff_ms=" << diffMs
              << "\ndiff_count=" << differences.size()
              << "\nhead=" << native::toHex(merged.revision.id)
              << "\nobjects=" << repository.objectCount()
              << "\npack_bytes=" << repository.packSize()
              << "\ndisk_bytes=" << directoryBytes(path)
              << "\npeak_rss_kib=" << peakRssKiB() << '\n';
}

void inspect(const std::filesystem::path& path, std::string_view backendName) {
    require(std::filesystem::is_directory(path), "repository does not exist");
    native::Hash originalHead{};
    std::uint64_t beforeBytes = 0;
    std::uint64_t beforeObjects = 0;
    double scanMs = 0;
    double auditMs = 0;
    double compactMs = 0;
    std::uint64_t scanCount = 0;
    std::uint64_t verifiedRevisions = 0;
    std::uint64_t verifiedReachable = 0;
    const std::uint64_t diskBefore = directoryBytes(path);
    {
        native::Repository repository(path, backend(backendName));
        originalHead = repository.head("main").id;
        beforeBytes = repository.packSize();
        beforeObjects = repository.objectCount();
        const Clock::time_point scanStart = Clock::now();
        scanCount = repository.scan("main").size();
        scanMs = elapsed(scanStart);
        require(scanCount > 0, "empty model repository");
        const Clock::time_point auditStart = Clock::now();
        const native::VerificationReport audit = repository.verifyRepository();
        auditMs = elapsed(auditStart);
        verifiedRevisions = audit.revisions;
        verifiedReachable = audit.reachableObjects;
        const Clock::time_point compactStart = Clock::now();
        (void)repository.compact();
        compactMs = elapsed(compactStart);
        require(repository.head("main").id == originalHead, "compaction changed main head");
    }
    const std::uint64_t diskAfter = directoryBytes(path);
    const Clock::time_point reopenStart = Clock::now();
    native::Repository reopened(path, backend(backendName));
    const double reopenMs = elapsed(reopenStart);
    require(reopened.head("main").id == originalHead, "reopen changed main head");
    require(reopened.scan("main").size() == scanCount, "compaction changed model records");
    const Clock::time_point postVerifyStart = Clock::now();
    const native::VerificationReport postAudit = reopened.verifyRepository();
    const double postCompactVerifyMs = elapsed(postVerifyStart);
    require(postAudit.revisions == verifiedRevisions &&
            postAudit.reachableObjects == verifiedReachable,
            "compaction changed verified history or reachable objects");
    std::cout << "scenario=inspect\nbackend=" << backendName
              << "\nhead=" << native::toHex(originalHead)
              << "\nscan_count=" << scanCount
              << "\nscan_ms=" << scanMs
              << "\naudit_ms=" << auditMs
              << "\ncompact_ms=" << compactMs
              << "\nreopen_ms=" << reopenMs
              << "\npost_compact_verify_ms=" << postCompactVerifyMs
              << "\nverified_revisions=" << verifiedRevisions
              << "\nverified_reachable=" << verifiedReachable
              << "\nobjects_before=" << beforeObjects
              << "\nobjects_after=" << reopened.objectCount()
              << "\npack_bytes_before=" << beforeBytes
              << "\npack_bytes_after=" << reopened.packSize()
              << "\ndisk_bytes_before=" << diskBefore
              << "\ndisk_bytes_after=" << diskAfter
              << "\npeak_rss_kib=" << peakRssKiB() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        std::cerr << "Usage: aowis-native-backend-comparison-benchmark model NEW_REPO pack|block PIPES EDITS\n"
                     "   or: aowis-native-backend-comparison-benchmark inspect EXISTING_REPO pack|block\n";
        return 2;
    }
    try {
        std::cout << std::fixed << std::setprecision(3);
        const std::string mode(argv[1]);
        if (mode == "model" && argc == 6) {
            createModel(argv[2], argv[3], number(argv[4]), number(argv[5]));
        } else if (mode == "inspect" && argc == 4) {
            inspect(argv[2], argv[3]);
        } else {
            throw std::runtime_error("invalid benchmark arguments");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Backend comparison failed: " << error.what() << '\n';
        return 1;
    }
}
