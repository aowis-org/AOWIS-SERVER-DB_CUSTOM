#include "aowis/native/native_store.h"
#include "aowis/native/block_store_prototype.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aowis::native {
namespace {

constexpr std::size_t headerSize = 45;
constexpr std::size_t indexHeaderSize = 8;
constexpr std::size_t indexEntrySize = 52;
constexpr std::array<std::uint8_t, indexHeaderSize> indexMagic{
    'A', 'O', 'I', 'D', 'X', '0', '1', 0
};
constexpr std::uint64_t maxObjectBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t stagedFlushThreshold = 8U * 1024U * 1024U;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void systemCall(bool ok, const std::string& operation) {
    if (!ok) {
        throw std::runtime_error(operation + ": " + std::strerror(errno));
    }
}

// Every open Repository holds a shared lifetime lock. Maintenance requires
// exclusive ownership: otherwise an existing process could keep writing to
// file descriptors referencing a retired pack after the pointer is switched.
class ScopedMaintenanceExclusive {
public:
    explicit ScopedMaintenanceExclusive(int fd) : fd_(fd) {
        // Linux flock conversion may DROP an existing shared lock if a
        // nonblocking exclusive upgrade fails. Never upgrade directly: while
        // repo.lock is held, release our shared lock, attempt exclusivity,
        // and restore SH on failure. Other constructors may acquire SH in
        // the gap, but then compaction safely refuses to proceed.
        systemCall(::flock(this->fd_, LOCK_UN) == 0, "release maintenance shared lock");
        if (::flock(this->fd_, LOCK_EX | LOCK_NB) != 0) {
            systemCall(::flock(this->fd_, LOCK_SH) == 0,
                       "restore maintenance shared lock after refused compaction");
            throw std::runtime_error(
                "compaction requires all other repository handles to be closed");
        }
    }
    ~ScopedMaintenanceExclusive() { ::flock(this->fd_, LOCK_SH); }
    ScopedMaintenanceExclusive(const ScopedMaintenanceExclusive&) = delete;
    ScopedMaintenanceExclusive& operator=(const ScopedMaintenanceExclusive&) = delete;
private:
    int fd_;
};

class ScopedFileLock {
public:
    explicit ScopedFileLock(int fd) : fd_(fd) {
        systemCall(::flock(this->fd_, LOCK_EX) == 0, "flock");
    }
    ~ScopedFileLock() { ::flock(this->fd_, LOCK_UN); }
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;
private:
    int fd_;
};

bool isZero(const Hash& hash) {
    return hash == Hash{};
}

unsigned nibble(const Key& key, unsigned depth) {
    return (depth % 2U == 0U)
        ? static_cast<unsigned>(key[depth / 2U] >> 4U)
        : static_cast<unsigned>(key[depth / 2U] & 0x0fU);
}

void appendHash(Bytes& bytes, const Hash& hash) {
    bytes.insert(bytes.end(), hash.begin(), hash.end());
}

// Revisions index the reverse edges in a second content-addressed tree.
// These entries are intentionally independent of the SQL V5 payload codec.
Bytes encodeReferrers(const std::set<Key>& referrers) {
    require(referrers.size() <= std::numeric_limits<std::uint32_t>::max(),
            "too many incoming model references");
    Bytes bytes{'I', 'N', 'R', '1'};
    const std::uint32_t count = static_cast<std::uint32_t>(referrers.size());
    for (unsigned shift = 24; ; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((count >> shift) & 255U));
        if (shift == 0) break;
    }
    for (const Key& source : referrers) {
        bytes.insert(bytes.end(), source.begin(), source.end());
    }
    return bytes;
}

std::vector<Key> decodeReferrers(const Bytes& bytes) {
    require(bytes.size() >= 8 && bytes[0] == 'I' && bytes[1] == 'N' &&
            bytes[2] == 'R' && bytes[3] == '1', "invalid incoming-reference index object");
    std::uint32_t count = 0;
    for (std::size_t i = 4; i < 8; ++i) {
        count = (count << 8U) | bytes[i];
    }
    require(count != 0 && static_cast<std::uint64_t>(count) * 16ULL == bytes.size() - 8,
            "invalid incoming-reference index size");
    std::vector<Key> referrers;
    referrers.reserve(count);
    std::size_t offset = 8;
    for (std::uint32_t i = 0; i < count; ++i) {
        Key source{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 16, source.begin());
        require(source != Key{} && (referrers.empty() || referrers.back() < source),
                "incoming-reference index is not canonical");
        referrers.push_back(source);
        offset += 16;
    }
    return referrers;
}

Hash readHash(const Bytes& bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 32, "truncated hash");
    Hash result{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 32, result.begin());
    return result;
}

void storeU64(std::uint8_t* destination, std::uint64_t number) {
    for (unsigned i = 0; i < 8; ++i) {
        destination[i] = static_cast<std::uint8_t>((number >> (56U - 8U * i)) & 0xffU);
    }
}


void storeU32(std::uint8_t* destination, std::uint32_t number) {
    for (unsigned i = 0; i < 4; ++i) {
        destination[i] = static_cast<std::uint8_t>(number >> (24U - 8U * i));
    }
}

std::uint32_t readU32(const std::uint8_t* source) {
    std::uint32_t number = 0;
    for (unsigned i = 0; i < 4; ++i) number = (number << 8U) | source[i];
    return number;
}

// Sidecar checksums detect torn and accidental index corruption. The object
// identity remains SHA-256 of the actual payload, verified by load/verifyPack.
std::uint32_t indexChecksum(const std::uint8_t* bytes) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            std::uint32_t value = static_cast<std::uint32_t>(i);
            for (unsigned bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
            }
            result[i] = value;
        }
        return result;
    }();
    std::uint32_t value = 0xffffffffU;
    for (std::size_t i = 0; i < indexEntrySize - 4; ++i) {
        value = (value >> 8U) ^ table[(value ^ bytes[i]) & 0xffU];
    }
    return ~value;
}

std::array<std::uint8_t, indexEntrySize> encodeIndexEntry(
        const Hash& hash, std::uint64_t payloadOffset, std::uint64_t payloadSize) {
    std::array<std::uint8_t, indexEntrySize> entry{};
    std::copy(hash.begin(), hash.end(), entry.begin());
    storeU64(entry.data() + 32, payloadOffset);
    storeU64(entry.data() + 40, payloadSize);
    storeU32(entry.data() + 48, indexChecksum(entry.data()));
    return entry;
}

std::uint64_t readU64(const std::uint8_t* source) {
    std::uint64_t number = 0;
    for (unsigned i = 0; i < 8; ++i) {
        number = (number << 8U) | source[i];
    }
    return number;
}

