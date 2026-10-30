#include "branch_name_validation.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <limits>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <set>
#include <tuple>
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

// Persistent service-root identity is independent of project and branch IDs.
// Its first publication is exclusive and durable. Older catalogues without
// this file receive an ID on their first open; an existing malformed file is
// corruption and must never be silently replaced with a new identity.
std::string loadRepositoryIdentity(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "repository.id";
    const auto readExisting = [&]() -> std::string {
        check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
              "unsafe DVMDB repository identity file: " + path.string());
        check(std::filesystem::file_size(path) <= 80,
              "oversized DVMDB repository identity: " + path.string());
        std::ifstream stream(path, std::ios::binary);
        check(stream.good(), "cannot read DVMDB repository identity");
        const std::string data((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        constexpr std::string_view marker = "AOWIS-REPOSITORY-1\n";
        check(data.starts_with(marker) && data.size() == marker.size() + 37
              && data.back() == '\n', "invalid DVMDB repository identity format");
        const std::string id = data.substr(marker.size(), 36);
        check(UuidV7::parse(id).has_value(), "invalid DVMDB repository UUIDv7");
        return id;
    };
    if (std::filesystem::exists(path) || std::filesystem::is_symlink(path))
        return readExisting();

    const std::string id = UuidV7::generate().toString();
    const std::string body = "AOWIS-REPOSITORY-1\n" + id + "\n";
    // Prepare on the same filesystem and publish only fully synchronized bytes.
    // A crash before link() cannot expose a partial authoritative identity.
    const std::filesystem::path staging = root / (".repository-id-creating-" + id);
    const int fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage DVMDB repository identity");
    try {
        std::size_t written = 0;
        while (written < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + written, body.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write DVMDB repository identity");
            written += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync DVMDB repository identity");
    } catch (...) {
        ::close(fd);
        throw;
    }
    syscallChecked(::close(fd) == 0, "close DVMDB repository identity");
    syscallChecked(::link(staging.c_str(), path.c_str()) == 0,
                   "publish DVMDB repository identity");
    syncDirectory(root);
    syscallChecked(::unlink(staging.c_str()) == 0, "remove DVMDB identity staging file");
    syncDirectory(root);
    return id;
}

// VC-03 root-level tracking schema. These are metadata observations, not
// authoritative refs. The only writable branch HEAD lives in this root's
// native Repository. Each remote reference is qualified by remote repository,
// logical project and logical branch UUIDv7, never by a mutable name.
using TrackingKeyOnDisk = std::tuple<std::string, std::string, std::string>;
struct TrackingStateOnDisk {
    RepositoryRoleV1 role = RepositoryRoleV1::Unconfigured;
    std::map<std::string, RemoteRepositoryV1> remotes;
    std::map<TrackingKeyOnDisk, RemoteTrackingRefV1> refs;
};

std::string roleToken(RepositoryRoleV1 role) {
    switch (role) {
    case RepositoryRoleV1::Unconfigured: return "unconfigured";
    case RepositoryRoleV1::Private: return "private";
    case RepositoryRoleV1::Shared: return "shared";
    }
    throw ProjectError(ProjectErrorKind::InvalidInput, "invalid repository role");
}

bool validRemoteEndpoint(std::string_view endpoint) {
    if (endpoint.empty() || endpoint.size() > 2048) return false;
    if (!endpoint.starts_with("http://") && !endpoint.starts_with("https://") &&
        !endpoint.starts_with("file:///")) return false;
    const std::size_t prefix = endpoint.starts_with("https://") ? 8 :
        endpoint.starts_with("http://") ? 7 : 7;
    if (endpoint.size() <= prefix) return false;
    if (!endpoint.starts_with("file:///")) {
        const std::size_t hostnameEnd = endpoint.find_first_of("/:?", prefix);
        if (hostnameEnd == prefix) return false;
    }
    for (unsigned char ch : endpoint) {
        if (ch <= 0x20U || ch >= 0x7fU || ch == '@' || ch == '#' || ch == '\\')
            return false;
    }
    return true;
}

std::string decodeRemoteEndpoint(std::string_view encoded) {
    check(encoded.size() % 2 == 0 && encoded.size() <= 4096,
          "invalid remote endpoint encoding");
    std::string result;
    result.reserve(encoded.size() / 2);
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return 10 + value - 'a';
        return -1;
    };
    for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
        const int first = digit(encoded[offset]);
        const int second = digit(encoded[offset + 1]);
        check(first >= 0 && second >= 0, "invalid remote endpoint encoding");
        result.push_back(static_cast<char>((first << 4) | second));
    }
    check(validRemoteEndpoint(result), "invalid stored remote endpoint");
    return result;
}

std::vector<std::string> trackingFields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

