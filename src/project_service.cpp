#include "branch_name_validation.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace aowis::dvmdb {
namespace {

// Catalogue metadata on disk violating its format is storage corruption.
void check(bool ok, const std::string& message,
           StorageErrorKind kind = StorageErrorKind::Corruption) {
    if (!ok) throw StorageError(kind, message);
}

void syscallChecked(bool ok, const char* action) {
    if (!ok) throwSystemError(action);
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > 200) return false;
    // Reject malformed UTF-8, overlong sequences, surrogates and scalar values
    // above U+10FFFF. A name must round-trip through the JSON HTTP boundary.
    for (std::size_t i = 0; i < name.size();) {
        const unsigned char first = static_cast<unsigned char>(name[i]);
        if (first < 0x80U) {
            if (first < 0x20U || first == 0x7fU) return false;
            ++i;
            continue;
        }
        unsigned count = 0;
        if (first >= 0xc2U && first <= 0xdfU) count = 2;
        else if (first >= 0xe0U && first <= 0xefU) count = 3;
        else if (first >= 0xf0U && first <= 0xf4U) count = 4;
        else return false;
        if (i + count > name.size()) return false;
        for (unsigned step = 1; step < count; ++step) {
            const unsigned char byte = static_cast<unsigned char>(name[i + step]);
            if ((byte & 0xc0U) != 0x80U) return false;
        }
        const unsigned char second = static_cast<unsigned char>(name[i + 1]);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second > 0x9fU) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU)) return false;
        i += count;
    }
    return true;
}

std::string encodeName(std::string_view name) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(name.size() * 2);
    for (unsigned char ch : name) {
        value.push_back(digits[ch >> 4]);
        value.push_back(digits[ch & 15U]);
    }
    return value;
}

std::string decodeName(const std::string& encoded) {
    check(encoded.size() % 2 == 0 && encoded.size() <= 400,
          "invalid project metadata name encoding");
    std::string output;
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        const std::string pair = encoded.substr(i, 2);
        check(std::all_of(pair.begin(), pair.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }), "invalid project metadata name encoding");
        output.push_back(static_cast<char>(std::stoi(pair, nullptr, 16)));
    }
    check(validName(output), "invalid project metadata name");
    return output;
}

void syncDirectory(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    syscallChecked(fd >= 0, "open directory for sync");
    const int status = ::fsync(fd);
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    syscallChecked(status == 0, "sync project directory");
}

void writeManifest(const std::filesystem::path& path, const ProjectInfo& info) {
    const std::string body = "AOWIS-PROJECT-1\n" + info.id + "\n" +
        "pack\n" +
        encodeName(info.name) + "\n";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "create project manifest");
    try {
        std::size_t written = 0;
        while (written < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + written, body.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write project manifest");
            written += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync project manifest");
    } catch (...) {
        ::close(fd);
        throw;
    }
    syscallChecked(::close(fd) == 0, "close project manifest");
}

ProjectInfo readManifest(const std::filesystem::path& projectDirectory) {
    const std::filesystem::path path = projectDirectory / "project.meta";
    check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
          "missing or unsafe project manifest: " + path.string());
    check(std::filesystem::file_size(path) <= 512,
          "oversized project manifest: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "cannot read project manifest");
    const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::size_t first = body.find('\n');
    const std::size_t second = first == std::string::npos ? first : body.find('\n', first + 1);
    const std::size_t third = second == std::string::npos ? second : body.find('\n', second + 1);
    check(first != std::string::npos && second != std::string::npos &&
          third != std::string::npos && body.back() == '\n' &&
          body.find('\n', third + 1) == body.size() - 1 &&
          body.substr(0, first) == "AOWIS-PROJECT-1", "invalid project manifest format");
    ProjectInfo info;
    info.id = body.substr(first + 1, second - first - 1);
    check(ProjectService::validProjectId(info.id) &&
          projectDirectory.filename().string() == info.id, "project manifest UUID mismatch");
    const std::string format = body.substr(second + 1, third - second - 1);
    check(format == "pack", "unsupported project storage format");
    info.name = decodeName(body.substr(third + 1, body.size() - third - 2));
    return info;
}