void readExact(int fd, std::uint8_t* out, std::size_t size, std::uint64_t offset) {
    std::size_t done = 0;
    while (done != size) {
        ssize_t count = ::pread(fd, out + done, size - done,
                                static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        systemCall(count >= 0, "pread");
        require(count != 0, "unexpected EOF reading content pack");
        done += static_cast<std::size_t>(count);
    }
}

void writeExact(int fd, const std::uint8_t* bytes, std::size_t size, std::uint64_t offset) {
    std::size_t done = 0;
    while (done != size) {
        ssize_t count = ::pwrite(fd, bytes + done, size - done,
                                 static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        systemCall(count >= 0, "pwrite");
        require(count != 0, "pwrite made no progress");
        done += static_cast<std::size_t>(count);
    }
}

void syncDirectory(const std::filesystem::path& directory) {
    int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    systemCall(fd >= 0, "open directory for sync");
    int status = ::fsync(fd);
    int saved = errno;
    ::close(fd);
    errno = saved;
    systemCall(status == 0, "fsync directory");
}

// A single atomic pointer is the only authoritative choice between complete
// pack generations. The legacy name is the initial generation (empty token).
// Never silently fall back if the pointer is malformed or files are missing.
std::string activePackToken(const std::filesystem::path& directory) {
    const std::filesystem::path pointer = directory / "objects.current";
    if (!std::filesystem::exists(pointer)) return {};
    std::ifstream stream(pointer, std::ios::binary);
    require(stream.good(), "cannot read active pack pointer");
    const std::string contents((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
    require(contents.size() == 39 && contents.substr(0, 6) == "PACK1 " &&
            contents.back() == '\n', "invalid active pack pointer");
    for (std::size_t i = 6; i < 38; ++i) {
        require((contents[i] >= '0' && contents[i] <= '9') ||
                (contents[i] >= 'a' && contents[i] <= 'f'),
                "invalid active pack generation");
    }
    return contents.substr(6, 32);
}

std::filesystem::path packPath(const std::filesystem::path& directory,
                               const std::string& token, bool index) {
    if (token.empty()) return directory / (index ? "objects.idx" : "objects.pack");
    return directory / ("objects." + token + (index ? ".idx" : ".pack"));
}

void checkBranch(std::string_view name) {
    require(!name.empty() && name.size() <= 96, "branch name must contain 1..96 characters");
    for (char character : name) {
        require((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '-' || character == '_', "invalid branch name");
    }
}

int nibbleHex(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    throw std::runtime_error("invalid SHA-256 hex digit");
}

} // namespace

std::string toHex(const Hash& value) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (std::uint8_t character : value) {
        result.push_back(alphabet[character >> 4U]);
        result.push_back(alphabet[character & 0x0fU]);
    }
    return result;
}

Hash fromHex(std::string_view value) {
    require(value.size() == 64, "SHA-256 hex must be 64 characters");
    Hash hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<std::uint8_t>((nibbleHex(value[2*i]) << 4U) |
                                             nibbleHex(value[2*i+1]));
    }
    return hash;
}

Hash sha256(const Bytes& value) {
    Hash output{};
    unsigned length = 0;
    require(EVP_Digest(value.data(), value.size(), output.data(), &length,
                       EVP_sha256(), nullptr) == 1 && length == output.size(),
            "OpenSSL SHA-256 failed");
    return output;
}

Repository::Repository(std::filesystem::path directory, PhysicalStore storage)
    : directory_(std::move(directory)), storage_(storage) {
    std::filesystem::create_directories(this->directory_ / "refs" / "heads");
    this->lifetimeFd_ = ::open((this->directory_ / "maintenance.lock").c_str(),
                               O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    systemCall(this->lifetimeFd_ >= 0, "open maintenance lock");
    try {
        systemCall(::flock(this->lifetimeFd_, LOCK_SH) == 0, "lock repository lifetime");
        this->lockFd_ = ::open((this->directory_ / "repo.lock").c_str(),
                                  O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        systemCall(this->lockFd_ >= 0, "open repository lock");
        ScopedFileLock locked(this->lockFd_);
        // Prevent either constructor from silently opening the wrong physical
        // repository type (or creating a second, disconnected object pack).
        const std::filesystem::path format = this->directory_ / "storage.format";
        if (this->storage_ == PhysicalStore::ExperimentalBlocks) {
            if (!std::filesystem::exists(format)) {
                require(!std::filesystem::exists(this->directory_ / "objects.pack") &&
                        !std::filesystem::exists(this->directory_ / "objects.current") &&
                        std::filesystem::is_empty(this->directory_ / "refs" / "heads"),
                        "block mode requires a fresh repository");
                const std::filesystem::path blocks = this->directory_ / "blocks";
                if (!std::filesystem::exists(blocks)) BlockStorePrototype::createEmpty(blocks);
                // Format marker precedes any published ref. A crash before
                // publication can only leave a harmless empty block directory.
                const int marker = ::open(format.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
                systemCall(marker >= 0, "create block repository format marker");
                try {
                    constexpr char signature[] = "AOWIS-BLOCK-EXPERIMENT-1\n";
                    writeExact(marker, reinterpret_cast<const std::uint8_t*>(signature),
                               sizeof(signature) - 1, 0);
                    systemCall(::fsync(marker) == 0, "sync block repository format marker");
                } catch (...) { ::close(marker); throw; }
                ::close(marker);
                syncDirectory(this->directory_);
            }
            std::ifstream marker(format, std::ios::binary);
            std::string signature;
            std::getline(marker, signature);
            require(signature == "AOWIS-BLOCK-EXPERIMENT-1" && marker.good(),
                    "repository physical storage format mismatch");
            // Missing commit markers are recoverable only for an entirely
            // empty, never-published repository. Once any ref exists, do NOT
            // reactivate an arbitrary candidate pack: it may contain crash
            // tails that were never committed.
            const std::filesystem::path activeBlocks =
                BlockStorePrototype::activeDirectory(this->directory_ / "blocks");
            const std::filesystem::path markerPath = activeBlocks / "candidate.current";
            if (!std::filesystem::exists(markerPath)) {
                require(std::filesystem::is_empty(this->directory_ / "refs" / "heads") &&
                        std::filesystem::file_size(activeBlocks / "candidate.pack") == 0,
                        "committed block marker missing from an initialized repository");
            }
            this->blockStore_ = std::make_unique<BlockStorePrototype>(this->directory_ / "blocks");
        } else {
            require(!std::filesystem::exists(format) &&
                    !std::filesystem::exists(this->directory_ / "blocks"),
                    "repository uses experimental block storage; select it explicitly");
            this->packToken_ = activePackToken(this->directory_);
            const int flags = O_RDWR | O_CLOEXEC | (this->packToken_.empty() ? O_CREAT : 0);
            this->packFd_ = ::open(packPath(this->directory_, this->packToken_, false).c_str(),
                                   flags, 0644);
            systemCall(this->packFd_ >= 0, "open active content pack");
            this->indexFd_ = ::open(packPath(this->directory_, this->packToken_, true).c_str(),
                                    flags | O_CREAT, 0644);
            systemCall(this->indexFd_ >= 0, "open active content index");
            refreshIndex();
        }
        syncDirectory(this->directory_);
        syncDirectory(this->directory_ / "refs");
    } catch (...) {
        if (this->indexFd_ >= 0) ::close(this->indexFd_);
        if (this->packFd_ >= 0) ::close(this->packFd_);
        if (this->lockFd_ >= 0) ::close(this->lockFd_);
        ::close(this->lifetimeFd_);
        throw;
    }
}

Repository::~Repository() {
    if (this->indexFd_ >= 0) ::close(this->indexFd_);
    if (this->packFd_ >= 0) ::close(this->packFd_);
    if (this->lockFd_ >= 0) ::close(this->lockFd_);
    if (this->lifetimeFd_ >= 0) ::close(this->lifetimeFd_);
}

void Repository::scanPackTail() {
    struct stat data{};
    systemCall(::fstat(this->packFd_, &data) == 0, "stat content pack");
    require(data.st_size >= 0, "invalid negative pack size");
    const std::uint64_t size = static_cast<std::uint64_t>(data.st_size);
    require(size >= this->scanOffset_, "content pack shrank while repository was open");
    std::uint64_t position = this->scanOffset_;
    while (position < size) {
        if (size - position < headerSize) break;
        std::array<std::uint8_t, headerSize> header{};
        readExact(this->packFd_, header.data(), header.size(), position);
        require(header[0] == 'A' && header[1] == 'O' && header[2] == 'B' &&
                header[3] == 'J' && header[4] == 1, "corrupt pack record header");
        const std::uint64_t payloadSize = readU64(header.data() + 37);
        require(payloadSize <= maxObjectBytes, "invalid content object length");
        if (size - position - headerSize < payloadSize) break;
        Bytes payload(static_cast<std::size_t>(payloadSize));
        if (!payload.empty()) readExact(this->packFd_, payload.data(), payload.size(),
                                        position + headerSize);
        Hash expected{};
        std::copy_n(header.begin() + 5, 32, expected.begin());
        require(sha256(payload) == expected, "content pack checksum mismatch");
        this->index_[expected] = Location{position + headerSize, payloadSize};
        const std::array<std::uint8_t, indexEntrySize> entry =
            encodeIndexEntry(expected, position + headerSize, payloadSize);
        this->pendingIndex_.insert(this->pendingIndex_.end(), entry.begin(), entry.end());
        position += headerSize + payloadSize;
    }
    if (position != size) {
        systemCall(::ftruncate(this->packFd_, static_cast<off_t>(position)) == 0,
                   "discard incomplete trailing pack record");
    }
    this->scanOffset_ = position;
    if (!this->pendingIndex_.empty() || position != size) syncPack();
}

// The sidecar is derived data. If missing, torn, or internally inconsistent,
// rebuild it from cryptographically verified pack records without changing
// any model, tree, or revision hash.
void Repository::rebuildIndex() {
    this->index_.clear();
    this->pendingIndex_.clear();
    this->scanOffset_ = 0;
    this->indexReadOffset_ = indexHeaderSize;
    systemCall(::ftruncate(this->indexFd_, 0) == 0, "truncate damaged pack index");
    writeExact(this->indexFd_, indexMagic.data(), indexMagic.size(), 0);
    scanPackTail();
    // Also persist an empty index (no objects => no pending entries).
    systemCall(::fsync(this->indexFd_) == 0, "sync rebuilt pack index");
}

void Repository::refreshIndex() {
    if (this->blockStore_) return; // block store owns its committed index
    // A preceding operation could have written objects before throwing;
    // make its orphan records discoverable by other processes too.
    if (!this->pendingIndex_.empty()) syncPack();
    struct stat packData{};
    struct stat indexData{};
    systemCall(::fstat(this->packFd_, &packData) == 0, "stat content pack");
    systemCall(::fstat(this->indexFd_, &indexData) == 0, "stat content index");
    require(packData.st_size >= 0 && indexData.st_size >= 0, "invalid storage size");
    const std::uint64_t packBytes = static_cast<std::uint64_t>(packData.st_size);
    const std::uint64_t indexBytes = static_cast<std::uint64_t>(indexData.st_size);
    if (indexBytes < indexHeaderSize) {
        rebuildIndex();
        return;
    }
    std::array<std::uint8_t, indexHeaderSize> magic{};
    readExact(this->indexFd_, magic.data(), magic.size(), 0);
    if (magic != indexMagic || indexBytes < this->indexReadOffset_ ||
        packBytes < this->scanOffset_) {
        rebuildIndex();
        return;
    }
    if (this->indexReadOffset_ == 0) this->indexReadOffset_ = indexHeaderSize;
    // A bounded buffer avoids 600,000+ small pread calls at startup.
    constexpr std::size_t entriesPerRead = 8192;
    std::array<std::uint8_t, entriesPerRead * indexEntrySize> buffer{};
    std::uint64_t position = this->indexReadOffset_;
    std::uint64_t coveredPack = this->scanOffset_;
    bool damagedIndex = false;
    while (indexBytes - position >= indexEntrySize) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(entriesPerRead, (indexBytes - position) / indexEntrySize));
        readExact(this->indexFd_, buffer.data(), count * indexEntrySize, position);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* entry = buffer.data() + i * indexEntrySize;
            const std::uint64_t offset = readU64(entry + 32);
            const std::uint64_t length = readU64(entry + 40);
            if (indexChecksum(entry) != readU32(entry + 48) ||
                offset != coveredPack + headerSize ||
                length > maxObjectBytes || offset > packBytes || length > packBytes - offset) {
                damagedIndex = true;
                break;
            }
            Hash hash{};
            std::copy_n(entry, hash.size(), hash.begin());
            if (isZero(hash)) { damagedIndex = true; break; }
            this->index_[hash] = Location{offset, length};
            coveredPack = offset + length;
        }
        if (damagedIndex) break;
        position += count * indexEntrySize;
    }
    if (damagedIndex) {
        rebuildIndex();
        return;
    }
    this->scanOffset_ = coveredPack;
    this->indexReadOffset_ = position;
    if (position != indexBytes) {
        // Incomplete sidecar tail (for instance interrupted index flush).
        systemCall(::ftruncate(this->indexFd_, static_cast<off_t>(position)) == 0,
                   "truncate incomplete index entry");
    }
    // A crash between writing pack and index may leave a valid pack suffix.
    // Hash-check only the uncovered suffix and restore its index entries.
    scanPackTail();
}

Bytes Repository::load(const Hash& hash) {
    require(!isZero(hash), "attempted to load empty tree hash");
    if (this->blockStore_) {
        const std::unordered_map<Hash, Bytes, HashHasher>::const_iterator pending =
            this->stagedObjects_.find(hash);
        if (pending != this->stagedObjects_.end()) return pending->second;
        const std::optional<Bytes> object = this->blockStore_->get(hash);
        require(object.has_value(), "referenced content missing from block store");
        return *object;
    }
    std::unordered_map<Hash, Location, HashHasher>::const_iterator it = this->index_.find(hash);
    require(it != this->index_.end(), "referenced content is missing from pack");
    Bytes payload(static_cast<std::size_t>(it->second.size));
    if (!payload.empty()) readExact(this->packFd_, payload.data(), payload.size(), it->second.offset);
    require(sha256(payload) == hash, "content checksum mismatch during read");
    return payload;
}

Hash Repository::store(const Bytes& bytes) {
    require(bytes.size() <= maxObjectBytes, "content object exceeds size limit");
    Hash hash = sha256(bytes);
    if (this->blockStore_) {
        if (!this->blockStore_->contains(hash)) {
            const std::pair<std::unordered_map<Hash, Bytes, HashHasher>::iterator, bool> inserted =
                this->stagedObjects_.try_emplace(hash, bytes);
            if (inserted.second) {
                this->stagedBytes_ += inserted.first->second.size();
                if (this->stagedBytes_ >= stagedFlushThreshold) syncPack();
            }
        }
        return hash;
    }
    if (this->index_.contains(hash)) return hash;
    std::array<std::uint8_t, headerSize> header{};
    header[0] = 'A'; header[1] = 'O'; header[2] = 'B'; header[3] = 'J'; header[4] = 1;
    std::copy(hash.begin(), hash.end(), header.begin() + 5);
    storeU64(header.data() + 37, static_cast<std::uint64_t>(bytes.size()));
    std::uint64_t offset = this->scanOffset_;
    writeExact(this->packFd_, header.data(), header.size(), offset);
    if (!bytes.empty()) writeExact(this->packFd_, bytes.data(), bytes.size(), offset + headerSize);
    this->index_.emplace(hash, Location{offset + headerSize, bytes.size()});
    const std::array<std::uint8_t, indexEntrySize> entry =
        encodeIndexEntry(hash, offset + headerSize, bytes.size());
    this->pendingIndex_.insert(this->pendingIndex_.end(), entry.begin(), entry.end());
    this->scanOffset_ += headerSize + bytes.size();
    return hash;
}

// Moving newly encoded objects into the unpublished block batch avoids a
// second heap allocation and payload copy for every Merkle node and record.
// The existing Stage 7 writer continues using the original immutable input.
Hash Repository::store(Bytes&& bytes) {
    if (!this->blockStore_) return store(static_cast<const Bytes&>(bytes));
    require(bytes.size() <= maxObjectBytes, "content object exceeds size limit");
    const Hash hash = sha256(bytes);
    if (!this->blockStore_->contains(hash)) {
        const std::pair<std::unordered_map<Hash, Bytes, HashHasher>::iterator, bool> inserted =
            this->stagedObjects_.try_emplace(hash, std::move(bytes));
        if (inserted.second) {
            this->stagedBytes_ += inserted.first->second.size();
            if (this->stagedBytes_ >= stagedFlushThreshold) syncPack();
        }
    }
    return hash;
}

void Repository::syncPack() {
    if (this->blockStore_) {
        if (this->stagedObjects_.empty()) return;
        // Do not expose a branch head until all its tree nodes, model blobs
        // and revision object are durably published by candidate.current.
        // store() already computed SHA-256 for every immutable staged value.
        // Pass references to those bytes instead of copying the entire batch
        // and hashing it a second time inside the physical block store.
        std::vector<std::pair<Hash, const Bytes*>> batch;
        batch.reserve(this->stagedObjects_.size());
        for (const std::pair<const Hash, Bytes>& entry : this->stagedObjects_) {
            batch.emplace_back(entry.first, &entry.second);
        }
        this->blockStore_->putHashedBatch(batch);
        this->stagedObjects_.clear();
        this->stagedBytes_ = 0;
        return;
    }
    // Branch durability depends on the pack, not on this reconstructible
    // sidecar. The index may lag across a crash; refreshIndex hashes the
    // uncovered durable pack suffix to rebuild it on the next open.
    systemCall(::fsync(this->packFd_) == 0, "fsync content pack");
    if (!this->pendingIndex_.empty()) {
        writeExact(this->indexFd_, this->pendingIndex_.data(),
                   this->pendingIndex_.size(), this->indexReadOffset_);
        this->indexReadOffset_ += this->pendingIndex_.size();
        this->pendingIndex_.clear();
    }
    // Deliberately no fsync(objects.idx) on each commit. Losing any sidecar
    // suffix is harmless once objects.pack and the branch ref are durable.
}

Repository::TreeNode Repository::readNode(const Hash& hash) {
    const Bytes bytes = load(hash);
    require(!bytes.empty(), "empty tree node");
    TreeNode result{};
    if (bytes[0] == 'L') {
        require(bytes.size() == 1 + 16 + 32, "invalid leaf node size");
        result.leaf = true;
        std::copy_n(bytes.begin() + 1, 16, result.key.begin());
        result.value = readHash(bytes, 17);
        require(!isZero(result.value), "leaf references zero content hash");
    } else if (bytes[0] == 'B') {
        require(bytes.size() == 2 + 16 + 16 * 32, "invalid branch node size");
        result.leaf = false;
        result.depth = bytes[1];
        std::copy_n(bytes.begin() + 2, 16, result.prefix.begin());
        require(result.depth < 32, "invalid trie branch depth");
        unsigned count = 0;
        for (std::size_t i = 0; i < 16; ++i) {
            result.children[i] = readHash(bytes, 18 + i * 32);
            if (!isZero(result.children[i])) ++count;
        }
        require(count >= 2, "trie branch must have at least two children");
    } else {
        throw std::runtime_error("invalid tree node tag");
    }
    return result;
}

Hash Repository::storeNode(const TreeNode& node) {
    Bytes bytes;
    if (node.leaf) {
        bytes.push_back('L');
        bytes.insert(bytes.end(), node.key.begin(), node.key.end());
        appendHash(bytes, node.value);
    } else {
        bytes.push_back('B');
        bytes.push_back(node.depth);
        // Only the prefix before depth is semantically meaningful. Canonicalize
        // the remaining nibbles so identical trees hash identically regardless
        // of insertion order or which key caused a particular branch split.
        Key canonicalPrefix = node.prefix;
        for (unsigned depth = node.depth; depth < 32; ++depth) {
            if ((depth & 1U) == 0U) {
                canonicalPrefix[depth / 2] &= 0x0fU;
            } else {
                canonicalPrefix[depth / 2] &= 0xf0U;
            }
        }
        bytes.insert(bytes.end(), canonicalPrefix.begin(), canonicalPrefix.end());
        for (const Hash& child : node.children) appendHash(bytes, child);
    }
    return store(std::move(bytes));
}

// Build an immutable subtree directly from its *final* sorted contents.
// Unlike inserting keys one by one, this never persists intermediate roots.
Hash Repository::buildTree(const std::vector<TreeUpdate>& updates, std::size_t begin,
                           std::size_t end, unsigned minDepth) {
    if (begin == end) return Hash{};
    if (end - begin == 1) {
        require(updates[begin].second.has_value(), "buildTree expects present values");
        TreeNode leaf{};
        leaf.key = updates[begin].first;
        leaf.value = *updates[begin].second;
        return storeNode(leaf);
    }
    unsigned divergence = minDepth;
    while (divergence < 32 &&
           nibble(updates[begin].first, divergence) == nibble(updates[end - 1].first, divergence)) {
        ++divergence;
    }
    require(divergence < 32, "duplicate UUID in sorted batch");
    TreeNode branch{};
    branch.leaf = false;
    branch.depth = static_cast<std::uint8_t>(divergence);
    branch.prefix = updates[begin].first;
    std::size_t current = begin;
    while (current < end) {
        const unsigned childIndex = nibble(updates[current].first, divergence);
        std::size_t next = current + 1;
        while (next < end && nibble(updates[next].first, divergence) == childIndex) ++next;
        branch.children[childIndex] = buildTree(updates, current, next, divergence + 1);
        current = next;
    }
    return storeNode(branch);
}

// Apply a sorted group of changes in one traversal. Existing unchanged
// children are reused; each modified branch is persisted at most once.
Hash Repository::applyTree(const Hash& root, const std::vector<TreeUpdate>& updates,
                           std::size_t begin, std::size_t end, unsigned minDepth) {
    if (begin == end) return root;
    if (isZero(root)) {
        std::vector<TreeUpdate> inserted;
        inserted.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            if (updates[i].second) inserted.push_back(updates[i]);
        }
        return buildTree(inserted, 0, inserted.size(), minDepth);
    }

    TreeNode node = readNode(root);
    if (node.leaf) {
        std::vector<TreeUpdate> combined;
        combined.reserve(end - begin + 1);
        bool handledExisting = false;
        for (std::size_t i = begin; i < end; ++i) {
            if (!handledExisting && node.key < updates[i].first) {
                combined.emplace_back(node.key, node.value);
                handledExisting = true;
            }
            if (updates[i].first == node.key) {
                handledExisting = true;
                if (updates[i].second && *updates[i].second == node.value && end - begin == 1) {
                    return root;
                }
            }
            if (updates[i].second) combined.push_back(updates[i]);
        }
        if (!handledExisting) combined.emplace_back(node.key, node.value);
        if (combined.size() == 1 && combined.front().first == node.key &&
            combined.front().second && *combined.front().second == node.value) return root;
        return buildTree(combined, 0, combined.size(), minDepth);
    }
    require(node.depth >= minDepth, "invalid compressed tree ordering");

    // If an inserted UUID diverges before this branch's compressed prefix,
    // split ABOVE the existing subtree instead of traversing a wrong child.
    unsigned splitDepth = node.depth;
    for (std::size_t i = begin; i < end; ++i) {
        if (!updates[i].second) continue;
        for (unsigned depth = minDepth; depth < splitDepth; ++depth) {
            if (nibble(updates[i].first, depth) != nibble(node.prefix, depth)) {
                splitDepth = depth;
                break;
            }
        }
    }
    if (splitDepth < node.depth) {
        TreeNode parent{};
        parent.leaf = false;
        parent.depth = static_cast<std::uint8_t>(splitDepth);
        parent.prefix = node.prefix;
        const unsigned originalSlot = nibble(node.prefix, splitDepth);
        bool changed = false;
        std::size_t current = begin;
        while (current < end) {
            const unsigned slot = nibble(updates[current].first, splitDepth);
            std::size_t next = current + 1;
            while (next < end && nibble(updates[next].first, splitDepth) == slot) ++next;
            if (slot == originalSlot) {
                parent.children[slot] = applyTree(root, updates, current, next, splitDepth + 1);
                changed |= parent.children[slot] != root;
            } else {
                parent.children[slot] = applyTree(Hash{}, updates, current, next, splitDepth + 1);
                changed |= !isZero(parent.children[slot]);
            }
            current = next;
        }
        if (isZero(parent.children[originalSlot])) {
            // No changes reached the original slot; retain the shared subtree.
            bool originalHadChanges = false;
            for (std::size_t i = begin; i < end; ++i) {
                if (nibble(updates[i].first, splitDepth) == originalSlot) {
                    originalHadChanges = true;
                    break;
                }
            }
            if (!originalHadChanges) parent.children[originalSlot] = root;
        }
        if (!changed) return root;
        unsigned childCount = 0;
        Hash only{};
        for (const Hash& child : parent.children) {
            if (!isZero(child)) { ++childCount; only = child; }
        }
        if (childCount == 0) return Hash{};
        if (childCount == 1) return only;
        return storeNode(parent);
    }

    bool changed = false;
    std::size_t current = begin;
    while (current < end) {
        const unsigned slot = nibble(updates[current].first, node.depth);
        std::size_t next = current + 1;
        while (next < end && nibble(updates[next].first, node.depth) == slot) ++next;
        const Hash replacement = applyTree(node.children[slot], updates, current, next,
                                           static_cast<unsigned>(node.depth) + 1U);
        if (replacement != node.children[slot]) {
            node.children[slot] = replacement;
            changed = true;
        }
        current = next;
    }
    if (!changed) return root;
    unsigned count = 0;
    Hash only{};
    for (const Hash& child : node.children) {
        if (!isZero(child)) { ++count; only = child; }
    }
    if (count == 0) return Hash{};
    if (count == 1) return only;
    return storeNode(node);
}

std::optional<Hash> Repository::lookup(const Hash& root, const Key& key) {
    Hash current = root;
    while (!isZero(current)) {
        const TreeNode node = readNode(current);
        if (node.leaf) {
            if (node.key == key) return node.value;
            return std::nullopt;
        }
        current = node.children[nibble(key, node.depth)];
    }
    return std::nullopt;
}

void Repository::listHashes(const Hash& root, std::vector<std::pair<Key, Hash>>& output) {
    if (isZero(root)) return;
    const TreeNode node = readNode(root);
    if (node.leaf) {
        output.emplace_back(node.key, node.value);
        return;
    }
    for (const Hash& child : node.children) listHashes(child, output);
}

void Repository::diffHashes(const Hash& from, const Hash& to,
                            std::vector<Difference>& output) {
    if (from == to) return;
    // Align matching branch nodes structurally. When insertion/removal changes
    // a compressed prefix, fall back to comparing just those subtrees.
    if (!isZero(from) && !isZero(to)) {
        const TreeNode left = readNode(from);
        const TreeNode right = readNode(to);
        bool aligned = !left.leaf && !right.leaf && left.depth == right.depth;
        if (aligned) {
            for (unsigned position = 0; position < left.depth; ++position) {
                if (nibble(left.prefix, position) != nibble(right.prefix, position)) {
                    aligned = false;
                    break;
                }
            }
        }
        if (aligned) {
            for (std::size_t i = 0; i < 16; ++i) {
                diffHashes(left.children[i], right.children[i], output);
            }
            return;
        }
    }
    std::vector<std::pair<Key, Hash>> leftValues;
    std::vector<std::pair<Key, Hash>> rightValues;
    listHashes(from, leftValues);
    listHashes(to, rightValues);
    std::map<Key, std::pair<std::optional<Hash>, std::optional<Hash>>> combined;
    for (const std::pair<Key, Hash>& entry : leftValues) combined[entry.first].first = entry.second;
    for (const std::pair<Key, Hash>& entry : rightValues) combined[entry.first].second = entry.second;
    for (const std::pair<const Key, std::pair<std::optional<Hash>, std::optional<Hash>>>& entry : combined) {
        if (entry.second.first != entry.second.second) {
            output.push_back(Difference{entry.first, entry.second.first, entry.second.second});
        }
    }
}

Revision Repository::readRevision(const Hash& hash) {
    const Bytes payload = load(hash);
    // Native prototype format 2. No pre-release migration is supported.
    // S | content root | reverse-reference root | indexed flag | parent count | parents
    require(payload.size() >= 67 && payload[0] == 'S', "invalid native revision object");
    const std::uint8_t flags = payload[65];
    const std::size_t parentCount = payload[66];
    require(flags <= 1 && parentCount <= 2 && payload.size() == 67 + 32 * parentCount,
            "invalid native revision metadata");
    Revision revision{};
    revision.id = hash;
    revision.root = readHash(payload, 1);
    revision.referencesRoot = readHash(payload, 33);
    revision.modelIndexed = (flags == 1);
    require(revision.modelIndexed || isZero(revision.referencesRoot),
            "raw revision contains a model reference index");
    for (std::size_t i = 0; i < parentCount; ++i) {
        revision.parents.push_back(readHash(payload, 67 + i * 32));
    }
    return revision;
}

Hash Repository::branchHead(std::string_view name) {
    checkBranch(name);
    const std::filesystem::path path = this->directory_ / "refs" / "heads" / std::string(name);
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "branch does not exist: " + std::string(name));
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    require(content.size() == 65 && content.back() == '\n', "invalid branch ref format");
    Hash hash = fromHex(std::string_view(content.data(), 64));
    require(!isZero(hash), "branch ref points to zero hash");
    return hash;
}

