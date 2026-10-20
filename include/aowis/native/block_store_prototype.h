#pragma once

#include "aowis/native/native_store.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace aowis::native {

// EXPERIMENTAL physical content store. Does not publish revision/branch heads
// and is not yet a replacement for Repository's authoritative Stage 7 pack.
// Can open an existing verified offline candidate and then append atomic
// batches of immutable content-addressed objects.
class BlockStorePrototype final {
public:
    struct Stats {
        std::uint64_t objects = 0;
        std::uint64_t packBytes = 0;
        std::uint64_t indexBytes = 0;
        std::uint64_t blocks = 0;
    };
    explicit BlockStorePrototype(const std::filesystem::path& directory,
                                 std::uint32_t targetBlockBytes = 65536);
    ~BlockStorePrototype();
    BlockStorePrototype(const BlockStorePrototype&) = delete;
    BlockStorePrototype& operator=(const BlockStorePrototype&) = delete;

    // Initialize a new empty block store, without importing a Stage-7 pack.
    static void createEmpty(const std::filesystem::path& directory);

    // One batch becomes visible/durable together after the manifest rename.
    // Already-present objects are deduplicated by SHA-256.
    std::vector<Hash> putBatch(const std::vector<Bytes>& payloads);
    std::optional<Bytes> get(const Hash& hash) const;
    bool contains(const Hash& hash) const;
    std::vector<Hash> hashes() const;
    Stats stats() const;
    // Independently scan committed blocks and check both block and object hashes.
    void verify() const;
    // A generation pointer selects an immutable replacement pack. With no pointer,
    // pre-compaction repositories retain the original directory layout.
    static std::filesystem::path activeDirectory(const std::filesystem::path& root);
    const std::filesystem::path& activeDataDirectory() const { return this->activeDir_; }

private:
    friend class Repository; // Repository supplies staged objects whose SHA-256 was already calculated.
    using HashedBytes = std::pair<Hash, const Bytes*>;
    // Internal-only handoff: Repository::store calculated each hash before
    // storing immutable bytes in its staging map. Never accept arbitrary
    // caller-provided hashes without checking them first.
    // Each hash must occur only once. Repository staging and putBatch enforce this.
    void putHashedBatch(const std::vector<HashedBytes>& values);
    struct Location {
        std::uint64_t block = 0;
        std::uint32_t slot = 0;
        std::uint32_t size = 0;
    };
    struct HashHasher {
        std::size_t operator()(const Hash& hash) const noexcept;
    };
    std::filesystem::path directory_;
    std::filesystem::path activeDir_;
    int lockFd_ = -1;
    int packFd_ = -1;
    int indexFd_ = -1;
    std::uint32_t blockTarget_ = 65536;
    std::uint64_t packBytes_ = 0;
    std::uint64_t indexBytes_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t blocks_ = 0;
    bool needsReopen_ = false; // fail closed after an interrupted in-process append
    std::unordered_map<Hash, Location, HashHasher> locations_;

    void writeManifest(std::uint64_t packBytes, std::uint64_t indexBytes,
                       std::uint64_t objects, std::uint64_t generation);
    void rebuildIndex(std::uint64_t committedPackBytes);
    void refreshLocations();
};

} // namespace aowis::native