// ProjectService methods report client mistakes as ProjectError (their
// documented contract). Server-side StorageError kinds (corruption, I/O,
// exhausted storage, internal) propagate unchanged.
template <typename Result, typename Function>
Result translated(Function&& function) {
    try {
        return function();
    } catch (const StorageError& error) {
        switch (error.kind()) {
        case StorageErrorKind::InvalidInput:
            throw ProjectError(ProjectErrorKind::InvalidInput, error.what());
        case StorageErrorKind::NotFound:
            throw ProjectError(ProjectErrorKind::NotFound, error.what());
        case StorageErrorKind::Conflict:
            throw ProjectError(ProjectErrorKind::Conflict, error.what());
        case StorageErrorKind::Unavailable:
            throw ProjectError(ProjectErrorKind::Unavailable, error.what());
        default:
            throw;
        }
    }
}

} // namespace

ProjectError::ProjectError(ProjectErrorKind kind, std::string message)
    : std::runtime_error(std::move(message)), kind_(kind) {}
ProjectErrorKind ProjectError::kind() const noexcept { return this->kind_; }

bool ProjectService::validProjectId(std::string_view id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') ||
                     (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

bool ProjectService::validBranchName(std::string_view name) {
    return detail::validBranchLabel(name);
}

ProjectService::Project::Project(ProjectInfo value) : info(std::move(value)) {}

ProjectService::Operation::Operation(std::shared_ptr<Project> project) noexcept
    : project_(std::move(project)) {}

ProjectService::Operation::Operation(Operation&& other) noexcept
    : project_(std::move(other.project_)) {}

ProjectService::Operation::~Operation() {
    if (this->project_) this->project_->pending.fetch_sub(1);
}

ProjectService::ProjectService(std::filesystem::path root, std::size_t maxPendingOperations)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()),
      projects_(this->root_ / "projects"), maxPending_(maxPendingOperations) {
    if (this->maxPending_ == 0) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                    "at least one pending operation per project is required");
    std::filesystem::create_directories(this->projects_);
    // Persist the newly created projects/ entry before project publication.
    syncDirectory(this->root_);
    this->lockFd_ = ::open((this->root_ / "catalogue.lock").c_str(),
                           O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(this->lockFd_ >= 0, "open project catalogue lock");
    if (::flock(this->lockFd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(this->lockFd_);
        this->lockFd_ = -1;
        throw ProjectError(ProjectErrorKind::Conflict,
                           "another database server owns this project root");
    }
    try {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(this->projects_)) {
            const std::string id = entry.path().filename().string();
            // A crash during creation may leave a staging directory. Never
            // advertise it as a project and never reuse its name.
            if (id.starts_with(".creating-")) continue;
            check(validProjectId(id) && !entry.is_symlink() && entry.is_directory(),
                  "unexpected entry in project catalogue: " + entry.path().string());
            this->entries_.emplace(id, std::make_shared<Project>(readManifest(entry.path())));
        }
    } catch (...) {
        ::flock(this->lockFd_, LOCK_UN);
        ::close(this->lockFd_);
        this->lockFd_ = -1;
        throw;
    }
}

ProjectService::~ProjectService() {
    this->entries_.clear();
    if (this->lockFd_ >= 0) {
        ::flock(this->lockFd_, LOCK_UN);
        ::close(this->lockFd_);
    }
}

// Called with the catalogue mutex held.
void ProjectService::ensureHealthy() const {
    if (this->uncertain_) throw ProjectError(ProjectErrorKind::Unavailable,
        "catalogue publication status is uncertain; restart the database server");
}

std::size_t ProjectService::maxPendingOperations() const noexcept {
    return this->maxPending_;
}

ProjectInfo ProjectService::createProject(std::string_view name) {
    return createProjectImpl(name, true);
}

ProjectInfo ProjectService::createUnbornProjectV2(std::string_view name) {
    return createProjectImpl(name, true, true);
}

ProjectInfo ProjectService::createProjectShaFixture(std::string_view name) {
    return createProjectImpl(name, false);
}

ProjectInfo ProjectService::createProjectV2(std::string_view name) {
    return createProjectImpl(name, true);
}

ProjectInfo ProjectService::createProjectImpl(std::string_view name, bool v2, bool unborn) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!validName(name)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                            "project name must contain 1..200 non-control bytes");
    ProjectInfo info;
    info.name = std::string(name);
    do {
        try { info.id = ProjectId::generate().toString(); }
        catch (const std::runtime_error& error) {
            throw StorageError(StorageErrorKind::Internal,
                               std::string("failed to generate secure UUIDv7 project ID: ") + error.what());
        }
    }
    while (std::filesystem::exists(this->projects_ / info.id) ||
           std::filesystem::exists(this->projects_ / (".creating-" + info.id)));
    const std::filesystem::path staging = this->projects_ / (".creating-" + info.id);
    const std::filesystem::path published = this->projects_ / info.id;
    std::filesystem::create_directory(staging);
    bool renamed = false;
    try {
        writeManifest(staging / "project.meta", info);
        {
            Repository repository(staging / "repository");
            if (v2) {
                const std::optional<ProjectId> project = ProjectId::parse(info.id);
                if (!project) throw StorageError(StorageErrorKind::Internal,
                                                 "new project UUIDv7 did not validate");
                if (unborn) repository.createUnbornBranchV2(*project, "main");
                else (void)repository.createBranchV2(*project, "main");
            } else {
                repository.createBranch("main");
            }
        }
        syncDirectory(staging / "repository" / "refs" / "heads");
        syncDirectory(staging / "repository" / "refs");
        syncDirectory(staging / "repository");
        syncDirectory(staging);
        syscallChecked(::rename(staging.c_str(), published.c_str()) == 0,
                       "publish project directory");
        renamed = true;
        syncDirectory(this->projects_);
        this->entries_.emplace(info.id, std::make_shared<Project>(info));
    } catch (...) {
        if (renamed) {
            this->uncertain_ = true;
        } else {
            std::error_code ignored; // never replace the original exception
            std::filesystem::remove_all(staging, ignored);
        }
        throw;
    }
    return info;
}