void Repository::publishRef(std::string_view branch, const Hash& commitHash, bool mustBeAbsent) {
    checkBranch(branch);
    const std::filesystem::path heads = this->directory_ / "refs" / "heads";
    const std::filesystem::path final = heads / std::string(branch);
    if (mustBeAbsent) require(!std::filesystem::exists(final), "branch already exists");
    const std::filesystem::path temporary = heads / ("." + std::string(branch) + ".tmp");
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    systemCall(fd >= 0, "open temporary branch ref");
    try {
        const std::string value = toHex(commitHash) + "\n";
        writeExact(fd, reinterpret_cast<const std::uint8_t*>(value.data()), value.size(), 0);
        systemCall(::fsync(fd) == 0, "fsync temporary branch ref");
        systemCall(::close(fd) == 0, "close temporary branch ref");
        fd = -1;
        systemCall(::rename(temporary.c_str(), final.c_str()) == 0, "publish branch ref");
        syncDirectory(heads);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        throw;
    }
}

void Repository::createBranch(std::string_view name, std::optional<std::string_view> from) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(name);
    const std::filesystem::path ref = this->directory_ / "refs" / "heads" / std::string(name);
    require(!std::filesystem::exists(ref), "branch already exists");
    Hash initial{};
    if (from) {
        initial = branchHead(*from);
    } else {
        Bytes revision{'S'};
        appendHash(revision, Hash{});
        appendHash(revision, Hash{});
        revision.push_back(0); // raw / not indexed
        revision.push_back(0); // no parents
        initial = store(std::move(revision));
        syncPack();
    }
    publishRef(name, initial, true);
}

