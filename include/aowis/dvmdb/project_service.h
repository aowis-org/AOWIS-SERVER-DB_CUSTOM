#pragma once

#include "aowis/dvmdb/native_store.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <optional>
#include <vector>

namespace aowis::dvmdb {

// Transport-independent catalogue and repository lifecycle API. One process
// owns a project root at a time; operations on this instance are thread-safe.
//
// Locking: the catalogue mutex guards only the project map and is never held
// while a repository is opened or used. Each project opens lazily under its
// own mutex. Scoped operations (withRepository, createBranch) of one project
// run one at a time under that project's operation mutex; projects never wait
// for each other. Single reads (head, history, ...) take no operation lock;
// Repository serializes its own calls internally.
struct ProjectInfo {
    std::string id;
    std::string name;
};

// Busy: the project already has its maximum number of scoped operations
// waiting or running; the caller should retry later (HTTP 503, Retry-After).
enum class ProjectErrorKind { InvalidInput, NotFound, Conflict, Unavailable, Busy };
class ProjectError : public std::runtime_error {
public:
    ProjectError(ProjectErrorKind kind, std::string message);
    ProjectErrorKind kind() const noexcept;
private:
    ProjectErrorKind kind_;
};

// VC-03 metadata. "Private" and "Shared" describe deployment intent;
// neither is authentication or an authorization capability.
enum class RepositoryRoleV1 { Unconfigured, Private, Shared };

struct RemoteRepositoryV1 {
    std::string repositoryId;      // Actual remote root UUIDv7, not a GUI profile name.
    std::string endpoint;          // Connection hint, NOT an authenticated identity.
};

struct RemoteTrackingRefV1 {
    std::string remoteRepositoryId;
    std::string projectId;         // Logical project UUIDv7 remains stable on clone.
    std::string branchId;          // Logical branch UUIDv7 remains stable on clone.
    std::optional<RevisionId> observedHead; // Null means observed unborn HEAD.
};

// VC-04: persisted, uncommitted working-copy identity. A working copy is
// private repository state, never a committed revision or mutable branch HEAD.
// Saves persist an overlay of typed model changes without advancing HEAD.
// The UUIDv7 fencing token is rotated on every lease acquisition or takeover.
struct WorkingCopyEventV1 {
    std::string id;  // UUIDv7 of a MODEL edit transaction.
    Bytes encoded;    // Exact ModelEditTransactionCodec binary frame.
};

struct WorkingCopyInfoV1 {
    std::string id;
    std::string projectId;
    std::string branchId;
    std::optional<RevisionId> baseRevision;
    std::string sessionId;
    std::uint64_t generation = 0;
    bool dirty = false;
    std::vector<ModelChange> candidateChanges;
    std::vector<WorkingCopyEventV1> journal;
    std::optional<std::string> lastSaveFingerprint; // Last journalled batch for idempotency.
    std::optional<std::string> leaseSessionId;
    std::optional<std::string> leaseToken;
    std::uint64_t leaseExpiresAtMs = 0;
};

struct WorkingCopyLeaseV1 {
    std::string token;                // UUIDv7, changes on every acquisition
    std::uint64_t expiresAtMs = 0;   // UTC epoch milliseconds
    std::uint64_t generation = 0;    // caller must pin for Save
};

class ProjectService final {
    struct Project;
public:
    // A reserved place in one project's operation queue. Reserving fails with
    // Busy when the queue is full; the place is released on destruction.
    // Lets a transport refuse work before it accepts a large request body.
    class Operation final {
    public:
        Operation(Operation&& other) noexcept;
        Operation& operator=(Operation&& other) = delete;
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        ~Operation();
    private:
        friend class ProjectService;
        explicit Operation(std::shared_ptr<Project> project) noexcept;
        std::shared_ptr<Project> project_;
    };

    // maxPendingOperations: scoped operations per project that may wait or
    // run at the same time (>= 1). Keep it below the server's worker count,
    // so one busy project can never occupy every worker.
    explicit ProjectService(std::filesystem::path root, std::size_t maxPendingOperations = 4);
    ~ProjectService();
    ProjectService(const ProjectService&) = delete;
    ProjectService& operator=(const ProjectService&) = delete;

