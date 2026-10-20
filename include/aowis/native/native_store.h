#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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

// A merge never overwrites conflicting edits. Conflicts leave both refs intact.
enum class MergeStatus { UpToDate, FastForward, Merged, Conflicts };

struct MergeConflict {
    Key key{};
    std::optional<Hash> base;
    std::optional<Hash> target;
    std::optional<Hash> source;
    std::string reason;
};

// Optional typed-record reconciliation. Callbacks execute while the repository
// lock is held: they must be pure, must not call Repository methods, and must
// not retain a candidate lookup beyond the callback.
struct MergeResolution {
    std::optional<ModelRecord> record; // nullopt = unresolved
    std::vector<std::string> conflictingFields;
};
struct MergeCandidate {
    // Proposed merged snapshot, including pending additions/deletions.
    std::function<std::optional<ModelRecord>(const Key&)> get;
    // Visits records in UUID order without retaining all decoded payloads.
    std::function<void(const std::function<void(const Key&, const ModelRecord&)>&)> forEach;
};
// An explicit whole-record selection is allowed only for a genuine same-UUID
// divergence (including delete-versus-modify). Model-specific callers must
// still validate the complete selected snapshot before publication.
enum class MergeChoiceSide { Target, Source };
struct MergeOptions {
    // This callback is only invoked for concurrently changed, different UUID
    // records on a model-indexed branch, before field-level reconciliation.
    // Returning nullopt preserves the normal conflict/field resolver behavior.
    std::function<std::optional<MergeChoiceSide>(const MergeConflict&)> chooseConflict;
    std::function<MergeResolution(const Key&, const ModelRecord&, const ModelRecord&,
                                  const ModelRecord&)> resolve;
    // Optional optimistic concurrency pins for UI-driven conflict resolution.
    // Checked under the same repository lock as merge publication.
    std::optional<Hash> expectedTargetHead;
    std::optional<Hash> expectedSourceHead;
    // Verify that every user-supplied choice was actually consumed by this
    // merge. Called before any pack writes, including fast-forward publication.
    // Must be pure with respect to the Repository; cannot call its methods.
    std::function<std::optional<std::string>()> checkSelections;
    // Return a nonempty diagnostic on semantic failure. Invoked before any
    // object bytes or branch refs are written; cannot call Repository methods.
    std::function<std::optional<std::string>(const MergeCandidate&)> validate;
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

struct MergeResult {
    MergeStatus status = MergeStatus::UpToDate;
    Hash base{};
    Hash targetBefore{};
    Hash sourceHead{};
    Revision revision; // resulting target head, also on conflict
    std::vector<MergeConflict> conflicts;
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
    // Immutable history: newest first along the first-parent chain. `after`
    // is an exclusive pagination cursor and must belong to this branch's
    // first-parent history. A branch advancing between pages is harmless.
    std::vector<Revision> history(std::string_view branch, std::size_t maxCount = 100,
                                  std::optional<Hash> after = std::nullopt);
    // Lookup by immutable SHA-256 revision identity, including older revisions.
    // These low-level APIs do not implement authorization or branch scoping.
    Revision revision(const Hash& revisionId);
    // Create a new branch from any ancestor (including merge parents) of
    // sourceBranch, without changing sourceBranch or writing a new revision.
    void createBranchAtRevision(std::string_view name, std::string_view sourceBranch,
                                const Hash& revisionId);
    // Three-way object-level merge. Both branch tips are read under one lock;
    // a conflicting merge leaves both heads and the pack unchanged. A normal
    // merge writes a two-parent revision, preserving source branch history.
    // Same-UUID divergent edits are conflicts unless an optional pure resolver
    // reconciles them. Semantic preflight may reject a tentative merged snapshot.
    MergeResult mergeBranches(std::string_view target, std::string_view source,
                              const MergeOptions& options = {});
    Revision commit(std::string_view branch, const std::vector<Change>& changes);
    // Validates reference existence and deletion safety under the SAME
    // repository lock as publication. No TOCTOU window with other writers.
    // If expectedHead is supplied, reject concurrent branch changes before
    // touching durable model/index objects (optimistic branch-head check).
    Revision commitModel(std::string_view branch, std::vector<ModelChange> changes,
                         std::optional<Hash> expectedHead = std::nullopt);
    std::optional<ModelRecord> getModel(std::string_view branch, const Key& key);
    // Historical reads use the revision's own roots, not the moving branch head.
    std::optional<Bytes> getAtRevision(const Hash& revisionId, const Key& key);
    std::optional<ModelRecord> getModelAtRevision(const Hash& revisionId, const Key& key);
    std::vector<Key> referencingModelAtRevision(const Hash& revisionId, const Key& key);
    // Sorted IDs of entities referring to key; O(log N + inbound references).
    // Available on branches created through commitModel, not raw commit.
    std::vector<Key> referencingModel(std::string_view branch, const Key& key);
    std::optional<Bytes> get(std::string_view branch, const Key& key);
    std::vector<std::pair<Key, Bytes>> scan(std::string_view branch);
    std::vector<Difference> diff(std::string_view from, std::string_view to);
    std::vector<Difference> diffRevisions(const Hash& fromRevision, const Hash& toRevision);
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
    bool revisionReachableFrom(const Hash& tip, const Hash& candidate);
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
                          std::optional<Hash> referencesRoot = std::nullopt,
                          std::optional<Hash> mergeParent = std::nullopt);
    Revision commitModelLocked(std::string_view branch, std::vector<ModelChange> changes,
                               std::optional<Hash> expectedHead,
                               std::optional<Hash> mergeParent);
    std::vector<Key> readReferrers(const Hash& root, const Key& target);
    void syncPack();
};

}  // namespace aowis::native