Revision Repository::head(std::string_view branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return readRevision(branchHead(branch));
}

Revision Repository::commit(std::string_view branch, const std::vector<Change>& changes) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    try {
        return commitLocked(branch, changes);
    } catch (...) {
        this->stagedObjects_.clear();
        this->stagedBytes_ = 0;
        throw;
    }
}

Revision Repository::commitLocked(std::string_view branch, const std::vector<Change>& changes,
                                  std::optional<Hash> referencesRoot) {
    const Hash parent = branchHead(branch);
    const Revision previous = readRevision(parent);
    require(!previous.modelIndexed || referencesRoot.has_value(),
            "raw commits cannot modify a model-indexed branch");
    require(!referencesRoot.has_value() || previous.modelIndexed || isZero(previous.root),
            "cannot enable model indexing on a populated raw branch");
    std::map<Key, std::optional<Bytes>> updates;
    for (const Change& change : changes) updates[change.key] = change.value;
    std::vector<TreeUpdate> treeUpdates;
    treeUpdates.reserve(updates.size());
    for (const std::pair<const Key, std::optional<Bytes>>& item : updates) {
        std::optional<Hash> newValue;
        if (item.second) {
            Bytes encoded{'V'};
            encoded.insert(encoded.end(), item.second->begin(), item.second->end());
            newValue = store(std::move(encoded));
        }
        treeUpdates.emplace_back(item.first, newValue);
    }
    const Hash newRoot = applyTree(previous.root, treeUpdates, 0, treeUpdates.size(), 0);
    Bytes encoded{'S'};
    appendHash(encoded, newRoot);
    appendHash(encoded, referencesRoot.value_or(Hash{}));
    encoded.push_back(referencesRoot.has_value() ? 1 : 0);
    encoded.push_back(1); // one parent
    appendHash(encoded, parent);
    const Hash newRevision = store(std::move(encoded));
    // Publication order is critical: a crash may leave unused packed objects,
    // but must never leave a published ref pointing to unsynced content.
    syncPack();
    publishRef(branch, newRevision, false);
    return readRevision(newRevision);
}