TrackingStateOnDisk loadTrackingState(const std::filesystem::path& root,
                                      std::string_view localRepositoryId) {
    const std::filesystem::path stage = root / "repository.tracking.tmp";
    if (std::filesystem::exists(stage) || std::filesystem::is_symlink(stage)) {
        check(!std::filesystem::is_symlink(stage) && std::filesystem::is_regular_file(stage),
              "unsafe stale tracking staging file");
        syscallChecked(::unlink(stage.c_str()) == 0, "discard unpublished tracking staging file");
        syncDirectory(root);
    }
    TrackingStateOnDisk result;
    const std::filesystem::path path = root / "repository.tracking";
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path)) return result;
    check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path) &&
          std::filesystem::file_size(path) <= 8 * 1024 * 1024,
          "invalid repository tracking metadata file");
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "cannot read repository tracking metadata");
    const std::string data((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    check(!data.empty() && data.back() == '\n', "truncated repository tracking metadata");
    std::istringstream input(data);
    std::string line;
    check(static_cast<bool>(std::getline(input, line)) && line == "AOWIS-TRACKING-1",
          "invalid repository tracking metadata marker");
    check(static_cast<bool>(std::getline(input, line)), "missing repository tracking role");
    if (line == "role\tunconfigured") result.role = RepositoryRoleV1::Unconfigured;
    else if (line == "role\tprivate") result.role = RepositoryRoleV1::Private;
    else if (line == "role\tshared") result.role = RepositoryRoleV1::Shared;
    else check(false, "invalid repository tracking role");
    while (std::getline(input, line)) {
        const std::vector<std::string> fields = trackingFields(line);
        if (fields.size() == 3 && fields[0] == "remote") {
            check(UuidV7::parse(fields[1]).has_value() && fields[1] != localRepositoryId,
                  "invalid remote repository UUID in tracking registry");
            const RemoteRepositoryV1 remote{fields[1], decodeRemoteEndpoint(fields[2])};
            check(result.remotes.emplace(remote.repositoryId, remote).second &&
                  result.remotes.size() <= 64, "duplicate or excessive remote registry entries");
        } else if (fields.size() == 5 && fields[0] == "ref") {
            check(UuidV7::parse(fields[1]).has_value() &&
                  UuidV7::parse(fields[2]).has_value() &&
                  UuidV7::parse(fields[3]).has_value() &&
                  (fields[4] == "-" || RevisionId::parse(fields[4]).has_value()),
                  "invalid qualified remote tracking ref");
            RemoteTrackingRefV1 ref{fields[1], fields[2], fields[3], std::nullopt};
            if (fields[4] != "-") ref.observedHead = *RevisionId::parse(fields[4]);
            check(result.refs.emplace(TrackingKeyOnDisk{ref.remoteRepositoryId, ref.projectId,
                  ref.branchId}, ref).second && result.refs.size() <= 10000,
                  "duplicate or excessive remote tracking refs");
        } else check(false, "unknown repository tracking record");
    }
    check(input.eof() && !input.bad(), "cannot read full repository tracking metadata");
    std::set<std::string> endpoints;
    for (const std::pair<const std::string, RemoteRepositoryV1>& entry : result.remotes)
        check(endpoints.insert(entry.second.endpoint).second,
              "remote endpoint assigned to multiple repository UUIDs");
    for (const std::pair<const TrackingKeyOnDisk, RemoteTrackingRefV1>& record : result.refs)
        check(result.remotes.contains(record.second.remoteRepositoryId),
              "tracking ref references an unregistered repository");
    return result;
}

void storeTrackingState(const std::filesystem::path& root, const TrackingStateOnDisk& state) {
    std::string body = "AOWIS-TRACKING-1\nrole\t" + roleToken(state.role) + "\n";
    for (const std::pair<const std::string, RemoteRepositoryV1>& entry : state.remotes)
        body += "remote\t" + entry.first + "\t" + encodeName(entry.second.endpoint) + "\n";
    for (const std::pair<const TrackingKeyOnDisk, RemoteTrackingRefV1>& entry : state.refs) {
        const RemoteTrackingRefV1& ref = entry.second;
        body += "ref\t" + ref.remoteRepositoryId + "\t" + ref.projectId + "\t" +
            ref.branchId + "\t" + (ref.observedHead ? ref.observedHead->toString() : "-") + "\n";
    }
    check(body.size() <= 8 * 1024 * 1024, "repository tracking metadata exceeds size limit",
          StorageErrorKind::InvalidInput);
    const std::filesystem::path stage = root / "repository.tracking.tmp";
    const std::filesystem::path path = root / "repository.tracking";
    const int fd = ::open(stage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage repository tracking metadata");
    try {
        std::size_t offset = 0;
        while (offset < body.size()) {
            const ssize_t written = ::write(fd, body.data() + offset, body.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            syscallChecked(written > 0, "write repository tracking metadata");
            offset += static_cast<std::size_t>(written);
        }
        syscallChecked(::fsync(fd) == 0, "sync repository tracking metadata");
    } catch (...) {
        ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(stage, ignored);
        throw;
    }
    syscallChecked(::close(fd) == 0, "close repository tracking metadata");
    if (::rename(stage.c_str(), path.c_str()) != 0) {
        std::error_code ignored;
        std::filesystem::remove(stage, ignored);
        throwSystemError("publish repository tracking metadata");
    }
    syncDirectory(root);
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

namespace {
// Private draft format: one authoritative pre-release record encoding.
// No readers or migrators for experimental V1/V2 drafts are retained.
constexpr std::string_view workingCopyMarker = "AOWIS-WORKING-COPY-3\n";
constexpr std::size_t workingCopyMaxBytes = 8 * 1024 * 1024;
constexpr std::size_t workingCopyMaxChanges = 20000;
constexpr std::size_t workingCopyMaxEvents = 4096;

std::filesystem::path workingCopyDirectory(const std::filesystem::path& root) {
    return root / "working-copies";
}

std::uint64_t nowEpochMs() {
    const std::int64_t millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    check(millis > 0, "invalid system clock", StorageErrorKind::Unavailable);
    return static_cast<std::uint64_t>(millis);
}

std::uint64_t decimalField(std::string_view value) {
    check(!value.empty() && (value.size() == 1 || value.front() != '0'),
          "invalid draft decimal field");
    std::uint64_t result = 0;
    const std::from_chars_result parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
          "invalid draft decimal field");
    return result;
}

std::string hexBytes(const std::uint8_t *data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        value.push_back(digits[data[i] >> 4]);
        value.push_back(digits[data[i] & 15U]);
    }
    return value;
}

Bytes unhexBytes(std::string_view encoded) {
    check(encoded.size() % 2 == 0 && encoded.size() <= workingCopyMaxBytes * 2,
          "invalid draft model record encoding");
    Bytes result;
    result.reserve(encoded.size() / 2);
    const std::function<unsigned(char)> digit = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
        check(false, "invalid draft hexadecimal digit");
        return 0;
    };
    for (std::size_t i = 0; i < encoded.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(digit(encoded[i]) * 16 + digit(encoded[i + 1])));
    return result;
}

// Basic transport/integrity verification for MODEL's AO WE frame. The full
// entity schema belongs to MODEL; the HTTP endpoint additionally decodes it
// with ModelEditTransactionCodec. This Qt-free service verifies framing,
// length, SHA-256, and the event's intrinsic UUIDv7 before persisting it.
WorkingCopyEventV1 verifiedEditEvent(const Bytes& encoded) {
    constexpr std::size_t header = 9;
    constexpr std::size_t digest = 32;
    if (encoded.size() < header + 16 + digest || encoded.size() > 8 * 1024 * 1024 + header + digest ||
        encoded[0] != 'A' || encoded[1] != 'O' || encoded[2] != 'W' ||
        encoded[3] != 'E' || encoded[4] != 1)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid edit event frame");
    const std::uint32_t size = (static_cast<std::uint32_t>(encoded[5]) << 24) |
        (static_cast<std::uint32_t>(encoded[6]) << 16) |
        (static_cast<std::uint32_t>(encoded[7]) << 8) | encoded[8];
    if (size < 16 || encoded.size() != header + static_cast<std::size_t>(size) + digest)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid edit event length");
    Bytes payload(encoded.begin() + header, encoded.begin() + header + size);
    const Hash hash = sha256(payload);
    if (!std::equal(hash.begin(), hash.end(), encoded.begin() + header + size))
        throw ProjectError(ProjectErrorKind::InvalidInput, "edit event digest mismatch");
    UuidV7 uuid;
    std::copy_n(payload.begin(), uuid.bytes.size(), uuid.bytes.begin());
    const std::string id = uuid.toString();
    if (!UuidV7::parse(id).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid edit event UUIDv7");
    return WorkingCopyEventV1{id, encoded};
}

std::string saveBatchFingerprint(const std::vector<ModelChange>& changes,
                                 const std::vector<WorkingCopyEventV1>& events) {
    // Canonical, unambiguous length-delimited digest of the complete request.
    // Sort changes by key because candidate overlay order is entity identity.
    std::map<Key, std::optional<ModelRecord>> ordered;
    for (const ModelChange& change : changes)
        if (!ordered.emplace(change.key, change.record).second)
            throw ProjectError(ProjectErrorKind::InvalidInput, "duplicate key in event save");
    Bytes bytes;
    const std::function<void(std::size_t)> appendSize = [&](std::size_t size) {
        const std::uint64_t count = static_cast<std::uint64_t>(size);
        for (unsigned shift = 0; shift < 8; ++shift)
            bytes.push_back(static_cast<std::uint8_t>(count >> (56 - shift * 8)));
    };
    appendSize(ordered.size());
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : ordered) {
        bytes.insert(bytes.end(), entry.first.begin(), entry.first.end());
        const Bytes record = entry.second ? encodeModelRecord(*entry.second) : Bytes{};
        appendSize(record.size());
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    appendSize(events.size());
    for (const WorkingCopyEventV1& event : events) {
        appendSize(event.encoded.size());
        bytes.insert(bytes.end(), event.encoded.begin(), event.encoded.end());
    }
    const Hash hash = sha256(bytes);
    return hexBytes(hash.data(), hash.size());
}

std::string draftBody(const WorkingCopyInfoV1& draft) {
    std::string body = std::string(workingCopyMarker)
        + "project\t" + draft.projectId + "\n"
        + "branch\t" + draft.branchId + "\n"
        + "base\t" + (draft.baseRevision ? draft.baseRevision->toString() : "-") + "\n"
        + "session\t" + draft.sessionId + "\n"
        + "generation\t" + std::to_string(draft.generation) + "\n"
        + "dirty\t" + (draft.dirty ? "1" : "0") + "\n"
        + "validation\tunvalidated\n";
    body += "lease-session\t" + draft.leaseSessionId.value_or("-") + "\n"
        + "lease-token\t" + draft.leaseToken.value_or("-") + "\n"
        + "lease-expires\t" + std::to_string(draft.leaseExpiresAtMs) + "\n";
    body += "changes\t" + std::to_string(draft.candidateChanges.size()) + "\n";
    for (const ModelChange& change : draft.candidateChanges) {
        body += "change\t" + hexBytes(change.key.data(), change.key.size()) + "\t";
        if (!change.record) body += "-";
        else {
            const Bytes encoded = encodeModelRecord(*change.record);
            body += hexBytes(encoded.data(), encoded.size());
        }
        body += "\n";
        check(body.size() <= workingCopyMaxBytes, "draft candidate exceeds storage limit",
              StorageErrorKind::InvalidInput);
    }
    body += "events\t" + std::to_string(draft.journal.size()) + "\n";
    for (const WorkingCopyEventV1& event : draft.journal) {
        body += "event\t" + event.id + "\t" +
            hexBytes(event.encoded.data(), event.encoded.size()) + "\n";
        check(body.size() <= workingCopyMaxBytes, "draft event journal exceeds storage limit",
              StorageErrorKind::InvalidInput);
    }
    body += "last-save\t" + draft.lastSaveFingerprint.value_or("-") + "\n";
    check(body.size() <= workingCopyMaxBytes,
          "draft candidate exceeds storage limit", StorageErrorKind::InvalidInput);
    return body;
}

WorkingCopyInfoV1 readWorkingCopy(const std::filesystem::path& path) {
    check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path)
          && std::filesystem::file_size(path) <= workingCopyMaxBytes,
          "unsafe or oversized working-copy record: " + path.string());
    const std::string id = path.filename().string();
    check(UuidV7::parse(id).has_value(), "invalid working-copy record UUIDv7");
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "cannot open working-copy record");
    const std::string body((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    check(body.starts_with(workingCopyMarker) && !body.empty() && body.back() == '\n',
          "invalid working-copy record marker");
    const std::string_view marker = workingCopyMarker;
    std::istringstream lines(body.substr(marker.size()));
    const std::function<std::string(std::string_view)> nextField =
        [&](std::string_view name) -> std::string {
        std::string line;
        check(static_cast<bool>(std::getline(lines, line)) &&
              line.starts_with(std::string(name) + "\t"),
              "invalid working-copy record field: " + std::string(name));
        return line.substr(name.size() + 1);
    };
    WorkingCopyInfoV1 draft;
    draft.id = id;
    draft.projectId = nextField("project");
    draft.branchId = nextField("branch");
    const std::string base = nextField("base");
    draft.sessionId = nextField("session");
    draft.generation = decimalField(nextField("generation"));
    const std::string dirty = nextField("dirty");
    check(dirty == "0" || dirty == "1", "invalid draft dirty state");
    draft.dirty = dirty == "1";
    check(nextField("validation") == "unvalidated", "invalid draft validation state");
    {
        const std::string owner = nextField("lease-session");
        const std::string token = nextField("lease-token");
        draft.leaseExpiresAtMs = decimalField(nextField("lease-expires"));
        check((owner == "-" && token == "-" && draft.leaseExpiresAtMs == 0) ||
              (UuidV7::parse(owner).has_value() && UuidV7::parse(token).has_value() &&
               draft.leaseExpiresAtMs != 0), "invalid draft lease record");
        if (owner != "-") {
            draft.leaseSessionId = owner;
            draft.leaseToken = token;
        }
    }
    const std::uint64_t count = decimalField(nextField("changes"));
    check(count <= workingCopyMaxChanges, "draft contains too many changes");
    std::optional<Key> lastKey;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::string value = nextField("change");
        const std::size_t tab = value.find('\t');
        check(tab == 32 && value.find('\t', tab + 1) == std::string::npos,
              "invalid draft change record");
        const Bytes keyBytes = unhexBytes(std::string_view(value).substr(0, tab));
        check(keyBytes.size() == Key{}.size(), "invalid draft change UUID");
        ModelChange change;
        std::copy(keyBytes.begin(), keyBytes.end(), change.key.begin());
        check(!lastKey || *lastKey < change.key, "unsorted or duplicate draft changes");
        lastKey = change.key;
        const std::string_view payload = std::string_view(value).substr(tab + 1);
        if (payload != "-") change.record = decodeModelRecord(unhexBytes(payload));
        draft.candidateChanges.push_back(std::move(change));
    }
    {
        const std::uint64_t events = decimalField(nextField("events"));
        check(events <= workingCopyMaxEvents, "too many saved draft events");
        std::set<std::string> eventIds;
        for (std::uint64_t i = 0; i < events; ++i) {
            const std::string field = nextField("event");
            const std::size_t tab = field.find('\t');
            check(tab == 36 && field.find('\t', tab + 1) == std::string::npos,
                  "malformed draft journal event");
            const WorkingCopyEventV1 event = verifiedEditEvent(
                unhexBytes(std::string_view(field).substr(tab + 1)));
            check(field.substr(0, tab) == event.id && eventIds.insert(event.id).second,
                  "invalid or duplicate draft journal event");
            draft.journal.push_back(event);
        }
        const std::string fingerprint = nextField("last-save");
        if (fingerprint != "-") {
            check(fingerprint.size() == 64 &&
                  std::all_of(fingerprint.begin(), fingerprint.end(), [](char ch) {
                      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                  }), "invalid last-save fingerprint");
            draft.lastSaveFingerprint = fingerprint;
        }
        check((draft.journal.empty() && !draft.lastSaveFingerprint) ||
              (!draft.journal.empty() && draft.lastSaveFingerprint && draft.generation > 0),
              "inconsistent draft journal metadata");
    }
    std::string extra;
    check(!std::getline(lines, extra) && lines.eof(), "trailing working-copy record content");
    check(ProjectId::parse(draft.projectId).has_value() &&
          BranchId::parse(draft.branchId).has_value() &&
          UuidV7::parse(draft.sessionId).has_value(),
          "invalid working-copy project, branch or session identity");
    if (base != "-") {
        draft.baseRevision = RevisionId::parse(base);
        check(draft.baseRevision.has_value(), "invalid working-copy base revision");
    }
    check(draft.dirty == !draft.candidateChanges.empty(), "invalid draft dirty/change mismatch");
    return draft;
}

// Stage, fsync, rename and fsync the parent. A crash yields either the old
// complete record or the new complete record, never an acknowledged half-save.
void replaceWorkingCopyRecord(const std::filesystem::path& root,
                              const WorkingCopyInfoV1& draft) {
    const std::filesystem::path directory = workingCopyDirectory(root);
    const std::filesystem::path published = directory / draft.id;
    const std::filesystem::path stage = directory / (".updating-" + UuidV7::generate().toString());
    const std::string body = draftBody(draft);
    const int fd = ::open(stage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage draft update");
    try {
        std::size_t offset = 0;
        while (offset < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + offset, body.size() - offset);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write draft update");
            offset += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync draft update");
    } catch (...) {
        ::close(fd);
        ::unlink(stage.c_str());
        throw;
    }
    syscallChecked(::close(fd) == 0, "close draft update");
    try {
        syscallChecked(::rename(stage.c_str(), published.c_str()) == 0,
                       "publish draft update");
        syncDirectory(directory);
    } catch (...) {
        // A rename may have succeeded even if the final directory fsync did not.
        // Callers must mark publication uncertain and require a restart.
        ::unlink(stage.c_str());
        throw;
    }
}

void createWorkingCopyRecord(const std::filesystem::path& root,
                             const WorkingCopyInfoV1& draft) {
    const std::filesystem::path directory = workingCopyDirectory(root);
    if (!std::filesystem::exists(directory)) {
        syscallChecked(std::filesystem::create_directory(directory), "create working-copy directory");
        syncDirectory(root);
    }
    check(!std::filesystem::is_symlink(directory) && std::filesystem::is_directory(directory),
          "invalid working-copy directory");
    const std::filesystem::path published = directory / draft.id;
    const std::filesystem::path staging = directory / (".creating-" + draft.id);
    const std::string body = draftBody(draft);
    const int fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage working-copy record");
    try {
        std::size_t offset = 0;
        while (offset < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + offset, body.size() - offset);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write working-copy record");
            offset += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync working-copy record");
    } catch (...) {
        ::close(fd);
        ::unlink(staging.c_str());
        throw;
    }
    syscallChecked(::close(fd) == 0, "close working-copy record");
    try {
        syscallChecked(::link(staging.c_str(), published.c_str()) == 0,
                       "publish working-copy record");
        syncDirectory(directory);
        syscallChecked(::unlink(staging.c_str()) == 0, "remove staged working copy");
        syncDirectory(directory);
    } catch (...) {
        throw;
    }
}

} // namespace

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
        this->repository_id_ = loadRepositoryIdentity(this->root_);
        const TrackingStateOnDisk tracking = loadTrackingState(this->root_, this->repository_id_);
        this->repository_role_ = tracking.role;
        this->remotes_ = tracking.remotes;
        this->tracking_ = tracking.refs;
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

std::string ProjectService::repositoryId() const {
    return this->repository_id_;
}

RepositoryRoleV1 ProjectService::repositoryRoleV1() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    return this->repository_role_;
}

