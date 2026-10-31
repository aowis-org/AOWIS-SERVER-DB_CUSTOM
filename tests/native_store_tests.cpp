#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"
#include <fcntl.h>
#include <sys/file.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;

using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Difference;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::Repository;
using aowis::dvmdb::test::CommittedRevision;
using Revision = CommittedRevision;

Key key(std::uint64_t number) {
    Key result{};
    for (unsigned i = 0; i < 8; ++i) {
        result[15 - i] = static_cast<std::uint8_t>((number >> (8U * i)) & 0xffU);
    }
    return result;
}

Bytes value(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temporaryPath(std::string_view name) {
    const std::uint64_t clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
        ("aowis-dvmdb-test-" + std::string(name) + "-" + std::to_string(::getpid()) +
         "-" + std::to_string(clock));
}

struct TemporaryRepo {
    explicit TemporaryRepo(std::string_view name) : directory(temporaryPath(name)) {}
    ~TemporaryRepo() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    std::filesystem::path directory;
};

void testEmptyAndBranches() {
    TemporaryRepo test("branches");
    RevisionId baseline = RevisionId::generate();
    {
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        expect(!repository.optionalHead("main") && repository.scan("main").empty() &&
                   !repository.get("main", key(1)),
               "a new branch must be unborn and empty");
        repository.commit("main", {{key(1), value("one")}, {key(2), value("two")}});
        baseline = repository.head("main");
        repository.createBranch("feature", "main");
        expect(repository.head("feature") == baseline, "branch must share initial revision");
        const Revision modified = repository.commitRevision("feature", {
            {key(1), value("revised")}, {key(2), std::nullopt}, {key(3), value("three")}
        });
        expect(modified.parents.size() == 1 && modified.parents[0] == baseline,
               "revision must link to its parent");
        expect(repository.get("main", key(1)) == value("one"), "branch isolation failed");
        expect(!repository.get("main", key(3)), "uncommitted branch object leaked");
        expect(repository.get("feature", key(1)) == value("revised"), "modified object missing");
        expect(!repository.get("feature", key(2)), "deletion failed");
        const std::vector<Difference> changes = repository.diffBranches("main", "feature");
        expect(changes.size() == 3, "diff failed to find insert, update and delete");
        expect(changes[0].before && changes[0].after, "modification misclassified");
        expect(changes[1].before && !changes[1].after, "deletion misclassified");
        expect(!changes[2].before && changes[2].after, "addition misclassified");
        expect(repository.scan("main").size() == 2, "full scan failed");
        expect(repository.scan("feature").size() == 2, "modified scan failed");
        expect(repository.commitRevision("feature", {{key(100), std::nullopt}}).root == modified.root,
               "deleting absent key should not change tree");
        try {
            repository.createBranch("../escape", "main");
            throw std::runtime_error("invalid branch name accepted");
        } catch (const std::runtime_error& failure) {
            expect(std::string(failure.what()) == "invalid branch name", "unexpected branch validation");
        }
    }
    {
        ProjectRepository reopened(test.directory);
        expect(reopened.head("main") == baseline, "head was not durable");
        expect(reopened.get("feature", key(1)) == value("revised"), "data not durable");
        expect(reopened.diffBranches("main", "feature").size() == 3, "diff not durable");
    }
}

void testCompressedTreeAndNoOp() {
    TemporaryRepo test("compression");
    ProjectRepository repository(test.directory);
    repository.createBranch("main");
    std::vector<Change> changes;
    for (std::uint64_t i = 0; i < 500; ++i) {
        changes.push_back(Change{key(i * 281474976710677ULL), value("object" + std::to_string(i))});
    }
    repository.commit("main", changes);
    repository.createBranch("base", "main");
    expect(repository.scan("main").size() == 500, "inserts not preserved");
    repository.commit("main", {{key(0), std::nullopt}, {key(281474976710677ULL), std::nullopt}});
    expect(repository.diffBranches("base", "main").size() == 2, "sparse diff failed");
    std::vector<Change> restore{{key(0), value("object0")},
                                {key(281474976710677ULL), value("object1")}};
    const Revision restored = repository.commitRevision("main", restore);
    expect(restored.root == repository.headRevision("base").root,
           "restoring previous content must restore identical tree hash");
    expect(repository.diffBranches("base", "main").empty(), "identical roots should have empty diff");
    const std::uint64_t before = repository.packSize();
    repository.commit("main", {{key(0), value("object0")}});
    expect(repository.packSize() > before, "revision should be recorded");
}

void testRecoveryAndCorruption() {
    TemporaryRepo test("recovery");
    {
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        repository.commit("main", {{key(5), value("still-here")}});
    }
    const std::filesystem::path pack = test.directory / "objects.pack";
    {
        std::ofstream out(pack, std::ios::app | std::ios::binary);
        out << "AOB"; // torn, incomplete record
    }
    {
        ProjectRepository repository(test.directory);
        expect(repository.get("main", key(5)) == value("still-here"),
               "incomplete trailing record damaged committed revision");
    }
    {
        std::fstream io(pack, std::ios::in | std::ios::out | std::ios::binary);
        io.seekg(45);
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(45);
        byte ^= static_cast<char>(0x01);
        io.write(&byte, 1);
    }
    bool rejected = false;
    try {
        ProjectRepository repository(test.directory);
        repository.verifyPack();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "corrupt committed content was accepted by full verification");
}

// Compaction is a reachability collector, NOT history pruning. Parent
// revisions and all retained branch heads must survive even when newer
// values have replaced their contents. A valid orphan pack object simulates
// an interrupted/unpublished operation without altering an existing ref.
void testCompactionAndRetainedHistory() {
    TemporaryRepo test("compaction-history");
    RevisionId mainBefore = RevisionId::generate();
    RevisionId mainAfter = RevisionId::generate();
    RevisionId featureBefore = RevisionId::generate();
    std::uint64_t withOrphan = 0;
    const Bytes orphan = value("unpublished-private-payload");
    {
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        repository.commit("main", {{key(1), value("old")}, {key(2), value("stable")}});
        repository.createBranch("feature", "main");
        featureBefore = repository.head("feature");
        mainBefore = repository.head("main");
        repository.commit("main", {{key(1), value("new")}, {key(3), value("third")}});
        mainAfter = repository.head("main");
        repository.verifyPack();
    }
    {
        const std::filesystem::path pack = test.directory / "objects.pack";
        const Hash orphanHash = aowis::dvmdb::sha256(orphan);
        std::array<std::uint8_t, 45> header{};
        header[0] = 'A'; header[1] = 'O'; header[2] = 'B'; header[3] = 'J'; header[4] = 1;
        std::copy(orphanHash.begin(), orphanHash.end(), header.begin() + 5);
        for (unsigned i = 0; i < 8; ++i) {
            header[37 + i] = static_cast<std::uint8_t>(
                static_cast<std::uint64_t>(orphan.size()) >> (56U - 8U * i));
        }
        std::ofstream output(pack, std::ios::binary | std::ios::app);
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        output.write(reinterpret_cast<const char*>(orphan.data()),
                     static_cast<std::streamsize>(orphan.size()));
        expect(output.good(), "failed to append test orphan");
    }
    {
        ProjectRepository repository(test.directory);
        repository.verifyPack();
        withOrphan = repository.objectCount();
        expect(repository.head("main") == mainAfter &&
               repository.head("feature") == featureBefore,
               "unpublished object changed retained heads");
        const aowis::dvmdb::CompactionStats stats = repository.compact();
        expect(stats.objectsBefore == withOrphan && stats.objectsAfter + 1 == withOrphan,
               "compaction did not remove exactly the unreachable object");
        expect(stats.packBytesBefore == stats.packBytesAfter + 45 + orphan.size(),
               "pack compaction did not recover orphan bytes");
        expect(stats.indexBytesBefore == stats.indexBytesAfter + 52,
               "index compaction did not recover orphan entry");
        expect(repository.head("main") == mainAfter &&
               repository.head("feature") == mainBefore,
               "compaction changed revision identity or branch ancestry");
        expect(repository.get("main", key(1)) == value("new") &&
               repository.get("feature", key(1)) == value("old"),
               "compaction lost a retained historical value");
        repository.verifyPack();
        const aowis::dvmdb::CompactionStats repeated = repository.compact();
        expect(repeated.objectsAfter == stats.objectsAfter &&
               repeated.packBytesAfter == stats.packBytesAfter,
               "repeating compaction should not change a clean repository");
        repository.commit("main", {{key(4), value("after-compaction")}});
        mainAfter = repository.head("main");
        repository.verifyPack();
    }
    {
        ProjectRepository reopened(test.directory);
        expect(reopened.head("main") == mainAfter &&
               reopened.head("feature") == featureBefore,
               "reopening after compaction changed branch heads");
        expect(reopened.get("main", key(4)) == value("after-compaction"),
               "writes following compaction were not durable");
        reopened.verifyPack();
        const std::filesystem::path marker = test.directory / "objects.current";
        expect(std::filesystem::exists(marker), "compaction did not publish active pack");
        std::ifstream input(marker, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        expect(text.size() == 39, "invalid compaction generation marker");
        const std::filesystem::path activeIndex =
            test.directory / ("objects." + text.substr(6, 32) + ".idx");
        std::filesystem::remove(activeIndex);
        // The next open must recover a missing derived index from the new pack.
    }
    {
        ProjectRepository reopened(test.directory);
        expect(reopened.head("main") == mainAfter,
               "missing compacted index was not rebuilt");
        reopened.verifyPack();
    }
}

void testCompactionRequiresExclusiveLifetime() {
    TemporaryRepo test("compaction-lock");
    ProjectRepository first(test.directory);
    first.createBranch("main");
    first.commit("main", {{key(9), value("protected")}});
    {
        ProjectRepository second(test.directory);
        bool rejected = false;
        try {
            (void)first.compact();
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        expect(rejected, "compaction must refuse another open repository handle");
        // A failed nonblocking upgrade must NOT relinquish its shared
        // lifetime lock and allow the other open handle to compact.
        bool otherRejected = false;
        try {
            (void)second.compact();
        } catch (const std::runtime_error&) {
            otherRejected = true;
        }
        expect(otherRejected, "failed lock upgrade released the shared lifetime lock");
        expect(second.get("main", key(9)) == value("protected"),
               "rejected compaction changed active repository");
    }
    (void)first.compact();
    expect(first.get("main", key(9)) == value("protected"),
           "compaction after closing other handle corrupted data");
}

void testMixedEditsAgainstReference() {
    TemporaryRepo test("reference");
    ProjectRepository repository(test.directory);
    repository.createBranch("main");
    std::map<Key, Bytes> expected;
    std::uint64_t random = 0x8b51a7d341ULL;
    for (unsigned revision = 0; revision < 35; ++revision) {
        std::vector<Change> edits;
        for (unsigned j = 0; j < 25; ++j) {
            random ^= random << 13U;
            random ^= random >> 7U;
            random ^= random << 17U;
            const Key target = key(random % 300);
            if (random % 7 == 0) {
                expected.erase(target);
                edits.push_back(Change{target, std::nullopt});
            } else {
                Bytes bytes = value("rev-" + std::to_string(revision) + "-" + std::to_string(j));
                expected[target] = bytes;
                edits.push_back(Change{target, bytes});
            }
        }
        repository.commit("main", edits);
        const std::vector<std::pair<Key, Bytes>> actual = repository.scan("main");
        expect(actual.size() == expected.size(), "reference scan count differs");
        std::size_t index = 0;
        for (const std::pair<const Key, Bytes>& item : expected) {
            expect(actual[index].first == item.first && actual[index].second == item.second,
                   "reference scan data differs");
            ++index;
        }
    }
}

// A versioned content-addressed tree must have the same root for the same
// key/value map regardless of insertion order or batch boundaries. This is
// important when replicas construct equivalent snapshots independently.
Key scatteredKey(std::uint64_t number) {
    Key result{};
    const std::uint64_t first = number * 0x9e3779b97f4a7c15ULL + 0x6295c58d01ULL;
    const std::uint64_t second = (number ^ 0xb7e151628aed2a6bULL) * 0xbf58476d1ce4e5b9ULL;
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>(first >> (56U - 8U * i));
        result[i + 8] = static_cast<std::uint8_t>(second >> (56U - 8U * i));
    }
    return result;
}

// The Git-style index is an expendable acceleration structure, never the
// source of truth. A missing, torn, or corrupted sidecar must recover from a
// verified pack without changing content hashes or logical revisions.
void testPersistentPackIndex() {
    TemporaryRepo test("persistent-pack-index");
    Hash root{};
    std::uint64_t packBytes = 0;
    std::uint64_t objectCount = 0;
    {
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        std::vector<Change> initial;
        for (std::uint64_t i = 0; i < 400; ++i) {
            initial.push_back({scatteredKey(i), value("object-" + std::to_string(i))});
        }
        repository.commit("main", initial);
        root = repository.headRevision("main").root;
        objectCount = repository.objectCount();
        packBytes = repository.packSize();
        repository.verifyPack();
    }
    const std::filesystem::path index = test.directory / "objects.idx";
    expect(std::filesystem::file_size(index) == 8 + objectCount * 52,
           "persistent index contains missing or duplicate entries");
    {
        ProjectRepository reopened(test.directory);
        expect(reopened.headRevision("main").root == root, "indexed reopen changed root");
        reopened.verifyPack();
    }
    // A missing index must be reconstructed from the pack, not erase data.
    std::filesystem::remove(index);
    {
        ProjectRepository rebuilt(test.directory);
        expect(rebuilt.headRevision("main").root == root && rebuilt.objectCount() == objectCount,
               "missing sidecar was not rebuilt");
        rebuilt.verifyPack();
    }
    // An incomplete trailing index entry should be repaired without changing
    // already indexed objects or requiring a new revision.
    std::filesystem::resize_file(index, std::filesystem::file_size(index) - 11);
    {
        ProjectRepository repaired(test.directory);
        expect(repaired.headRevision("main").root == root && repaired.packSize() == packBytes,
               "torn index entry affected committed data");
        expect(std::filesystem::file_size(index) == 8 + objectCount * 52,
               "torn sidecar entry not restored");
    }
    // A complete but invalid index checksum also triggers a verified rebuild.
    {
        std::fstream file(index, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(8 + 37);
        const char changed = static_cast<char>(0x6a);
        file.write(&changed, 1);
    }
    {
        ProjectRepository rebuilt(test.directory);
        expect(rebuilt.headRevision("main").root == root, "corrupt index damaged revision");
        rebuilt.verifyPack();
    }
    // A valid unindexed pack suffix (e.g. interruption during index publish)
    // is recovered by hashing only that suffix.
    {
        ProjectRepository repository(test.directory);
        repository.commit("main", {{scatteredKey(401), value("new-value")}});
        root = repository.headRevision("main").root;
        objectCount = repository.objectCount();
    }
    std::filesystem::resize_file(index, std::filesystem::file_size(index) - 52);
    {
        ProjectRepository repaired(test.directory);
        expect(repaired.headRevision("main").root == root && repaired.objectCount() == objectCount,
               "valid pack suffix was lost due to truncated index");
        repaired.verifyPack();
    }
}

void testBatchCanonicalRootsAndBranchIndependence() {
    TemporaryRepo test("bulk-canonical");
    // Independent root histories live in separate repositories; their
    // content roots are compared by hash.
    TemporaryRepo reverseTest("bulk-canonical-reverse");
    TemporaryRepo batchesTest("bulk-canonical-batches");
    ProjectRepository repository(test.directory);
    ProjectRepository reverseRepository(reverseTest.directory);
    ProjectRepository batchesRepository(batchesTest.directory);
    repository.createBranch("bulk");
    reverseRepository.createBranch("reverse");
    batchesRepository.createBranch("small-batches");

    std::vector<Change> initial;
    for (std::uint64_t i = 0; i < 1600; ++i) {
        initial.push_back({scatteredKey(i), value("value-" + std::to_string(i))});
    }
    const Revision bulk = repository.commitRevision("bulk", initial);
    std::reverse(initial.begin(), initial.end());
    reverseRepository.commit("reverse", initial);
    for (std::size_t i = 0; i < initial.size(); i += 37) {
        const std::size_t last = std::min(initial.size(), i + 37);
        batchesRepository.commit("small-batches", std::vector<Change>(initial.begin() +
                          static_cast<std::ptrdiff_t>(i), initial.begin() +
                          static_cast<std::ptrdiff_t>(last)));
    }
    expect(bulk.root == reverseRepository.headRevision("reverse").root,
           "reverse insertion changed tree hash");
    expect(bulk.root == batchesRepository.headRevision("small-batches").root,
           "batch shape changed tree hash");

    repository.createBranch("baseline", "bulk");
    std::vector<Change> mixed;
    std::map<Key, Bytes> expected;
    for (std::uint64_t i = 0; i < 1600; ++i) {
        expected[scatteredKey(i)] = value("value-" + std::to_string(i));
    }
    for (std::uint64_t i = 0; i < 2300; ++i) {
        const std::uint64_t number = (i * 1229ULL + 347ULL) % 2550ULL;
        if (i % 7 == 0) {
            mixed.push_back({scatteredKey(number), std::nullopt});
            expected.erase(scatteredKey(number));
        } else {
            Bytes payload = value("changed-" + std::to_string(i));
            mixed.push_back({scatteredKey(number), payload});
            expected[scatteredKey(number)] = payload;
        }
    }
    repository.commit("bulk", mixed);
    const std::vector<std::pair<Key, Bytes>> current = repository.scan("bulk");
    expect(current.size() == expected.size(), "mixed bulk update lost data");
    std::size_t i = 0;
    for (const std::pair<const Key, Bytes>& item : expected) {
        expect(current[i].first == item.first && current[i].second == item.second,
               "mixed batch result differs from reference map");
        ++i;
    }
    expect(repository.headRevision("baseline").root == bulk.root, "bulk modifications affected base branch");
    std::vector<Change> finalEdits;
    for (std::uint64_t index = 0; index < 2550; ++index) {
        const Key target = scatteredKey(index);
        if (index < 1600) finalEdits.push_back({target, value("value-" + std::to_string(index))});
        else finalEdits.push_back({target, std::nullopt});
    }
    repository.commit("bulk", finalEdits);
    expect(repository.headRevision("bulk").root == bulk.root, "restored bulk tree root is not canonical");
    expect(repository.diffBranches("bulk", "baseline").empty(), "equivalent trees have a nonempty diff");
}


void testTypedModelRecordsAndReferences() {
    TemporaryRepo test("model-references");
    ProjectRepository repository(test.directory);
    repository.createBranch("main");
    const Key nodeA = key(11);
    const Key nodeB = key(12);
    const Key pipe = key(13);
    const aowis::dvmdb::ModelRecord junction{1, {}, value("junction-canonical-codec-bytes")};
    const aowis::dvmdb::ModelRecord link{4, {nodeA, nodeB}, value("pipe-canonical-codec-bytes")};
    const Bytes serialized = aowis::dvmdb::encodeModelRecord(link);
    const aowis::dvmdb::ModelRecord unpacked = aowis::dvmdb::decodeModelRecord(serialized);
    expect(unpacked.kind == link.kind && unpacked.references == link.references &&
           unpacked.payload == link.payload, "model envelope changed codec bytes");
    const aowis::dvmdb::ModelRecord unordered{4, {nodeB, nodeA, nodeB}, link.payload};
    expect(aowis::dvmdb::encodeModelRecord(unordered) == serialized,
           "reference set order changed canonical envelope hash");
    for (std::size_t length = 0; length < serialized.size(); ++length) {
        bool rejected = false;
        try {
            aowis::dvmdb::decodeModelRecord(Bytes(serialized.begin(), serialized.begin() +
                static_cast<std::ptrdiff_t>(length)));
        } catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "accepted truncated model envelope");
    }
    // Forward references inside the same atomic batch must work.
    repository.commitModel("main", {{pipe, link}, {nodeB, junction}, {nodeA, junction}});
    repository.createBranch("base", "main");
    expect(repository.getModel("main", pipe)->references == link.references,
           "model references missing after commit");
    expect(repository.getModel("main", pipe)->payload == link.payload,
           "canonical payload changed after storage");
    const Hash savedRoot = repository.headRevision("main").root;
    const std::uint64_t savedPackSize = repository.packSize();
    const std::function<void(const std::vector<aowis::dvmdb::ModelChange>&)> invalid =
        [&](const std::vector<aowis::dvmdb::ModelChange>& changes) {
        bool rejected = false;
        try { repository.commitModel("main", changes); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "invalid model transaction accepted");
        expect(repository.headRevision("main").root == savedRoot, "rejected model transaction changed head");
        expect(repository.packSize() == savedPackSize, "rejected transaction appended pack data");
    };
    invalid({{nodeA, std::nullopt}});
    invalid({{key(14), aowis::dvmdb::ModelRecord{4, {key(99)}, value("missing")}}});
    invalid({{nodeA, std::nullopt}, {pipe, link}});
    invalid({{key(20), aowis::dvmdb::ModelRecord{1, {key(20)}, value("self")}}});
    invalid({{key(21), junction}, {key(21), junction}});
    invalid({{Key{}, junction}});
    // Removing both an edge and one endpoint is legal in one commit.
    repository.commitModel("main", {{pipe, std::nullopt}, {nodeA, std::nullopt}});
    expect(!repository.getModel("main", pipe), "pipe deletion not persisted");
    expect(!repository.getModel("main", nodeA), "node deletion not persisted");
    expect(repository.getModel("base", pipe)->payload == link.payload,
           "base branch lost pipe");
    {
        ProjectRepository reopened(test.directory);
        expect(reopened.getModel("base", pipe)->references == link.references,
               "model references not durable");
        expect(!reopened.getModel("main", nodeA), "model deletion not durable");
    }
}

void testTypedModelConcurrentHandles() {
    TemporaryRepo test("model-concurrent");
    ProjectRepository first(test.directory);
    first.createBranch("main");
    first.commitModel("main", {{key(1), aowis::dvmdb::ModelRecord{1, {}, value("node")}}});
    ProjectRepository second(test.directory);
    second.commitModel("main", {{key(2), aowis::dvmdb::ModelRecord{4, {key(1)}, value("pipe")}}});
    bool rejected = false;
    try { first.commitModel("main", {{key(1), std::nullopt}}); }
    catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, "stale handle allowed deleting reference target");
}

void testDurableReverseReferences() {
    TemporaryRepo test("reverse-links");
    const Key nodeA = key(101);
    const Key nodeB = key(102);
    const Key nodeC = key(103);
    const Key pipeA = key(201);
    const Key pipeB = key(202);
    const aowis::dvmdb::ModelRecord node{1, {}, value("junction")};
    const aowis::dvmdb::ModelRecord ab{4, {nodeA, nodeB}, value("pipe-ab")};
    const aowis::dvmdb::ModelRecord ac{4, {nodeA, nodeC}, value("pipe-ac")};
    const std::vector<aowis::dvmdb::ModelChange> initial{
        {pipeA, ab}, {pipeB, ab}, {nodeA, node}, {nodeB, node}, {nodeC, node}};
    Hash firstIndex{};
    // A repository has one root history: independently built histories that
    // are compared by content live in their own repositories.
    TemporaryRepo sequentialTest("reverse-links-sequential");
    TemporaryRepo rawTest("reverse-links-raw");
    {
        ProjectRepository repository(test.directory);
        ProjectRepository sequential(sequentialTest.directory);
        repository.createBranch("main");
        sequential.createBranch("sequential");
        repository.commitModel("main", initial);
        firstIndex = repository.headRevision("main").referencesRoot;
        expect(repository.headRevision("main").modelIndexed && firstIndex != Hash{},
               "typed revision did not publish reverse-reference index");
        expect(repository.referencing("main", nodeA) == std::vector<Key>({pipeA, pipeB}),
               "incoming references are not sorted and complete");
        expect(repository.referencing("main", nodeB) == std::vector<Key>({pipeA, pipeB}),
               "incorrect references for second endpoint");
        expect(repository.referencing("main", nodeC).empty(),
               "unreferenced node should have no index entry");
        bool rawRejected = false;
        try { repository.commit("main", {{key(250), value("unchecked")}}); }
        catch (const std::runtime_error&) { rawRejected = true; }
        expect(rawRejected, "raw write bypassed model reference validation");
        sequential.commitModel("sequential", {{nodeA, node}, {nodeB, node}, {nodeC, node}});
        sequential.commitModel("sequential", {{pipeB, ab}});
        sequential.commitModel("sequential", {{pipeA, ab}});
        expect(sequential.headRevision("sequential").root == repository.headRevision("main").root,
               "reference-index changes altered primary canonical model root");
        expect(sequential.headRevision("sequential").referencesRoot == firstIndex,
               "incoming index depends on insertion order / batch boundaries");

        repository.createBranch("original", "main");
        repository.commitModel("main", {{pipeA, ac}});
        expect(repository.referencing("main", nodeB) == std::vector<Key>({pipeB}),
               "rewiring did not remove old incoming edge");
        expect(repository.referencing("main", nodeC) == std::vector<Key>({pipeA}),
               "rewiring did not create new incoming edge");
        expect(repository.referencing("original", nodeB) == std::vector<Key>({pipeA, pipeB}),
               "branch reference index was mutated in place");
        const RevisionId protectedHead = repository.head("main");
        const std::uint64_t protectedBytes = repository.packSize();
        bool rejected = false;
        try { repository.commitModel("main", {{nodeB, std::nullopt}}); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected && protectedHead == repository.head("main") &&
               protectedBytes == repository.packSize(),
               "rejected referenced-node deletion wrote data or advanced revision");
        repository.commitModel("main", {{pipeB, std::nullopt}, {nodeB, std::nullopt}});
        expect(repository.referencing("main", nodeB).empty(),
               "deleted endpoint still appears in reverse index");
        expect(repository.referencing("main", nodeA) == std::vector<Key>({pipeA}),
               "unrelated surviving reference disappeared");
        // Reverse-edge insertion/deletion can return to an identical root.
        repository.commitModel("main", {{nodeB, node}, {pipeB, ab}, {pipeA, ab}});
        expect(repository.headRevision("main").root == repository.headRevision("original").root &&
               repository.headRevision("main").referencesRoot == firstIndex,
               "restored model and reference-index roots are not deterministic");

        ProjectRepository raw(rawTest.directory);
        raw.createBranch("raw");
        raw.commit("raw", {{key(17), value("plain-data")}});
        rejected = false;
        try { raw.commitModel("raw", {{nodeA, node}}); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "model commit was allowed onto existing unindexed raw data");
    }
    {
        ProjectRepository reopened(test.directory);
        expect(reopened.headRevision("original").referencesRoot == firstIndex,
               "reopened revision lost reverse-reference root");
        expect(reopened.referencing("main", nodeA) == std::vector<Key>({pipeA, pipeB}),
               "reopened reverse reference index returned incorrect IDs");
    }
}

void testMultipleConnections() {
    TemporaryRepo test("process-index");
    ProjectRepository first(test.directory);
    first.createBranch("main");
    // The project is pinned on first commit; a concurrent handle opened
    // before it binds explicitly.
    ProjectRepository second(test.directory, first.project());
    first.commit("main", {{key(7), value("first")}});
    expect(second.get("main", key(7)) == value("first"), "second repository index is stale");
    second.commit("main", {{key(8), value("second")}});
    expect(first.get("main", key(8)) == value("second"), "first repository index is stale");
    expect(first.scan("main").size() == 2, "concurrent handles lost updates");
}


// A commit much larger than the block writer's bounded staging window must
// preserve exact revision identities. It may durably write unreachable chunks
// before publishing the branch ref; an interrupted ref publication must still
// leave the previous committed tree intact and readable after reopening.





void testMoveAwareModelAndRawCommits() {
    {
        // One root history per repository: each independent root lives in
        // its own repository, and roots are compared by content hash.
        TemporaryRepo borrowedTest("move-aware-borrowed");
        TemporaryRepo movedTest("move-aware-moved");
        TemporaryRepo duplicateTest("move-aware-raw-duplicate");
        TemporaryRepo canonicalTest("move-aware-raw-canonical");
        ProjectRepository borrowedRepository(borrowedTest.directory);
        ProjectRepository movedRepository(movedTest.directory);
        ProjectRepository duplicateRepository(duplicateTest.directory);
        ProjectRepository canonicalRepository(canonicalTest.directory);
        borrowedRepository.createBranch("borrowed");
        movedRepository.createBranch("moved");
        duplicateRepository.createBranch("raw-duplicate");
        canonicalRepository.createBranch("raw-canonical");

        std::vector<aowis::dvmdb::ModelChange> changes;
        changes.reserve(2048);
        const Bytes nodeValue(96, static_cast<std::uint8_t>('n'));
        const Bytes linkValue(96, static_cast<std::uint8_t>('l'));
        for (std::uint64_t id = 1; id <= 1024; ++id) {
            changes.push_back({key(id), aowis::dvmdb::ModelRecord{1, {}, nodeValue}});
            changes.push_back({key(id + 4096),
                aowis::dvmdb::ModelRecord{4, {key(id), key(id)}, linkValue}});
        }
        std::reverse(changes.begin(), changes.end());
        const Revision borrowed = borrowedRepository.commitModelRevision("borrowed", changes);
        expect(changes.size() == 2048 && changes.front().record.has_value() &&
               changes.front().record->payload == linkValue,
               "borrowed model changes were modified by commit");
        const Revision moved = movedRepository.commitModelRevision("moved", std::move(changes));
        expect(borrowed.root == moved.root &&
               borrowed.referencesRoot == moved.referencesRoot,
               "move-aware model import changed canonical model or index roots");
        expect(movedRepository.referencing("moved", key(31)) == std::vector<Key>({key(4127)}),
               "move-aware model import lost or duplicated reverse references");
        expect(movedRepository.getModel("moved", key(4127))->payload == linkValue,
               "move-aware import modified canonical payload bytes");

        duplicateRepository.commit("raw-duplicate", {
            {key(7), value("old")}, {key(8), value("eight")},
            {key(7), value("new")}, {key(8), std::nullopt},
            {key(8), value("final")}
        });
        canonicalRepository.commit("raw-canonical", {
            {key(7), value("new")}, {key(8), value("final")}
        });
        expect(duplicateRepository.headRevision("raw-duplicate").root ==
               canonicalRepository.headRevision("raw-canonical").root,
               "raw duplicate-key semantics changed under zero-copy sorting");
        borrowedRepository.verifyPack();
        movedRepository.verifyPack();
        duplicateRepository.verifyPack();
        canonicalRepository.verifyPack();
    }
}

void testModelExpectedBranchHead() {
    {
        TemporaryRepo test("expected-model-head");
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        const aowis::dvmdb::ModelRecord initial{1, {}, value("first")};
        const Revision first = repository.commitModelRevision("main", {{key(1), initial}});
        const Revision second = repository.commitModelRevision("main", {
            {key(1), aowis::dvmdb::ModelRecord{1, {}, value("second")}}
        }, first.id);
        expect(second.id != first.id, "model update failed to move branch head");
        const std::uint64_t before = repository.packSize();
        bool rejected = false;
        try {
            repository.commitModel("main", {
                {key(1), aowis::dvmdb::ModelRecord{1, {}, value("stale")}}
            }, first.id);
        } catch (const std::runtime_error&) { rejected = true; }
        expect(rejected && repository.head("main") == second.id &&
               repository.packSize() == before,
               "stale model commit changed the ref or persistent data");
        const std::optional<aowis::dvmdb::ModelRecord> restored =
            repository.getModel("main", key(1));
        expect(restored.has_value() && restored->payload == value("second"),
               "stale model commit replaced committed content");
    }
}

// Historical APIs operate on immutable revision IDs rather than branch heads.
// Verify that compaction retains old roots.
void testHistoricalRevisionApis() {
    {
        TemporaryRepo test("historical-revision-api");
        TemporaryRepo modelTest("historical-revision-api-models");
        RevisionId first = RevisionId::generate();
        RevisionId second = RevisionId::generate();
        RevisionId featureOnly = RevisionId::generate();
        RevisionId indexed = RevisionId::generate();
        RevisionId indexedNext = RevisionId::generate();
        {
            ProjectRepository repository(test.directory);
            repository.createBranch("main");
            first = repository.commit("main", {{key(1), value("old")},
                                               {key(2), value("two")}});
            second = repository.commit("main", {{key(1), value("new")},
                                                {key(2), std::nullopt}});
            repository.createBranch("feature", "main");
            featureOnly = repository.commit("feature", {{key(3), value("feature")}});
            expect(repository.revision(first).id == first &&
                   repository.revision(first).parents.empty() &&
                   repository.revision(second).parents == std::vector<RevisionId>{first},
                   "historical revision metadata missing");
            expect(repository.getAt(first, key(1)) == value("old") &&
                   repository.getAt(second, key(1)) == value("new") &&
                   repository.getAt(first, key(2)) == value("two") &&
                   !repository.getAt(second, key(2)),
                   "historical raw lookup returned moving branch content");
            const std::vector<aowis::dvmdb::RevisionGraphRevision> firstPage = repository.history("main", 2);
            expect(firstPage.size() == 2 && firstPage[0].id == second &&
                   firstPage[1].id == first, "first history page incorrect");
            const std::vector<aowis::dvmdb::RevisionGraphRevision> lastPage =
                repository.history("main", 2, second);
            expect(lastPage.size() == 1 && lastPage[0].id == first &&
                   lastPage[0].parents.empty() &&
                   repository.history("main", 2, first).empty(),
                   "history cursor pagination did not reach the root revision");
            bool zeroRejected = false;
            try { (void)repository.history("main", 0); }
            catch (const aowis::dvmdb::StorageError& failure) {
                zeroRejected = failure.kind() == aowis::dvmdb::StorageErrorKind::InvalidInput;
            }
            expect(zeroRejected, "zero-sized history page was not rejected as invalid input");
            expect(repository.history("feature", 2)[0].id == featureOnly,
                   "branch histories are not independent");
            bool rejected = false;
            try { (void)repository.history("main", 3, featureOnly); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "history accepted a cursor from an unrelated branch");
            rejected = false;
            try { (void)repository.history("main", 1001); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "unbounded history page accepted");
            rejected = false;
            try { (void)repository.revision(RevisionId::generate()); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "unknown revision was accepted");
            const std::vector<Difference> difference = repository.diff(first, second);
            expect(difference.size() == 2 && difference[0].key == key(1) &&
                   difference[0].before && difference[0].after &&
                   difference[1].key == key(2) && difference[1].before &&
                   !difference[1].after && repository.diff(first, first).empty(),
                   "historical Merkle diff incorrect");
            repository.createBranchAt("recovered", "main", first);
            expect(repository.head("recovered") == first &&
                   repository.get("recovered", key(1)) == value("old") &&
                   repository.head("main") == second,
                   "historical branch creation changed source branch");
            const RevisionId recoveredCommit = repository.commit("recovered", {
                {key(2), value("restored")}
            });
            expect(repository.revision(recoveredCommit).parents == std::vector<RevisionId>{first} &&
                   repository.getAt(first, key(2)) == value("two"),
                   "editing new branch mutated immutable revision");
            rejected = false;
            try { repository.createBranchAt("invalid", "main", featureOnly); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "branch from non-ancestor revision was accepted");
            rejected = false;
            try { repository.createBranchAt("recovered", "main", first); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "historical branch name reused");

            rejected = false;
            try { (void)repository.getModelAt(first, key(1)); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "typed historical lookup accepted raw revision");
            rejected = false;
            try { (void)repository.referencingAt(first, key(101)); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "historical reverse index accepted raw revision");
            repository.verifyPack();
        }
        {
            // A model-indexed history is its own repository (a branch is
            // either raw or model-indexed, one root history per repository).
            ProjectRepository models(modelTest.directory);
            models.createBranch("models");
            const aowis::dvmdb::ModelRecord source{11, {}, value("source")};
            const aowis::dvmdb::ModelRecord dependent{12, {key(101)}, value("child")};
            indexed = models.commitModel("models", {{key(101), source},
                                                   {key(102), dependent}});
            indexedNext = models.commitModel("models", {{key(102), std::nullopt}});
            const std::optional<aowis::dvmdb::ModelRecord> saved =
                models.getModelAt(indexed, key(102));
            expect(saved && saved->kind == 12 && saved->payload == value("child") &&
                   saved->references == std::vector<Key>{key(101)} &&
                   !models.getModelAt(indexedNext, key(102)),
                   "historical typed model lookup incorrect");
            expect(models.referencingAt(indexed, key(101)) ==
                       std::vector<Key>{key(102)} &&
                   models.referencingAt(indexedNext, key(101)).empty(),
                   "historical reverse-reference index incorrect");
            models.verifyPack();
        }
        {
            ProjectRepository reopened(test.directory);
            ProjectRepository reopenedModels(modelTest.directory);
            expect(reopened.history("main", 3).size() == 2 &&
                   reopened.getAt(first, key(1)) == value("old") &&
                   reopenedModels.referencingAt(indexed, key(101)) ==
                       std::vector<Key>{key(102)},
                   "historical APIs failed after repository reopening");
            reopened.compact();
            reopenedModels.compact();
            expect(reopened.getAt(first, key(1)) == value("old") &&
                   reopened.getAt(featureOnly, key(3)) == value("feature") &&
                   reopenedModels.getModelAt(indexed, key(102)).has_value() &&
                   reopened.diff(first, second).size() == 2 &&
                   reopenedModels.revision(indexedNext).parents ==
                       std::vector<RevisionId>{indexed},
                   "compaction discarded retained historical data");
            reopened.verifyPack();
            reopenedModels.verifyPack();
        }
    }
}

// Merge options together with the optimistic HEAD pins that the UUIDv7
// merge API takes as separate arguments, so a test can update them in place.
struct PinnedMergeOptions : aowis::dvmdb::MergeOptions {
    std::optional<RevisionId> expectedTargetHead;
    std::optional<RevisionId> expectedSourceHead;
};

aowis::dvmdb::MergeResult pinnedMerge(ProjectRepository& repository, std::string_view target,
                                        std::string_view source,
                                        const PinnedMergeOptions& options) {
    return repository.merge(target, source, options, options.expectedTargetHead,
                            options.expectedSourceHead);
}

} // namespace