std::vector<Key> Repository::readReferrers(const Hash& root, const Key& target) {
    const std::optional<Hash> indexValue = lookup(root, target);
    if (!indexValue) return {};
    return decodeReferrers(load(*indexValue));
}

Revision Repository::commitModel(std::string_view branch, const std::vector<ModelChange>& changes,
                                 std::optional<Hash> expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    try {
    const Revision previous = readRevision(branchHead(branch));
    require(!expectedHead || previous.id == *expectedHead,
            "branch head changed since the model update was prepared");
    require(previous.modelIndexed || isZero(previous.root),
            "cannot commit model records to a populated raw branch");

    std::map<Key, std::optional<ModelRecord>> updates;
    for (const ModelChange& change : changes) {
        require(change.key != Key{}, "null model UUID is not allowed");
        std::optional<ModelRecord> canonical = change.record;
        if (canonical) {
            require(canonical->kind != 0, "model kind must be nonzero");
            std::sort(canonical->references.begin(), canonical->references.end());
            canonical->references.erase(std::unique(canonical->references.begin(),
                                        canonical->references.end()), canonical->references.end());
        }
        const std::pair<std::map<Key, std::optional<ModelRecord>>::iterator, bool> inserted =
            updates.emplace(change.key, std::move(canonical));
        require(inserted.second, "duplicate model UUID in commit");
    }

    // Evaluate old and new outgoing edges without traversing unrelated model
    // objects. An update only touches its own incident reverse-index entries.
    std::map<Key, std::set<Key>> removedEdges;
    std::map<Key, std::set<Key>> addedEdges;
    for (const std::pair<const Key, std::optional<ModelRecord>>& item : updates) {
        const std::optional<Hash> existing = lookup(previous.root, item.first);
        std::vector<Key> oldReferences;
        if (existing) {
            const Bytes stored = load(*existing);
            require(!stored.empty() && stored.front() == 'V', "invalid model value object");
            const ModelRecord old = decodeModelRecord(Bytes(stored.begin() + 1, stored.end()));
            oldReferences = old.references;
        }
        const std::vector<Key> newReferences = item.second ? item.second->references : std::vector<Key>{};
        for (const Key& referenced : oldReferences) {
            if (!std::binary_search(newReferences.begin(), newReferences.end(), referenced)) {
                removedEdges[referenced].insert(item.first);
            }
        }
        for (const Key& referenced : newReferences) {
            require(referenced != Key{} && referenced != item.first,
                    "invalid model UUID reference");
            if (!std::binary_search(oldReferences.begin(), oldReferences.end(), referenced)) {
                addedEdges[referenced].insert(item.first);
            }
            const std::map<Key, std::optional<ModelRecord>>::const_iterator pending = updates.find(referenced);
            if (pending != updates.end()) {
                require(pending->second.has_value(),
                        "model reference target deleted by same commit");
            } else {
                const std::optional<Hash> destination = lookup(previous.root, referenced);
                require(destination.has_value(), "model reference target does not exist");
            }
        }
    }

    std::set<Key> affected;
    for (const std::pair<const Key, std::set<Key>>& item : removedEdges) affected.insert(item.first);
    for (const std::pair<const Key, std::set<Key>>& item : addedEdges) affected.insert(item.first);

    // Merge incoming edges in-memory first, without writing any pack bytes.
    // Rejected deletions leave both the branch head and pack unchanged.
    std::vector<std::pair<Key, std::set<Key>>> nextIncoming;
    nextIncoming.reserve(affected.size());
    for (const Key& target : affected) {
        const std::vector<Key> original = readReferrers(previous.referencesRoot, target);
        std::set<Key> inbound(original.begin(), original.end());
        const std::map<Key, std::set<Key>>::const_iterator remove = removedEdges.find(target);
        if (remove != removedEdges.end()) {
            for (const Key& source : remove->second) inbound.erase(source);
        }
        const std::map<Key, std::set<Key>>::const_iterator add = addedEdges.find(target);
        if (add != addedEdges.end()) inbound.insert(add->second.begin(), add->second.end());
        nextIncoming.emplace_back(target, std::move(inbound));
    }
    for (const std::pair<const Key, std::optional<ModelRecord>>& item : updates) {
        if (item.second) continue;
        const std::vector<std::pair<Key, std::set<Key>>>::const_iterator changed =
            std::lower_bound(nextIncoming.begin(), nextIncoming.end(), item.first,
                [](const std::pair<Key, std::set<Key>>& candidate, const Key& key) {
                    return candidate.first < key;
                });
        if (changed != nextIncoming.end() && changed->first == item.first) {
            require(changed->second.empty(), "cannot delete referenced model UUID");
        } else {
            require(readReferrers(previous.referencesRoot, item.first).empty(),
                    "cannot delete referenced model UUID");
        }
    }

    // Preflight serialization and size limits before writing ANY pack data.
    // Invalid requests must not leave even unreachable index objects behind.
    std::vector<Change> encoded;
    encoded.reserve(updates.size());
    for (const std::pair<const Key, std::optional<ModelRecord>>& item : updates) {
        if (item.second) {
            Bytes value = encodeModelRecord(*item.second);
            require(value.size() < maxObjectBytes, "encoded model value exceeds pack limit");
            encoded.push_back(Change{item.first, std::move(value)});
        } else {
            encoded.push_back(Change{item.first, std::nullopt});
        }
    }
    std::vector<std::pair<Key, std::optional<Bytes>>> pendingIndex;
    pendingIndex.reserve(nextIncoming.size());
    for (const std::pair<Key, std::set<Key>>& item : nextIncoming) {
        if (item.second.empty()) {
            pendingIndex.emplace_back(item.first, std::nullopt);
        } else {
            Bytes value = encodeReferrers(item.second);
            require(value.size() <= maxObjectBytes, "incoming model index exceeds pack limit");
            pendingIndex.emplace_back(item.first, std::move(value));
        }
    }

    // Append-only index and primary tree roots are published together in a
    // single revision after fsync. Interrupted writes remain unreachable.
    std::vector<TreeUpdate> indexUpdates;
    indexUpdates.reserve(pendingIndex.size());
    for (std::pair<Key, std::optional<Bytes>>& item : pendingIndex) {
        if (item.second) indexUpdates.emplace_back(item.first, store(std::move(*item.second)));
        else indexUpdates.emplace_back(item.first, std::nullopt);
    }
    const Hash updatedReferences = applyTree(previous.referencesRoot, indexUpdates,
                                              0, indexUpdates.size(), 0);
    return commitLocked(branch, encoded, updatedReferences);
    } catch (...) {
        this->stagedObjects_.clear();
        this->stagedBytes_ = 0;
        throw;
    }
}

