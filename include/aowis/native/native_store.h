#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aowis/native/storage_error.h"

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
    // Explicit resolver for two independently added records with the SAME UUID
    // (no ancestor record). Needed by deterministic membership-chunk IDs.
    // Callers must verify they represent the same logical internal object.
    std::function<MergeResolution(const Key&, const ModelRecord&,
                                  const ModelRecord&)> resolveConcurrentAddition;
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
    // Optional model-aware reconciliation of coordinated changes. Operates on a
    // private copy-on-write candidate BEFORE reference and semantic preflight.
    // `set` changes candidate records only; nothing is written until every
    // validator succeeds. Readers provide immutable branch-tip records.
    std::function<std::optional<std::string>(
        const MergeCandidate&,
        const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
        const std::function<std::optional<ModelRecord>(const Key&)>& target,
        const std::function<std::optional<ModelRecord>(const Key&)>& source)> reconcile;
    std::function<std::optional<std::string>(const MergeCandidate&)> validate;
};

// Offline, physical-format-independent replication. Bundles carry the full
// immutable ancestry and both trees for a single branch tip. Imported branch
// refs are created or fast-forwarded only; divergent history is never replaced.
enum class BundleImportStatus { Created, FastForward, UpToDate };
// Optional per-transfer ceilings. Zero disables that particular ceiling for
// the existing offline API; HTTP always supplies finite limits.
struct BundleLimits {
    std::uint64_t maxBytes = 0;
    std::uint64_t maxObjects = 0;
    std::uint64_t maxObjectBytes = 0;
};

// Optional import-stage timings, measured inside the native importer. All
// durations are wall-clock milliseconds and can overlap neither each other
// nor the separate post-import repository audit. Populated on success only.
struct BundleImportProfile {
    double scanMs = 0;               // Header, per-object SHA-256, and disk index.
    double graphTraversalMs = 0;     // Typed revision/visited trie verification.
    double modelReferenceMs = 0;     // Semantic model and reverse-index checks.
    double reachabilityMs = 0;       // Unreachable object and ancestry checks.
    double branchCheckMs = 0;        // Fast-forward and branch status checks.
    double objectWriteMs = 0;        // Copy missing objects into local storage.
    double publicationMs = 0;       // Sync and atomic branch ref publication.
    double totalMs = 0;
    std::uint64_t bundleObjectReads = 0;
    std::uint64_t bundleObjectBytes = 0;
    std::uint64_t localObjectReads = 0;
    std::uint64_t localObjectBytes = 0;
};

struct BundleImportResult {
    BundleImportStatus status = BundleImportStatus::UpToDate;
    Hash head{};
    std::uint64_t objectsImported = 0;
};

// Results from physical verification and validation of every revision and
// logical index reachable from each published branch head.
struct VerificationReport {
    std::uint64_t physicalObjects = 0;
    std::uint64_t reachableObjects = 0;
    std::uint64_t branches = 0;
    std::uint64_t revisions = 0;
    std::uint64_t treeNodes = 0;
    std::uint64_t modelSnapshots = 0;
};

