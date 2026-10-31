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
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aowis/dvmdb/storage_error.h"
#include "aowis/dvmdb/object_id.h"

namespace aowis::dvmdb {

struct RevisionRecordV2;

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;
using Key = std::array<std::uint8_t, 16>;

// Process-wide logical-extent cap for native internal replication/verification
// scratch files. Zero disables the limit (offline library default). Configure
// before starting operations; changing while scratch is active is rejected.
// Does not include HTTP transfer spools, pack growth or compaction copies;
// not a physical-space guarantee or an interprocess filesystem quota.
void setInternalScratchLimit(std::uint64_t maximumBytes);
std::uint64_t internalScratchReservedBytes();

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
    // Optional model-aware reconciliation of coordinated changes. Operates on a
    // private copy-on-write candidate BEFORE reference and semantic preflight.
    // `set` changes candidate records only; nothing is written until every
    // validator succeeds. Readers provide immutable branch-tip records.
    std::function<std::optional<std::string>(
        const MergeCandidate&,
        const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
        const std::function<std::optional<ModelRecord>(const Key&)>& target,
        const std::function<std::optional<ModelRecord>(const Key&)>& source)> reconcile;
    // Return a nonempty diagnostic on semantic failure. Invoked on divergent
    // candidate merges AND fast-forwards (against the immutable source tip)
    // before any object bytes or branch refs are written. Up-to-date merges
    // do not revalidate an already incorporated snapshot. The source tip must
    // be model-indexed when validate is supplied for a fast-forward. Callbacks
    // cannot call Repository methods while its lock is held.
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

// Native UUIDv7 revision replication, independent of the SHA-revision
// AOWSYNC3/4 protocol. Branch identity and project provenance are preserved.
// v2 full and incremental bundles use verified UUIDv7 revision identities.
struct RevisionBundleImportV2 {
    BundleImportStatus status = BundleImportStatus::UpToDate;
    RevisionId head;
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

// Consistent, stateless all-parent graph pagination. A cursor pins the sorted
// branch names and their HEAD hashes at the first request; subsequent pages
// never re-read moving refs. The cursor is a native value, not an HTTP token.
struct RevisionGraphHead {
    std::string name;
    Hash revision{};
};

struct RevisionGraphCursor {
    std::vector<RevisionGraphHead> heads;
    std::size_t skip = 0; // unique revisions already emitted, from newest first
};

struct RevisionGraphPage {
    std::vector<RevisionGraphHead> heads;
    std::vector<Revision> revisions;
    std::optional<RevisionGraphCursor> next;
};

// UUIDv7 equivalent of the full all-parent graph. IDs are domain identities;
// recordHash and expectedRecordHash authenticate the immutable object bytes.
// Cursor pins a complete branch HEAD snapshot, independent of moving refs.
struct RevisionGraphParentV2 {
    RevisionId id;
    Hash expectedRecordHash{};
};

struct RevisionGraphRevisionV2 {
    RevisionId id;
    Hash recordHash{};
    Hash root{};
    Hash referencesRoot{};
    bool modelIndexed = false;
    std::vector<RevisionGraphParentV2> parents;
};

struct RevisionGraphHeadV2 {
    BranchId branchId;
    std::string name;
    std::optional<RevisionId> revision;
    Hash recordHash{};
    std::optional<BranchId> originBranchId; // Immutable creation provenance.
    std::optional<RevisionId> forkRevisionId; // Exact immutable fork point, not a revision parent.
};

struct RevisionGraphCursorV2 {
    std::vector<RevisionGraphHeadV2> heads;
    std::size_t skip = 0;
};

struct RevisionGraphPageV2 {
    std::vector<RevisionGraphHeadV2> heads;
    std::vector<RevisionGraphRevisionV2> revisions;
    std::optional<RevisionGraphCursorV2> next;
};

// The identity and immutable provenance returned by one atomic branch creation.
// These fields are creation metadata, never revision-parent edges.
struct BranchCreationResultV2 {
    BranchId branchId;
    RevisionId revisionId;
    std::optional<BranchId> originBranchId;
    std::optional<RevisionId> forkRevisionId;
};

struct MergeResult {
    MergeStatus status = MergeStatus::UpToDate;
    Hash base{};
    Hash targetBefore{};
    Hash sourceHead{};
    Revision revision; // resulting target head, also on conflict
    std::vector<MergeConflict> conflicts;
};

// UUIDv7 merge result: the target and source branch names retain their own
// identities; only a successful merge can advance the target HEAD.
struct MergeResultV2 {
    MergeStatus status = MergeStatus::UpToDate;
    std::optional<RevisionId> base;
    std::optional<RevisionId> targetBefore;
    std::optional<RevisionId> sourceHead;
    std::optional<RevisionId> resultHead;
    std::vector<MergeConflict> conflicts;
};

// Anonymous file in a repository's scratch directory (<repository>/tmp: the
// pack's filesystem, never TMPDIR). It has no name and disappears on close.
class ScratchFile final {
public:
    ScratchFile() = default;
    explicit ScratchFile(int fd) noexcept;
    ~ScratchFile();
    ScratchFile(ScratchFile&& other) noexcept;
    ScratchFile& operator=(ScratchFile&& other) noexcept;
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    int fd() const noexcept;
    // Hands the descriptor to a new owner (for example QFile/AutoCloseHandle).
    int release() noexcept;
private:
    int fd_ = -1;
};

// Failures are reported as StorageError (a std::runtime_error) carrying a
// StorageErrorKind; callers must classify by kind, never by message text.
// Linux/POSIX prototype. All operations on one instance are thread-safe;
// processes using the same repository serialize operations with flock(2).
// A successful commit syncs its append-only pack before publishing its ref.
// Storage is one append-only, content-addressed object pack per repository.
class Repository final {
public:
    explicit Repository(std::filesystem::path directory);
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    void createBranch(std::string_view name, std::optional<std::string_view> from = std::nullopt);
    // One atomically published UUIDv7 record: rename preserves ID and HEAD.
    void renameBranch(std::string_view previous, std::string_view renamed,
                      const Hash& expectedHead);
    // Phase 2c-1: store actual UUIDv7 revision objects in the repository's
    // durable SHA-addressed pack and publish verified UUID->SHA bindings.
    // This is an internal storage building block, NOT yet a branch commit:
    // branches still carry SHA heads until the coordinated v2 cutover.
    // The caller supplies the owning project UUIDv7, which is pinned on the
    // first write and cannot be changed for the lifetime of the repository.
    // Phase 2c-2: UUIDv7 HEAD publication for native raw revisions. These
    // cannot be mixed with SHA-based branches in one repository.
    RevisionId createBranchV2(const ProjectId& project, std::string_view name,
                              std::optional<std::string_view> from = std::nullopt);
    // Return the new branch UUID and fork metadata from the same locked write;
    // the revision-only API above remains available to existing consumers.
    BranchCreationResultV2 createBranchWithIdentityV2(
        const ProjectId& project, std::string_view name,
        std::optional<std::string_view> from = std::nullopt);
    void createUnbornBranchV2(const ProjectId& project, std::string_view name);
    // Clone an unborn branch with its original UUIDv7 into a branchless project.
    // A repeated identical request is safe after a lost acknowledgement.
    // No initial revision, manifest or synthetic fork is created.
    void provisionUnbornBranchV2(const ProjectId& project, std::string_view name,
                                 const BranchId& identity);
    RevisionId headV2(const ProjectId& project, std::string_view branch);
    // VC-01 transition: optional committed HEAD contract; existing branches remain born.
    std::optional<RevisionId> optionalHeadV2(const ProjectId& project, std::string_view branch);
    // First-parent UUIDv7 history, newest-first; after is an exclusive revision
    // UUID cursor and must belong to the pinned branch's first-parent chain.
    std::vector<RevisionGraphRevisionV2> historyV2(
        const ProjectId& project, std::string_view branch, std::size_t maxCount = 100,
        std::optional<RevisionId> after = std::nullopt);
    // Stateless all-parent pagination across all UUIDv7 branch HEADs.
    // Cursor freezes branch ID/name/HEAD/digest; never reads moving refs on
    // continuation. Missing snapshot revisions fail, never truncate history.
    RevisionGraphPageV2 revisionGraphV2(
        const ProjectId& project, std::size_t maxCount = 100,
        std::optional<RevisionGraphCursorV2> cursor = std::nullopt);
    void renameBranchV2(const ProjectId& project, std::string_view previous,
                        std::string_view renamed, const RevisionId& expectedHead);
    RevisionId commitV2(const ProjectId& project, std::string_view branch,
                        const std::vector<Change>& changes,
                        std::optional<RevisionId> expectedHead = std::nullopt);
    // Native UUIDv7 model-indexed commit, including reverse-reference checks.
    // A failed preflight never advances HEAD or publishes a revision.
    // Existing unpinned writes may omit expectedHead. VC-05 local Commit must
    // requireExactHead=true so an explicit null pins an unborn branch instead
    // of silently accepting a concurrently published first commit.
    RevisionId commitModelV2(const ProjectId& project, std::string_view branch,
                             std::vector<ModelChange> changes,
                             std::optional<RevisionId> expectedHead = std::nullopt,
                             bool requireExactHead = false);
    // Read a model object by a pinned UUIDv7 revision, not a moving branch ref.
    std::optional<ModelRecord> getModelAtRevisionV2(const ProjectId& project,
                                                   const RevisionId& revisionId, const Key& key);
    // Reverse-reference lookup at an immutable UUIDv7 revision. The returned
    // keys belong to the same model-indexed snapshot as getModelAtRevisionV2().
    std::vector<Key> referencingModelAtRevisionV2(const ProjectId& project,
                                                   const RevisionId& revisionId, const Key& key);
    // All-parent ancestry; historical branches keep the source HEAD unchanged.
    bool revisionReachableFromV2(const ProjectId& project, const RevisionId& tip,
                                 const RevisionId& candidate);
    RevisionId createBranchAtRevisionV2(const ProjectId& project, std::string_view name,
                                        std::string_view sourceBranch, const RevisionId& revision);
    // The branch with this UUIDv7 regardless of its current (renamable) name;
    // nullopt when no branch has it. An unborn branch has no revision.
    std::optional<RevisionGraphHeadV2> branchByIdV2(const ProjectId& project, const BranchId& branch);
    // Create `name` from the branch identified by `source`: at its HEAD, or
    // at the exact `revision` reachable from it. `created` is the caller's
    // new branch UUIDv7. Repeating the same request returns the original
    // creation result instead of failing, so a retried request is safe;
    // `created` naming any different branch is a Conflict.
    BranchCreationResultV2 createBranchFromIdV2(const ProjectId& project, std::string_view name,
                                                const BranchId& source,
                                                std::optional<RevisionId> revision,
                                                const BranchId& created);
    BranchCreationResultV2 createBranchAtRevisionWithIdentityV2(
        const ProjectId& project, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision);
    // UUIDv7 three-way merge. Domain callbacks are pure and execute under the
    // repository lock; both explicit UUIDv7 HEAD pins are checked atomically.
    // Empty options retains the conservative native merge behavior.
    MergeResultV2 mergeBranchesV2(const ProjectId& project, std::string_view target,
                                  std::string_view source,
                                  std::optional<RevisionId> expectedTarget = std::nullopt,
                                  std::optional<RevisionId> expectedSource = std::nullopt,
                                  const MergeOptions& options = {});
    RevisionId writeRevisionV2(const ProjectId& project, const Hash& root,
                               const Hash& referencesRoot, bool modelIndexed,
                               const std::vector<RevisionId>& parents = {});
    // Verifies SHA integrity, project binding and the complete parent DAG.
    // Returns canonical v2 bytes, or nullopt for an unknown UUID.
    std::optional<Bytes> readRevisionV2(const ProjectId& project, const RevisionId& id);
    // Complete full-ancestry, identity-preserving v2 bundle over a caller-owned
    // empty regular-file descriptor. Never uses a user-derived storage filename.
    // Exports and imports are serialized with the repository lock; caller
    // handles publication/transport of the resulting file descriptor.
    void exportRevisionBundleV2(const ProjectId& project, std::string_view branch,
                                int fd, BundleLimits limits = {});
    // Incremental transfer pins the recipient's known base UUID/SHA. It does
    // not derive any path from the user-controlled branch display name.
    void exportIncrementalRevisionBundleV2(const ProjectId& project, std::string_view branch,
                                           const RevisionId& recipientBase, int fd,
                                           BundleLimits limits = {});
    RevisionBundleImportV2 importRevisionBundleV2(const ProjectId& expectedProject, int fd,
                                                  std::string_view localBranch,
                                                  BundleLimits limits = {});
    Revision head(std::string_view branch);
    // A single validated branch-file lookup, independent of how many other
    // branches are present. A symlink or a non-regular ref is never a branch.
    bool hasBranch(std::string_view branch);
    // Sorted branch names, independent of authorization. Filter at the service boundary.
    std::vector<std::string> listBranches();
    // Every published branch has a durable UUIDv7 name-independent ID.
    // Reading an ID never generates a replacement or derives it from the name.
    std::optional<BranchId> branchId(std::string_view name);
    // Pre-release UUID-only identity check for a SHA-based branch.
    // This does not mutate UUID-named branch records or migrate older stores.
    void adoptBranchId(std::string_view name, const BranchId& identity,
                       const Hash& expectedHead);
    // Immutable history: newest first along the first-parent chain. `after`
    // is an exclusive pagination cursor and must belong to this branch's
    // first-parent history. A branch advancing between pages is harmless.
    std::vector<Revision> history(std::string_view branch, std::size_t maxCount = 100,
                                  std::optional<Hash> after = std::nullopt);
    // Lookup by immutable SHA-256 revision identity, including older revisions.
    // These low-level APIs do not implement authorization or branch scoping.
    Revision revision(const Hash& revisionId);
    // All revisions reachable through either parent of the captured branch
    // HEAD set. Deterministic DFS; pages never mix HEAD snapshots.
    // No on-disk state is changed. Limit: 1..1000 revisions per page.
    RevisionGraphPage revisionGraph(std::size_t maxCount = 100,
                                    std::optional<RevisionGraphCursor> cursor = std::nullopt);
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
    // UUIDv7 revision comparison; verifies both project-bound immutable records.
    // The caller bounds materialization of changed trie keys.
    std::vector<Difference> diffRevisionsV2(const ProjectId& project,
            const RevisionId& fromRevision, const RevisionId& toRevision,
            std::size_t maxDifferences);
    // True when revisionId is the head of `branch` or any ancestor of it
    // (through either merge parent). Unknown or non-revision ids: NotFound.
    bool revisionOnBranch(std::string_view branch, const Hash& revisionId);
    // A self-contained branch snapshot for offline transfer between independent
    // native repositories, including different physical storage backends.
    // Export refuses to replace an existing bundle file.
    void exportBranchBundle(std::string_view branch, const std::filesystem::path& bundlePath,
                            BundleLimits limits = {});
    // Opt-in identity-bearing bundles (AOWSYNC3/AOWSYNC4). V1 export methods
    // above and below remain wire-compatible with frozen fixtures. Import
    // accepts both formats and preserves the embedded branch UUIDv7; it
    // rejects attempts to rebind a different existing branch identity.
    void exportIdentifiedBranchBundle(std::string_view branch,
                                      const std::filesystem::path& bundlePath,
                                      BundleLimits limits = {});
    void exportIdentifiedBranchBundle(std::string_view branch, int fd,
                                      BundleLimits limits = {});
    void exportIdentifiedIncrementalBranchBundle(std::string_view branch,
                                                 const Hash& recipientBase,
                                                 const std::filesystem::path& bundlePath,
                                                 BundleLimits limits = {});
    void exportIdentifiedIncrementalBranchBundle(std::string_view branch,
                                                 const Hash& recipientBase, int fd,
                                                 BundleLimits limits = {});
    // Delta relative to a revision already present at the recipient. The base
    // must be an ancestor (through either merge parent) of the exported head.
    // Carries the revisions not reachable from the base and, per revision, the
    // trie objects that differ from one of its parents. Reads only the base's
    // revision ancestry and the changed trie paths. May include objects the
    // recipient already holds (e.g. a restored older snapshot); those are not
    // stored twice.
    void exportIncrementalBranchBundle(std::string_view branch, const Hash& recipientBase,
                                       const std::filesystem::path& bundlePath, BundleLimits limits = {});
    // Import creates a new local branch or advances it only by fast-forward.
    // It never overwrites divergent local work or creates an implicit merge.
    // All bundle SHA-256 hashes and object graph edges are verified before
    // writing any objects or publishing a ref. Authorization is not provided.
    // Objects an incremental bundle references but does not carry must exist
    // locally; they were verified when stored and are checked only where they
    // attach to bundled objects, so the work follows the delta, not history.
    BundleImportResult importBranchBundle(const std::filesystem::path& bundlePath,
                                          std::string_view localBranch, BundleLimits limits = {},
                                          BundleImportProfile* profile = nullptr);
    // Descriptor variants (e.g. for ScratchFile spools). The caller keeps
    // ownership. Import reads the regular file fd positionally; export
    // requires an empty regular file and leaves it empty on failure.
    BundleImportResult importBranchBundle(int fd, std::string_view localBranch,
                                          BundleLimits limits = {},
                                          BundleImportProfile* profile = nullptr);
    void exportBranchBundle(std::string_view branch, int fd, BundleLimits limits = {});
    void exportIncrementalBranchBundle(std::string_view branch, const Hash& recipientBase, int fd,
                                       BundleLimits limits = {});
    // Scratch space on the repository's filesystem (see ScratchFile).
    ScratchFile createScratchFile();
    const std::filesystem::path& scratchDirectory() const noexcept;

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
    std::filesystem::path scratch_; // <directory_>/tmp
    int lockFd_ = -1;
    int lifetimeFd_ = -1;
    std::string packToken_;
    int packFd_ = -1;
    int indexFd_ = -1;
    std::mutex mutex_;
    struct HashHasher {
        std::size_t operator()(const Hash& hash) const noexcept {
            std::uint64_t value = 14695981039346656037ULL;
            for (std::uint8_t byte : hash) value = (value ^ byte) * 1099511628211ULL;
            return static_cast<std::size_t>(value);
        }
    };
    // Compact volatile accelerator over the checksummed append-only sidecar.
    // Each slot is [32-bit hash fingerprint | 32-bit 1-based entry ordinal].
    // Full SHA-256 equality and pack locations are read from objects.idx on
    // fingerprint matches. No alternative physical storage format is created.
    std::vector<std::uint64_t> indexSlots_;
    std::uint64_t indexCount_ = 0;
    struct CachedIndexEntry {
        Hash hash{};
        Location location{};
    };
    // Bounded direct-mapped hot-object cache; lookups remain correct without it.
    mutable std::vector<CachedIndexEntry> indexCache_;
    std::uint32_t indexFingerprint(const Hash& hash) const noexcept;
    void reserveIndexEntries(std::uint64_t entries);
    void indexEntry(const Hash& hash, std::uint64_t position);
    std::optional<Location> indexLocation(const Hash& hash) const;
    Hash indexedHash(std::uint32_t ordinal) const;
    void readIndexedEntry(std::uint32_t ordinal, std::uint8_t* destination) const;
    void clearIndex() noexcept;
    bool containsIndex(const Hash& hash) const;
    std::uint64_t scanOffset_ = 0;
    std::uint64_t indexReadOffset_ = 0;
    bool needsReopen_ = false; // fatal failure after publishing a compaction pointer
    std::uint64_t recoveredPackTailBytes_ = 0;
    Bytes pendingIndex_;