std::vector<Key> Repository::referencingModel(std::string_view branch, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision revision = readRevision(branchHead(branch));
    require(revision.modelIndexed, "reverse references are unavailable on raw branch");
    return readReferrers(revision.referencesRoot, key);
}

std::optional<ModelRecord> Repository::getModel(std::string_view branch, const Key& key) {
    const std::optional<Bytes> raw = get(branch, key);
    if (!raw) return std::nullopt;
    return decodeModelRecord(*raw);
}

std::optional<Bytes> Repository::get(std::string_view branch, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision revision = readRevision(branchHead(branch));
    const std::optional<Hash> payloadHash = lookup(revision.root, key);
    if (!payloadHash) return std::nullopt;
    const Bytes encoded = load(*payloadHash);
    require(!encoded.empty() && encoded.front() == 'V', "invalid value object");
    return Bytes(encoded.begin() + 1, encoded.end());
}

std::vector<std::pair<Key, Bytes>> Repository::scan(std::string_view branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision revision = readRevision(branchHead(branch));
    std::vector<std::pair<Key, Hash>> entries;
    listHashes(revision.root, entries);
    std::sort(entries.begin(), entries.end(), [](const std::pair<Key, Hash>& first,
                                                  const std::pair<Key, Hash>& second) {
        return first.first < second.first;
    });
    std::vector<std::pair<Key, Bytes>> result;
    result.reserve(entries.size());
    for (const std::pair<Key, Hash>& entry : entries) {
        const Bytes encoded = load(entry.second);
        require(!encoded.empty() && encoded.front() == 'V', "invalid value object");
        result.emplace_back(entry.first, Bytes(encoded.begin() + 1, encoded.end()));
    }
    return result;
}

