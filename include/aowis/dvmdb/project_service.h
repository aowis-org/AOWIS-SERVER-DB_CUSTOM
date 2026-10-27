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
    RevisionId headV2(std::string_view projectId, std::string_view branch);
    std::vector<RevisionGraphRevisionV2> historyV2(
        std::string_view projectId, std::string_view branch, std::size_t maxCount = 100,
        std::optional<RevisionId> after = std::nullopt);
    RevisionGraphPageV2 revisionGraphV2(
        std::string_view projectId, std::size_t maxCount = 100,
        std::optional<RevisionGraphCursorV2> cursor = std::nullopt);
    RevisionId createBranchAtRevisionV2(std::string_view projectId, std::string_view name,
                                        std::string_view sourceBranch, const RevisionId& revision);
    MergeResultV2 mergeBranchesV2(std::string_view projectId, std::string_view target,
                                  std::string_view source,
                                  std::optional<RevisionId> expectedTarget = std::nullopt,
                                  std::optional<RevisionId> expectedSource = std::nullopt);
    RevisionId commitModelV2(std::string_view projectId, std::string_view branch,
                             std::vector<ModelChange> changes,
                             std::optional<RevisionId> expectedHead = std::nullopt);
    std::optional<ModelRecord> getModelAtRevisionV2(std::string_view projectId,
                                                   const RevisionId& revision, const Key& key);
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
    ProjectInfo createProjectImpl(std::string_view name, bool v2);

    std::filesystem::path root_;
    std::filesystem::path projects_;
    int lockFd_ = -1;
    const std::size_t maxPending_;
    mutable std::mutex mutex_;                   // catalogue: entries_, uncertain_
    bool uncertain_ = false;
    std::map<std::string, std::shared_ptr<Project>> entries_;
};

} // namespace aowis::dvmdb
