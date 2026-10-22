#pragma once

#include "aowis/native/native_store.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::native {

// Transport-independent catalogue and repository lifecycle API. One process
// owns a project root at a time; operations on this instance are thread-safe.
struct ProjectInfo {
    std::string id;
    std::string name;
    PhysicalStore storage = PhysicalStore::Pack;
};

enum class ProjectErrorKind { InvalidInput, NotFound, Conflict, Unavailable };
class ProjectError : public std::runtime_error {
public:
    ProjectError(ProjectErrorKind kind, std::string message);
    ProjectErrorKind kind() const noexcept;
private:
    ProjectErrorKind kind_;
};

class ProjectService final {
public:
    explicit ProjectService(std::filesystem::path root);
    ~ProjectService();
    ProjectService(const ProjectService&) = delete;
    ProjectService& operator=(const ProjectService&) = delete;

    ProjectInfo createProject(std::string_view name, PhysicalStore storage);
    std::vector<ProjectInfo> listProjects() const;
    ProjectInfo project(std::string_view id) const;
    std::vector<std::string> listBranches(std::string_view projectId);
    Revision createBranch(std::string_view projectId, std::string_view name,
                          std::optional<std::string_view> fromBranch = std::nullopt,
                          std::optional<Hash> fromRevision = std::nullopt);
    Revision head(std::string_view projectId, std::string_view branch);
    std::vector<Revision> history(std::string_view projectId, std::string_view branch,
                                  std::size_t maxCount,
                                  std::optional<Hash> after = std::nullopt);
    Revision revision(std::string_view projectId, const Hash& id);

    // Execute a scoped operation with this project's repository and the catalogue
    // mutex held. Callbacks must not retain the Repository or call ProjectService.
    // This is the transport-independent adapter boundary for model operations.
    void withRepository(std::string_view projectId,
                        const std::function<void(Repository&)>& action);

    static bool validProjectId(std::string_view id);
    static bool validBranchName(std::string_view name);
private:
    struct Entry {
        ProjectInfo info;
        std::unique_ptr<Repository> repository;
    };
    Entry& repositoryEntry(std::string_view id);
    const Entry& findEntry(std::string_view id) const;
    void ensureHealthy() const;

    std::filesystem::path root_;
    std::filesystem::path projects_;
    int lockFd_ = -1;
    mutable std::mutex mutex_;
    bool uncertain_ = false;
    std::map<std::string, Entry> entries_;
};

} // namespace aowis::native