std::vector<Difference> Repository::diff(std::string_view from, std::string_view to) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision left = readRevision(branchHead(from));
    const Revision right = readRevision(branchHead(to));
    std::vector<Difference> result;
    diffHashes(left.root, right.root, result);
    std::sort(result.begin(), result.end(), [](const Difference& first, const Difference& second) {
        return first.key < second.key;
    });
    return result;
}

void Repository::verifyPack() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    if (this->blockStore_) { this->blockStore_->verify(); return; }
    refreshIndex();
    std::uint64_t position = 0;
    std::uint64_t checked = 0;
    while (position < this->scanOffset_) {
        require(this->scanOffset_ - position >= headerSize, "truncated pack header");
        std::array<std::uint8_t, headerSize> header{};
        readExact(this->packFd_, header.data(), header.size(), position);
        require(header[0] == 'A' && header[1] == 'O' && header[2] == 'B' &&
                header[3] == 'J' && header[4] == 1, "corrupt pack record header");
        Hash expected{};
        std::copy_n(header.begin() + 5, 32, expected.begin());
        const std::uint64_t length = readU64(header.data() + 37);
        require(this->scanOffset_ - position >= headerSize &&
                length <= maxObjectBytes && length <= this->scanOffset_ - position - headerSize,
                "invalid content pack object boundary");
        Bytes payload(static_cast<std::size_t>(length));
        if (!payload.empty()) {
            readExact(this->packFd_, payload.data(), payload.size(), position + headerSize);
        }
        require(sha256(payload) == expected, "content pack checksum mismatch");
        const std::unordered_map<Hash, Location, HashHasher>::const_iterator entry =
            this->index_.find(expected);
        require(entry != this->index_.end() && entry->second.offset == position + headerSize &&
                entry->second.size == length, "index differs from content pack");
        ++checked;
        position += headerSize + length;
    }
    require(position == this->scanOffset_ && checked == this->index_.size(),
            "pack verification found inconsistent object count");
}

std::uint64_t Repository::objectCount() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    if (this->blockStore_) return this->blockStore_->stats().objects;
    refreshIndex();
    return static_cast<std::uint64_t>(this->index_.size());
}

std::uint64_t Repository::packSize() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    if (this->blockStore_) return this->blockStore_->stats().packBytes;
    refreshIndex();
    return this->scanOffset_;
}