void ProjectService::persistTrackingMetadata() {
    TrackingStateOnDisk current;
    current.role = this->repository_role_;
    current.remotes = this->remotes_;
    current.refs = this->tracking_;
    // Never continue accepting metadata writes after an uncertain durable
    // publication. The authoritative state must be reloaded on restart.
    try { storeTrackingState(this->root_, current); }
    catch (...) { this->uncertain_ = true; throw; }
}

void ProjectService::configureRepositoryRoleV1(RepositoryRoleV1 role) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (role == RepositoryRoleV1::Unconfigured)
        throw ProjectError(ProjectErrorKind::InvalidInput, "choose private or shared role explicitly");
    if (this->repository_role_ == role) return;
    if (this->repository_role_ != RepositoryRoleV1::Unconfigured)
        throw ProjectError(ProjectErrorKind::Conflict, "repository role already configured");
    this->repository_role_ = role;
    persistTrackingMetadata();
}

std::vector<RemoteRepositoryV1> ProjectService::remoteRepositoriesV1() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<RemoteRepositoryV1> output;
    for (const std::pair<const std::string, RemoteRepositoryV1>& entry : this->remotes_)
        output.push_back(entry.second);
    return output;
}

void ProjectService::registerRemoteRepositoryV1(std::string_view remoteRepositoryId,
                                                 std::string_view endpoint) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!UuidV7::parse(remoteRepositoryId).has_value() ||
        remoteRepositoryId == this->repository_id_ || !validRemoteEndpoint(endpoint))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid remote identity or endpoint");
    const std::string id(remoteRepositoryId);
    const std::string url(endpoint);
    const std::map<std::string, RemoteRepositoryV1>::const_iterator existing =
        this->remotes_.find(id);
    if (existing != this->remotes_.end()) {
        if (existing->second.endpoint == url) return;
        throw ProjectError(ProjectErrorKind::Conflict,
                           "remote repository UUID already registered at another endpoint");
    }
    for (const std::pair<const std::string, RemoteRepositoryV1>& entry : this->remotes_) {
        if (entry.second.endpoint == url)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "remote endpoint already registered under another UUID");
    }
    if (this->remotes_.size() >= 64)
        throw ProjectError(ProjectErrorKind::InvalidInput, "remote registry limit exceeded");
    this->remotes_.emplace(id, RemoteRepositoryV1{id, url});
    persistTrackingMetadata();
}

