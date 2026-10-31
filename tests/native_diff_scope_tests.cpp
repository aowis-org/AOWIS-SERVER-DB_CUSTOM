// Review findings F9 (bounded diffs) and F10a (branch-scoped revisions).
#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Difference;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::Repository;
using aowis::dvmdb::test::CommittedRevision;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-dvmdb-diff-scope-" + std::string(label) + "-" + std::to_string(::getpid()) +
             "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++counter));
        std::filesystem::create_directories(this->path);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

void expectKind(StorageErrorKind expected, const std::string& label,
                const std::function<void()>& action) {
    try {
        action();
    } catch (const StorageError& error) {
        expect(error.kind() == expected, label + ": expected " + StorageError::kindName(expected) +
               ", got " + StorageError::kindName(error.kind()) + " (" + error.what() + ")");
        return;
    }
    throw std::runtime_error(label + ": no exception");
}

Hash valueHash(const Bytes& payload) {
    Bytes encoded{'V'};
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return aowis::dvmdb::sha256(encoded);
}

using Model = std::map<Key, Bytes>;

// Independent reference: compare two plain maps key by key.
std::vector<Difference> naiveDiff(const Model& before, const Model& after) {
    std::vector<Difference> result;
    Model::const_iterator left = before.begin();
    Model::const_iterator right = after.begin();
    while (left != before.end() || right != after.end()) {
        if (right == after.end() || (left != before.end() && left->first < right->first)) {
            result.push_back(Difference{left->first, valueHash(left->second), std::nullopt});
            ++left;
        } else if (left == before.end() || right->first < left->first) {
            result.push_back(Difference{right->first, std::nullopt, valueHash(right->second)});
            ++right;
        } else {
            if (left->second != right->second) {
                result.push_back(Difference{left->first, valueHash(left->second),
                                            valueHash(right->second)});
            }
            ++left;
            ++right;
        }
    }
    return result;
}

void expectSameDiff(const std::vector<Difference>& actual, const std::vector<Difference>& expected,
                    const std::string& label) {
    expect(actual.size() == expected.size(), label + ": difference count " +
           std::to_string(actual.size()) + " != " + std::to_string(expected.size()));
    for (std::size_t i = 0; i < actual.size(); ++i) {
        expect(actual[i].key == expected[i].key && actual[i].before == expected[i].before &&
               actual[i].after == expected[i].after, label + ": difference " + std::to_string(i));
    }
}

// Keys drawn from a few shared prefixes force deep compressed branches and
// prefix splits above existing subtrees (the case the old diff listed fully).
Key randomKey(std::mt19937_64& random) {
    Key key{};
    const std::uint64_t shape = random() % 4U;
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(random());
    if (shape == 0) { key[0] = 0x11; key[1] = 0x22; }
    else if (shape == 1) { key[0] = 0x11; key[1] = static_cast<std::uint8_t>(0x20U | (random() & 3U)); }
    else if (shape == 2) { key[0] = static_cast<std::uint8_t>(random() & 0x0fU); }
    if (key == Key{}) key[15] = 1;
    return key;
}

void testRandomizedDiffMatchesReference() {
    TestDirectory directory("random");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    std::mt19937_64 random(20261023);
    // A branch starts unborn; the first commit is the first diffable state.
    Key seed{};
    seed[15] = 0x5e;
    Model model{{seed, Bytes{0}}};
    std::vector<std::pair<RevisionId, Model>> history{
        {repository.commit("main", {Change{seed, Bytes{0}}}), model}};
    for (unsigned round = 0; round < 120; ++round) {
        std::vector<Change> changes;
        const unsigned count = 1U + static_cast<unsigned>(random() % (round % 10U == 0U ? 200U : 12U));
        for (unsigned i = 0; i < count; ++i) {
            const std::uint64_t action = random() % 10U;
            if (action < 2U && !model.empty()) {
                Model::iterator victim = model.begin();
                std::advance(victim, static_cast<std::ptrdiff_t>(random() % model.size()));
                changes.push_back(Change{victim->first, std::nullopt});
                model.erase(victim);
            } else if (action < 4U && !model.empty()) {
                Model::iterator target = model.begin();
                std::advance(target, static_cast<std::ptrdiff_t>(random() % model.size()));
                target->second = Bytes{static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(round)};
                changes.push_back(Change{target->first, target->second});
            } else {
                const Key key = randomKey(random);
                const Bytes payload{static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(i)};
                model[key] = payload;
                changes.push_back(Change{key, payload});
            }
        }
        // Keep the reference consistent with last-write-wins on duplicate keys.
        Model expectedModel = history.back().second;
        for (const Change& change : changes) {
            if (change.value) expectedModel[change.key] = *change.value;
            else expectedModel.erase(change.key);
        }
        model = expectedModel;
        const CommittedRevision revision = repository.revision(repository.commit("main", changes));
        history.emplace_back(revision.id, model);
        // Adjacent, distant and reversed pairs.
        const std::size_t other = static_cast<std::size_t>(random() % history.size());
        const std::pair<RevisionId, Model>& previous = history[history.size() - 2];
        expectSameDiff(repository.diff(previous.first, revision.id),
                       naiveDiff(previous.second, model), "adjacent round " + std::to_string(round));
        expectSameDiff(repository.diff(history[other].first, revision.id),
                       naiveDiff(history[other].second, model), "distant round " + std::to_string(round));
        expectSameDiff(repository.diff(revision.id, history[other].first),
                       naiveDiff(model, history[other].second), "reverse round " + std::to_string(round));
        // The bounded overload returns the identical result when it fits.
        const std::vector<Difference> exact = naiveDiff(previous.second, model);
        expectSameDiff(repository.diff(previous.first, revision.id, exact.size()), exact,
                       "bounded exact round " + std::to_string(round));
        if (!exact.empty()) {
            expectKind(StorageErrorKind::LimitExceeded, "bounded too small round " + std::to_string(round),
                       [&] { (void)repository.diff(previous.first, revision.id, exact.size() - 1); });
        }
    }
    expect(model.size() > 300, "randomized fixture stayed too small: " + std::to_string(model.size()));
}

