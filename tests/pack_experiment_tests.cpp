#include "aowis/native/native_store.h"
#include "aowis/native/pack_experiment.h"
#include "aowis/native/block_store_prototype.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
#include <string_view>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::PackExperimentStats;
using aowis::native::Repository;

void require(bool valid, std::string_view message) {
    if (!valid) throw std::runtime_error(std::string(message));
}
Key key(std::uint64_t value) {
    Key result{};
    for (unsigned i = 0; i < 8; ++i) result[15 - i] = static_cast<std::uint8_t>(value >> (8 * i));
    return result;
}
struct Temporary {
    Temporary() {
        const std::uint64_t timestamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-native-pack-test-" + std::to_string(::getpid()) + "-" + std::to_string(timestamp));
    }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(this->path, ignored); }
    std::filesystem::path path;
};
void expectFailure(const std::function<void()>& operation, std::string_view message) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}
void flipByte(const std::filesystem::path& path, std::uint64_t position) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    require(stream.good(), "cannot open corruption fixture");
    char byte = 0;
    stream.seekg(static_cast<std::streamoff>(position));
    stream.get(byte);
    require(stream.good(), "cannot read corruption fixture byte");
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x40U);
    stream.seekp(static_cast<std::streamoff>(position));
    stream.put(byte);
    stream.flush();
    require(stream.good(), "cannot write corruption fixture byte");
}
void testWritableBlocks() {
    Temporary test;
    const auto repoPath = test.path / "repository";
    {
        Repository repo(repoPath);
        repo.createBranch("main");
        repo.commit("main", {{key(1), Bytes{1, 2, 3}}, {key(2), Bytes{4, 5, 6}}});
    }
    const auto candidate = test.path / "candidate";
    aowis::native::runPackExperiment(repoPath, candidate, 1024);
    const Bytes first{8, 9, 10};
    const Bytes second(3200, 71);
    const Hash firstHash = aowis::native::sha256(first);
    const Hash secondHash = aowis::native::sha256(second);
    std::uint64_t packCommitted = 0;
    std::uint64_t indexCommitted = 0;
    std::uint64_t baseline = 0;
    {
        aowis::native::BlockStorePrototype store(candidate, 1024);
        baseline = store.stats().objects;
        expectFailure([&] {
            aowis::native::BlockStorePrototype another(candidate);
        }, "concurrent writable handle accepted");
        const auto result = store.putBatch({first, second, first});
        require(result == std::vector<Hash>({firstHash, secondHash, firstHash}),
                "batch returned incorrect SHA-256 identities");
        require(store.stats().objects == baseline + 2, "duplicate was not removed");
        require(store.get(firstHash) == first && store.get(secondHash) == second,
                "indexed lookup did not return committed contents");
        require(store.putBatch({first}).size() == 1 && store.stats().objects == baseline + 2,
                "second insertion duplicated existing object");
        store.verify();
        packCommitted = store.stats().packBytes;
        indexCommitted = store.stats().indexBytes;
    }
    // Simulate a killed process after writing both pack and index, before
    // publishing the new committed-length marker. Unpublished data is ignored.
    {
        std::ofstream pack(candidate / "candidate.pack", std::ios::binary | std::ios::app);
        std::ofstream index(candidate / "candidate.idx", std::ios::binary | std::ios::app);
        pack << "uncommitted-physical-tail";
        index << "uncommitted-index-tail";
    }
    {
        aowis::native::BlockStorePrototype store(candidate);
        require(store.stats().packBytes == packCommitted &&
                store.stats().indexBytes == indexCommitted,
                "uncommitted tails were not removed");
        require(store.get(firstHash) == first && store.get(secondHash) == second,
                "committed objects lost during tail recovery");
        store.verify();
    }
    require(std::filesystem::file_size(candidate / "candidate.pack") == packCommitted,
            "pack was not physically truncated to committed length");
    // A missing derived index must be rebuildable from the committed pack.
    std::filesystem::remove(candidate / "candidate.idx");
    {
        aowis::native::BlockStorePrototype store(candidate);
        require(store.stats().objects == baseline + 2, "index rebuild lost committed objects");
        require(store.get(firstHash) == first && store.get(secondHash) == second,
                "reconstructed index cannot find committed data");
        store.verify();
    }
    // A damaged derived index can also be replaced from the committed pack.
    flipByte(candidate / "candidate.idx", 0);
    {
        aowis::native::BlockStorePrototype store(candidate);
        require(store.get(firstHash) == first, "index-magic recovery failed");
        store.verify();
    }
    // Corrupt a pack offset in an index entry without modifying the pack.
    flipByte(candidate / "candidate.idx", 8 + 32);
    {
        aowis::native::BlockStorePrototype store(candidate);
        require(store.get(secondHash) == second, "invalid-offset recovery failed");
        store.verify();
    }
    // Corrupt committed content: never treat it as a recoverable partial tail.
    flipByte(candidate / "candidate.pack", packCommitted - 1);
    expectFailure([&] {
        aowis::native::BlockStorePrototype store(candidate);
        store.verify();
    }, "corrupted committed block accepted");
    flipByte(candidate / "candidate.pack", packCommitted - 1);
    {
        aowis::native::BlockStorePrototype store(candidate);
        store.verify();
    }
    flipByte(candidate / "candidate.current", 44);
    expectFailure([&] {
        aowis::native::BlockStorePrototype store(candidate);
    }, "corrupted committed-length marker accepted");
}