void testThreeWayBranchMerges() {
    using aowis::dvmdb::MergeStatus;
    using aowis::dvmdb::ModelChange;
    using aowis::dvmdb::ModelRecord;
    {
        TemporaryRepo test("branch-merging");
        RevisionId firstMerge = RevisionId::generate();
        RevisionId sourceOnly = RevisionId::generate();
        {
            ProjectRepository repository(test.directory);
            repository.createBranch("main");
            const Revision base = repository.commitRevision("main", {
                {key(1), value("base")}, {key(2), value("stable")},
                {key(3), value("deleted")}
            });
            repository.createBranch("feature", "main");
            const Revision mainTip = repository.commitRevision("main", {
                {key(1), value("main edit")}, {key(3), std::nullopt}
            });
            const Revision featureTip = repository.commitRevision("feature", {
                {key(2), value("feature edit")}, {key(4), value("feature add")}
            });
            sourceOnly = featureTip.id;
            const aowis::dvmdb::MergeResult merged = repository.merge("main", "feature");
            expect(merged.status == MergeStatus::Merged && merged.base == base.id &&
                   merged.targetBefore == mainTip.id && merged.sourceHead == featureTip.id &&
                   merged.conflicts.empty() &&
                   repository.revision(*merged.resultHead).parents == std::vector<RevisionId>({mainTip.id, featureTip.id}),
                   "raw three-way merge did not record correct ancestry");
            firstMerge = *merged.resultHead;
            expect(repository.get("main", key(1)) == value("main edit") &&
                   repository.get("main", key(2)) == value("feature edit") &&
                   !repository.get("main", key(3)) &&
                   repository.get("main", key(4)) == value("feature add") &&
                   repository.get("feature", key(1)) == value("base"),
                   "raw three-way merge lost independent values or changed source branch");
            expect(repository.history("main", 2).size() == 2 &&
                   repository.history("main", 2)[1].id == mainTip.id,
                   "merge altered first-parent history");
            expect(repository.merge("main", "feature").status == MergeStatus::UpToDate &&
                   repository.head("main") == firstMerge,
                   "repeated merge should be up to date through second parent");
            const Revision followup = repository.commitRevision("feature", {
                {key(5), value("follow up")}
            });
            const aowis::dvmdb::MergeResult again = repository.merge("main", "feature");
            expect(again.status == MergeStatus::Merged && again.base == featureTip.id &&
                   repository.revision(*again.resultHead).parents == std::vector<RevisionId>({firstMerge, followup.id}) &&
                   repository.get("main", key(5)) == value("follow up"),
                   "repeated merge did not follow both sides of revision graph");
            repository.verifyPack();
        }
        {
            ProjectRepository reopened(test.directory);
            expect(reopened.revision(firstMerge).parents.size() == 2 &&
                   reopened.getAt(firstMerge, key(4)) == value("feature add") &&
                   reopened.getAt(sourceOnly, key(1)) == value("base"),
                   "reopening lost a merge parent or historical snapshot");
            (void)reopened.compact();
            expect(reopened.revision(firstMerge).parents.size() == 2 &&
                   reopened.revision(sourceOnly).id == sourceOnly,
                   "compaction did not retain both parents of a merge");
            reopened.verifyPack();
        }
    }
}

