#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aowis::native {

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;
using Key = std::array<std::uint8_t, 16>;

std::string toHex(const Hash& value);
Hash fromHex(std::string_view value);
Hash sha256(const Bytes& value);

struct Change {
    Key key{};
    std::optional<Bytes> value;  // nullopt deletes the key
};

// Typed model envelope: payload bytes come from AOWIS ModelCodec, never from
// the native store. References describe required live UUID dependencies.
struct ModelRecord {
    std::uint16_t kind = 0;
    std::vector<Key> references;
    Bytes payload;
};

struct ModelChange {
    Key key{};
    std::optional<ModelRecord> record;
};

Bytes encodeModelRecord(const ModelRecord& record);
ModelRecord decodeModelRecord(const Bytes& bytes);

struct Difference {
    Key key{};
    std::optional<Hash> before;
    std::optional<Hash> after;
};

struct CompactionStats {
    std::uint64_t objectsBefore = 0;
    std::uint64_t objectsAfter = 0;
    std::uint64_t packBytesBefore = 0;
    std::uint64_t packBytesAfter = 0;
    std::uint64_t indexBytesBefore = 0;
    std::uint64_t indexBytesAfter = 0;
};

struct Revision {
    Hash id{};
    Hash root{};
    Hash referencesRoot{}; // persistent UUID -> incoming-referrer UUID set index
    bool modelIndexed = false;
    std::vector<Hash> parents;
};

// Linux/POSIX prototype. All operations on one instance are thread-safe;
// processes using the same repository serialize operations with flock(2).
// A successful commit syncs its append-only pack before publishing its ref.
class BlockStorePrototype;

// Stage-7 is the default. The block store is explicitly experimental and
// currently permits only one repository handle at a time.
enum class PhysicalStore { Stage7Pack, ExperimentalBlocks };

class Repository final {
public:
    explicit Repository(std::filesystem::path directory,
                        PhysicalStore storage = PhysicalStore::Stage7Pack);
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    void createBranch(std::string_view name, std::optional<std::string_view> from = std::nullopt);
    Revision head(std::string_view branch);
    Revision commit(std::string_view branch, const std::vector<Change>& changes);
    // Validates reference existence and deletion safety under the SAME
    // repository lock as publication. No TOCTOU window with other writers.
    // If expectedHead is supplied, reject concurrent branch changes before
    // touching durable model/index objects (optimistic branch-head check).
    Revision commitModel(std::string_view branch, const std::vector<ModelChange>& changes,
                         std::optional<Hash> expectedHead = std::nullopt);
    std::optional<ModelRecord> getModel(std::string_view branch, const Key& key);
    // Sorted IDs of entities referring to key; O(log N + inbound references).
    // Available on branches created through commitModel, not raw commit.
    std::vector<Key> referencingModel(std::string_view branch, const Key& key);
    std::optional<Bytes> get(std::string_view branch, const Key& key);
    std::vector<std::pair<Key, Bytes>> scan(std::string_view branch);
    std::vector<Difference> diff(std::string_view from, std::string_view to);
    std::uint64_t packSize();
    std::uint64_t objectCount();
    // Full SHA-256 verification of every physical object. Normal reopening
    // trusts the checksummed sidecar index and verifies object bytes on read.
    void verifyPack();
    // Offline maintenance: retain all branch heads AND their full parent histories.
    // Refuses to run while another Repository instance has this directory open.
    // Publishes an immutable replacement pack using an fsynced atomic pointer.
    CompactionStats compact();

private:
    struct Location {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };
    struct TreeNode {
        bool leaf = true;
        Key key{};
        Hash value{};
        std::uint8_t depth = 0;
        Key prefix{}; // representative UUID; nibbles before depth are shared
        std::array<Hash, 16> children{};
    };

    std::filesystem::path directory_;
    PhysicalStore storage_ = PhysicalStore::Stage7Pack;
    std::unique_ptr<BlockStorePrototype> blockStore_;
    int lockFd_ = -1;
    int lifetimeFd_ = -1;
    std::string packToken_;
    int packFd_ = -1;
    int indexFd_ = -1;
    std::mutex mutex_;
    struct HashHasher {
        std::size_t operator()(const Hash& hash) const noexcept {
            std::uint64_t value = 14695981039346656037ULL;
            for (std::uint8_t byte : hash) {
                value = (value ^ byte) * 1099511628211ULL;
            }
            return static_cast<std::size_t>(value);
        }
    };
    std::unordered_map<Hash, Location, HashHasher> index_;
    // Unpublished immutable nodes produced while assembling one block-backed
    // revision. They are persisted together before its branch ref is moved.
    std::unordered_map<Hash, Bytes, HashHasher> stagedObjects_;
    // Bound unpublished payload retention for large block-backed commits.
    // Flushed content is durable but unreachable until the branch ref advances.
    std::size_t stagedBytes_ = 0;
    std::uint64_t scanOffset_ = 0;
    std::uint64_t indexReadOffset_ = 0;
    Bytes pendingIndex_;

    void scanPackTail();
    void rebuildIndex();

    void refreshIndex();
    Bytes load(const Hash& hash);
    Hash store(const Bytes& bytes);
    Hash store(Bytes&& bytes);
    Revision readRevision(const Hash& hash);
    Hash branchHead(std::string_view name);
    void publishRef(std::string_view branch, const Hash& commitHash, bool mustBeAbsent);
    TreeNode readNode(const Hash& hash);
    Hash storeNode(const TreeNode& node);
    using TreeUpdate = std::pair<Key, std::optional<Hash>>;
    Hash buildTree(const std::vector<TreeUpdate>& updates, std::size_t begin,
                   std::size_t end, unsigned minDepth);
    Hash applyTree(const Hash& root, const std::vector<TreeUpdate>& updates,
                   std::size_t begin, std::size_t end, unsigned minDepth);
    std::optional<Hash> lookup(const Hash& root, const Key& key);
    void listHashes(const Hash& root, std::vector<std::pair<Key, Hash>>& output);
    void diffHashes(const Hash& from, const Hash& to, std::vector<Difference>& output);
    Revision commitLocked(std::string_view branch, const std::vector<Change>& changes,
                          std::optional<Hash> referencesRoot = std::nullopt);
    std::vector<Key> readReferrers(const Hash& root, const Key& target);
    void syncPack();
};

}  // namespace aowis::native