    // Canonical native service API: revision IDs are UUIDv7, never SHA-256.
    ProjectInfo createProject(std::string_view name);
    // Explicit opt-in until all clients support nullable initial HEADs.
    // With a caller-chosen projectId (UUIDv7) the creation is idempotent: a
    // repeated request returns the existing project of that ID and name, so
    // a client may safely retry after a lost response; the same ID with a
    // different name is a Conflict.
    ProjectInfo createUnbornProjectV2(std::string_view name,
                                      std::optional<std::string_view> projectId = std::nullopt);
    // Provision an empty catalogue project with an existing logical UUIDv7.
    // Only a full verified branch bundle may populate it. Safe to retry.
    ProjectInfo provisionCloneTargetV2(std::string_view projectId, std::string_view name);
    // Import the exact identity of a root unborn branch into an empty private
    // clone target. Does not make an initial revision. Safe to retry.
    void provisionUnbornBranchV2(std::string_view projectId, std::string_view name,
                                 std::string_view branchId);
    RevisionId createBranch(std::string_view projectId, std::string_view name,
                            std::optional<std::string_view> source = std::nullopt,
                            std::optional<RevisionId> fromRevision = std::nullopt);
    RevisionId head(std::string_view projectId, std::string_view branch);
    std::vector<RevisionGraphRevisionV2> history(
        std::string_view projectId, std::string_view branch, std::size_t maxCount = 100,
        std::optional<RevisionId> after = std::nullopt);
    RevisionGraphPageV2 revisionGraph(
        std::string_view projectId, std::size_t maxCount = 100,
        std::optional<RevisionGraphCursorV2> cursor = std::nullopt);
    RevisionGraphRevisionV2 revision(std::string_view projectId, const RevisionId& id);
    void renameBranch(std::string_view projectId, std::string_view previous,
                      std::string_view renamed, const RevisionId& expectedHead);
    RevisionId commitModel(std::string_view projectId, std::string_view branch,
                           std::vector<ModelChange> changes,
                           std::optional<RevisionId> expectedHead = std::nullopt);
    MergeResultV2 mergeBranches(std::string_view projectId, std::string_view target,
                                std::string_view source,
                                std::optional<RevisionId> expectedTarget = std::nullopt,
                                std::optional<RevisionId> expectedSource = std::nullopt);
    std::optional<ModelRecord> getModelAtRevision(std::string_view projectId,
                                                  const RevisionId& revision, const Key& key);
    // Transitional UUIDv7 aliases for existing HTTP consumers.
    ProjectInfo createProjectV2(std::string_view name);
    RevisionId createBranchV2(std::string_view projectId, std::string_view name,
                              std::optional<std::string_view> source = std::nullopt);
    BranchCreationResultV2 createBranchWithIdentityV2(
        std::string_view projectId, std::string_view name,
        std::optional<std::string_view> source = std::nullopt);
    RevisionId headV2(std::string_view projectId, std::string_view branch);
    std::optional<RevisionId> optionalHeadV2(std::string_view projectId, std::string_view branch);
    std::vector<RevisionGraphRevisionV2> historyV2(
        std::string_view projectId, std::string_view branch, std::size_t maxCount = 100,
        std::optional<RevisionId> after = std::nullopt);
    RevisionGraphPageV2 revisionGraphV2(
        std::string_view projectId, std::size_t maxCount = 100,
        std::optional<RevisionGraphCursorV2> cursor = std::nullopt);
    RevisionId createBranchAtRevisionV2(std::string_view projectId, std::string_view name,
                                        std::string_view sourceBranch, const RevisionId& revision);
    BranchCreationResultV2 createBranchAtRevisionWithIdentityV2(
        std::string_view projectId, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision);
    // Branches by UUIDv7, independent of renamable names (DG-01). See
    // Repository::branchByIdV2() / createBranchFromIdV2(): the creation is
    // idempotent for the caller-chosen `createdBranchId`.
    std::optional<RevisionGraphHeadV2> branchByIdV2(std::string_view projectId,
                                                    std::string_view branchId);
    BranchCreationResultV2 createBranchFromIdV2(
        std::string_view projectId, std::string_view name, std::string_view sourceBranchId,
        std::optional<RevisionId> revision, std::string_view createdBranchId);
    MergeResultV2 mergeBranchesV2(std::string_view projectId, std::string_view target,
                                  std::string_view source,
                                  std::optional<RevisionId> expectedTarget = std::nullopt,
                                  std::optional<RevisionId> expectedSource = std::nullopt);
    RevisionId commitModelV2(std::string_view projectId, std::string_view branch,
                             std::vector<ModelChange> changes,
                             std::optional<RevisionId> expectedHead = std::nullopt);
    // Strict expected-local-HEAD CAS for VC-05 on a private repository.
    // Unlike commitModelV2(), nullopt means the branch MUST still be unborn.
    // Does not consume a draft
    // or provide operation-ID replay yet; only use under a commit coordinator.
    RevisionId commitModelAtExpectedHeadV2(std::string_view projectId,
        std::string_view branch, std::vector<ModelChange> changes,
        std::optional<RevisionId> expectedLocalHead);
    std::optional<ModelRecord> getModelAtRevisionV2(std::string_view projectId,
                                                   const RevisionId& revision, const Key& key);
    // Persistent identity of this DVMDB catalogue / repository root (UUIDv7).
    // Not a user identity and not an authorization credential.
    std::string repositoryId() const;
    // Metadata only: these calls NEVER mutate a local or remote branch HEAD.
    // Missing tracking entry differs from an observed null (unborn) HEAD.
    RepositoryRoleV1 repositoryRoleV1() const;
    void configureRepositoryRoleV1(RepositoryRoleV1 role);
    std::vector<RemoteRepositoryV1> remoteRepositoriesV1() const;
    // Re-registering the same ID/endpoint is idempotent. Conflicting endpoint
    // or a remote equal to this repository UUID is rejected.
    void registerRemoteRepositoryV1(std::string_view remoteRepositoryId,
                                    std::string_view endpoint);
    std::vector<RemoteTrackingRefV1> remoteTrackingRefsV1() const;
    // expectedObservedHead: nullopt => no tracking record exists;
    // engaged with null inner value => record exists with unborn HEAD;
    // engaged with revision => record exists with that exact HEAD.
    // The caller must obtain the observation from a verified fetch; recording
    // it does not verify an endpoint or grant access to that endpoint.
    void recordRemoteTrackingRefV1(
        const RemoteTrackingRefV1& observation,
        std::optional<std::optional<RevisionId>> expectedObservedHead);
    // Begin a durable, empty draft at an exact currently observed local HEAD.
    // An unborn branch supplies null base. Only a private repository can
    // create drafts. Does not commit or change any branch HEAD.
    WorkingCopyInfoV1 createWorkingCopyV1(
        std::string_view projectId, std::string_view branch,
        std::optional<RevisionId> baseRevision, std::string_view sessionId);
    // The same, addressing the branch by UUIDv7. With a caller-chosen draftId
    // the creation is idempotent: a repeated request returns the existing
    // draft if project, branch, base and session match; otherwise Conflict.
    WorkingCopyInfoV1 createWorkingCopyForBranchV1(
        std::string_view projectId, std::string_view branchId,
        std::optional<RevisionId> baseRevision, std::string_view sessionId,
        std::optional<std::string_view> draftId = std::nullopt);
    WorkingCopyInfoV1 workingCopyV1(std::string_view draftId) const;
    std::vector<WorkingCopyInfoV1> listWorkingCopiesV1() const;
    // Only one nonexpired writer may hold a draft lease. The same session
    // may reacquire, invalidating its previous token. Expired leases may be
    // explicitly taken over by a different session; no candidate is lost.
    WorkingCopyLeaseV1 acquireWorkingCopyLeaseV1(
        std::string_view draftId, std::string_view sessionId, std::uint32_t seconds);
    // All changes are applied atomically to the draft's candidate overlay.
    // The full resulting record is fsynced before this returns; Save never
    // touches committed HEADs or immutable revisions.
    WorkingCopyInfoV1 saveWorkingCopyChangesV1(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes);
    // Atomically persist MODEL transaction frames and the corresponding candidate
    // model changes. Original event UUIDs are retained across retries. A replay
    // of the immediately preceding identical batch is idempotent.
    WorkingCopyInfoV1 saveWorkingCopyTransactionsV1(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes,
        const std::vector<Bytes>& encodedTransactions,
        // Optional exact logical request fingerprint for server-materialized
        // membership writes. Remains stable across dropped responses even
        // though native chunk records are derived from the previous draft.
        std::optional<std::string> logicalRequestFingerprint = std::nullopt);
    void releaseWorkingCopyLeaseV1(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken);
    std::vector<ProjectInfo> listProjects() const;
    ProjectInfo project(std::string_view id) const;
    std::vector<std::string> listBranches(std::string_view projectId);
    // Unconverted legacy and v1-imported branch references return nullopt.
    std::optional<BranchId> branchId(std::string_view projectId, std::string_view branch);
    // SHA-based native fixtures are retained until old pack/storage tests are
    // migrated. They are NOT the canonical application interface and must
    // never be invoked on a UUIDv7 project's repository.
    ProjectInfo createProjectShaFixture(std::string_view name);
    void adoptBranchIdShaFixture(std::string_view projectId, std::string_view branch,
                                 const BranchId& identity, const Hash& expectedHead);
    Revision createBranchShaFixture(std::string_view projectId, std::string_view name,
                                    std::optional<std::string_view> fromBranch = std::nullopt,
                                    std::optional<Hash> fromRevision = std::nullopt);
    void renameBranchShaFixture(std::string_view projectId, std::string_view previous,
                                std::string_view renamed, const Hash& expectedHead);
    Revision headShaFixture(std::string_view projectId, std::string_view branch);
    std::vector<Revision> historyShaFixture(std::string_view projectId, std::string_view branch,
                                           std::size_t maxCount,
                                           std::optional<Hash> after = std::nullopt);
    Revision revisionShaFixture(std::string_view projectId, const Hash& id);
    RevisionGraphPage revisionGraphShaFixture(std::string_view projectId, std::size_t maxCount = 100,
                                              std::optional<RevisionGraphCursor> cursor = std::nullopt);