CompactionStats Repository::compact() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    ScopedMaintenanceExclusive alone(this->lifetimeFd_);
    if (!this->blockStore_) refreshIndex();

    CompactionStats stats{};
    if (this->blockStore_) {
        const BlockStorePrototype::Stats before = this->blockStore_->stats();
        stats.objectsBefore = before.objects;
        stats.packBytesBefore = before.packBytes;
        stats.indexBytesBefore = before.indexBytes;
    } else {
        stats.objectsBefore = static_cast<std::uint64_t>(this->index_.size());
        stats.packBytesBefore = this->scanOffset_;
        struct stat oldIndex{};
        systemCall(::fstat(this->indexFd_, &oldIndex) == 0, "stat content index");
        require(oldIndex.st_size >= 0, "invalid index size");
        stats.indexBytesBefore = static_cast<std::uint64_t>(oldIndex.st_size);
    }

    // The retention policy is intentionally conservative: preserve EVERY
    // revision on the parent chain of EVERY branch head, plus both trees and
    // every leaf payload (including reverse-reference index values).
    enum class ReachableKind { Revision, Tree, Value };
    std::vector<std::pair<Hash, ReachableKind>> pending;
    const std::filesystem::path heads = this->directory_ / "refs" / "heads";
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(heads)) {
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') continue; // abandoned temporary ref
        require(entry.is_regular_file() && !entry.is_symlink(),
                "unexpected non-file in branch refs");
        checkBranch(name);
        pending.emplace_back(branchHead(name), ReachableKind::Revision);
    }
    std::unordered_set<Hash, HashHasher> reachable;
    reachable.reserve(static_cast<std::size_t>(stats.objectsBefore));
    while (!pending.empty()) {
        const std::pair<Hash, ReachableKind> next = pending.back();
        pending.pop_back();
        if (isZero(next.first) || !reachable.insert(next.first).second) continue;
        if (next.second == ReachableKind::Revision) {
            const Revision revision = readRevision(next.first);
            pending.emplace_back(revision.root, ReachableKind::Tree);
            pending.emplace_back(revision.referencesRoot, ReachableKind::Tree);
            for (const Hash& parent : revision.parents) {
                pending.emplace_back(parent, ReachableKind::Revision);
            }
        } else if (next.second == ReachableKind::Tree) {
            const TreeNode node = readNode(next.first);
            if (node.leaf) {
                pending.emplace_back(node.value, ReachableKind::Value);
            } else {
                for (const Hash& child : node.children) {
                    pending.emplace_back(child, ReachableKind::Tree);
                }
            }
        } else {
            (void)load(next.first); // validate all retained content
        }
    }
    require(reachable.size() <= stats.objectsBefore, "invalid reachable object count");
    if (this->blockStore_) {
        // Publish an entirely new, verified generation before retiring the old
        // one. Branch references remain untouched and are protected by the
        // repository-wide lifetime/operation locks throughout maintenance.
        const std::filesystem::path blocksDir = this->directory_ / "blocks";
        const std::filesystem::path oldActive = this->blockStore_->activeDataDirectory();
        std::array<std::uint8_t, 16> nonce{};
        require(RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) == 1,
                "cannot generate block generation token");
        constexpr char hex[] = "0123456789abcdef";
        std::string token;
        token.reserve(nonce.size() * 2);
        for (const std::uint8_t byte : nonce) {
            token += hex[byte >> 4];
            token += hex[byte & 15];
        }
        const std::filesystem::path nextDirectory = blocksDir / ("generation-" + token);
        const std::filesystem::path pending = blocksDir / "candidate.active.pending";
        bool published = false;
        bool pendingOwned = false;
        try {
            BlockStorePrototype::createEmpty(nextDirectory);
            {
                BlockStorePrototype replacement(nextDirectory);
                // Deterministic order makes compaction reproducible. Memory
                // stays bounded even when hundreds of thousands of objects
                // remain reachable through retained historical revisions.
                std::vector<Hash> sorted(reachable.begin(), reachable.end());
                std::sort(sorted.begin(), sorted.end());
                std::vector<Bytes> batch;
                std::size_t bytes = 0;
                for (const Hash& hash : sorted) {
                    Bytes payload = load(hash); // SHA-256 validated on every read
                    bytes += payload.size();
                    batch.emplace_back(std::move(payload));
                    if (bytes >= 4U * 1024U * 1024U) {
                        replacement.putBatch(batch);
                        batch.clear();
                        bytes = 0;
                    }
                }
                if (!batch.empty()) replacement.putBatch(batch);
                replacement.verify();
                const BlockStorePrototype::Stats after = replacement.stats();
                stats.objectsAfter = after.objects;
                stats.packBytesAfter = after.packBytes;
                stats.indexBytesAfter = after.indexBytes;
                require(stats.objectsAfter == reachable.size(),
                        "compacted block object count differs from retention set");
            }
            syncDirectory(blocksDir);
            const std::string pointer = "AOBGEN1 " + token + "\n";
            const int fd = ::open(pending.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                  O_NOFOLLOW | O_CLOEXEC, 0600);
            systemCall(fd >= 0, "create pending block generation pointer");
            pendingOwned = true;
            try {
                writeExact(fd, reinterpret_cast<const std::uint8_t*>(pointer.data()),
                           pointer.size(), 0);
                systemCall(::fsync(fd) == 0, "sync pending block generation pointer");
            } catch (...) { ::close(fd); throw; }
            systemCall(::close(fd) == 0, "close pending block generation pointer");
            systemCall(::rename(pending.c_str(), (blocksDir / "candidate.active").c_str()) == 0,
                       "publish block generation pointer");
            published = true;
            syncDirectory(blocksDir);
            // New generation is now authoritative. Reopen before reclaiming
            // the old one; failure here must not delete either generation.
            this->blockStore_.reset();
            this->blockStore_ = std::make_unique<BlockStorePrototype>(blocksDir);
            if (oldActive == blocksDir) {
                std::filesystem::remove(blocksDir / "candidate.pack");
                std::filesystem::remove(blocksDir / "candidate.idx");
                std::filesystem::remove(blocksDir / "candidate.current");
            } else {
                std::filesystem::remove_all(oldActive);
            }
            syncDirectory(blocksDir);
        } catch (...) {
            if (!published) {
                std::error_code ignored;
                if (pendingOwned) std::filesystem::remove(pending, ignored);
                std::filesystem::remove_all(nextDirectory, ignored);
            } else {
                // Failure after publication must not allow use of a handle
                // possibly tied to the retired generation.
                this->blockStore_.reset();
            }
            throw;
        }
        return stats;
    }

    std::array<std::uint8_t, 16> random{};
    require(RAND_bytes(random.data(), static_cast<int>(random.size())) == 1,
            "failed to generate pack generation identifier");
    constexpr char digits[] = "0123456789abcdef";
    std::string nextToken;
    nextToken.reserve(random.size() * 2);
    for (std::uint8_t byte : random) {
        nextToken.push_back(digits[byte >> 4U]);
        nextToken.push_back(digits[byte & 15U]);
    }
    const std::filesystem::path nextPack = packPath(this->directory_, nextToken, false);
    const std::filesystem::path nextIndex = packPath(this->directory_, nextToken, true);
    const std::filesystem::path markerTemp =
        this->directory_ / (".objects.current." + nextToken + ".tmp");
    int newPackFd = -1;
    int newIndexFd = -1;
    int markerFd = -1;
    bool published = false;
    try {
        newPackFd = ::open(nextPack.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
        systemCall(newPackFd >= 0, "create compacted pack");
        newIndexFd = ::open(nextIndex.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
        systemCall(newIndexFd >= 0, "create compacted index");
        writeExact(newIndexFd, indexMagic.data(), indexMagic.size(), 0);
        std::uint64_t oldPosition = 0;
        std::uint64_t newPosition = 0;
        std::uint64_t indexPosition = indexHeaderSize;
        while (oldPosition < this->scanOffset_) {
            std::array<std::uint8_t, headerSize> header{};
            readExact(this->packFd_, header.data(), header.size(), oldPosition);
            require(header[0] == 'A' && header[1] == 'O' && header[2] == 'B' &&
                    header[3] == 'J' && header[4] == 1, "invalid existing pack header");
            const std::uint64_t size = readU64(header.data() + 37);
            require(size <= maxObjectBytes &&
                    this->scanOffset_ - oldPosition >= headerSize &&
                    size <= this->scanOffset_ - oldPosition - headerSize,
                    "invalid existing pack record boundary");
            Hash hash{};
            std::copy_n(header.begin() + 5, 32, hash.begin());
            if (reachable.contains(hash)) {
                const std::unordered_map<Hash, Location, HashHasher>::const_iterator entry =
                    this->index_.find(hash);
                require(entry != this->index_.end() &&
                        entry->second.offset == oldPosition + headerSize &&
                        entry->second.size == size, "pack and index mismatch during compaction");
                const Bytes payload = load(hash); // verify SHA-256 before copying
                writeExact(newPackFd, header.data(), header.size(), newPosition);
                if (!payload.empty()) {
                    writeExact(newPackFd, payload.data(), payload.size(),
                               newPosition + headerSize);
                }
                const std::array<std::uint8_t, indexEntrySize> indexed =
                    encodeIndexEntry(hash, newPosition + headerSize, size);
                writeExact(newIndexFd, indexed.data(), indexed.size(), indexPosition);
                newPosition += headerSize + size;
                indexPosition += indexEntrySize;
                ++stats.objectsAfter;
            }
            oldPosition += headerSize + size;
        }
        require(stats.objectsAfter == reachable.size(),
                "not all reachable objects were found in the pack");
        stats.packBytesAfter = newPosition;
        stats.indexBytesAfter = indexPosition;
        systemCall(::fsync(newPackFd) == 0, "sync compacted pack");
        systemCall(::fsync(newIndexFd) == 0, "sync compacted index");
        syncDirectory(this->directory_);

        markerFd = ::open(markerTemp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        systemCall(markerFd >= 0, "create temporary active-pack marker");
        const std::string marker = "PACK1 " + nextToken + "\n";
        writeExact(markerFd, reinterpret_cast<const std::uint8_t*>(marker.data()),
                   marker.size(), 0);
        systemCall(::fsync(markerFd) == 0, "sync active-pack marker");
        systemCall(::close(markerFd) == 0, "close active-pack marker");
        markerFd = -1;
        systemCall(::rename(markerTemp.c_str(),
                            (this->directory_ / "objects.current").c_str()) == 0,
                   "publish active-pack marker");
        published = true;
        syncDirectory(this->directory_);

        const std::filesystem::path oldPack =
            packPath(this->directory_, this->packToken_, false);
        const std::filesystem::path oldSidecar =
            packPath(this->directory_, this->packToken_, true);
        ::close(this->packFd_);
        ::close(this->indexFd_);
        this->packFd_ = newPackFd;
        this->indexFd_ = newIndexFd;
        newPackFd = -1;
        newIndexFd = -1;
        this->packToken_ = nextToken;
        this->index_.clear();
        this->pendingIndex_.clear();
        this->scanOffset_ = 0;
        this->indexReadOffset_ = 0;
        refreshIndex();
        systemCall(::unlink(oldPack.c_str()) == 0, "retire previous pack");
        systemCall(::unlink(oldSidecar.c_str()) == 0, "retire previous index");
        syncDirectory(this->directory_);
    } catch (...) {
        if (markerFd >= 0) ::close(markerFd);
        if (newPackFd >= 0) ::close(newPackFd);
        if (newIndexFd >= 0) ::close(newIndexFd);
        if (!published) {
            (void)::unlink(markerTemp.c_str());
            (void)::unlink(nextPack.c_str());
            (void)::unlink(nextIndex.c_str());
        }
        throw;
    }
    return stats;
}

}  // namespace aowis::native