struct CompactionStats {
    std::uint64_t objectsBefore = 0;
    std::uint64_t objectsAfter = 0;
    std::uint64_t packBytesBefore = 0;
    std::uint64_t packBytesAfter = 0;
    std::uint64_t indexBytesBefore = 0;
    std::uint64_t indexBytesAfter = 0;
    // Leftover pack generations and pointer temporaries from interrupted
    // earlier compactions, removed before this compaction started.
    std::uint64_t staleFilesRemoved = 0;
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

// Failures are reported as StorageError (a std::runtime_error) carrying a
// StorageErrorKind; callers must classify by kind, never by message text.
// Linux/POSIX prototype. All operations on one instance are thread-safe;
// processes using the same repository serialize operations with flock(2).
// A successful commit syncs its append-only pack before publishing its ref.
// The database uses one append-only, content-addressed pack backend.
enum class PhysicalStore { Pack };

class Repository final {
public:
    explicit Repository(std::filesystem::path directory,
                        PhysicalStore storage = PhysicalStore::Pack);
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    void createBranch(std::string_view name, std::optional<std::string_view> from = std::nullopt);
    Revision head(std::string_view branch);
    // Sorted branch names, independent of authorization. Filter at the service boundary.
    std::vector<std::string> listBranches();
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
    // Bounded variant for untrusted callers: work is proportional to the
    // changed trie paths, and more than maxDifferences results raise
    // StorageError(LimitExceeded) before the full difference is materialized.
    std::vector<Difference> diffRevisions(const Hash& fromRevision, const Hash& toRevision,
                                          std::size_t maxDifferences);
    // True when revisionId is the head of `branch` or any ancestor of it
    // (through either merge parent). Unknown or non-revision ids: NotFound.
    bool revisionOnBranch(std::string_view branch, const Hash& revisionId);
    // A self-contained branch snapshot for offline transfer between independent
    // native repositories, including different physical storage backends.
    // Export refuses to replace an existing bundle file.
    void exportBranchBundle(std::string_view branch, const std::filesystem::path& bundlePath,
                            BundleLimits limits = {});
    // Delta relative to a revision already present at the recipient. The base
    // must be an ancestor (through either merge parent) of the exported head.
    // Every object reachable from that base is omitted from the transfer.
    void exportIncrementalBranchBundle(std::string_view branch, const Hash& recipientBase,
                                       const std::filesystem::path& bundlePath, BundleLimits limits = {});
    // Import creates a new local branch or advances it only by fast-forward.
    // It never overwrites divergent local work or creates an implicit merge.
    // All bundle SHA-256 hashes and object graph edges are verified before
    // writing any objects or publishing a ref. Authorization is not provided.
    BundleImportResult importBranchBundle(const std::filesystem::path& bundlePath,
                                          std::string_view localBranch, BundleLimits limits = {},
                                          BundleImportProfile* profile = nullptr);

    std::uint64_t packSize();
    std::uint64_t objectCount();
    // Full SHA-256 verification of every physical object. Normal reopening
    // trusts the checksummed sidecar index and verifies object bytes on read.
    void verifyPack();
    // Read-only full verification: physical objects, published revision history,
    // canonical raw/model/reference trees and exact model reverse references.
    // Checks every reachable ancestor, including second merge parents.
    VerificationReport verifyRepository();
    // Offline maintenance: retain all branch heads AND their full parent histories.
    // Refuses to run while another Repository instance has this directory open.
    // Publishes an immutable replacement pack using an fsynced atomic pointer.
    CompactionStats compact();
    // Bytes of an unpublished, torn pack tail discarded by this handle while
    // opening or refreshing the repository. A copy of every discarded region
    // is kept under <repository>/recovered-pack-tails/ for inspection.
    std::uint64_t recoveredPackTailBytes();

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
    std::uint64_t scanOffset_ = 0;
    std::uint64_t indexReadOffset_ = 0;
    bool needsReopen_ = false; // fatal failure after publishing a compaction pointer
    std::uint64_t recoveredPackTailBytes_ = 0;
    Bytes pendingIndex_;

    void exportBundleLocked(std::string_view branch, const std::filesystem::path& bundlePath,
                            std::optional<Hash> recipientBase, BundleLimits limits);
    void scanPackTail();
    void discardTornPackTail(std::uint64_t position, std::uint64_t size);
    void preserveDiscardedPackBytes(std::uint64_t position, std::uint64_t size);
    std::uint64_t removeStalePackGenerations();
    void rebuildIndex();

    void verifyPackLocked();
    void refreshIndex();
    Bytes load(const Hash& hash);
    Hash store(const Bytes& bytes);
    Hash store(Bytes&& bytes);
    Revision readRevision(const Hash& hash);
    Revision clientRevision(const Hash& revisionId);
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
    void diffHashes(const Hash& from, const Hash& to, std::vector<Difference>& output,
                    std::size_t limit = std::numeric_limits<std::size_t>::max());
    void diffWholeSubtree(const Hash& root, bool added, std::vector<Difference>& output,
                          std::size_t limit);
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
