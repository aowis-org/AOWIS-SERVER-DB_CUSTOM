#include "aowis/native/native_store.h"
#include "aowis/native/block_store_prototype.h"
#include <fcntl.h>
#include <sys/file.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Difference;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::Repository;
using aowis::native::Revision;

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
        ("aowis-native-test-" + std::string(name) + "-" + std::to_string(::getpid()) +
         "-" + std::to_string(clock));
}

struct TemporaryRepo {
    explicit TemporaryRepo(std::string_view name) : directory(temporaryPath(name)) {}
    ~TemporaryRepo() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    std::filesystem::path directory;
};

void testEmptyAndBranches() {
    TemporaryRepo test("branches");
    Hash baseline{};
    {
        Repository repository(test.directory);
        repository.createBranch("main");
        expect(repository.head("main").root == Hash{}, "initial tree must be empty");
        repository.commit("main", {{key(1), value("one")}, {key(2), value("two")}});
        baseline = repository.head("main").id;
        repository.createBranch("feature", "main");
        expect(repository.head("feature").id == baseline, "branch must share initial revision");
        const Revision modified = repository.commit("feature", {
            {key(1), value("revised")}, {key(2), std::nullopt}, {key(3), value("three")}
        });
        expect(modified.parents.size() == 1 && modified.parents[0] == baseline,
               "revision must link to its parent");
        expect(repository.get("main", key(1)) == value("one"), "branch isolation failed");
        expect(!repository.get("main", key(3)), "uncommitted branch object leaked");
        expect(repository.get("feature", key(1)) == value("revised"), "modified object missing");
        expect(!repository.get("feature", key(2)), "deletion failed");
        const std::vector<Difference> changes = repository.diff("main", "feature");
        expect(changes.size() == 3, "diff failed to find insert, update and delete");
        expect(changes[0].before && changes[0].after, "modification misclassified");
        expect(changes[1].before && !changes[1].after, "deletion misclassified");
        expect(!changes[2].before && changes[2].after, "addition misclassified");
        expect(repository.scan("main").size() == 2, "full scan failed");
        expect(repository.scan("feature").size() == 2, "modified scan failed");
        expect(repository.commit("feature", {{key(100), std::nullopt}}).root == modified.root,
               "deleting absent key should not change tree");
        try {
            repository.createBranch("../escape");
            throw std::runtime_error("invalid branch name accepted");
        } catch (const std::runtime_error& failure) {
            expect(std::string(failure.what()) == "invalid branch name", "unexpected branch validation");
        }
    }
    {
        Repository reopened(test.directory);
        expect(reopened.head("main").id == baseline, "head was not durable");
        expect(reopened.get("feature", key(1)) == value("revised"), "data not durable");
        expect(reopened.diff("main", "feature").size() == 3, "diff not durable");
    }
}

void testCompressedTreeAndNoOp() {
    TemporaryRepo test("compression");
    Repository repository(test.directory);
    repository.createBranch("main");
    std::vector<Change> changes;
    for (std::uint64_t i = 0; i < 500; ++i) {
        changes.push_back(Change{key(i * 281474976710677ULL), value("object" + std::to_string(i))});
    }
    repository.commit("main", changes);
    repository.createBranch("base", "main");
    expect(repository.scan("main").size() == 500, "inserts not preserved");
    repository.commit("main", {{key(0), std::nullopt}, {key(281474976710677ULL), std::nullopt}});
    expect(repository.diff("base", "main").size() == 2, "sparse diff failed");
    std::vector<Change> restore{{key(0), value("object0")},
                                {key(281474976710677ULL), value("object1")}};
    const Revision restored = repository.commit("main", restore);
    expect(restored.root == repository.head("base").root,
           "restoring previous content must restore identical tree hash");
    expect(repository.diff("base", "main").empty(), "identical roots should have empty diff");
    const std::uint64_t before = repository.packSize();
    repository.commit("main", {{key(0), value("object0")}});
    expect(repository.packSize() > before, "revision should be recorded");
}