void testMergeConflictsAndFastForward() {
    using aowis::dvmdb::MergeStatus;
    {
        TemporaryRepo test("merge-conflicts");
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        repository.commit("main", {{key(1), value("initial")}});
        repository.createBranch("feature", "main");
        const Revision main = repository.commitRevision("main", {{key(1), value("main")}});
        repository.commit("feature", {{key(1), value("feature")}});
        const std::uint64_t bytesBefore = repository.packSize();
        const aowis::dvmdb::MergeResult rejected = repository.merge("main", "feature");
        expect(rejected.status == MergeStatus::Conflicts && rejected.conflicts.size() == 1 &&
               rejected.conflicts[0].key == key(1) && rejected.conflicts[0].base &&
               rejected.conflicts[0].target && rejected.conflicts[0].source &&
               repository.head("main") == main.id &&
               repository.packSize() == bytesBefore,
               "overlapping divergent edit was not rejected atomically");
        repository.createBranch("deleted", "main");
        repository.createBranch("modified", "main");
        repository.commit("deleted", {{key(1), std::nullopt}});
        repository.commit("modified", {{key(1), value("modified")}});
        const aowis::dvmdb::MergeResult deleteVsUpdate =
            repository.merge("deleted", "modified");
        expect(deleteVsUpdate.status == MergeStatus::Conflicts &&
               deleteVsUpdate.conflicts.size() == 1 &&
               deleteVsUpdate.conflicts[0].key == key(1),
               "delete versus update must not silently discard either edit");
        repository.createBranch("new", "main");
        const Revision advanced = repository.commitRevision("new", {{key(10), value("advanced")}});
        const aowis::dvmdb::MergeResult fast = repository.merge("main", "new");
        expect(fast.status == MergeStatus::FastForward && *fast.resultHead == advanced.id &&
               repository.head("main") == advanced.id &&
               repository.get("main", key(10)) == value("advanced"),
               "fast-forward merge unnecessarily recreated a revision");
        repository.createBranch("empty", "main");
        expect(repository.merge("main", "empty").status == MergeStatus::UpToDate,
               "equal-head merge must leave branch untouched");

        // Identical edits made independently still need a merge parent, even
        // when the resulting content root is already equal to the target.
        repository.createBranch("same-left", "main");
        repository.createBranch("same-right", "main");
        const Revision sameLeft = repository.commitRevision("same-left", {
            {key(20), value("identical")}
        });
        repository.commit("same-right", {{key(20), value("temporary")}});
        const Revision sameRight = repository.commitRevision("same-right", {
            {key(20), value("identical")}
        });
        const aowis::dvmdb::MergeResult convergent =
            repository.merge("same-left", "same-right");
        expect(convergent.status == MergeStatus::Merged &&
               repository.revision(*convergent.resultHead).root == sameLeft.root &&
               repository.revision(*convergent.resultHead).parents == std::vector<RevisionId>({sameLeft.id, sameRight.id}),
               "convergent changes lost their explicit merge ancestry");
        // Do not pick an arbitrary merge base in a criss-cross DAG.
        repository.createBranch("left", "main");
        repository.createBranch("right", "main");
        repository.commit("left", {{key(21), value("left")}});
        repository.commit("right", {{key(22), value("right")}});
        repository.createBranch("left-copy", "left");
        expect(repository.merge("left", "right").status == MergeStatus::Merged &&
               repository.merge("right", "left-copy").status == MergeStatus::Merged,
               "criss-cross fixture could not create independent merge parents");
        const RevisionId beforeAmbiguous = repository.head("left");
        bool ambiguousRejected = false;
        try { (void)repository.merge("left", "right"); }
        catch (const std::runtime_error&) { ambiguousRejected = true; }
        expect(ambiguousRejected && repository.head("left") == beforeAmbiguous,
               "criss-cross merge chose an arbitrary common ancestor");
    }
}