std::shared_ptr<ProjectService::Project> ProjectService::findProject(std::string_view id) const {
    if (!validProjectId(id)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                "invalid project UUID");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    const std::map<std::string, std::shared_ptr<Project>>::const_iterator it =
        this->entries_.find(std::string(id));
    if (it == this->entries_.end())
        throw ProjectError(ProjectErrorKind::NotFound, "project not found");
    return it->second;
}

// Opening scans the pack tail and loads the index; only this project waits.
std::shared_ptr<Repository> ProjectService::openRepository(Project& project) const {
    std::lock_guard<std::mutex> guard(project.openMutex);
    if (!project.repository) {
        const std::filesystem::path directory = this->projects_ / project.info.id / "repository";
        check(!std::filesystem::is_symlink(directory) &&
              std::filesystem::is_directory(directory), "project repository directory is unavailable");
        project.repository = std::make_shared<Repository>(directory);
        const std::uint64_t recovered = project.repository->recoveredPackTailBytes();
        if (recovered != 0) {
            std::cerr << "AOWIS database: project " << project.info.id << " discarded "
                      << recovered << " bytes of an unpublished torn pack tail; a copy is in "
                      << (directory / "recovered-pack-tails").string() << '\n';
        }
    }
    return project.repository;
}

std::vector<ProjectInfo> ProjectService::listProjects() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<ProjectInfo> result;
    result.reserve(this->entries_.size());
    for (const std::pair<const std::string, std::shared_ptr<Project>>& entry : this->entries_)
        result.push_back(entry.second->info);
    return result;
}