// The public API accepts repeated values; the internal repository handoff is
// unique by hash. Exercise both constraints across multiple physical blocks.
void testLargeWritableBatchDeduplication() {
    Temporary temporary;
    const std::filesystem::path path = temporary.path / "many-blocks";
    std::filesystem::create_directory(temporary.path);
    aowis::native::BlockStorePrototype::createEmpty(path);
    std::vector<Bytes> values;
    values.reserve(640);
    for (std::size_t i = 0; i < 600; ++i) {
        Bytes bytes(96 + i % 41);
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            bytes[j] = static_cast<std::uint8_t>((i * 37U + j * 19U + (i >> 8U)) & 255U);
        }
        values.push_back(std::move(bytes));
    }
    for (std::size_t i = 0; i < 40; ++i) values.push_back(values[i * 7]);
    std::vector<Hash> hashes;
    {
        aowis::native::BlockStorePrototype store(path, 1024);
        hashes = store.putBatch(values);
        require(hashes.size() == values.size(), "repeated input lost hash output");
        require(store.stats().objects == 600 && store.stats().blocks > 2,
                "large batch was not deduplicated across blocks");
        store.verify();
    }
    {
        aowis::native::BlockStorePrototype store(path, 1024);
        require(store.stats().objects == 600, "reopen changed batch object count");
        for (std::size_t i = 0; i < values.size(); ++i) {
            require(store.get(hashes[i]) == values[i], "batch changed content identity");
        }
        // A second large commit mixes hashes already in the committed index,
        // repeated hashes within the new batch, and many new multi-block values.
        // Compact staging must retain physical offsets for each inserted hash.
        std::vector<Bytes> next;
        for (std::size_t i = 0; i < 250; ++i) next.push_back(values[i * 2]);
        for (std::size_t i = 600; i < 850; ++i) {
            Bytes bytes(110 + i % 53);
            for (std::size_t j = 0; j < bytes.size(); ++j) {
                bytes[j] = static_cast<std::uint8_t>((i * 31U + j * 23U + (i >> 8U)) & 255U);
            }
            next.push_back(std::move(bytes));
        }
        for (std::size_t i = 0; i < 50; ++i) next.push_back(next[250 + i * 3]);
        const std::vector<Hash> nextHashes = store.putBatch(next);
        require(store.stats().objects == 850, "mixed batch did not deduplicate existing/new hashes");
        for (std::size_t i = 0; i < next.size(); ++i) {
            require(store.get(nextHashes[i]) == next[i], "mixed batch returned wrong object");
        }
        store.verify();
    }
    {
        aowis::native::BlockStorePrototype store(path, 1024);
        require(store.stats().objects == 850, "mixed batch count changed after reopening");
        for (std::size_t i = 0; i < values.size(); ++i) {
            require(store.get(hashes[i]) == values[i], "old objects lost after mixed batch");
        }
        store.verify();
    }
}