std::vector<RemoteTrackingRefV1> ProjectService::remoteTrackingRefsV1() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<RemoteTrackingRefV1> output;
    for (const std::pair<const TrackingKey, RemoteTrackingRefV1>& entry : this->tracking_)
        output.push_back(entry.second);
    return output;
}

void ProjectService::recordRemoteTrackingRefV1(
    const RemoteTrackingRefV1& observation,
    std::optional<std::optional<RevisionId>> expectedObservedHead) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!this->remotes_.contains(observation.remoteRepositoryId) ||
        !ProjectId::parse(observation.projectId).has_value() ||
        !BranchId::parse(observation.branchId).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "tracking ref must target a registered repository and UUIDv7 project/branch");
    const TrackingKey key{observation.remoteRepositoryId, observation.projectId,
                          observation.branchId};
    const std::map<TrackingKey, RemoteTrackingRefV1>::const_iterator current =
        this->tracking_.find(key);
    if (!expectedObservedHead.has_value()) {
        if (current != this->tracking_.end())
            throw ProjectError(ProjectErrorKind::Conflict, "remote tracking ref already exists");
    } else if (current == this->tracking_.end() ||
               current->second.observedHead != *expectedObservedHead) {
        throw ProjectError(ProjectErrorKind::Conflict, "remote tracking HEAD changed");
    }
    if (current != this->tracking_.end() &&
        current->second.observedHead == observation.observedHead) return;
    if (current == this->tracking_.end() && this->tracking_.size() >= 10000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "remote tracking ref limit exceeded");
    this->tracking_.insert_or_assign(key, observation);
    persistTrackingMetadata();
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