ProjectInfo ProjectService::project(std::string_view id) const {
    return findProject(id)->info;
}

std::vector<std::string> ProjectService::listBranches(std::string_view projectId) {
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<std::vector<std::string>>([&]() {
        return openRepository(*found)->listBranches();
    });
}

std::optional<BranchId> ProjectService::branchId(std::string_view projectId,
                                                  std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<std::optional<BranchId>>([&] {
        return openRepository(*found)->branchId(branch);
    });
}

void ProjectService::adoptBranchIdShaFixture(std::string_view projectId, std::string_view branch,
                                   const BranchId& identity, const Hash& expectedHead) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    withRepository(projectId, [&](Repository& repository) {
        repository.adoptBranchId(branch, identity, expectedHead);
    });
}

Revision ProjectService::createBranchShaFixture(std::string_view projectId, std::string_view name,
                                      std::optional<std::string_view> fromBranch,
                                      std::optional<Hash> fromRevision) {
    if (!validBranchName(name) || (fromBranch && !validBranchName(*fromBranch)) ||
        (fromRevision && !fromBranch))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid branch name or revision source");
    Revision created;
    // Check and creation form one operation of this project.
    withRepository(projectId, [&](Repository& repository) {
        if (repository.hasBranch(name))
            throw ProjectError(ProjectErrorKind::Conflict, "branch already exists");
        if (fromBranch && !repository.hasBranch(*fromBranch))
            throw ProjectError(ProjectErrorKind::NotFound, "source branch not found");
        created = translated<Revision>([&]() {
            if (fromRevision) repository.createBranchAtRevision(name, *fromBranch, *fromRevision);
            else repository.createBranch(name, fromBranch);
            return repository.head(name);
        });
    });
    return created;
}

RevisionId ProjectService::createBranchV2(std::string_view projectId,
              std::string_view name, std::optional<std::string_view> source) {
    return createBranchWithIdentityV2(projectId, name, source).revisionId;
}

BranchCreationResultV2 ProjectService::createBranchWithIdentityV2(
        std::string_view projectId, std::string_view name,
        std::optional<std::string_view> source) {
    if (!validBranchName(name) || (source && !validBranchName(*source)))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    std::optional<BranchCreationResultV2> created;
    withRepository(projectId, [&](Repository& repository) {
        created = translated<BranchCreationResultV2>([&]() {
            return repository.createBranchWithIdentityV2(*project, name, source);
        });
    });
    return *created;
}

RevisionId ProjectService::createBranchAtRevisionV2(std::string_view projectId,
                std::string_view name, std::string_view sourceBranch, const RevisionId& revision) {
    return createBranchAtRevisionWithIdentityV2(projectId, name, sourceBranch, revision).revisionId;
}

BranchCreationResultV2 ProjectService::createBranchAtRevisionWithIdentityV2(
        std::string_view projectId, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision) {
    if (!validBranchName(name) || !validBranchName(sourceBranch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    std::optional<BranchCreationResultV2> created;
    withRepository(projectId, [&](Repository& repository) {
        created = translated<BranchCreationResultV2>([&]() {
            return repository.createBranchAtRevisionWithIdentityV2(*project, name, sourceBranch, revision);
        });
    });
    return *created;
}

MergeResultV2 ProjectService::mergeBranchesV2(std::string_view projectId,
                std::string_view target, std::string_view source,
                std::optional<RevisionId> expectedTarget,
                std::optional<RevisionId> expectedSource) {
    if (!validBranchName(target) || !validBranchName(source))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    MergeResultV2 result;
    withRepository(projectId, [&](Repository& repository) {
        result = translated<MergeResultV2>([&]() {
            return repository.mergeBranchesV2(*project, target, source,
                                               expectedTarget, expectedSource);
        });
    });
    return result;
}

std::optional<RevisionId> ProjectService::optionalHeadV2(std::string_view projectId, std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<std::optional<RevisionId>>([&]() {
        return openRepository(*entry)->optionalHeadV2(*project, branch);
    });
}

RevisionId ProjectService::headV2(std::string_view projectId, std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<RevisionId>([&]() { return openRepository(*entry)->headV2(*project, branch); });
}

std::vector<RevisionGraphRevisionV2> ProjectService::historyV2(
        std::string_view projectId, std::string_view branch, std::size_t maxCount,
        std::optional<RevisionId> after) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<std::vector<RevisionGraphRevisionV2>>([&]() {
        return openRepository(*entry)->historyV2(*project, branch, maxCount, after);
    });
}

RevisionGraphPageV2 ProjectService::revisionGraphV2(
        std::string_view projectId, std::size_t maxCount,
        std::optional<RevisionGraphCursorV2> cursor) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<RevisionGraphPageV2>([&]() {
        return openRepository(*entry)->revisionGraphV2(*project, maxCount, std::move(cursor));
    });
}