void testBlockCandidate() {
    Temporary test;
    const std::filesystem::path repoPath = test.path / "repository";
    Hash mainHead{};
    {
        Repository repo(repoPath);
        repo.createBranch("main");
        std::vector<Change> changes;
        for (std::uint64_t i = 0; i < 1200; ++i) {
            Bytes bytes(30 + static_cast<std::size_t>(i % 200));
            for (std::size_t n = 0; n < bytes.size(); ++n) {
                bytes[n] = static_cast<std::uint8_t>((i * 113 + n * 17) % 251);
            }
            changes.push_back(Change{key(i + 1), bytes});
        }
        repo.commit("main", changes);
        repo.createBranch("feature", "main");
        repo.commit("feature", {{key(1), Bytes{3, 4, 5}}, {key(4), std::nullopt}});
        mainHead = repo.head("main").id;
        repo.verifyPack();
    }
    const std::filesystem::path candidate = test.path / "candidate";
    const PackExperimentStats results = aowis::native::runPackExperiment(repoPath, candidate, 2048);
    require(results.objects > 1200 && results.blocks > 1, "candidate did not create multiple blocks");
    require(results.sampleTrials == 6 && results.sampleReads > 0,
            "paired warm-cache random-read benchmark did not execute");
    require(results.candidatePackBytes + results.candidateIndexBytes <
            results.originalPackBytes + results.originalIndexBytes,
            "candidate did not eliminate per-object overhead");
    aowis::native::verifyPackExperiment(candidate);
    expectFailure([&] { (void)aowis::native::runPackExperiment(repoPath, candidate); },
                  "candidate overwrote an existing path");
    flipByte(candidate / "candidate.pack", 55);
    expectFailure([&] { aowis::native::verifyPackExperiment(candidate); },
                  "block digest failed to catch corruption");
    flipByte(candidate / "candidate.pack", 55);
    aowis::native::verifyPackExperiment(candidate);
    flipByte(candidate / "candidate.idx", 12);
    expectFailure([&] { aowis::native::verifyPackExperiment(candidate); },
                  "candidate index corruption went unnoticed");
    std::filesystem::remove(candidate / "candidate.idx");
    aowis::native::rebuildPackExperimentIndex(candidate);
    aowis::native::verifyPackExperiment(candidate);
    std::filesystem::remove(candidate / "candidate.idx");
    flipByte(candidate / "candidate.pack", 60);
    expectFailure([&] { aowis::native::rebuildPackExperimentIndex(candidate); },
                  "index rebuild accepted corrupted block");
    flipByte(candidate / "candidate.pack", 60);
    std::filesystem::remove(candidate / "candidate.idx");
    aowis::native::rebuildPackExperimentIndex(candidate);
    {
        Repository repo(repoPath);
        require(repo.head("main").id == mainHead, "experiment modified source branch head");
        require(repo.scan("main").size() == 1200, "experiment modified source objects");
        repo.compact();
    }
    const std::filesystem::path afterCompact = test.path / "after-compaction";
    const PackExperimentStats compacted = aowis::native::runPackExperiment(repoPath, afterCompact, 4096);
    require(compacted.objects == results.objects, "compacted source changed object count");
    aowis::native::verifyPackExperiment(afterCompact);
}
}
int main() {
    testBlockCandidate();
    testWritableBlocks();
    testLargeWritableBatchDeduplication();
}