ProjectInfo ProjectService::provisionCloneTargetV2(std::string_view projectId,
                                                    std::string_view name) {
    if (!ProjectId::parse(projectId))
        throw ProjectError(ProjectErrorKind::InvalidInput, "clone target project must be UUIDv7");
    return createProjectImpl(name, true, false, std::string(projectId));
}

ProjectInfo ProjectService::createProjectShaFixture(std::string_view name) {
    return createProjectImpl(name, false);
}

ProjectInfo ProjectService::createProjectV2(std::string_view name) {
    return createProjectImpl(name, true);
}

ProjectInfo ProjectService::createProjectImpl(std::string_view name, bool v2, bool unborn,
                                             std::optional<std::string> cloneProjectId) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!validName(name)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                            "project name must contain 1..200 non-control bytes");
    ProjectInfo info;
    info.name = std::string(name);
    if (cloneProjectId) {
        info.id = *cloneProjectId;
        const std::map<std::string, std::shared_ptr<Project>>::const_iterator existing =
            this->entries_.find(info.id);
        if (existing != this->entries_.end()) {
            if (existing->second->info.name != info.name)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "clone project UUID already has a different name");
            return existing->second->info;
        }
        if (std::filesystem::exists(this->projects_ / (".creating-" + info.id)))
            throw ProjectError(ProjectErrorKind::Busy, "clone project publication is pending");
    } else do {
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
            if (cloneProjectId) {
                // No local branch, revision or model is fabricated: importing
                // the verified bundle establishes the original branch UUID.
            } else if (v2) {
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

WorkingCopyInfoV1 ProjectService::createWorkingCopyV1(
        std::string_view projectId, std::string_view branch,
        std::optional<RevisionId> baseRevision, std::string_view sessionId) {
    if (!UuidV7::parse(sessionId).has_value() || !validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft session or branch");
    {
        std::lock_guard<std::mutex> guard(this->mutex_);
        ensureHealthy();
        if (this->repository_role_ != RepositoryRoleV1::Private)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "working copies require an explicitly private repository");
    }
    const std::optional<BranchId> localBranch = branchId(projectId, branch);
    if (!localBranch)
        throw ProjectError(ProjectErrorKind::InvalidInput, "draft requires a UUIDv7 branch");
    // This first VC-04 slice supports exact HEAD checkout only. A concurrent
    // commit after this read cannot change the immutable pinned draft base.
    if (optionalHeadV2(projectId, branch) != baseRevision)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "working-copy base must match current local HEAD");
    WorkingCopyInfoV1 draft;
    draft.id = UuidV7::generate().toString();
    draft.projectId = std::string(projectId);
    draft.branchId = localBranch->toString();
    draft.baseRevision = baseRevision;
    draft.sessionId = std::string(sessionId);
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    try { createWorkingCopyRecord(this->root_, draft); }
    catch (...) { this->uncertain_ = true; throw; }
    return draft;
}

WorkingCopyInfoV1 ProjectService::workingCopyV1(std::string_view draftId) const {
    if (!UuidV7::parse(draftId).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid working-copy UUIDv7");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    const std::filesystem::path path = workingCopyDirectory(this->root_) / std::string(draftId);
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path))
        throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    return readWorkingCopy(path);
}