void testIndexedModelMerges() {
    using aowis::dvmdb::MergeStatus;
    using aowis::dvmdb::ModelRecord;
    {
        TemporaryRepo test("indexed-model-merge");
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        repository.commitModel("main", {
            {key(1), ModelRecord{1, {}, value("node")}},
            {key(2), ModelRecord{2, {key(1)}, value("first dependent")}},
            {key(3), ModelRecord{3, {}, value("independent")}}
        });
        repository.createBranch("feature", "main");
        repository.commitModel("main", {
            {key(3), ModelRecord{3, {}, value("edited independent")}}
        });
        repository.commitModel("feature", {
            {key(4), ModelRecord{2, {key(1)}, value("second dependent")}}
        });
        const aowis::dvmdb::MergeResult merged = repository.merge("main", "feature");
        expect(merged.status == MergeStatus::Merged && repository.revision(*merged.resultHead).modelIndexed &&
               repository.revision(*merged.resultHead).parents.size() == 2 &&
               repository.referencing("main", key(1)) ==
                   std::vector<Key>({key(2), key(4)}) &&
               repository.getModel("main", key(3))->payload == value("edited independent") &&
               repository.getModel("main", key(4))->payload == value("second dependent") &&
               repository.referencingAt(*merged.resultHead, key(1)) ==
                   std::vector<Key>({key(2), key(4)}),
               "model merge did not merge reference indexes and independent edits");
        repository.verifyPack();

        repository.createBranch("delete-branch", "main");
        repository.createBranch("new-dependent", "main");
        repository.commitModel("delete-branch", {
            {key(2), std::nullopt}, {key(4), std::nullopt}, {key(1), std::nullopt}
        });
        repository.commitModel("new-dependent", {
            {key(5), ModelRecord{2, {key(1)}, value("new dependency")}}
        });
        const RevisionId unchanged = repository.head("delete-branch");
        const std::uint64_t bytesBefore = repository.packSize();
        const aowis::dvmdb::MergeResult dependency =
            repository.merge("delete-branch", "new-dependent");
        expect(dependency.status == MergeStatus::Conflicts &&
               !dependency.conflicts.empty() &&
               repository.head("delete-branch") == unchanged &&
               repository.packSize() == bytesBefore,
               "cross-branch reference conflict was not rejected without writes");

        repository.createBranch("edit-left", "main");
        repository.createBranch("edit-right", "main");
        repository.commitModel("edit-left", {
            {key(3), ModelRecord{3, {}, value("left")}}
        });
        repository.commitModel("edit-right", {
            {key(3), ModelRecord{3, {}, value("right")}}
        });
        const aowis::dvmdb::MergeResult sameKey =
            repository.merge("edit-left", "edit-right");
        expect(sameKey.status == MergeStatus::Conflicts &&
               sameKey.conflicts.size() == 1 && sameKey.conflicts[0].key == key(3),
               "conflicting typed model payloads were overwritten");

        repository.verifyPack();
    }
    {
        // A history that is model-indexed from its root revision merges
        // two-sided additions without any raw ancestor.
        TemporaryRepo freshTest("model-merges-fresh-root");
        ProjectRepository fresh(freshTest.directory);
        fresh.createBranch("fresh");
        const RevisionId root = fresh.commitModel("fresh", {
            {key(10), ModelRecord{1, {}, value("root")}}});
        fresh.createBranch("fresh-other", "fresh");
        fresh.commitModel("fresh", {{key(11), ModelRecord{1, {}, value("one")}}});
        fresh.commitModel("fresh-other", {{key(12), ModelRecord{1, {}, value("two")}}});
        const aowis::dvmdb::MergeResult fromRoot = fresh.merge("fresh", "fresh-other");
        expect(fromRoot.status == MergeStatus::Merged && fromRoot.base == root &&
               fresh.revision(*fromRoot.resultHead).modelIndexed &&
               fresh.revision(root).parents.empty() &&
               fresh.getModel("fresh", key(11)) && fresh.getModel("fresh", key(12)),
               "model merge from a model-indexed root was rejected");
        fresh.verifyPack();
    }
}