void testRecoveryAndCorruption() {
    TemporaryRepo test("recovery");
    {
        Repository repository(test.directory);
        repository.createBranch("main");
        repository.commit("main", {{key(5), value("still-here")}});
    }
    const std::filesystem::path pack = test.directory / "objects.pack";
    {
        std::ofstream out(pack, std::ios::app | std::ios::binary);
        out << "AOB"; // torn, incomplete record
    }
    {
        Repository repository(test.directory);
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
        Repository repository(test.directory);
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
    Hash mainBefore{};
    Hash mainAfter{};
    Hash featureBefore{};
    std::uint64_t withOrphan = 0;
    const Bytes orphan = value("unpublished-private-payload");
    {
        Repository repository(test.directory);
        repository.createBranch("main");
        repository.commit("main", {{key(1), value("old")}, {key(2), value("stable")}});
        repository.createBranch("feature", "main");
        featureBefore = repository.head("feature").id;
        mainBefore = repository.head("main").id;
        repository.commit("main", {{key(1), value("new")}, {key(3), value("third")}});
        mainAfter = repository.head("main").id;
        repository.verifyPack();
    }
    {
        const std::filesystem::path pack = test.directory / "objects.pack";
        const Hash orphanHash = aowis::native::sha256(orphan);
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
        Repository repository(test.directory);
        repository.verifyPack();
        withOrphan = repository.objectCount();
        expect(repository.head("main").id == mainAfter &&
               repository.head("feature").id == featureBefore,
               "unpublished object changed retained heads");
        const aowis::native::CompactionStats stats = repository.compact();
        expect(stats.objectsBefore == withOrphan && stats.objectsAfter + 1 == withOrphan,
               "compaction did not remove exactly the unreachable object");
        expect(stats.packBytesBefore == stats.packBytesAfter + 45 + orphan.size(),
               "pack compaction did not recover orphan bytes");
        expect(stats.indexBytesBefore == stats.indexBytesAfter + 52,
               "index compaction did not recover orphan entry");
        expect(repository.head("main").id == mainAfter &&
               repository.head("feature").id == mainBefore,
               "compaction changed revision identity or branch ancestry");
        expect(repository.get("main", key(1)) == value("new") &&
               repository.get("feature", key(1)) == value("old"),
               "compaction lost a retained historical value");
        repository.verifyPack();
        const aowis::native::CompactionStats repeated = repository.compact();
        expect(repeated.objectsAfter == stats.objectsAfter &&
               repeated.packBytesAfter == stats.packBytesAfter,
               "repeating compaction should not change a clean repository");
        repository.commit("main", {{key(4), value("after-compaction")}});
        mainAfter = repository.head("main").id;
        repository.verifyPack();
    }
    {
        Repository reopened(test.directory);
        expect(reopened.head("main").id == mainAfter &&
               reopened.head("feature").id == featureBefore,
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
        Repository reopened(test.directory);
        expect(reopened.head("main").id == mainAfter,
               "missing compacted index was not rebuilt");
        reopened.verifyPack();
    }
}

void testCompactionRequiresExclusiveLifetime() {
    TemporaryRepo test("compaction-lock");
    Repository first(test.directory);
    first.createBranch("main");
    first.commit("main", {{key(9), value("protected")}});
    {
        Repository second(test.directory);
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
    Repository repository(test.directory);
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
        Repository repository(test.directory);
        repository.createBranch("main");
        std::vector<Change> initial;
        for (std::uint64_t i = 0; i < 400; ++i) {
            initial.push_back({scatteredKey(i), value("object-" + std::to_string(i))});
        }
        repository.commit("main", initial);
        root = repository.head("main").root;
        objectCount = repository.objectCount();
        packBytes = repository.packSize();
        repository.verifyPack();
    }
    const std::filesystem::path index = test.directory / "objects.idx";
    expect(std::filesystem::file_size(index) == 8 + objectCount * 52,
           "persistent index contains missing or duplicate entries");
    {
        Repository reopened(test.directory);
        expect(reopened.head("main").root == root, "indexed reopen changed root");
        reopened.verifyPack();
    }
    // A missing index must be reconstructed from the pack, not erase data.
    std::filesystem::remove(index);
    {
        Repository rebuilt(test.directory);
        expect(rebuilt.head("main").root == root && rebuilt.objectCount() == objectCount,
               "missing sidecar was not rebuilt");
        rebuilt.verifyPack();
    }
    // An incomplete trailing index entry should be repaired without changing
    // already indexed objects or requiring a new revision.
    std::filesystem::resize_file(index, std::filesystem::file_size(index) - 11);
    {
        Repository repaired(test.directory);
        expect(repaired.head("main").root == root && repaired.packSize() == packBytes,
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
        Repository rebuilt(test.directory);
        expect(rebuilt.head("main").root == root, "corrupt index damaged revision");
        rebuilt.verifyPack();
    }
    // A valid unindexed pack suffix (e.g. interruption during index publish)
    // is recovered by hashing only that suffix.
    {
        Repository repository(test.directory);
        repository.commit("main", {{scatteredKey(401), value("new-value")}});
        root = repository.head("main").root;
        objectCount = repository.objectCount();
    }
    std::filesystem::resize_file(index, std::filesystem::file_size(index) - 52);
    {
        Repository repaired(test.directory);
        expect(repaired.head("main").root == root && repaired.objectCount() == objectCount,
               "valid pack suffix was lost due to truncated index");
        repaired.verifyPack();
    }
}

void testBatchCanonicalRootsAndBranchIndependence() {
    TemporaryRepo test("bulk-canonical");
    Repository repository(test.directory);
    repository.createBranch("bulk");
    repository.createBranch("reverse");
    repository.createBranch("small-batches");

    std::vector<Change> initial;
    for (std::uint64_t i = 0; i < 1600; ++i) {
        initial.push_back({scatteredKey(i), value("value-" + std::to_string(i))});
    }
    const Revision bulk = repository.commit("bulk", initial);
    std::reverse(initial.begin(), initial.end());
    repository.commit("reverse", initial);
    for (std::size_t i = 0; i < initial.size(); i += 37) {
        const std::size_t last = std::min(initial.size(), i + 37);
        repository.commit("small-batches", std::vector<Change>(initial.begin() +
                          static_cast<std::ptrdiff_t>(i), initial.begin() +
                          static_cast<std::ptrdiff_t>(last)));
    }
    expect(bulk.root == repository.head("reverse").root, "reverse insertion changed tree hash");
    expect(bulk.root == repository.head("small-batches").root, "batch shape changed tree hash");

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
    expect(repository.head("baseline").root == bulk.root, "bulk modifications affected base branch");
    std::vector<Change> finalEdits;
    for (std::uint64_t index = 0; index < 2550; ++index) {
        const Key target = scatteredKey(index);
        if (index < 1600) finalEdits.push_back({target, value("value-" + std::to_string(index))});
        else finalEdits.push_back({target, std::nullopt});
    }
    repository.commit("bulk", finalEdits);
    expect(repository.head("bulk").root == bulk.root, "restored bulk tree root is not canonical");
    expect(repository.diff("bulk", "baseline").empty(), "equivalent trees have a nonempty diff");
}


void testTypedModelRecordsAndReferences() {
    TemporaryRepo test("model-references");
    Repository repository(test.directory);
    repository.createBranch("main");
    const Key nodeA = key(11);
    const Key nodeB = key(12);
    const Key pipe = key(13);
    const aowis::native::ModelRecord junction{1, {}, value("junction-canonical-codec-bytes")};
    const aowis::native::ModelRecord link{4, {nodeA, nodeB}, value("pipe-canonical-codec-bytes")};
    const Bytes serialized = aowis::native::encodeModelRecord(link);
    const aowis::native::ModelRecord unpacked = aowis::native::decodeModelRecord(serialized);
    expect(unpacked.kind == link.kind && unpacked.references == link.references &&
           unpacked.payload == link.payload, "model envelope changed codec bytes");
    const aowis::native::ModelRecord unordered{4, {nodeB, nodeA, nodeB}, link.payload};
    expect(aowis::native::encodeModelRecord(unordered) == serialized,
           "reference set order changed canonical envelope hash");
    for (std::size_t length = 0; length < serialized.size(); ++length) {
        bool rejected = false;
        try {
            aowis::native::decodeModelRecord(Bytes(serialized.begin(), serialized.begin() +
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
    const Hash savedRoot = repository.head("main").root;
    const std::uint64_t savedPackSize = repository.packSize();
    auto invalid = [&](const std::vector<aowis::native::ModelChange>& changes) {
        bool rejected = false;
        try { repository.commitModel("main", changes); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "invalid model transaction accepted");
        expect(repository.head("main").root == savedRoot, "rejected model transaction changed head");
        expect(repository.packSize() == savedPackSize, "rejected transaction appended pack data");
    };
    invalid({{nodeA, std::nullopt}});
    invalid({{key(14), aowis::native::ModelRecord{4, {key(99)}, value("missing")}}});
    invalid({{nodeA, std::nullopt}, {pipe, link}});
    invalid({{key(20), aowis::native::ModelRecord{1, {key(20)}, value("self")}}});
    invalid({{key(21), junction}, {key(21), junction}});
    invalid({{Key{}, junction}});
    // Removing both an edge and one endpoint is legal in one commit.
    repository.commitModel("main", {{pipe, std::nullopt}, {nodeA, std::nullopt}});
    expect(!repository.getModel("main", pipe), "pipe deletion not persisted");
    expect(!repository.getModel("main", nodeA), "node deletion not persisted");
    expect(repository.getModel("base", pipe)->payload == link.payload,
           "base branch lost pipe");
    {
        Repository reopened(test.directory);
        expect(reopened.getModel("base", pipe)->references == link.references,
               "model references not durable");
        expect(!reopened.getModel("main", nodeA), "model deletion not durable");
    }
}

void testTypedModelConcurrentHandles() {
    TemporaryRepo test("model-concurrent");
    Repository first(test.directory);
    first.createBranch("main");
    first.commitModel("main", {{key(1), aowis::native::ModelRecord{1, {}, value("node")}}});
    Repository second(test.directory);
    second.commitModel("main", {{key(2), aowis::native::ModelRecord{4, {key(1)}, value("pipe")}}});
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
    const aowis::native::ModelRecord node{1, {}, value("junction")};
    const aowis::native::ModelRecord ab{4, {nodeA, nodeB}, value("pipe-ab")};
    const aowis::native::ModelRecord ac{4, {nodeA, nodeC}, value("pipe-ac")};
    const std::vector<aowis::native::ModelChange> initial{
        {pipeA, ab}, {pipeB, ab}, {nodeA, node}, {nodeB, node}, {nodeC, node}};
    Hash firstIndex{};
    {
        Repository repository(test.directory);
        repository.createBranch("main");
        repository.createBranch("sequential");
        repository.commitModel("main", initial);
        firstIndex = repository.head("main").referencesRoot;
        expect(repository.head("main").modelIndexed && firstIndex != Hash{},
               "typed revision did not publish reverse-reference index");
        expect(repository.referencingModel("main", nodeA) == std::vector<Key>({pipeA, pipeB}),
               "incoming references are not sorted and complete");
        expect(repository.referencingModel("main", nodeB) == std::vector<Key>({pipeA, pipeB}),
               "incorrect references for second endpoint");
        expect(repository.referencingModel("main", nodeC).empty(),
               "unreferenced node should have no index entry");
        bool rawRejected = false;
        try { repository.commit("main", {{key(250), value("unchecked")}}); }
        catch (const std::runtime_error&) { rawRejected = true; }
        expect(rawRejected, "raw write bypassed model reference validation");
        repository.commitModel("sequential", {{nodeA, node}, {nodeB, node}, {nodeC, node}});
        repository.commitModel("sequential", {{pipeB, ab}});
        repository.commitModel("sequential", {{pipeA, ab}});
        expect(repository.head("sequential").root == repository.head("main").root,
               "reference-index changes altered primary canonical model root");
        expect(repository.head("sequential").referencesRoot == firstIndex,
               "incoming index depends on insertion order / batch boundaries");

        repository.createBranch("original", "main");
        repository.commitModel("main", {{pipeA, ac}});
        expect(repository.referencingModel("main", nodeB) == std::vector<Key>({pipeB}),
               "rewiring did not remove old incoming edge");
        expect(repository.referencingModel("main", nodeC) == std::vector<Key>({pipeA}),
               "rewiring did not create new incoming edge");
        expect(repository.referencingModel("original", nodeB) == std::vector<Key>({pipeA, pipeB}),
               "branch reference index was mutated in place");
        const Hash protectedHead = repository.head("main").id;
        const std::uint64_t protectedBytes = repository.packSize();
        bool rejected = false;
        try { repository.commitModel("main", {{nodeB, std::nullopt}}); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected && protectedHead == repository.head("main").id &&
               protectedBytes == repository.packSize(),
               "rejected referenced-node deletion wrote data or advanced revision");
        repository.commitModel("main", {{pipeB, std::nullopt}, {nodeB, std::nullopt}});
        expect(repository.referencingModel("main", nodeB).empty(),
               "deleted endpoint still appears in reverse index");
        expect(repository.referencingModel("main", nodeA) == std::vector<Key>({pipeA}),
               "unrelated surviving reference disappeared");
        // Reverse-edge insertion/deletion can return to an identical root.
        repository.commitModel("main", {{nodeB, node}, {pipeB, ab}, {pipeA, ab}});
        expect(repository.head("main").root == repository.head("original").root &&
               repository.head("main").referencesRoot == firstIndex,
               "restored model and reference-index roots are not deterministic");

        repository.createBranch("raw");
        repository.commit("raw", {{key(17), value("plain-data")}});
        rejected = false;
        try { repository.commitModel("raw", {{nodeA, node}}); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "model commit was allowed onto existing unindexed raw data");
    }
    {
        Repository reopened(test.directory);
        expect(reopened.head("original").referencesRoot == firstIndex,
               "reopened revision lost reverse-reference root");
        expect(reopened.referencingModel("main", nodeA) == std::vector<Key>({pipeA, pipeB}),
               "reopened reverse reference index returned incorrect IDs");
    }
}

void testMultipleConnections() {
    TemporaryRepo test("process-index");
    Repository first(test.directory);
    first.createBranch("main");
    Repository second(test.directory);
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
void testLargeBlockCommitAndPublicationFailure() {
    using aowis::native::PhysicalStore;
    TemporaryRepo test("bounded-staging");
    const std::filesystem::path loosePath = test.directory / "loose";
    const std::filesystem::path blockPath = test.directory / "blocks";
    std::vector<Change> changes;
    for (std::uint64_t i = 1; i <= 48; ++i) {
        Bytes bytes(200000);
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            bytes[j] = static_cast<std::uint8_t>((i * 37U + j * 13U + (j >> 7U)) & 255U);
        }
        for (unsigned n = 0; n < 8; ++n) {
            bytes[n] = static_cast<std::uint8_t>(i >> (n * 8U));
        }
        changes.push_back(Change{key(i), std::move(bytes)});
    }
    Hash expectedRoot{};
    {
        Repository loose(loosePath);
        Repository blocks(blockPath, PhysicalStore::ExperimentalBlocks);
        loose.createBranch("main");
        blocks.createBranch("main");
        const Revision lhs = loose.commit("main", changes);
        const Revision rhs = blocks.commit("main", changes);
        expect(lhs.id == rhs.id && lhs.root == rhs.root,
               "bounded block staging changed canonical revision identity");
        expectedRoot = rhs.root;
        expect(blocks.get("main", key(48)) == changes[47].value,
               "large block commit lost a published value");
        const std::uint64_t before = blocks.packSize();
        // The second commit again exceeds the staging window. Its objects
        // can be durable, but the branch head must not be published.
        std::vector<Change> replacement = changes;
        for (Change& entry : replacement) {
            (*entry.value)[15] ^= 0x7fU;
        }
        const std::filesystem::path obstruction = blockPath / "refs" / "heads" / ".main.tmp";
        std::filesystem::create_directory(obstruction);
        bool rejected = false;
        try { blocks.commit("main", replacement); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected && blocks.head("main").root == expectedRoot,
               "failed large-commit ref publication changed the branch head");
        expect(blocks.packSize() > before,
               "failed large-commit test did not write any unreachable data");
        expect(blocks.get("main", key(48)) == changes[47].value,
               "failed large-commit publication changed old model content");
        std::filesystem::remove(obstruction);
        blocks.verifyPack();
    }
    {
        Repository blocks(blockPath, PhysicalStore::ExperimentalBlocks);
        expect(blocks.head("main").root == expectedRoot &&
               blocks.get("main", key(1)) == changes[0].value,
               "reopened large block repository lost committed history");
        blocks.commit("main", {{key(49), value("subsequent-commit")}});
        expect(blocks.get("main", key(49)) == value("subsequent-commit"),
               "block repository cannot continue after orphaned writes");
        blocks.verifyPack();
    }
}

// Stage-16: a block generation is published only after every reachable
// revision, both Merkle roots and their immutable payloads are retained.
void testBlockCompactionRetentionAndRecovery() {
    using aowis::native::ModelRecord;
    using aowis::native::ModelChange;
    using aowis::native::PhysicalStore;
    TemporaryRepo test("block-generation-compaction");
    const std::filesystem::path blocks = test.directory / "blocks";
    const Key nodeA = key(200), nodeB = key(201), pipe = key(202);
    const ModelRecord junction{1, {}, value("junction")};
    const ModelRecord link{4, {nodeA, nodeB}, value("pipe")};
    Hash mainBefore{};
    Hash featureBefore{};
    {
        Repository repository(test.directory, PhysicalStore::ExperimentalBlocks);
        repository.createBranch("main");
        repository.commitModel("main", {{nodeA, junction}, {nodeB, junction}, {pipe, link}});
        mainBefore = repository.head("main").id;
        repository.createBranch("feature", "main");
        repository.commitModel("feature", {{pipe, ModelRecord{4, {nodeA}, value("rewired")}}});
        featureBefore = repository.head("feature").id;
        const std::filesystem::path obstructed =
            test.directory / "refs" / "heads" / ".main.tmp";
        std::filesystem::create_directory(obstructed);
        bool failed = false;
        try {
            repository.commitModel("main", {
                {key(299), ModelRecord{1, {}, value("orphan-after-failed-ref")}}
            });
        } catch (const std::runtime_error&) { failed = true; }
        std::filesystem::remove(obstructed);
        expect(failed && repository.head("main").id == mainBefore,
               "test did not create unreachable content safely");
        const std::uint64_t objectsBefore = repository.objectCount();
        const aowis::native::CompactionStats stats = repository.compact();
        expect(stats.objectsBefore == objectsBefore &&
               stats.objectsAfter < stats.objectsBefore &&
               stats.packBytesAfter < stats.packBytesBefore &&
               stats.indexBytesAfter < stats.indexBytesBefore,
               "block compaction did not reclaim orphan objects");
        expect(repository.head("main").id == mainBefore &&
               repository.head("feature").id == featureBefore,
               "block compaction altered branch heads");
        expect(repository.getModel("main", pipe)->payload == link.payload &&
               repository.getModel("feature", pipe)->payload == value("rewired") &&
               repository.referencingModel("main", nodeB) == std::vector<Key>{pipe} &&
               repository.referencingModel("feature", nodeB).empty(),
               "block compaction lost historical model or reference data");
        repository.verifyPack();
        const aowis::native::CompactionStats again = repository.compact();
        expect(again.objectsBefore == again.objectsAfter,
               "repeat block compaction removed retained content");
        repository.commitModel("main", {{key(300), junction}});
        repository.verifyPack();
    }
    const std::filesystem::path active =
        aowis::native::BlockStorePrototype::activeDirectory(blocks);
    expect(active != blocks && std::filesystem::exists(blocks / "candidate.active"),
           "block compaction did not switch active generation");
    std::filesystem::remove(active / "candidate.idx");
    {
        Repository reopened(test.directory, PhysicalStore::ExperimentalBlocks);
        expect(reopened.head("feature").id == featureBefore &&
               reopened.getModel("feature", pipe)->payload == value("rewired") &&
               reopened.getModel("main", key(300)).has_value(),
               "reopened compacted blocks lost revision history or later edits");
        reopened.verifyPack();
    }
    // A tampered active-generation pointer must never fall back to the old
    // pre-compaction pack, which could silently resurrect obsolete objects.
    {
        std::ofstream pointer(blocks / "candidate.active", std::ios::trunc);
        pointer << "AOBGEN1 ../../../../evil\n";
    }
    bool badPointerRejected = false;
    try { Repository broken(test.directory, PhysicalStore::ExperimentalBlocks); }
    catch (const std::runtime_error&) { badPointerRejected = true; }
    expect(badPointerRejected, "malformed block generation pointer was accepted");
}

void testBlockCompactionRequiresExclusiveLifetime() {
    using aowis::native::PhysicalStore;
    TemporaryRepo test("block-generation-lock");
    Repository first(test.directory, PhysicalStore::ExperimentalBlocks);
    first.createBranch("main");
    // BlockStorePrototype rejects concurrent handles; the lifetime lock must
    // also prevent future multi-reader implementations from compacting behind
    // a second process's descriptor.
    const int extra = ::open((test.directory / "maintenance.lock").c_str(), O_RDWR);
    expect(extra >= 0, "cannot open extra maintenance lock");
    expect(::flock(extra, LOCK_SH | LOCK_NB) == 0, "cannot acquire extra lifetime lock");
    bool rejected = false;
    try { (void)first.compact(); }
    catch (const std::runtime_error&) { rejected = true; }
    ::flock(extra, LOCK_UN);
    ::close(extra);
    expect(rejected, "block compaction ignored another maintenance handle");
    (void)first.compact();
    first.verifyPack();
}

void testBlockCompactionPublicationFailure() {
    using aowis::native::PhysicalStore;
    TemporaryRepo test("block-generation-publish-failure");
    Hash original{};
    {
        Repository repo(test.directory, PhysicalStore::ExperimentalBlocks);
        repo.createBranch("main");
        repo.commit("main", {{key(1), value("keep")}});
        original = repo.head("main").id;
        // Publishing a pointer must fail before the committed generation can
        // become active. The old branch is still readable and writable.
        std::filesystem::create_directory(test.directory / "blocks" /
                                         "candidate.active.pending");
        bool failed = false;
        try { (void)repo.compact(); }
        catch (const std::runtime_error&) { failed = true; }
        expect(failed && repo.head("main").id == original &&
               repo.get("main", key(1)) == value("keep") &&
               !std::filesystem::exists(test.directory / "blocks" / "candidate.active"),
               "failed pointer publication changed authoritative block store");
        std::filesystem::remove(test.directory / "blocks" / "candidate.active.pending");
        repo.verifyPack();
        (void)repo.compact();
        expect(repo.head("main").id == original, "retry changed committed history");
    }
    Repository reopened(test.directory, PhysicalStore::ExperimentalBlocks);
    expect(reopened.get("main", key(1)) == value("keep"),
           "retry after failed publication lost committed model");
}

// Both backends must publish the EXACT SAME content-addressed revision DAG.
// A failed ref rename after a durable block batch is allowed to leave orphan
// objects, but must not advance the branch head.
void testExperimentalBlockRevisionPublication() {
    using aowis::native::PhysicalStore;
    using aowis::native::ModelRecord;
    using aowis::native::ModelChange;
    TemporaryRepo test("block-revisions");
    const std::filesystem::path loosePath = test.directory / "loose";
    const std::filesystem::path blocksPath = test.directory / "blocks";
    const Key nodeA = key(20), nodeB = key(21), pipe = key(22);
    const ModelRecord junction{1, {}, value("node-v5-opaque")};
    const ModelRecord link{4, {nodeA, nodeB}, value("pipe-v5-opaque")};
    const std::vector<ModelChange> initial{
        {pipe, link}, {nodeA, junction}, {nodeB, junction}
    };
    Hash original{};
    Hash afterEdit{};
    {
        Repository loose(loosePath);
        Repository blocks(blocksPath, PhysicalStore::ExperimentalBlocks);
        loose.createBranch("main");
        blocks.createBranch("main");
        expect(loose.head("main").id == blocks.head("main").id,
               "empty revision differs between pack layouts");
        loose.commitModel("main", initial);
        blocks.commitModel("main", initial);
        const Revision first = loose.head("main");
        expect(first.id == blocks.head("main").id &&
               first.root == blocks.head("main").root &&
               first.referencesRoot == blocks.head("main").referencesRoot,
               "initial model roots or revision identity differ between stores");
        original = first.id;
        loose.createBranch("feature", "main");
        blocks.createBranch("feature", "main");
        const std::vector<ModelChange> update{
            {pipe, ModelRecord{4, {nodeA}, value("pipe-moved")}}
        };
        loose.commitModel("feature", update);
        blocks.commitModel("feature", update);
        afterEdit = loose.head("feature").id;
        expect(afterEdit == blocks.head("feature").id,
               "experimental block revision differs from Stage-7 revision");
        expect(loose.referencingModel("feature", nodeB) ==
               blocks.referencingModel("feature", nodeB),
               "reverse-reference index differs between physical stores");
        const std::vector<aowis::native::Difference> left = loose.diff("main", "feature");
        const std::vector<aowis::native::Difference> right = blocks.diff("main", "feature");
        expect(left.size() == 1 && right.size() == 1 &&
               left[0].key == right[0].key && left[0].before == right[0].before &&
               left[0].after == right[0].after, "model diff differs by physical store");
        expect(blocks.getModel("main", pipe)->payload == link.payload,
               "branch cloning lost the original model");
        expect(blocks.getModel("feature", pipe)->payload == value("pipe-moved"),
               "feature branch model update was lost");
        // Failed validation must never change the branch root or append data.
        const std::uint64_t beforeBad = blocks.packSize();
        bool invalidRejected = false;
        try { blocks.commitModel("main", {{nodeA, std::nullopt}}); }
        catch (const std::runtime_error&) { invalidRejected = true; }
        expect(invalidRejected && blocks.head("main").id == original &&
               blocks.packSize() == beforeBad, "failed validation published block data");
        // Force publication to fail AFTER the block store has committed objects.
        const std::filesystem::path obstruction = blocksPath / "refs" / "heads" / ".main.tmp";
        std::filesystem::create_directory(obstruction);
        bool publicationRejected = false;
        try {
            blocks.commitModel("main", {
                {key(90), ModelRecord{1, {}, value("unpublished-object")}}
            });
        } catch (const std::runtime_error&) { publicationRejected = true; }
        expect(publicationRejected && blocks.head("main").id == original,
               "failed ref publication changed the block branch head");
        std::filesystem::remove(obstruction);
        blocks.verifyPack();
        expect(blocks.packSize() > beforeBad,
               "test did not durably write objects before refused ref publication");
        // Experiment must not accidentally open as a Stage-7 repository.
        bool rejectedWrongStorage = false;
        try { Repository wrong(blocksPath); }
        catch (const std::runtime_error&) { rejectedWrongStorage = true; }
        expect(rejectedWrongStorage, "block repository opened as legacy Stage-7 pack");
    }
    {
        Repository reopened(blocksPath, PhysicalStore::ExperimentalBlocks);
        expect(reopened.head("main").id == original &&
               reopened.head("feature").id == afterEdit,
               "reopening after failed branch publish changed committed refs");
        expect(reopened.referencingModel("feature", nodeB).empty(),
               "reopened block reverse refs differ");
        reopened.commitModel("main", {{key(91), ModelRecord{1, {}, value("published")}}});
        expect(reopened.head("main").id != original,
               "valid block-backed commit did not advance branch head");
        reopened.verifyPack();
        bool concurrentRejected = false;
        try { Repository another(blocksPath, PhysicalStore::ExperimentalBlocks); }
        catch (const std::runtime_error&) { concurrentRejected = true; }
        expect(concurrentRejected, "block backend failed to enforce single-handle limitation");
    }
    // A torn, *unpublished* pack suffix must be discarded on startup.
    const std::filesystem::path pack = blocksPath / "blocks" / "candidate.pack";
    const std::uint64_t committed = std::filesystem::file_size(pack);
    {
        std::ofstream tail(pack, std::ios::app | std::ios::binary);
        tail << "torn-uncommitted-tail";
    }
    {
        Repository recovered(blocksPath, PhysicalStore::ExperimentalBlocks);
        expect(std::filesystem::file_size(pack) == committed,
               "uncommitted block tail survived reopening");
        recovered.verifyPack();
    }
    bool rejectedWrongStorage = false;
    try { Repository wrong(loosePath, PhysicalStore::ExperimentalBlocks); }
    catch (const std::runtime_error&) { rejectedWrongStorage = true; }
    expect(rejectedWrongStorage, "existing Stage-7 repository opened in block mode");
    // Losing the authoritative committed-length marker must fail closed. It
    // cannot be treated as a first-time activation of an offline candidate.
    std::filesystem::remove(blocksPath / "blocks" / "candidate.current");
    bool missingMarkerRejected = false;
    try { Repository wrong(blocksPath, PhysicalStore::ExperimentalBlocks); }
    catch (const std::runtime_error&) { missingMarkerRejected = true; }
    expect(missingMarkerRejected, "missing block commit marker was silently reconstructed");
}

void testModelExpectedBranchHead() {
    for (const aowis::native::PhysicalStore physical :
         {aowis::native::PhysicalStore::Stage7Pack,
          aowis::native::PhysicalStore::ExperimentalBlocks}) {
        TemporaryRepo test("expected-model-head");
        Repository repository(test.directory, physical);
        repository.createBranch("main");
        const aowis::native::ModelRecord initial{1, {}, value("first")};
        const Revision first = repository.commitModel("main", {{key(1), initial}});
        const Revision second = repository.commitModel("main", {
            {key(1), aowis::native::ModelRecord{1, {}, value("second")}}
        }, first.id);
        expect(second.id != first.id, "model update failed to move branch head");
        const std::uint64_t before = repository.packSize();
        bool rejected = false;
        try {
            repository.commitModel("main", {
                {key(1), aowis::native::ModelRecord{1, {}, value("stale")}}
            }, first.id);
        } catch (const std::runtime_error&) { rejected = true; }
        expect(rejected && repository.head("main").id == second.id &&
               repository.packSize() == before,
               "stale model commit changed the ref or persistent data");
        const std::optional<aowis::native::ModelRecord> restored =
            repository.getModel("main", key(1));
        expect(restored.has_value() && restored->payload == value("second"),
               "stale model commit replaced committed content");
    }
}

} // namespace

int main() {
    try {
        testEmptyAndBranches();
        testCompressedTreeAndNoOp();
        testRecoveryAndCorruption();
        testPersistentPackIndex();
        testCompactionAndRetainedHistory();
        testCompactionRequiresExclusiveLifetime();
        testBlockCompactionRetentionAndRecovery();
        testBlockCompactionRequiresExclusiveLifetime();
        testBlockCompactionPublicationFailure();
        testMixedEditsAgainstReference();
        testBatchCanonicalRootsAndBranchIndependence();
        testMultipleConnections();
        testTypedModelRecordsAndReferences();
        testTypedModelConcurrentHandles();
        testDurableReverseReferences();
        testExperimentalBlockRevisionPublication();
        testLargeBlockCommitAndPublicationFailure();
        testModelExpectedBranchHead();
        std::cout << "AOWIS native storage tests: passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "AOWIS native storage tests: FAILED: " << failure.what() << '\n';
        return 1;
    }
}