std::vector<WorkingCopyInfoV1> ProjectService::listWorkingCopiesV1() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<WorkingCopyInfoV1> result;
    const std::filesystem::path directory = workingCopyDirectory(this->root_);
    if (!std::filesystem::exists(directory) && !std::filesystem::is_symlink(directory))
        return result;
    check(!std::filesystem::is_symlink(directory) && std::filesystem::is_directory(directory),
          "invalid working-copy directory");
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        const std::string id = entry.path().filename().string();
        if (id.starts_with(".creating-") || id.starts_with(".updating-")) {
            const std::string_view prefix = id.starts_with(".creating-") ?
                std::string_view(".creating-") : std::string_view(".updating-");
            check(UuidV7::parse(std::string_view(id).substr(prefix.size())).has_value() &&
                  !entry.is_symlink() && entry.is_regular_file(),
                  "unsafe staged working-copy record");
            continue; // unpublished crash remnant, never advertise as a draft
        }
        result.push_back(readWorkingCopy(entry.path()));
    }
    std::sort(result.begin(), result.end(),
              [](const WorkingCopyInfoV1& left, const WorkingCopyInfoV1& right) {
                  return left.id < right.id;
              });
    return result;
}

WorkingCopyLeaseV1 ProjectService::acquireWorkingCopyLeaseV1(
        std::string_view draftId, std::string_view sessionId, std::uint32_t seconds) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        seconds < 1 || seconds > 120)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft lease parameters");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (this->repository_role_ != RepositoryRoleV1::Private)
        throw ProjectError(ProjectErrorKind::Conflict, "draft requires a private repository");
    const std::filesystem::path path = workingCopyDirectory(this->root_) / std::string(draftId);
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path))
        throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    WorkingCopyInfoV1 draft = readWorkingCopy(path);
    const std::uint64_t now = nowEpochMs();
    if (draft.leaseSessionId && draft.leaseExpiresAtMs > now &&
        *draft.leaseSessionId != sessionId)
        throw ProjectError(ProjectErrorKind::Conflict, "draft held by another writer session");
    draft.leaseSessionId = std::string(sessionId);
    draft.leaseToken = UuidV7::generate().toString();
    draft.leaseExpiresAtMs = now + static_cast<std::uint64_t>(seconds) * 1000;
    try { replaceWorkingCopyRecord(this->root_, draft); }
    catch (...) { this->uncertain_ = true; throw; }
    return WorkingCopyLeaseV1{*draft.leaseToken, draft.leaseExpiresAtMs, draft.generation};
}

