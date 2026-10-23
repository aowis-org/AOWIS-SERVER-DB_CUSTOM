#include "aowis/native/native_store.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

#include <unistd.h>

namespace {

using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::MergeResult;
using aowis::native::MergeStatus;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::Repository;
using aowis::native::Revision;

void expect(bool condition, const std::string& diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / "aowis-merge-ancestry-XXXXXX";
        std::string name = directory.string();
        char* created = ::mkdtemp(name.data());
        if (created == nullptr) throw std::runtime_error("cannot create temporary directory");
        this->path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path_, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const { return this->path_; }
private:
    std::filesystem::path path_;
};

Key key(std::uint32_t sequence) {
    Key result{};
    result[12] = static_cast<std::uint8_t>(sequence >> 24U);
    result[13] = static_cast<std::uint8_t>(sequence >> 16U);
    result[14] = static_cast<std::uint8_t>(sequence >> 8U);
    result[15] = static_cast<std::uint8_t>(sequence);
    return result;
}

Bytes value(std::uint32_t sequence) {
    const std::string encoded = "item-" + std::to_string(sequence);
    return Bytes(encoded.begin(), encoded.end());
}

std::set<Hash> fullAncestors(Repository& repository, const Hash& tip) {
    std::set<Hash> result;
    std::vector<Hash> pending{tip};
    while (!pending.empty()) {
        const Hash current = pending.back();
        pending.pop_back();
        if (!result.insert(current).second) continue;
        const Revision record = repository.revision(current);
        for (const Hash& parent : record.parents) pending.push_back(parent);
    }
    return result;
}

struct ExpectedMerge {
    MergeStatus status = MergeStatus::UpToDate;
    std::optional<Hash> base;
    bool ambiguous = false;
};

// Deliberately independent, exhaustive oracle: build both complete ancestry
// graphs and compare every common candidate against every other common
// candidate. The production algorithm instead prunes its source traversal.
ExpectedMerge expectedMerge(Repository& repository, const Hash& target, const Hash& source) {
    const std::set<Hash> targetHistory = fullAncestors(repository, target);
    const std::set<Hash> sourceHistory = fullAncestors(repository, source);
    if (targetHistory.contains(source)) return {MergeStatus::UpToDate, source, false};
    if (sourceHistory.contains(target)) return {MergeStatus::FastForward, target, false};

    std::vector<Hash> common;
    for (const Hash& candidate : targetHistory) {
        if (sourceHistory.contains(candidate)) common.push_back(candidate);
    }
    expect(!common.empty(), "fixture contains unrelated initial histories");
    std::vector<Hash> lowest;
    for (const Hash& candidate : common) {
        bool dominated = false;
        for (const Hash& other : common) {
            if (candidate == other) continue;
            if (fullAncestors(repository, other).contains(candidate)) {
                dominated = true;
                break;
            }
        }
        if (!dominated) lowest.push_back(candidate);
    }
    if (lowest.size() > 1) return {MergeStatus::UpToDate, std::nullopt, true};
    expect(lowest.size() == 1, "oracle could not find a unique merge base");
    return {MergeStatus::Merged, lowest.front(), false};
}

std::uint32_t randomValue(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

void makeCommit(Repository& repository, const std::string& branch,
                std::uint32_t sequence, bool modelIndexed) {
    if (modelIndexed) {
        const ModelRecord record{1, {}, value(sequence)};
        (void)repository.commitModel(branch,
                                    std::vector<ModelChange>{{key(sequence), record}});
    } else {
        (void)repository.commit(branch, std::vector<Change>{{key(sequence), value(sequence)}});
    }
}

void checkMerge(Repository& repository, const std::string& target,
                const std::string& source) {
    const Hash before = repository.head(target).id;
    const Hash incoming = repository.head(source).id;
    const ExpectedMerge expected = expectedMerge(repository, before, incoming);
    if (expected.ambiguous) {
        bool rejected = false;
        try {
            (void)repository.mergeBranches(target, source);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("multiple independent merge bases") !=
                       std::string::npos;
        }
        expect(rejected, "criss-cross ancestry was not rejected");
        expect(repository.head(target).id == before, "ambiguous merge modified target");
        return;
    }
    const MergeResult actual = repository.mergeBranches(target, source);
    expect(actual.status == expected.status, "incorrect merge status");
    expect(expected.base && actual.base == *expected.base, "incorrect merge base");
    expect(actual.targetBefore == before && actual.sourceHead == incoming,
           "merge returned incorrect original branch heads");
    expect(repository.head(source).id == incoming, "merge modified source branch");
    if (expected.status == MergeStatus::Merged) {
        expect(actual.conflicts.empty(), "independent UUID edits unexpectedly conflicted");
        expect(actual.revision.parents == std::vector<Hash>({before, incoming}),
               "merge did not record both parents");
        expect(repository.head(target).id == actual.revision.id,
               "merge did not publish revision");
    } else if (expected.status == MergeStatus::FastForward) {
        expect(repository.head(target).id == incoming, "fast forward lost source identity");
    } else {
        expect(repository.head(target).id == before, "up-to-date merge altered head");
    }
}

void testRandomizedMergeDag(bool modelIndexed) {
    TemporaryDirectory temporary;
    Repository repository(temporary.path() / "repo");
    repository.createBranch("b0");
    makeCommit(repository, "b0", 1, modelIndexed);
    const std::array<std::string, 5> branches{"b0", "b1", "b2", "b3", "b4"};
    for (std::size_t index = 1; index < branches.size(); ++index) {
        repository.createBranch(branches[index], "b0");
    }
    std::uint32_t state = modelIndexed ? 0x13ea347dU : 0xb6f84912U;
    std::uint32_t sequence = 2;
    for (int operation = 0; operation < 64; ++operation) {
        const std::size_t target = randomValue(state) % branches.size();
        if (operation < 8 || randomValue(state) % 4U != 0U) {
            makeCommit(repository, branches[target], sequence++, modelIndexed);
        } else {
            std::size_t source = randomValue(state) % branches.size();
            if (source == target) source = (source + 1U) % branches.size();
            checkMerge(repository, branches[target], branches[source]);
        }
        if (operation % 13 == 0) {
            checkMerge(repository, branches[0], branches[0]);
        }
    }
    for (std::size_t target = 0; target < branches.size(); ++target) {
        for (std::size_t source = 0; source < branches.size(); ++source) {
            if (target != source) {
                checkMerge(repository, branches[target], branches[source]);
            }
        }
    }
    (void)repository.verifyRepository();
}

void testCrissCross() {
    TemporaryDirectory temporary;
    Repository repository(temporary.path() / "repo");
    repository.createBranch("main");
    makeCommit(repository, "main", 1, false);
    repository.createBranch("left", "main");
    repository.createBranch("right", "main");
    makeCommit(repository, "left", 2, false);
    makeCommit(repository, "right", 3, false);
    repository.createBranch("left-base", "left");
    checkMerge(repository, "left", "right");
    checkMerge(repository, "right", "left-base");
    checkMerge(repository, "left", "right"); // exactly two independent merge bases
    (void)repository.verifyRepository();
}

void testDominatedCommonFrontier() {
    TemporaryDirectory temporary;
    Repository repository(temporary.path() / "repo");
    repository.createBranch("main");
    makeCommit(repository, "main", 1, false);
    repository.createBranch("old", "main");
    makeCommit(repository, "main", 2, false);
    const Hash expectedBase = repository.head("main").id;
    repository.createBranch("feature", "main");
    makeCommit(repository, "main", 3, false);
    makeCommit(repository, "old", 4, false);
    makeCommit(repository, "feature", 5, false);
    // 'feature' now has one path through the newer common revision and a
    // second path through an older one. Only the newer common node is a
    // lowest common ancestor, even if traversal visits the older one first.
    checkMerge(repository, "feature", "old");
    const MergeResult result = repository.mergeBranches("main", "feature");
    expect(result.status == MergeStatus::Merged && result.base == expectedBase,
           "dominated common-frontier candidate chosen as a merge base");
    checkMerge(repository, "feature", "old");
    (void)repository.verifyRepository();
}

void testLongSharedHistory() {
    TemporaryDirectory temporary;
    Repository repository(temporary.path() / "repo");
    repository.createBranch("main");
    for (std::uint32_t sequence = 1; sequence <= 260; ++sequence) {
        makeCommit(repository, "main", sequence, false);
    }
    const Hash fork = repository.head("main").id;
    repository.createBranch("feature", "main");
    for (std::uint32_t sequence = 261; sequence <= 295; ++sequence) {
        makeCommit(repository, "main", sequence, false);
    }
    for (std::uint32_t sequence = 296; sequence <= 330; ++sequence) {
        makeCommit(repository, "feature", sequence, false);
    }
    const MergeResult merged = repository.mergeBranches("main", "feature");
    expect(merged.status == MergeStatus::Merged && merged.base == fork,
           "deeply shared history yielded incorrect base");
    checkMerge(repository, "main", "feature");
    makeCommit(repository, "feature", 331, false);
    checkMerge(repository, "main", "feature");
    (void)repository.verifyRepository();
}

} // namespace

int main() {
    try {
        {
            testRandomizedMergeDag(false);
            testRandomizedMergeDag(true);
            testCrissCross();
            testDominatedCommonFrontier();
            testLongSharedHistory();
        }
        std::cout << "Merge ancestry: exhaustive oracle, deterministic DAGs, criss-cross, "
                     "and long shared history passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Merge ancestry test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