void testMergeHooksAndSemanticPreflight() {
    using aowis::dvmdb::MergeStatus;
    using aowis::dvmdb::MergeOptions;
    using aowis::dvmdb::MergeResolution;
    using aowis::dvmdb::ModelRecord;
    {
        TemporaryRepo fixture("merge-candidate-hooks");
        ProjectRepository repository(fixture.directory);
        repository.createBranch("main");
        repository.commitModel("main", {{key(1), ModelRecord{1, {}, value("base")}},
                                        {key(2), ModelRecord{2, {key(1)}, value("dependent")}}});
        repository.createBranch("feature", "main");
        repository.commitModel("main", {{key(1), ModelRecord{1, {}, value("left")}}});
        repository.commitModel("feature", {{key(1), ModelRecord{1, {}, value("right")}}});
        const RevisionId before = repository.head("main");
        const std::uint64_t bytesBefore = repository.packSize();
        PinnedMergeOptions options;
        int resolverCalls = 0;
        int validationCalls = 0;
        options.resolve = [&resolverCalls](const Key& identity, const ModelRecord& base,
                                             const ModelRecord& left, const ModelRecord& right) {
            ++resolverCalls;
            expect(identity == key(1) && base.payload == value("base") &&
                   left.payload == value("left") && right.payload == value("right"),
                   "merge resolver received wrong snapshots");
            return MergeResolution{ModelRecord{1, {}, value("merged")}, {}};
        };
        options.validate = [&validationCalls](const aowis::dvmdb::MergeCandidate& candidate)
            -> std::optional<std::string> {
            ++validationCalls;
            expect(candidate.get(key(1))->payload == value("merged") &&
                   candidate.get(key(2))->references == std::vector<Key>{key(1)} &&
                   !candidate.get(key(99)), "merge snapshot lookup is incorrect");
            std::vector<Key> keys;
            candidate.forEach([&keys](const Key& identity, const ModelRecord&) {
                keys.push_back(identity);
            });
            expect(keys == std::vector<Key>({key(1), key(2)}),
                   "merge snapshot enumeration incorrect");
            return std::string("semantic model constraint failed");
        };
        // Optimistic head checks happen under the merge lock, and no choice
        // verifier may be bypassed by the normal merge publication path.
        options.expectedTargetHead = repository.head("feature");
        bool staleRejected = false;
        try { (void)pinnedMerge(repository, "main", "feature", options); }
        catch (const std::runtime_error&) { staleRejected = true; }
        expect(staleRejected && repository.head("main") == before &&
               repository.packSize() == bytesBefore,
               "stale merge target unexpectedly changed repository");
        options.expectedTargetHead = before;
        options.expectedSourceHead = repository.head("feature");
        options.checkSelections = []() -> std::optional<std::string> {
            return std::string("unused user selections");
        };
        bool unusedRejected = false;
        try { (void)pinnedMerge(repository, "main", "feature", options); }
        catch (const std::runtime_error&) { unusedRejected = true; }
        expect(unusedRejected && repository.head("main") == before &&
               repository.packSize() == bytesBefore,
               "unused selections unexpectedly changed repository");
        options.checkSelections = {};
        resolverCalls = 0;
        validationCalls = 0;
        const aowis::dvmdb::MergeResult rejected = pinnedMerge(repository, "main", "feature", options);
        expect(rejected.status == MergeStatus::Conflicts && !rejected.conflicts.empty() &&
               rejected.conflicts.front().reason == "semantic model constraint failed" &&
               repository.head("main") == before && repository.packSize() == bytesBefore &&
               resolverCalls == 1 && validationCalls == 1,
               "semantic preflight did not reject merge before writing");
        options.validate = [](const aowis::dvmdb::MergeCandidate&) -> std::optional<std::string> {
            return std::nullopt;
        };
        const aowis::dvmdb::MergeResult merged = pinnedMerge(repository, "main", "feature", options);
        expect(merged.status == MergeStatus::Merged && repository.revision(*merged.resultHead).parents.size() == 2 &&
               repository.getModel("main", key(1))->payload == value("merged") &&
               repository.referencing("main", key(1)) == std::vector<Key>{key(2)},
               "resolved typed model merge failed");
        repository.verifyPack();
        options.expectedTargetHead = *merged.resultHead;
        options.checkSelections = []() -> std::optional<std::string> {
            return std::string("stale selection on already merged heads");
        };
        const std::uint64_t alreadyMergedBytes = repository.packSize();
        bool upToDateRejected = false;
        try { (void)pinnedMerge(repository, "main", "feature", options); }
        catch (const std::runtime_error&) { upToDateRejected = true; }
        expect(upToDateRejected && repository.packSize() == alreadyMergedBytes,
               "up-to-date merge bypassed selection verification");

        repository.createBranch("left", "main");
        repository.createBranch("right", "main");
        repository.commitModel("left", {{key(1), ModelRecord{1, {}, value("left2")}}});
        repository.commitModel("right", {{key(1), ModelRecord{1, {}, value("right2")}}});
        const RevisionId unchanged = repository.head("left");
        MergeOptions unresolved;
        unresolved.resolve = [](const Key&, const ModelRecord&, const ModelRecord&,
                                const ModelRecord&) {
            return MergeResolution{std::nullopt, {"diameter_mm", "node_uuid_from"}};
        };
        const aowis::dvmdb::MergeResult conflict = repository.merge("left", "right", unresolved);
        expect(conflict.status == MergeStatus::Conflicts && conflict.conflicts.size() == 1 &&
               conflict.conflicts[0].reason.find("diameter_mm") != std::string::npos &&
               repository.head("left") == unchanged,
               "merge field diagnostics missing or target changed");
    }
}