RevisionId ProjectService::commitModelV2(std::string_view projectId,
                          std::string_view branch, std::vector<ModelChange> changes,
                          std::optional<RevisionId> expectedHead) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    RevisionId committed = RevisionId::generate();
    withRepository(projectId, [&](Repository& repository) {
        committed = translated<RevisionId>([&]() {
            return repository.commitModelV2(*project, branch, std::move(changes), expectedHead);
        });
    });
    return committed;
}

std::optional<ModelRecord> ProjectService::getModelAtRevisionV2(
        std::string_view projectId, const RevisionId& revisionId, const Key& key) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<std::optional<ModelRecord>>([&]() {
        return openRepository(*entry)->getModelAtRevisionV2(*project, revisionId, key);
    });
}

RevisionId ProjectService::createBranch(std::string_view projectId, std::string_view name,
          std::optional<std::string_view> source, std::optional<RevisionId> fromRevision) {
    if (fromRevision) {
        if (!source)
            throw ProjectError(ProjectErrorKind::InvalidInput,
                               "historical branch source requires a source branch");
        return createBranchAtRevisionV2(projectId, name, *source, *fromRevision);
    }
    return createBranchV2(projectId, name, source);
}

RevisionId ProjectService::head(std::string_view projectId, std::string_view branch) {
    return headV2(projectId, branch);
}