WorkingCopyInfoV1 ProjectService::saveWorkingCopyChangesV1(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value() || changes.size() > workingCopyMaxChanges)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft save parameters");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (this->repository_role_ != RepositoryRoleV1::Private)
        throw ProjectError(ProjectErrorKind::Conflict, "draft requires a private repository");
    const std::filesystem::path path = workingCopyDirectory(this->root_) / std::string(draftId);
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path))
        throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    WorkingCopyInfoV1 draft = readWorkingCopy(path);
    if (!draft.journal.empty())
        throw ProjectError(ProjectErrorKind::Conflict,
                           "journalled draft requires atomic event saves");
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        draft.leaseExpiresAtMs <= nowEpochMs() || draft.generation != expectedGeneration)
        throw ProjectError(ProjectErrorKind::Conflict, "draft lease, fencing token or generation changed");
    if (draft.generation == std::numeric_limits<std::uint64_t>::max())
        throw ProjectError(ProjectErrorKind::Conflict, "draft generation exhausted");
    std::map<Key, std::optional<ModelRecord>> merged;
    for (const ModelChange& change : draft.candidateChanges) merged.emplace(change.key, change.record);
    std::set<Key> seen;
    for (const ModelChange& change : changes) {
        if (!seen.insert(change.key).second)
            throw ProjectError(ProjectErrorKind::InvalidInput, "duplicate key in draft save batch");
        merged[change.key] = change.record;
    }
    if (merged.size() > workingCopyMaxChanges)
        throw ProjectError(ProjectErrorKind::InvalidInput, "draft candidate has too many changes");
    draft.candidateChanges.clear();
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : merged)
        draft.candidateChanges.push_back(ModelChange{entry.first, entry.second});
    draft.dirty = !draft.candidateChanges.empty();
    ++draft.generation;
    // Serialization and size/record validation is done BEFORE publishing.
    (void)draftBody(draft);
    try { replaceWorkingCopyRecord(this->root_, draft); }
    catch (...) { this->uncertain_ = true; throw; }
    return draft;
}

