#include "aowis/native/project_service.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sys/file.h>
#include <unistd.h>

namespace aowis::native {
namespace {

// Catalogue metadata on disk violating its format is storage corruption.
void check(bool ok, const std::string& message,
           StorageErrorKind kind = StorageErrorKind::Corruption) {
    if (!ok) throw StorageError(kind, message);
}

void syscallChecked(bool ok, const char* action) {
    if (!ok) throwSystemError(action);
}

std::string randomId() {
    std::array<unsigned char, 16> bytes{};
    check(RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1,
          "failed to generate secure project ID", StorageErrorKind::Internal);
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result.push_back('-');
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 15U]);
    }
    return result;
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
    info.storage = PhysicalStore::Pack;
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
    if (name.empty() || name.size() > 96) return false;
    for (char ch : name) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    return true;
}

ProjectService::ProjectService(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()),
      projects_(this->root_ / "projects") {
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
            ProjectInfo info = readManifest(entry.path());
            this->entries_.emplace(id, Entry{std::move(info), nullptr});
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

void ProjectService::ensureHealthy() const {
    if (this->uncertain_) throw ProjectError(ProjectErrorKind::Unavailable,
        "catalogue publication status is uncertain; restart the database server");
}

ProjectInfo ProjectService::createProject(std::string_view name, PhysicalStore storage) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!validName(name)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                            "project name must contain 1..200 non-control bytes");
    if (storage != PhysicalStore::Pack)
        throw ProjectError(ProjectErrorKind::InvalidInput, "unsupported storage backend");
    ProjectInfo info;
    info.name = std::string(name);
    info.storage = storage;
    do { info.id = randomId(); }
    while (std::filesystem::exists(this->projects_ / info.id) ||
           std::filesystem::exists(this->projects_ / (".creating-" + info.id)));
    const std::filesystem::path staging = this->projects_ / (".creating-" + info.id);
    const std::filesystem::path published = this->projects_ / info.id;
    std::filesystem::create_directory(staging);
    bool renamed = false;
    try {
        writeManifest(staging / "project.meta", info);
        {
            Repository repository(staging / "repository", storage);
            repository.createBranch("main");
        }
        syncDirectory(staging / "repository" / "refs" / "heads");
        syncDirectory(staging / "repository" / "refs");
        syncDirectory(staging / "repository");
        syncDirectory(staging);
        syscallChecked(::rename(staging.c_str(), published.c_str()) == 0,
                       "publish project directory");
        renamed = true;
        syncDirectory(this->projects_);
        this->entries_.emplace(info.id, Entry{info, nullptr});
    } catch (...) {
        if (renamed) this->uncertain_ = true;
        else std::filesystem::remove_all(staging);
        throw;
    }
    return info;
}

const ProjectService::Entry& ProjectService::findEntry(std::string_view id) const {
    if (!validProjectId(id)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                "invalid project UUID");
    const std::map<std::string, Entry>::const_iterator it = this->entries_.find(std::string(id));
    if (it == this->entries_.end())
        throw ProjectError(ProjectErrorKind::NotFound, "project not found");
    return it->second;
}

ProjectService::Entry& ProjectService::repositoryEntry(std::string_view id) {
    ensureHealthy();
    findEntry(id);
    Entry& entry = this->entries_.at(std::string(id));
    if (!entry.repository) {
        const std::filesystem::path directory = this->projects_ / entry.info.id / "repository";
        check(!std::filesystem::is_symlink(directory) &&
              std::filesystem::is_directory(directory), "project repository directory is unavailable");
        entry.repository = std::make_unique<Repository>(directory, entry.info.storage);
        const std::uint64_t recovered = entry.repository->recoveredPackTailBytes();
        if (recovered != 0) {
            std::cerr << "AOWIS database: project " << entry.info.id << " discarded "
                      << recovered << " bytes of an unpublished torn pack tail; a copy is in "
                      << (directory / "recovered-pack-tails").string() << '\n';
        }
    }
    return entry;
}

std::vector<ProjectInfo> ProjectService::listProjects() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<ProjectInfo> result;
    result.reserve(this->entries_.size());
    for (const std::pair<const std::string, Entry>& entry : this->entries_)
        result.push_back(entry.second.info);
    return result;
}

ProjectInfo ProjectService::project(std::string_view id) const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    return findEntry(id).info;
}

std::vector<std::string> ProjectService::listBranches(std::string_view projectId) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    return translated<std::vector<std::string>>([&]() {
        return repositoryEntry(projectId).repository->listBranches();
    });
}

Revision ProjectService::createBranch(std::string_view projectId, std::string_view name,
                                      std::optional<std::string_view> fromBranch,
                                      std::optional<Hash> fromRevision) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    if (!validBranchName(name) || (fromBranch && !validBranchName(*fromBranch)) ||
        (fromRevision && !fromBranch))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid branch name or revision source");
    Repository& repository = *repositoryEntry(projectId).repository;
    const std::vector<std::string> branches = repository.listBranches();
    if (std::find(branches.begin(), branches.end(), std::string(name)) != branches.end()) {
        throw ProjectError(ProjectErrorKind::Conflict, "branch already exists");
    }
    if (fromBranch && std::find(branches.begin(), branches.end(), *fromBranch) == branches.end())
        throw ProjectError(ProjectErrorKind::NotFound, "source branch not found");
    return translated<Revision>([&]() {
        if (fromRevision) repository.createBranchAtRevision(name, *fromBranch, *fromRevision);
        else repository.createBranch(name, fromBranch);
        return repository.head(name);
    });
}

Revision ProjectService::head(std::string_view projectId, std::string_view branch) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    Repository& repository = *repositoryEntry(projectId).repository;
    const std::vector<std::string> branches = repository.listBranches();
    if (std::find(branches.begin(), branches.end(), branch) == branches.end())
        throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
    return translated<Revision>([&]() { return repository.head(branch); });
}

std::vector<Revision> ProjectService::history(std::string_view projectId,
                    std::string_view branch, std::size_t maxCount, std::optional<Hash> after) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    if (!validBranchName(branch) || maxCount == 0 || maxCount > 1000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch or history limit");
    Repository& repository = *repositoryEntry(projectId).repository;
    const std::vector<std::string> branches = repository.listBranches();
    if (std::find(branches.begin(), branches.end(), branch) == branches.end())
        throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
    return translated<std::vector<Revision>>([&]() {
        return repository.history(branch, maxCount, after);
    });
}

Revision ProjectService::revision(std::string_view projectId, const Hash& id) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    return translated<Revision>([&]() {
        return repositoryEntry(projectId).repository->revision(id);
    });
}


ScratchFile ProjectService::createScratchFile(std::string_view projectId) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    return translated<ScratchFile>([&]() {
        return repositoryEntry(projectId).repository->createScratchFile();
    });
}

void ProjectService::withRepository(std::string_view projectId,
                                    const std::function<void(Repository&)>& action) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    Repository& repository = *repositoryEntry(projectId).repository;
    action(repository);
}

} // namespace aowis::native