std::vector<RevisionGraphRevisionV2> ProjectService::history(
        std::string_view projectId, std::string_view branch, std::size_t maxCount,
        std::optional<RevisionId> after) {
    if (maxCount == 0 || maxCount > 1000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid history limit");
    return historyV2(projectId, branch, maxCount, after);
}

RevisionGraphPageV2 ProjectService::revisionGraph(
        std::string_view projectId, std::size_t maxCount,
        std::optional<RevisionGraphCursorV2> cursor) {
    if (maxCount == 0 || maxCount > 1000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid revision graph limit");
    return revisionGraphV2(projectId, maxCount, std::move(cursor));
}

RevisionGraphRevisionV2 ProjectService::revision(std::string_view projectId, const RevisionId& id) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<RevisionGraphRevisionV2>([&]() {
        const std::optional<Bytes> bytes = openRepository(*found)->readRevisionV2(*project, id);
        if (!bytes) throw ProjectError(ProjectErrorKind::NotFound, "revision not found");
        const RevisionRecordV2 record = decodeRevisionRecordV2(*bytes);
        RevisionGraphRevisionV2 result{record.id, hashRevisionRecordV2(record),
                                      record.root, record.referencesRoot, record.modelIndexed, {}};
        result.parents.reserve(record.parents.size());
        for (const RevisionParentV2& parent : record.parents)
            result.parents.push_back(RevisionGraphParentV2{parent.id, parent.expectedRecordHash});
        return result;
    });
}

void ProjectService::renameBranch(std::string_view projectId, std::string_view previous,
                                  std::string_view renamed, const RevisionId& expectedHead) {
    if (!validBranchName(previous) || !validBranchName(renamed))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    withRepository(projectId, [&](Repository& repository) {
        translated<int>([&]() {
            repository.renameBranchV2(*project, previous, renamed, expectedHead);
            return 0;
        });
    });
}

RevisionId ProjectService::commitModel(std::string_view projectId, std::string_view branch,
                  std::vector<ModelChange> changes, std::optional<RevisionId> expectedHead) {
    return commitModelV2(projectId, branch, std::move(changes), expectedHead);
}

MergeResultV2 ProjectService::mergeBranches(std::string_view projectId,
                  std::string_view target, std::string_view source,
                  std::optional<RevisionId> expectedTarget,
                  std::optional<RevisionId> expectedSource) {
    return mergeBranchesV2(projectId, target, source, expectedTarget, expectedSource);
}

std::optional<ModelRecord> ProjectService::getModelAtRevision(
                  std::string_view projectId, const RevisionId& revision, const Key& key) {
    return getModelAtRevisionV2(projectId, revision, key);
}

void ProjectService::renameBranchShaFixture(std::string_view projectId, std::string_view previous,
                                  std::string_view renamed, const Hash& expectedHead) {
    if (!validBranchName(previous) || !validBranchName(renamed))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    withRepository(projectId, [&](Repository& repository) {
        translated<int>([&]() { repository.renameBranch(previous, renamed, expectedHead); return 0; });
    });
}

Revision ProjectService::headShaFixture(std::string_view projectId, std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<Revision>([&]() {
        const std::shared_ptr<Repository> repository = openRepository(*found);
        if (!repository->hasBranch(branch))
            throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
        return repository->head(branch);
    });
}

std::vector<Revision> ProjectService::historyShaFixture(std::string_view projectId,
                    std::string_view branch, std::size_t maxCount, std::optional<Hash> after) {
    if (!validBranchName(branch) || maxCount == 0 || maxCount > 1000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch or history limit");
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<std::vector<Revision>>([&]() {
        const std::shared_ptr<Repository> repository = openRepository(*found);
        if (!repository->hasBranch(branch))
            throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
        return repository->history(branch, maxCount, after);
    });
}

RevisionGraphPage ProjectService::revisionGraphShaFixture(std::string_view projectId,
                                                    std::size_t maxCount,
                                                    std::optional<RevisionGraphCursor> cursor) {
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<RevisionGraphPage>([&]() {
        return openRepository(*found)->revisionGraph(maxCount, std::move(cursor));
    });
}

Revision ProjectService::revisionShaFixture(std::string_view projectId, const Hash& id) {
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<Revision>([&]() { return openRepository(*found)->revision(id); });
}

ScratchFile ProjectService::createScratchFile(std::string_view projectId) {
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<ScratchFile>([&]() { return openRepository(*found)->createScratchFile(); });
}

ProjectService::Operation ProjectService::reserveOperation(std::string_view projectId) {
    std::shared_ptr<Project> found = findProject(projectId);
    if (found->pending.fetch_add(1) >= this->maxPending_) {
        found->pending.fetch_sub(1);
        throw ProjectError(ProjectErrorKind::Busy,
                           "project has too many pending operations; retry later");
    }
    return Operation(std::move(found));
}

void ProjectService::withRepository(Operation& operation,
                                    const std::function<void(Repository&)>& action) {
    if (!operation.project_) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                "operation reservation was moved from");
    const std::shared_ptr<Project> project = operation.project_;
    std::lock_guard<std::mutex> guard(project->operationMutex);
    const std::shared_ptr<Repository> repository =
        translated<std::shared_ptr<Repository>>([&]() { return openRepository(*project); });
    action(*repository);
}

void ProjectService::withRepository(std::string_view projectId,
                                    const std::function<void(Repository&)>& action) {
    Operation operation = reserveOperation(projectId);
    withRepository(operation, action);
}

std::size_t ProjectService::pendingOperations(std::string_view projectId) const {
    return findProject(projectId)->pending.load();
}

} // namespace aowis::dvmdb