void testWholeObjectMergeChoices() {
    using aowis::dvmdb::MergeChoiceSide;
    using aowis::dvmdb::MergeOptions;
    using aowis::dvmdb::MergeStatus;
    using aowis::dvmdb::ModelRecord;
    {
        TemporaryRepo temporary("merge-object-choices");
        {
            ProjectRepository repository(temporary.directory);
            repository.createBranch("main");
            repository.commitModel("main", {{key(1), ModelRecord{1, {}, value("base")}},
                                            {key(2), ModelRecord{2, {}, value("other")}}});
            repository.createBranch("choose-source", "main");
            repository.createBranch("choose-target", "main");
            repository.createBranch("other-source", "main");
            repository.commitModel("choose-source", {{key(1), ModelRecord{1, {}, value("left")}}});
            repository.commitModel("other-source", {{key(1), ModelRecord{1, {}, value("right")}}});
            repository.commitModel("choose-target", {{key(1), ModelRecord{1, {}, value("left")}}});
            const RevisionId targetBefore = repository.head("choose-source");
            const RevisionId sourceBefore = repository.head("other-source");
            const std::uint64_t bytesBefore = repository.packSize();
            expect(repository.merge("choose-source", "other-source").status ==
                       MergeStatus::Conflicts && repository.packSize() == bytesBefore,
                   "unresolved concurrent edit was silently selected");
            std::size_t chosenCount = 0;
            PinnedMergeOptions sourceChoice;
            sourceChoice.expectedTargetHead = targetBefore;
            sourceChoice.expectedSourceHead = sourceBefore;
            sourceChoice.chooseConflict = [&chosenCount](const aowis::dvmdb::MergeConflict& conflict)
                -> std::optional<MergeChoiceSide> {
                expect(conflict.key == key(1) && conflict.base && conflict.target && conflict.source,
                       "unexpected whole-object conflict keys");
                ++chosenCount;
                return MergeChoiceSide::Source;
            };
            sourceChoice.checkSelections = [&chosenCount]() -> std::optional<std::string> {
                if (chosenCount != 1) return std::string("object choice unused");
                return std::nullopt;
            };
            const aowis::dvmdb::MergeResult merged =
                pinnedMerge(repository, "choose-source", "other-source", sourceChoice);
            expect(merged.status == MergeStatus::Merged && repository.revision(*merged.resultHead).parents ==
                       std::vector<RevisionId>({targetBefore, sourceBefore}) &&
                   repository.getModel("choose-source", key(1))->payload == value("right") &&
                   repository.getModel("other-source", key(1))->payload == value("right"),
                   "source whole-record choice did not preserve ancestry/branch isolation");

            MergeOptions targetChoice;
            targetChoice.chooseConflict = [](const aowis::dvmdb::MergeConflict& conflict)
                -> std::optional<MergeChoiceSide> {
                return conflict.key == key(1) ? std::optional<MergeChoiceSide>{MergeChoiceSide::Target}
                                              : std::nullopt;
            };
            expect(repository.merge("choose-target", "other-source", targetChoice).status ==
                       MergeStatus::Merged &&
                   repository.getModel("choose-target", key(1))->payload == value("left"),
                   "target whole-record selection failed");

            repository.createBranch("deleted", "main");
            repository.createBranch("modified", "main");
            repository.commitModel("deleted", {{key(1), std::nullopt}});
            repository.commitModel("modified", {{key(1), ModelRecord{1, {}, value("modified")}}});
            const RevisionId deletedHead = repository.head("deleted");
            const std::uint64_t oldBytes = repository.packSize();
            expect(repository.merge("deleted", "modified").status == MergeStatus::Conflicts &&
                   repository.head("deleted") == deletedHead &&
                   repository.packSize() == oldBytes,
                   "delete-versus-modify must require explicit choice");
            MergeOptions deleteChoice;
            deleteChoice.chooseConflict = [](const aowis::dvmdb::MergeConflict& conflict)
                -> std::optional<MergeChoiceSide> {
                if (conflict.key == key(1) && !conflict.target && conflict.source)
                    return MergeChoiceSide::Target;
                return std::nullopt;
            };
            const aowis::dvmdb::MergeResult deletedMerge =
                repository.merge("deleted", "modified", deleteChoice);
            expect(deletedMerge.status == MergeStatus::Merged &&
                   !repository.getModel("deleted", key(1)) &&
                   repository.getModel("modified", key(1))->payload == value("modified"),
                   "explicit delete selection did not retain deletion");
            repository.verifyPack();
        }
        {
            ProjectRepository reopened(temporary.directory);
            expect(!reopened.getModel("deleted", key(1)) &&
                   reopened.getModel("choose-source", key(1))->payload == value("right"),
                   "structural merge choice was lost on reopening");
            reopened.verifyPack();
        }
    }
}