WorkingCopyInfoV1 ProjectService::saveWorkingCopyTransactionsV1(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes,
        const std::vector<Bytes>& encodedTransactions) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value() || changes.size() > workingCopyMaxChanges ||
        encodedTransactions.empty() || encodedTransactions.size() > 128)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft event-save parameters");
    std::vector<WorkingCopyEventV1> events;
    std::set<std::string> incoming;
    for (const Bytes& bytes : encodedTransactions) {
        WorkingCopyEventV1 event = verifiedEditEvent(bytes);
        if (!incoming.insert(event.id).second)
            throw ProjectError(ProjectErrorKind::InvalidInput, "duplicate edit event UUID");
        events.push_back(std::move(event));
    }
    const std::string fingerprint = saveBatchFingerprint(changes, events);
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (this->repository_role_ != RepositoryRoleV1::Private)
        throw ProjectError(ProjectErrorKind::Conflict, "draft requires a private repository");
    const std::filesystem::path path = workingCopyDirectory(this->root_) / std::string(draftId);
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path))
        throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    WorkingCopyInfoV1 draft = readWorkingCopy(path);
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        draft.leaseExpiresAtMs <= nowEpochMs())
        throw ProjectError(ProjectErrorKind::Conflict, "stale draft writer lease");
    // Lost HTTP acknowledgements are resolved without repeating a Save.
    if (expectedGeneration < std::numeric_limits<std::uint64_t>::max() &&
        draft.generation == expectedGeneration + 1 &&
        draft.lastSaveFingerprint == fingerprint)
        return draft;
    if (draft.generation != expectedGeneration ||
        draft.generation == std::numeric_limits<std::uint64_t>::max())
        throw ProjectError(ProjectErrorKind::Conflict, "draft generation changed");
    std::set<std::string> applied;
    for (const WorkingCopyEventV1& event : draft.journal) applied.insert(event.id);
    for (const WorkingCopyEventV1& event : events)
        if (applied.contains(event.id))
            throw ProjectError(ProjectErrorKind::Conflict, "edit event already saved");
    if (draft.journal.size() + events.size() > workingCopyMaxEvents)
        throw ProjectError(ProjectErrorKind::InvalidInput, "draft event journal is full");
    std::map<Key, std::optional<ModelRecord>> merged;
    for (const ModelChange& change : draft.candidateChanges) merged.emplace(change.key, change.record);
    for (const ModelChange& change : changes) merged[change.key] = change.record;
    if (merged.size() > workingCopyMaxChanges)
        throw ProjectError(ProjectErrorKind::InvalidInput, "draft candidate has too many changes");
    draft.candidateChanges.clear();
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : merged)
        draft.candidateChanges.push_back(ModelChange{entry.first, entry.second});
    draft.journal.insert(draft.journal.end(), events.begin(), events.end());
    draft.dirty = !draft.candidateChanges.empty();
    draft.lastSaveFingerprint = fingerprint;
    ++draft.generation;
    (void)draftBody(draft); // Validate complete overlay + journal before publishing.
    try { replaceWorkingCopyRecord(this->root_, draft); }
    catch (...) { this->uncertain_ = true; throw; }
    return draft;
}

void ProjectService::releaseWorkingCopyLeaseV1(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft lease identifiers");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    const std::filesystem::path path = workingCopyDirectory(this->root_) / std::string(draftId);
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path))
        throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    WorkingCopyInfoV1 draft = readWorkingCopy(path);
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken)
        throw ProjectError(ProjectErrorKind::Conflict, "stale draft lease token");
    draft.leaseSessionId.reset();
    draft.leaseToken.reset();
    draft.leaseExpiresAtMs = 0;
    try { replaceWorkingCopyRecord(this->root_, draft); }
    catch (...) { this->uncertain_ = true; throw; }
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