// One key inserted above a large compressed subtree: the result has one
// difference, so even limit 1 must succeed (the subtree is not listed).
// Flips one payload byte of the pack record whose SHA-256 is `hash`.
void corruptPackObject(const std::filesystem::path& pack, const Hash& hash) {
    std::ifstream input(pack, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Bytes header{'A', 'O', 'B', 'J', 1};
    header.insert(header.end(), hash.begin(), hash.end());
    const Bytes::const_iterator found = std::search(bytes.begin(), bytes.end(), header.begin(), header.end());
    expect(found != bytes.end(), "object to corrupt not found in pack");
    const std::streamoff offset = static_cast<std::streamoff>(found - bytes.begin()) + 45 + 1;
    std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
    stream.seekg(offset);
    char byte = 0;
    stream.read(&byte, 1);
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
    stream.seekp(offset);
    stream.write(&byte, 1);
    expect(stream.good(), "cannot corrupt pack object");
}

void testPrefixSplitAboveLargeSubtree() {
    TestDirectory directory("split");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    std::vector<Change> changes;
    for (unsigned i = 0; i < 20000; ++i) {
        Key key{};
        key[0] = 0xab; key[1] = 0xcd; // shared prefix: root branch sits deep
        key[14] = static_cast<std::uint8_t>(i >> 8U);
        key[15] = static_cast<std::uint8_t>(i);
        key[13] = static_cast<std::uint8_t>(i % 7U);
        changes.push_back(Change{key, Bytes{static_cast<std::uint8_t>(i)}});
    }
    const CommittedRevision large = repository.revision(repository.commit("main", changes));
    // Damage one leaf deep inside the subtree that the next commit leaves
    // unchanged. A structural diff compares subtree hashes and never reads it;
    // a diff that lists unaligned subtrees would hit the checksum error.
    const Key& probeKey = changes[5000].key;
    {
        Bytes leaf{'L'};
        leaf.insert(leaf.end(), probeKey.begin(), probeKey.end());
        const Hash value = valueHash(*changes[5000].value);
        leaf.insert(leaf.end(), value.begin(), value.end());
        corruptPackObject(directory.path / "repo" / "objects.pack", aowis::dvmdb::sha256(leaf));
    }
    expectKind(StorageErrorKind::Corruption, "probe leaf is really damaged",
               [&] { (void)repository.get("main", probeKey); });
    Key outsider{};
    outsider[0] = 0x01; // diverges at the first nibble: new root above the subtree
    const CommittedRevision split = repository.revision(
        repository.commit("main", {Change{outsider, Bytes{9}}}));
    const std::vector<Difference> one = repository.diff(large.id, split.id, 1);
    expect(one.size() == 1 && one.front().key == outsider && !one.front().before,
           "prefix split diff incorrect");
    const std::vector<Difference> back = repository.diff(split.id, large.id, 1);
    expect(back.size() == 1 && back.front().key == outsider && !back.front().after,
           "prefix merge (removal) diff incorrect");
    // Deleting the outsider collapses the root again: same single difference.
    const CommittedRevision collapsed = repository.revision(
        repository.commit("main", {Change{outsider, std::nullopt}}));
    expect(collapsed.root == large.root, "canonical trie did not collapse to the original root");
    expect(repository.diff(large.id, collapsed.id, 0).empty(), "identical roots differ");
}

void testLimitBoundaries() {
    TestDirectory directory("limits");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    // The baseline holds one sentinel key that both sides of every diff share.
    Key sentinel{};
    sentinel[0] = 0xfe;
    const RevisionId empty = repository.commit("main", {Change{sentinel, Bytes{0}}});
    std::vector<Change> changes;
    for (unsigned i = 0; i < 1001; ++i) {
        Key key{};
        key[0] = static_cast<std::uint8_t>(i * 37U);
        key[15] = static_cast<std::uint8_t>(i);
        key[14] = static_cast<std::uint8_t>(i >> 8U);
        changes.push_back(Change{key, Bytes{1}});
    }
    const RevisionId full = repository.commit("main", changes);
    expect(repository.diff(empty, full, 1001).size() == 1001, "limit equal to size rejected");
    expectKind(StorageErrorKind::LimitExceeded, "1001 additions over limit 1000",
               [&] { (void)repository.diff(empty, full, 1000); });
    expectKind(StorageErrorKind::LimitExceeded, "1001 removals over limit 1000",
               [&] { (void)repository.diff(full, empty, 1000); });
    expect(repository.diff(full, full, 0).empty(), "self diff not empty");
    expectKind(StorageErrorKind::NotFound, "bounded diff with unknown revision", [&] {
        (void)repository.diff(RevisionId::generate(), full, 10);
    });
}

// Branch scoping as the HTTP read routes use it: a revision belongs to a
// branch when it is the HEAD or an ancestor through either merge parent.
bool onBranch(ProjectRepository& repository, std::string_view branch, const RevisionId& revision) {
    return repository.reachable(repository.head(branch), revision);
}

void testRevisionOnBranch() {
    TestDirectory directory("scope");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    Key a{}; a[15] = 1;
    Key b{}; b[15] = 2;
    Key c{}; c[15] = 3;
    const RevisionId first = repository.commit("main", {Change{a, Bytes{1}}});
    repository.createBranch("side", "main");
    const RevisionId sideOnly = repository.commit("side", {Change{b, Bytes{2}}});
    const RevisionId mainOnly = repository.commit("main", {Change{c, Bytes{3}}});
    expect(onBranch(repository, "main", mainOnly), "head not on its branch");
    expect(onBranch(repository, "main", first), "ancestor not on branch");
    expect(onBranch(repository, "side", first), "shared ancestor not on forked branch");
    expect(!onBranch(repository, "main", sideOnly), "other branch revision accepted");
    expect(!onBranch(repository, "side", mainOnly), "other branch revision accepted (side)");
    // A merge makes the source history (second parent) part of the target.
    expect(repository.merge("main", "side").status == MergeStatus::Merged, "merge failed");
    expect(onBranch(repository, "main", sideOnly), "merged second-parent history not on branch");
    expect(!onBranch(repository, "side", mainOnly), "merge leaked target history into source");
    // An unpublished UUID (e.g. an arbitrary ?revision= value) is never on a
    // branch, and answering that must not depend on the history length.
    expect(!onBranch(repository, "main", RevisionId::generate()), "unknown revision accepted");
    const RevisionId branchUuid(repository.branchId("main")->uuid());
    expect(!onBranch(repository, "main", branchUuid), "branch UUID accepted as revision");
    expectKind(StorageErrorKind::NotFound, "unknown branch",
               [&] { (void)onBranch(repository, "missing", first); });
    expectKind(StorageErrorKind::InvalidInput, "invalid branch name",
               [&] { (void)onBranch(repository, "../x", first); });
}

// Reachability walks each revision once: on a long linear history, the walk
// for an ancestor deep in history must not re-read the ancestry per node.
void testReachabilityIsLinear() {
    TestDirectory directory("reachability");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    Key key{}; key[15] = 4;
    const RevisionId first = repository.commit("main", {Change{key, Bytes{0}}});
    const std::function<double(unsigned)> timeWalk = [&](unsigned commits) {
        for (unsigned i = 0; i < commits; ++i)
            (void)repository.commit("main", {Change{key, Bytes{static_cast<std::uint8_t>(i)}}});
        // The fastest of several walks filters out scheduling noise from
        // parallel test runs; a quadratic walk is slow on every attempt.
        double fastest = std::numeric_limits<double>::max();
        for (unsigned attempt = 0; attempt < 7; ++attempt) {
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            expect(onBranch(repository, "main", first), "root not reachable");
            fastest = std::min(fastest, std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count());
        }
        return fastest;
    };
    const double shorter = timeWalk(100);  // history 101
    const double longer = timeWalk(300);   // history 401: 4x the revisions
    // A quadratic walk takes ~16x as long; allow generous noise for linear.
    expect(longer < shorter * 9.0 + 0.05,
           "reachability walk grows faster than linearly with history");
}

} // namespace

int main() {
    try {
        testRandomizedDiffMatchesReference();
        testPrefixSplitAboveLargeSubtree();
        testLimitBoundaries();
        testRevisionOnBranch();
        testReachabilityIsLinear();
        std::cout << "Bounded diff and branch scoping tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Bounded diff/branch scoping test failed: " << error.what() << '\n';
        return 1;
    }
}