void testMergeCandidateReconciliation() {
    using aowis::dvmdb::MergeOptions;
    using aowis::dvmdb::MergeStatus;
    using aowis::dvmdb::MergeCandidate;
    using aowis::dvmdb::ModelRecord;
    {
        TemporaryRepo temporary("merge-candidate-reconciliation");
        ProjectRepository repository(temporary.directory);
        repository.createBranch("main");
        repository.commitModel("main", {{key(1), ModelRecord{1, {}, value("one")}},
                                       {key(2), ModelRecord{1, {}, value("two")}}});
        repository.createBranch("feature", "main");
        repository.commitModel("main", {{key(1), ModelRecord{1, {}, value("main")}}});
        repository.commitModel("feature", {{key(2), ModelRecord{1, {}, value("feature")}}});
        const RevisionId original = repository.head("main");
        const std::uint64_t bytes = repository.packSize();
        MergeOptions invalid;
        invalid.reconcile = [](const MergeCandidate& candidate,
            const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
            const std::function<std::optional<ModelRecord>(const Key&)>& target,
            const std::function<std::optional<ModelRecord>(const Key&)>& source)
            -> std::optional<std::string> {
            expect(candidate.get(key(1)) && target(key(1)) && source(key(1)) &&
                   target(key(1))->payload == value("main") &&
                   source(key(1))->payload == value("one"),
                   "candidate reconciliation saw incorrect branch snapshots");
            set(key(3), ModelRecord{1, {key(99)}, value("invalid")});
            return std::nullopt;
        };
        expect(repository.merge("main", "feature", invalid).status ==
                   MergeStatus::Conflicts &&
               repository.head("main") == original && repository.packSize() == bytes,
               "merge reconciler published an invalid candidate");
        MergeOptions valid;
        valid.reconcile = [](const MergeCandidate& candidate,
            const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
            const std::function<std::optional<ModelRecord>(const Key&)>&,
            const std::function<std::optional<ModelRecord>(const Key&)>&)
            -> std::optional<std::string> {
            expect(candidate.get(key(2)) && candidate.get(key(2))->payload == value("feature"),
                   "reconciliation did not see source changes");
            set(key(3), ModelRecord{1, {key(1)}, value("combined")});
            return std::nullopt;
        };
        const aowis::dvmdb::MergeResult merged =
            repository.merge("main", "feature", valid);
        expect(merged.status == MergeStatus::Merged &&
               repository.getModel("main", key(3))->payload == value("combined") &&
               repository.referencing("main", key(1)) == std::vector<Key>{key(3)} &&
               repository.getModel("main", key(2))->payload == value("feature") &&
               !repository.getModel("feature", key(3)),
               "candidate reconciliation lost changes or reference indexes");
        repository.verifyPack();
    }
}


