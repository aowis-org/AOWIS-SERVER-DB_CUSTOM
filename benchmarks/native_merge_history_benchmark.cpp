#include "aowis/native/native_store.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/resource.h>

namespace {

using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::MergeStatus;
using aowis::native::PhysicalStore;
using aowis::native::Repository;

Key key(std::uint32_t sequence) {
    Key result{};
    for (unsigned offset = 0; offset < 4; ++offset) {
        result[15 - offset] = static_cast<std::uint8_t>(sequence >> (8U * offset));
    }
    return result;
}

std::uint32_t parseCount(const char* argument, const char* label) {
    const std::string input(argument);
    std::size_t parsed = 0;
    const unsigned long value = std::stoul(input, &parsed);
    if (parsed != input.size() || value == 0 ||
        value > std::numeric_limits<std::uint32_t>::max() / 2U) {
        throw std::runtime_error(std::string("invalid ") + label);
    }
    return static_cast<std::uint32_t>(value);
}

void commit(Repository& repository, const std::string& branch,
            std::uint32_t sequence, std::string_view payload) {
    const Bytes data(payload.begin(), payload.end());
    (void)repository.commit(branch, {{key(sequence), data}});
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cerr << "Usage: " << argv[0]
                      << " NEW_REPOSITORY_DIR SHARED_COMMITS DIVERGENT_COMMITS pack\n";
            return EXIT_FAILURE;
        }
        const std::filesystem::path directory(argv[1]);
        if (std::filesystem::exists(directory)) {
            throw std::runtime_error("benchmark repository must not already exist");
        }
        const std::uint32_t shared = parseCount(argv[2], "shared commit count");
        const std::uint32_t divergent = parseCount(argv[3], "divergent commit count");
        if (static_cast<std::uint64_t>(shared) +
            2ULL * static_cast<std::uint64_t>(divergent) + 1ULL >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("requested history exceeds supported benchmark UUID range");
        }
        const std::string backendName(argv[4]);
        if (backendName != "pack") {
            throw std::runtime_error("physical backend must be pack");
        }
        const PhysicalStore backend = PhysicalStore::Pack;
        Repository repository(directory, backend);
        repository.createBranch("main");
        for (std::uint32_t index = 1; index <= shared; ++index) {
            commit(repository, "main", index, "shared");
        }
        const Hash expectedBase = repository.head("main").id;
        repository.createBranch("feature", "main");
        const std::uint32_t firstDivergent = shared + 1U;
        // Update a distinct UUID per commit on each side, apart from the
        // deliberately conflicting key below. The merge cannot publish any
        // revision and thus measures lookup and diff/conflict work rather
        // than commit fsync latency.
        for (std::uint32_t index = 0; index < divergent; ++index) {
            commit(repository, "main", firstDivergent + index, "main");
            commit(repository, "feature", firstDivergent + divergent + index, "feature");
        }
        const std::uint32_t conflictKey = firstDivergent + 2U * divergent;
        commit(repository, "main", conflictKey, "main-version");
        commit(repository, "feature", conflictKey, "feature-version");

        const Hash targetBefore = repository.head("main").id;
        const Hash sourceBefore = repository.head("feature").id;
        const std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
        const aowis::native::MergeResult result = repository.mergeBranches("main", "feature");
        const std::chrono::steady_clock::time_point finish =
            std::chrono::steady_clock::now();
        if (result.status != MergeStatus::Conflicts || result.base != expectedBase ||
            result.conflicts.size() != 1 ||
            repository.head("main").id != targetBefore ||
            repository.head("feature").id != sourceBefore) {
            throw std::runtime_error("merge benchmark violated correctness invariants");
        }
        struct rusage resources{};
        if (::getrusage(RUSAGE_SELF, &resources) != 0) {
            throw std::runtime_error("getrusage failed");
        }
        const double mergeMs = std::chrono::duration<double, std::milli>(finish - start).count();
        std::cout << "backend=" << backendName << '\n'
                  << "shared_commits=" << shared << '\n'
                  << "divergent_commits_per_branch=" << divergent + 1U << '\n'
                  << "merge_conflict_ms=" << mergeMs << '\n'
                  << "process_peak_rss_kib=" << resources.ru_maxrss << '\n'
                  << "storage_objects=" << repository.objectCount() << '\n'
                  << "storage_pack_bytes=" << repository.packSize() << '\n'
                  << "merge_conflicts=" << result.conflicts.size() << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Merge history benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