    // Execute a scoped operation with this project's repository and its
    // operation mutex held (other projects are not blocked). Callbacks must
    // not retain the Repository or call ProjectService. This is the
    // transport-independent adapter boundary for model operations.
    // Throws ProjectError(Busy) when the project's queue is full.
    void withRepository(std::string_view projectId,
                        const std::function<void(Repository&)>& action);
    Operation reserveOperation(std::string_view projectId);
    void withRepository(Operation& operation, const std::function<void(Repository&)>& action);
    // Scoped operations of this project currently waiting or running.
    std::size_t pendingOperations(std::string_view projectId) const;
    std::size_t maxPendingOperations() const noexcept;
    // Nameless scratch file in the project's repository (for streaming
    // uploads). The catalogue lock is held only while creating it.
    ScratchFile createScratchFile(std::string_view projectId);

    static bool validProjectId(std::string_view id);
    static bool validBranchName(std::string_view name);
private:
    struct Project {
        explicit Project(ProjectInfo value);
        const ProjectInfo info;
        std::mutex openMutex;                    // guards repository
        std::shared_ptr<Repository> repository;  // opened on first use
        std::mutex operationMutex;               // one scoped operation at a time
        std::atomic<std::size_t> pending{0};     // reserved Operations
    };
    // Looks the project up under the catalogue mutex only.
    std::shared_ptr<Project> findProject(std::string_view id) const;
    std::shared_ptr<Repository> openRepository(Project& project) const;
    void ensureHealthy() const;
    // requestedProjectId: caller-chosen UUIDv7, idempotent by (ID, name).
    // With unborn == false it provisions a clone target (no branch at all).
    ProjectInfo createProjectImpl(std::string_view name, bool v2, bool unborn = false,
                                  std::optional<std::string> requestedProjectId = std::nullopt);

    std::filesystem::path root_;
    std::string repository_id_;
    RepositoryRoleV1 repository_role_ = RepositoryRoleV1::Unconfigured;
    std::map<std::string, RemoteRepositoryV1> remotes_;
    using TrackingKey = std::tuple<std::string, std::string, std::string>;
    std::map<TrackingKey, RemoteTrackingRefV1> tracking_;
    void persistTrackingMetadata(); // called with catalogue mutex held
    std::filesystem::path projects_;
    int lockFd_ = -1;
    const std::size_t maxPending_;
    mutable std::mutex mutex_;                   // catalogue: entries_, uncertain_
    bool uncertain_ = false;
    std::map<std::string, std::shared_ptr<Project>> entries_;
};

} // namespace aowis::dvmdb