// A fast-forward is still a publication: optional domain validation must see
// the complete source model snapshot under the same publication lock.
void testFastForwardSemanticPreflight() {
    using aowis::dvmdb::MergeCandidate;
    using aowis::dvmdb::MergeOptions;
    using aowis::dvmdb::MergeStatus;
    using aowis::dvmdb::ModelChange;
    using aowis::dvmdb::ModelRecord;
    TemporaryRepo test("fast-forward-validation");
    ProjectRepository repository(test.directory);
    repository.createBranch("main");
    repository.commitModel("main", {{key(1), ModelRecord{1, {}, value("baseline")}}});
    repository.createBranch("feature", "main");
    const RevisionId baseline = repository.head("main");
    const RevisionId source = repository.commitModel("feature", {
        {key(1), ModelRecord{1, {}, value("updated")}},
        {key(2), ModelRecord{2, {key(1)}, value("additional")}}});
    const std::uint64_t bytes = repository.packSize();
    int validations = 0;
    PinnedMergeOptions options;
    options.expectedTargetHead = baseline;
    options.expectedSourceHead = source;
    options.validate = [&](const MergeCandidate& candidate) -> std::optional<std::string> {
        ++validations;
        expect(candidate.get(key(1)) && candidate.get(key(1))->payload == value("updated") &&
               candidate.get(key(2)) && candidate.get(key(2))->references == std::vector<Key>{key(1)},
               "fast-forward validator did not see the source tip");
        std::vector<Key> keys;
        candidate.forEach([&](const Key& id, const ModelRecord& record) {
            keys.push_back(id);
            expect(record.payload == (id == key(1) ? value("updated") : value("additional")),
                   "fast-forward traversal returned wrong payload");
        });
        expect(keys == std::vector<Key>({key(1), key(2)}),
               "fast-forward validator did not traverse source records in UUID order");
        return std::string("domain failure");
    };
    const aowis::dvmdb::MergeResult rejected = pinnedMerge(repository, "main", "feature", options);
    expect(rejected.status == MergeStatus::Conflicts && rejected.base == baseline &&
           rejected.conflicts.size() == 1 && rejected.conflicts[0].reason == "domain failure" &&
           validations == 1 && repository.head("main") == baseline &&
           repository.head("feature") == source && repository.packSize() == bytes,
           "invalid fast-forward published a target or wrote objects");

    options.validate = [&](const MergeCandidate&) -> std::optional<std::string> {
        ++validations;
        return std::nullopt;
    };
    options.expectedSourceHead = baseline;
    bool stale = false;
    try { (void)pinnedMerge(repository, "main", "feature", options); }
    catch (const aowis::dvmdb::StorageError&) { stale = true; }
    expect(stale && validations == 1 && repository.head("main") == baseline &&
           repository.packSize() == bytes, "stale source pin reached fast-forward validator");
    options.expectedSourceHead = source;
    options.expectedTargetHead = source;
    stale = false;
    try { (void)pinnedMerge(repository, "main", "feature", options); }
    catch (const aowis::dvmdb::StorageError&) { stale = true; }
    expect(stale && validations == 1 && repository.head("main") == baseline &&
           repository.packSize() == bytes, "stale target pin reached fast-forward validator");
    options.expectedTargetHead = baseline;
    const aowis::dvmdb::MergeResult accepted = pinnedMerge(repository, "main", "feature", options);
    expect(accepted.status == MergeStatus::FastForward && *accepted.resultHead == source &&
           repository.revision(*accepted.resultHead).parents.size() == 1 && validations == 2 &&
           repository.head("main") == source && repository.head("feature") == source &&
           repository.packSize() == bytes,
           "valid fast-forward did not reuse the source revision without pack writes");
    options.expectedTargetHead = source;
    const aowis::dvmdb::MergeResult same = pinnedMerge(repository, "main", "feature", options);
    expect(same.status == MergeStatus::UpToDate && validations == 2 &&
           repository.packSize() == bytes,
           "equal-head no-op unexpectedly revalidated an existing snapshot");
    repository.commitModel("main", {{key(3), ModelRecord{1, {}, value("more")}}});
    const aowis::dvmdb::MergeResult incorporated = repository.merge("main", "feature");
    expect(incorporated.status == MergeStatus::UpToDate, "source ancestor not up-to-date");
    options.expectedTargetHead = repository.head("main");
    const aowis::dvmdb::MergeResult incorporatedValidated =
        pinnedMerge(repository, "main", "feature", options);
    expect(incorporatedValidated.status == MergeStatus::UpToDate && validations == 2,
           "incorporated source unexpectedly invoked validator");
    repository.verifyPack();

    // A typed semantic validator must not accidentally accept an untyped
    // source merely because the merge can reuse its physical revision.
    // A raw history is its own repository (a branch is raw or model-indexed).
    TemporaryRepo rawTest("fast-forward-validation-raw");
    ProjectRepository rawRepository(rawTest.directory);
    rawRepository.createBranch("raw-main");
    rawRepository.commit("raw-main", {{key(19), value("seed")}});
    rawRepository.createBranch("raw-source", "raw-main");
    rawRepository.commit("raw-source", {{key(20), value("raw")}});
    const RevisionId rawBefore = rawRepository.head("raw-main");
    const std::uint64_t rawBytes = rawRepository.packSize();
    MergeOptions rawOptions;
    rawOptions.validate = [](const MergeCandidate&) -> std::optional<std::string> {
        throw std::runtime_error("raw snapshot must not be given to model validator");
    };
    const aowis::dvmdb::MergeResult raw =
        rawRepository.merge("raw-main", "raw-source", rawOptions);
    expect(raw.status == MergeStatus::Conflicts && !raw.conflicts.empty() &&
           rawRepository.head("raw-main") == rawBefore && rawRepository.packSize() == rawBytes,
           "untyped fast-forward silently bypassed model validation");
}

// Fingerprints are only probes: distinct complete SHA-256 hashes must never
// alias even when their compact in-RAM index signatures collide.
void testPhysicalIndexFingerprintCollision() {
    TemporaryRepo test("compact-index-collision");
    std::unordered_map<std::uint32_t, std::string> observed;
    std::string first;
    std::string second;
    for (std::uint64_t attempt = 0; attempt < 1000000; ++attempt) {
        const std::string payload = "fingerprint-collision-" + std::to_string(attempt);
        const Hash hash = aowis::dvmdb::sha256(value(payload));
        std::uint64_t fingerprint64 = 14695981039346656037ULL;
        for (std::uint8_t byte : hash) {
            fingerprint64 = (fingerprint64 ^ byte) * 1099511628211ULL;
        }
        const std::uint32_t fingerprint =
            static_cast<std::uint32_t>(fingerprint64 ^ (fingerprint64 >> 32U));
        const std::pair<std::unordered_map<std::uint32_t, std::string>::iterator, bool> result =
            observed.emplace(fingerprint, payload);
        if (!result.second) {
            first = result.first->second;
            second = payload;
            break;
        }
    }
    expect(!first.empty() && !second.empty() && first != second,
           "deterministic fingerprint collision not found");
    {
        ProjectRepository repository(test.directory);
        repository.createBranch("main");
        repository.commit("main", {{key(1), value(first)}, {key(2), value(second)}});
        expect(repository.get("main", key(1)) == value(first), "first colliding hash unavailable");
        expect(repository.get("main", key(2)) == value(second), "second colliding hash unavailable");
        repository.verifyPack();
    }
    {
        ProjectRepository repository(test.directory);
        expect(repository.get("main", key(1)) == value(first), "reopen lost first colliding hash");
        expect(repository.get("main", key(2)) == value(second), "reopen lost second colliding hash");
        repository.verifyPack();
    }
    std::filesystem::remove(test.directory / "objects.idx");
    {
        ProjectRepository repository(test.directory);
        expect(repository.get("main", key(1)) == value(first), "rebuild lost first colliding hash");
        expect(repository.get("main", key(2)) == value(second), "rebuild lost second colliding hash");
        repository.verifyPack();
    }
}

int main() {
    try {
        testEmptyAndBranches();
        testCompressedTreeAndNoOp();
        testRecoveryAndCorruption();
        testPersistentPackIndex();
        testPhysicalIndexFingerprintCollision();
        testCompactionAndRetainedHistory();
        testCompactionRequiresExclusiveLifetime();
        testMixedEditsAgainstReference();
        testBatchCanonicalRootsAndBranchIndependence();
        testMultipleConnections();
        testTypedModelRecordsAndReferences();
        testTypedModelConcurrentHandles();
        testDurableReverseReferences();
        testModelExpectedBranchHead();
        testMoveAwareModelAndRawCommits();
        testHistoricalRevisionApis();
        testThreeWayBranchMerges();
        testMergeConflictsAndFastForward();
        testIndexedModelMerges();
        testMergeHooksAndSemanticPreflight();
        testFastForwardSemanticPreflight();
        testWholeObjectMergeChoices();
        testMergeCandidateReconciliation();
        std::cout << "AOWIS native storage tests: passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "AOWIS native storage tests: FAILED: " << failure.what() << '\n';
        return 1;
    }
}