    void exportBundleLocked(std::string_view branch, const std::filesystem::path* bundlePath,
                            int targetFd, std::optional<Hash> recipientBase, BundleLimits limits,
                            bool includeBranchIdentity = true);
    void prepareScratchDirectory();
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
    RevisionId writeRevisionV2Locked(const ProjectId& project, const Hash& root,
                                     const Hash& referencesRoot, bool modelIndexed,
                                     const std::vector<RevisionId>& parents);
    void exportRevisionBundleV2Locked(const ProjectId& project, std::string_view branch,
                                      std::optional<RevisionId> base, int fd,
                                      BundleLimits limits);
    struct RevisionRecordV2 branchRevisionV2Locked(const ProjectId& project,
                                                   std::string_view branch);
    Hash branchHead(std::string_view name);
    std::optional<BranchId> branchIdLocked(std::string_view name);
    BranchCreationResultV2 createBranchLockedV2(const ProjectId& project, std::string_view name,
                                                std::optional<std::string_view> from,
                                                const BranchId& createdId);
    BranchCreationResultV2 createBranchAtRevisionLockedV2(const ProjectId& project,
                                                          std::string_view name,
                                                          std::string_view sourceBranch,
                                                          const RevisionId& revision,
                                                          const BranchId& createdId);
    void publishRef(std::string_view branch, const Hash& commitHash, bool mustBeAbsent,
                    std::optional<BranchId> requestedId = std::nullopt);
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
    // Incremental export: objects reachable from `tip` but not held by a
    // recipient of `base`, collected per new revision by a trie delta.
    std::set<Hash> collectDelta(const Hash& tip, const Hash& base, std::uint64_t maxObjects);
    void collectTreeDelta(const Hash& before, const Hash& after, std::set<Hash>& output,
                          std::uint64_t maxObjects);
    Revision commitLocked(std::string_view branch, const std::vector<Change>& changes,
                          std::optional<Hash> referencesRoot = std::nullopt,
                          std::optional<Hash> mergeParent = std::nullopt);
    struct PreparedModelCommit {
        std::vector<Change> encoded;
        Hash referencesRoot{};
    };
    PreparedModelCommit prepareModelCommitLocked(const Hash& root, const Hash& referencesRoot,
                                                  std::vector<ModelChange> changes);
    Hash applyEncodedChangesLocked(const Hash& original, const std::vector<Change>& changes);
    Revision commitModelLocked(std::string_view branch, std::vector<ModelChange> changes,
                               std::optional<Hash> expectedHead,
                               std::optional<Hash> mergeParent);
    std::vector<Key> readReferrers(const Hash& root, const Key& target);
    void syncPack();
};

}  // namespace aowis::dvmdb
