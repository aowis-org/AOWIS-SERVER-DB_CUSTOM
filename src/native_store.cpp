#include "branch_name_validation.h"
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_resolver_v2.h"
#include "aowis/dvmdb/revision_record_v2.h"
#include "aowis/dvmdb/storage_error.h"
#include "native_test_faults.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <limits>
#include <memory>
#include <queue>
#include <utility>
#include <sys/mman.h>
#include <stdexcept>
#include <unordered_set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aowis::dvmdb {
namespace {

constexpr std::array<std::uint8_t, 8> bundleMagic{
    'A', 'O', 'W', 'S', 'Y', 'N', 'C', '1'
};
constexpr std::array<std::uint8_t, 8> incrementalBundleMagic{
    'A', 'O', 'W', 'S', 'Y', 'N', 'C', '2'
};
constexpr std::array<std::uint8_t, 8> identifiedBundleMagic{
    'A', 'O', 'W', 'S', 'Y', 'N', 'C', '3'
};
constexpr std::array<std::uint8_t, 8> identifiedIncrementalBundleMagic{
    'A', 'O', 'W', 'S', 'Y', 'N', 'C', '4'
};
constexpr std::uint64_t bundleHeaderBytes = 48; // marker, tip, object count
constexpr std::uint64_t incrementalBundleHeaderBytes = 80; // plus required recipient base
constexpr std::uint64_t identifiedBundleHeaderBytes = 64; // plus UUIDv7 branch identity
constexpr std::uint64_t identifiedIncrementalHeaderBytes = 96; // base + UUIDv7 branch identity
constexpr std::uint64_t bundleEntryBytes = 40;  // hash, big-endian size

constexpr std::size_t headerSize = 45;
constexpr std::size_t indexHeaderSize = 8;
constexpr std::size_t indexEntrySize = 52;
constexpr std::array<std::uint8_t, indexHeaderSize> indexMagic{
    'A', 'O', 'I', 'D', 'X', '0', '1', 0
};
constexpr std::uint64_t maxObjectBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t stagedFlushThreshold = 8U * 1024U * 1024U;

using Err = StorageErrorKind;

void require(Err kind, bool condition, const std::string& message) {
    if (!condition) throw StorageError(kind, message);
}

// Bound export graph discovery, not merely the finished bundle. A duplicate
// content hash does not consume a second slot (shared Merkle subtrees and
// histories routinely point at the same immutable object).
bool insertBundleObject(std::set<Hash>& objects, const Hash& hash, std::uint64_t maxObjects) {
    const bool inserted = objects.insert(hash).second;
    if (inserted) {
        require(Err::LimitExceeded, maxObjects == 0 || objects.size() <= maxObjects,
                "bundle transfer limit: object count exceeded");
    }
    return inserted;
}

// ENOSPC/EDQUOT become ResourceExhausted, every other errno Io.
void systemCall(bool ok, const std::string& operation) {
    if (!ok) throwSystemError(operation);
}

// Failure while reading an object of the LOCAL repository during a bundle
// import. Keeps local corruption from being reported as an invalid bundle.
class LocalStorageFailure final : public StorageError {
public:
    using StorageError::StorageError;
};

// Adds context to the in-flight exception without losing its classification.
[[noreturn]] void rethrowWithContext(const std::string& prefix) {
    try {
        throw;
    } catch (const LocalStorageFailure& error) {
        throw LocalStorageFailure(error.kind(), prefix + error.what());
    } catch (const StorageError& error) {
        throw StorageError(error.kind(), prefix + error.what());
    } catch (const std::exception& error) {
        throw StorageError(Err::Internal, prefix + error.what());
    }
}

// The structural verifier is shared with verifyRepository(), where a failed
// check means local Corruption. Applied to untrusted bundle data, the same
// failure is invalid client input. Failures while reading LOCAL objects keep
// their kind (an incremental import also visits local ancestors).
[[noreturn]] void rethrowBundleFailure() {
    try {
        throw;
    } catch (const LocalStorageFailure& error) {
        throw StorageError(error.kind(), error.what());
    } catch (const StorageError& error) {
        if (error.kind() != Err::Corruption) throw;
        throw StorageError(Err::InvalidInput,
                           std::string("invalid replication bundle: ") + error.what());
    }
}

// Scratch files live in <repository>/tmp: same filesystem as the pack (real
// disk, evictable page cache), never TMPDIR, which is often a RAM-backed
// tmpfs. O_TMPFILE files have no name at all; the fallback creates a named
// file and unlinks it at once. Leftovers of that fallback are removed when
// the repository is opened (they cannot be in use, see Repository()).
constexpr std::string_view scratchPrefix = "aowis-scratch-";

int createScratchFile(const std::filesystem::path& directory, std::string_view purpose) {
    if (!detail::forceNamedScratchFiles()) {
        const int anonymous = ::open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        if (anonymous >= 0) return anonymous;
        // Filesystems without O_TMPFILE support report one of these.
        if (errno != EOPNOTSUPP && errno != EISDIR && errno != EINVAL)
            throwSystemError("create scratch file for " + std::string(purpose));
    }
    std::string name = (directory / (std::string(scratchPrefix) + std::string(purpose) + "-XXXXXX")).string();
    const int fd = ::mkostemp(name.data(), O_CLOEXEC);
    systemCall(fd >= 0, "create scratch file for " + std::string(purpose));
    // ENOENT: a concurrently opening handle already removed the leftover.
    if (::unlink(name.c_str()) != 0 && errno != ENOENT) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        throwSystemError("unlink scratch file for " + std::string(purpose));
    }
    return fd;
}

// Process-wide accounting for internal native scratch files. Reserve logical
// file extent BEFORE allocation or writing; account overlapping resize files
// separately and release on close. This is intentionally distinct from HTTP
// transfer spools and is not a cross-process filesystem quota.
struct InternalScratchAccounting {
    std::mutex mutex;
    std::uint64_t maximum = 0; // zero disables accounting limit (offline library default)
    std::uint64_t reserved = 0;
};

InternalScratchAccounting& scratchAccounting() {
    static InternalScratchAccounting state;
    return state;
}

void writeExact(int fd, const std::uint8_t* bytes, std::size_t size, std::uint64_t offset);

class AccountedScratchFile {
public:
    AccountedScratchFile() = default;
    AccountedScratchFile(const std::filesystem::path& directory, std::string_view purpose)
        : fd_(createScratchFile(directory, purpose)) {}
    ~AccountedScratchFile() { reset(); }
    AccountedScratchFile(const AccountedScratchFile&) = delete;
    AccountedScratchFile& operator=(const AccountedScratchFile&) = delete;
    AccountedScratchFile(AccountedScratchFile&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), extent_(std::exchange(other.extent_, 0)) {}
    AccountedScratchFile& operator=(AccountedScratchFile&& other) noexcept {
        if (this != &other) {
            reset();
            this->fd_ = std::exchange(other.fd_, -1);
            this->extent_ = std::exchange(other.extent_, 0);
        }
        return *this;
    }
    int fd() const noexcept { return this->fd_; }
    void reserve(std::uint64_t end) {
        if (end <= this->extent_) return;
        const std::uint64_t increase = end - this->extent_;
        InternalScratchAccounting& accounting = scratchAccounting();
        const std::lock_guard<std::mutex> lock(accounting.mutex);
        require(Err::Busy,
                increase <= std::numeric_limits<std::uint64_t>::max() - accounting.reserved &&
                (accounting.maximum == 0 ||
                 (accounting.reserved <= accounting.maximum &&
                  increase <= accounting.maximum - accounting.reserved)),
                "native internal scratch limit exceeded");
        accounting.reserved += increase;
        this->extent_ = end;
    }
    void write(const std::uint8_t* data, std::size_t bytes, std::uint64_t offset) {
        require(Err::LimitExceeded, offset <= std::numeric_limits<std::uint64_t>::max() - bytes,
                "native scratch file offset overflow");
        reserve(offset + bytes);
        detail::faultPartialWrite(this->fd_, data, bytes, offset, "scratch.partial.write");
        writeExact(this->fd_, data, bytes, offset);
    }
    void allocate(std::uint64_t bytes, const char* operation) {
        require(Err::LimitExceeded, bytes <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()),
                "native scratch allocation exceeds file size limit");
        reserve(bytes);
        const int result = ::posix_fallocate(this->fd_, 0, static_cast<off_t>(bytes));
        if (result != 0) {
            errno = result;
            systemCall(false, operation);
        }
    }
private:
    void reset() noexcept {
        if (this->fd_ >= 0) ::close(this->fd_);
        this->fd_ = -1;
        if (this->extent_ != 0) {
            InternalScratchAccounting& accounting = scratchAccounting();
            const std::lock_guard<std::mutex> lock(accounting.mutex);
            accounting.reserved -= this->extent_;
            this->extent_ = 0;
        }
    }
    int fd_ = -1;
    std::uint64_t extent_ = 0;
};

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
            throw StorageError(Err::Conflict,
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
// These entries are intentionally independent of the SQL model payload codec.
Bytes encodeReferrers(const std::set<Key>& referrers) {
    require(Err::LimitExceeded, referrers.size() <= std::numeric_limits<std::uint32_t>::max(),
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
    require(Err::Corruption, bytes.size() >= 8 && bytes[0] == 'I' && bytes[1] == 'N' &&
            bytes[2] == 'R' && bytes[3] == '1', "invalid incoming-reference index object");
    std::uint32_t count = 0;
    for (std::size_t i = 4; i < 8; ++i) {
        count = (count << 8U) | bytes[i];
    }
    require(Err::Corruption, count != 0 && static_cast<std::uint64_t>(count) * 16ULL == bytes.size() - 8,
            "invalid incoming-reference index size");
    std::vector<Key> referrers;
    referrers.reserve(count);
    std::size_t offset = 8;
    for (std::uint32_t i = 0; i < count; ++i) {
        Key source{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 16, source.begin());
        require(Err::Corruption, source != Key{} && (referrers.empty() || referrers.back() < source),
                "incoming-reference index is not canonical");
        referrers.push_back(source);
        offset += 16;
    }
    return referrers;
}

Hash readHash(const Bytes& bytes, std::size_t offset) {
    require(Err::Corruption, offset <= bytes.size() && bytes.size() - offset >= 32, "truncated hash");
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
        require(Err::Corruption, count != 0, "unexpected EOF reading content pack");
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
        require(Err::Internal, count != 0, "pwrite made no progress");
        done += static_cast<std::size_t>(count);
    }
}

// Full-history exports may contain millions of reachable immutable objects.
// Keep the visited hash set on repository-local scratch storage, not in a
// process-size std::set. A sparse open-addressed table uses pread/pwrite
// rather than writable mmap: ENOSPC is a recoverable StorageError, never a
// SIGBUS from a failed page fault. A zero hash denotes an unoccupied slot.
class DiskBundleHashes {
public:
    explicit DiskBundleHashes(const std::filesystem::path& scratch)
        : scratch_(scratch), fd_(scratch, "export-visited") {
        resize(1024);
    }

    DiskBundleHashes(const DiskBundleHashes&) = delete;
    DiskBundleHashes& operator=(const DiskBundleHashes&) = delete;

    bool insert(const Hash& hash, std::uint64_t maxObjects) {
        require(Err::Internal, !isZero(hash), "zero object in export graph");
        const std::pair<std::uint64_t, bool> found = slot(hash, this->fd_.fd(), this->capacity_);
        if (found.second) return false;
        require(Err::LimitExceeded, maxObjects == 0 || this->count_ < maxObjects,
                "bundle transfer limit: object count exceeded");
        std::uint64_t destination = found.first;
        if (this->count_ >= this->capacity_ / 2) {
            require(Err::LimitExceeded, this->capacity_ <= maximumSlots / 2,
                    "temporary export hash table exceeds file size limit");
            resize(this->capacity_ * 2);
            destination = slot(hash, this->fd_.fd(), this->capacity_).first;
        }
        this->fd_.write(hash.data(), hash.size(), destination * hash.size());
        ++this->count_;
        return true;
    }

    std::uint64_t size() const { return this->count_; }

    bool contains(const Hash& hash) const {
        return !isZero(hash) && slot(hash, this->fd_.fd(), this->capacity_).second;
    }

    template<class Consumer>
    void forEach(Consumer&& consume) const {
        constexpr std::size_t batchHashes = 256;
        std::array<Hash, batchHashes> batch{};
        for (std::uint64_t index = 0; index < this->capacity_;) {
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(batchHashes, this->capacity_ - index));
            readExact(this->fd_.fd(), reinterpret_cast<std::uint8_t*>(batch.data()),
                      count * sizeof(Hash), index * sizeof(Hash));
            for (std::size_t offset = 0; offset < count; ++offset)
                if (!isZero(batch[offset])) consume(batch[offset]);
            index += count;
        }
    }

private:
    static constexpr std::uint64_t maximumSlots =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / sizeof(Hash);

    static std::pair<std::uint64_t, bool> slot(const Hash& hash, int fd,
                                              std::uint64_t capacity) {
        // SHA-256 already distributes the first eight bytes uniformly.
        std::uint64_t first = 0;
        for (unsigned index = 0; index < 8; ++index)
            first = (first << 8U) | hash[index];
        std::uint64_t position = first & (capacity - 1);
        Hash candidate{};
        for (std::uint64_t probe = 0; probe < capacity; ++probe) {
            readExact(fd, candidate.data(), candidate.size(), position * candidate.size());
            if (isZero(candidate)) return {position, false};
            if (candidate == hash) return {position, true};
            position = (position + 1) & (capacity - 1);
        }
        throw StorageError(Err::Internal, "full export hash table exhausted");
    }

    void resize(std::uint64_t newCapacity) {
        require(Err::LimitExceeded, newCapacity <= maximumSlots,
                "temporary export hash table exceeds file size limit");
        AccountedScratchFile replacement(this->scratch_, "export-visited-grow");
        replacement.reserve(newCapacity * sizeof(Hash));
        systemCall(::ftruncate(replacement.fd(), static_cast<off_t>(newCapacity * sizeof(Hash))) == 0,
                   "size export visited table");
        if (this->capacity_ != 0) {
            this->forEach([&](const Hash& value) {
                const std::uint64_t position = slot(value, replacement.fd(), newCapacity).first;
                replacement.write(value.data(), value.size(), position * value.size());
            });
        }
        this->fd_ = std::move(replacement);
        this->capacity_ = newCapacity;
    }

    std::filesystem::path scratch_;
    AccountedScratchFile fd_;
    std::uint64_t capacity_ = 0;
    std::uint64_t count_ = 0;
};

// A bounded in-memory chunk sorter with an unlinked scratch run file. Sorting
// the visited table by SHA-256 preserves byte-for-byte bundle compatibility.
// Only a hash and run number per sorted run are resident during the merge.
class SortedBundleHashes {
public:
    static constexpr std::size_t chunkHashes = 2048;

    explicit SortedBundleHashes(const std::filesystem::path& scratch) : scratch_(scratch) {
        this->buffer_.reserve(chunkHashes);
    }

    SortedBundleHashes(const SortedBundleHashes&) = delete;
    SortedBundleHashes& operator=(const SortedBundleHashes&) = delete;

    void append(const Hash& hash) {
        require(Err::Internal, !this->finalized_, "cannot append to finalized export sorter");
        this->buffer_.push_back(hash);
        if (this->buffer_.size() == chunkHashes) flush();
    }

    void finalize() {
        require(Err::Internal, !this->finalized_, "export sorter finalized twice");
        this->finalized_ = true;
        if (this->runs_.empty()) {
            std::sort(this->buffer_.begin(), this->buffer_.end());
            return;
        }
        flush();
        for (std::size_t index = 0; index < this->runs_.size(); ++index) {
            this->next_.push(Head{readRunHash(this->runs_[index].start), index});
        }
    }

    std::optional<Hash> next() {
        require(Err::Internal, this->finalized_, "export sorter must be finalized");
        if (this->runs_.empty()) {
            if (this->cursor_ == this->buffer_.size()) return std::nullopt;
            return this->buffer_[this->cursor_++];
        }
        if (this->next_.empty()) return std::nullopt;
        const Head first = this->next_.top();
        this->next_.pop();
        Run& run = this->runs_[first.run];
        ++run.used;
        if (run.used < run.count)
            this->next_.push(Head{readRunHash(run.start + run.used), first.run});
        return first.hash;
    }

private:
    struct Run { std::uint64_t start = 0; std::uint64_t count = 0; std::uint64_t used = 0; };
    struct Head { Hash hash{}; std::size_t run = 0; };
    struct GreaterHead {
        bool operator()(const Head& left, const Head& right) const {
            return left.hash > right.hash;
        }
    };

    Hash readRunHash(std::uint64_t ordinal) const {
        Hash hash{};
        readExact(this->fd_.fd(), hash.data(), hash.size(), ordinal * hash.size());
        return hash;
    }

    void flush() {
        if (this->buffer_.empty()) return;
        std::sort(this->buffer_.begin(), this->buffer_.end());
        if (this->fd_.fd() < 0) this->fd_ = AccountedScratchFile(this->scratch_, "export-sort");
        require(Err::LimitExceeded,
                this->records_ <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max() /
                                     sizeof(Hash)) - this->buffer_.size(),
                "temporary export sorter exceeds file size limit");
        this->fd_.write(reinterpret_cast<const std::uint8_t*>(this->buffer_.data()),
                   this->buffer_.size() * sizeof(Hash), this->records_ * sizeof(Hash));
        this->runs_.push_back(Run{this->records_, this->buffer_.size(), 0});
        this->records_ += this->buffer_.size();
        this->buffer_.clear();
    }

    std::filesystem::path scratch_;
    AccountedScratchFile fd_;
    std::uint64_t records_ = 0;
    std::size_t cursor_ = 0;
    bool finalized_ = false;
    std::vector<Hash> buffer_;
    std::vector<Run> runs_;
    std::priority_queue<Head, std::vector<Head>, GreaterHead> next_;
};

// The on-disk bundle is sorted by SHA-256 but has variable-sized payloads.
// Record fixed-size file offsets in an unlinked temporary index rather than
// retaining one std::map node per transferred object on the C++ heap. Mapping
// it read-only lets the kernel evict cold pages under memory pressure.
constexpr std::size_t bundleLocationRecordBytes = 48;

struct BundleLocation {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

class BundleLocationIndex {
public:
    BundleLocationIndex(std::uint64_t capacity, const std::filesystem::path& scratch) {
        require(Err::LimitExceeded, capacity <= std::numeric_limits<std::size_t>::max() /
                                bundleLocationRecordBytes,
                "replication bundle index exceeds addressable memory");
        this->fd_ = AccountedScratchFile(scratch, "bundle-index");
    }

    ~BundleLocationIndex() {
        if (this->mapped_ != nullptr)
            ::munmap(this->mapped_, this->count_ * bundleLocationRecordBytes);
    }

    BundleLocationIndex(const BundleLocationIndex&) = delete;
    BundleLocationIndex& operator=(const BundleLocationIndex&) = delete;

    void append(const Hash& hash, BundleLocation location) {
        require(Err::Internal, this->mapped_ == nullptr, "cannot append to finalized replication index");
        std::array<std::uint8_t, bundleLocationRecordBytes> record{};
        std::copy(hash.begin(), hash.end(), record.begin());
        storeU64(record.data() + 32, location.offset);
        storeU64(record.data() + 40, location.size);
        this->fd_.write(record.data(), record.size(),
                   static_cast<std::uint64_t>(this->count_) * bundleLocationRecordBytes);
        ++this->count_;
    }

    void finalize() {
        if (this->count_ == 0) return;
        void* const mapped = ::mmap(nullptr, this->count_ * bundleLocationRecordBytes,
                                    PROT_READ, MAP_SHARED, this->fd_.fd(), 0);
        systemCall(mapped != MAP_FAILED, "map temporary replication index");
        this->mapped_ = mapped;
    }

    std::size_t size() const { return this->count_; }

    std::pair<Hash, BundleLocation> at(std::size_t index) const {
        require(Err::Internal, index < this->count_ && this->mapped_ != nullptr,
                "replication index lookup out of range");
        const std::uint8_t* record = static_cast<const std::uint8_t*>(this->mapped_) +
                                     index * bundleLocationRecordBytes;
        Hash hash{};
        std::copy_n(record, hash.size(), hash.begin());
        return {hash, BundleLocation{readU64(record + 32), readU64(record + 40)}};
    }

    std::optional<std::size_t> ordinal(const Hash& hash) const {
        if (this->count_ == 0) return std::nullopt;
        std::size_t first = 0;
        std::size_t last = this->count_;
        while (first < last) {
            const std::size_t middle = first + (last - first) / 2;
            const std::uint8_t* record = static_cast<const std::uint8_t*>(this->mapped_) +
                                         middle * bundleLocationRecordBytes;
            if (std::memcmp(record, hash.data(), hash.size()) < 0) first = middle + 1;
            else last = middle;
        }
        if (first >= this->count_) return std::nullopt;
        const std::uint8_t* record = static_cast<const std::uint8_t*>(this->mapped_) +
                                     first * bundleLocationRecordBytes;
        if (std::memcmp(record, hash.data(), hash.size()) != 0) return std::nullopt;
        return first;
    }

    std::optional<BundleLocation> find(const Hash& hash) const {
        const std::optional<std::size_t> index = ordinal(hash);
        if (!index) return std::nullopt;
        return at(*index).second;
    }

private:
    AccountedScratchFile fd_;
    std::size_t count_ = 0;
    void* mapped_ = nullptr;
};

// A type mask for each hash in the sorted bundle index. Keeping these bytes
// in an unlinked, shared file mapping avoids allocating a separate red-black
// tree node (and a duplicate reachable-object set node) for every transferred
// object. Cold pages can be reclaimed by the OS during very large imports.
class BundleVisitMarks {
public:
    BundleVisitMarks(std::size_t count, const std::filesystem::path& scratch) : count_(count) {
        if (count == 0) return;
        this->fd_ = AccountedScratchFile(scratch, "bundle-marks");
        try {
            require(Err::LimitExceeded, count <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()),
                    "verification marks exceed file address limits");
            // ftruncate() can leave sparse pages whose first write would
            // SIGBUS under ENOSPC. Reserve the temporary storage now so a
            // full disk produces a catchable import error before publication.
            this->fd_.allocate(count, "reserve temporary verification marks");
            void* const mapped = ::mmap(nullptr, count, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, this->fd_.fd(), 0);
            systemCall(mapped != MAP_FAILED, "map temporary verification marks");
            this->mapped_ = static_cast<std::uint8_t*>(mapped);
        } catch (...) {
            throw;
        }
    }

    ~BundleVisitMarks() {
        if (this->mapped_ != nullptr) ::munmap(this->mapped_, this->count_);
    }
    BundleVisitMarks(const BundleVisitMarks&) = delete;
    BundleVisitMarks& operator=(const BundleVisitMarks&) = delete;

    std::uint8_t at(std::size_t index) const {
        require(Err::Internal, index < this->count_, "verification mark index out of bounds");
        return this->mapped_[index];
    }

    void mark(std::size_t index, std::uint8_t bits) {
        require(Err::Internal, index < this->count_, "verification mark index out of bounds");
        require(Err::Internal, bits != 0, "empty verification mark");
        if (this->mapped_[index] == 0) ++this->marked_;
        this->mapped_[index] = bits;
    }

    std::uint64_t markedCount() const { return this->marked_; }

private:
    AccountedScratchFile fd_;
    std::size_t count_ = 0;
    std::uint8_t* mapped_ = nullptr;
    std::uint64_t marked_ = 0;
};

// A verified trie summary consists of one representative UUID and the root
// depth (or a leaf flag). During large bundle imports there can be many more
// summaries than reachable revisions. Keep summaries for transferred objects
// in a private, disk-backed table indexed by the sorted bundle object ordinal.
// A hash may be interpreted as a raw, model, or reference tree, so each kind
// has an independent slot: leaf VALUE validation depends on that kind.
struct TreeSummary {
    Key representative{};
    bool leaf = false;
    unsigned depth = 0;
};

class BundleTreeSummaries {
public:
    static constexpr std::size_t slotBytes = 20;
    static constexpr std::size_t kinds = 3;
    static constexpr std::size_t entryBytes = slotBytes * kinds;

    BundleTreeSummaries(std::size_t count, const std::filesystem::path& scratch) : count_(count) {
        if (count == 0) return;
        require(Err::LimitExceeded, count <= std::numeric_limits<std::size_t>::max() / entryBytes &&
                    count <= static_cast<std::uint64_t>(
                        std::numeric_limits<off_t>::max()) / entryBytes,
                "replication trie summary table exceeds file address limits");
        this->fd_ = AccountedScratchFile(scratch, "trie-summaries");
        try {
            const std::size_t bytes = count * entryBytes;
            // A sparse mapped file can SIGBUS on first access under ENOSPC.
            // Reserve its blocks up front and fail the import cleanly instead.
            this->fd_.allocate(bytes, "reserve temporary trie summary table");
            void* const mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, this->fd_.fd(), 0);
            systemCall(mapped != MAP_FAILED, "map temporary trie summary table");
            this->mapped_ = static_cast<std::uint8_t*>(mapped);
        } catch (...) {
            throw;
        }
    }

    ~BundleTreeSummaries() {
        if (this->mapped_ != nullptr) ::munmap(this->mapped_, this->count_ * entryBytes);
    }
    BundleTreeSummaries(const BundleTreeSummaries&) = delete;
    BundleTreeSummaries& operator=(const BundleTreeSummaries&) = delete;

    std::optional<TreeSummary> read(std::size_t ordinal, unsigned kind) const {
        const std::uint8_t* slot = slotAt(ordinal, kind);
        if (slot[0] == 0) return std::nullopt;
        TreeSummary summary{};
        std::copy_n(slot + 4, summary.representative.size(), summary.representative.begin());
        summary.leaf = slot[1] != 0;
        summary.depth = slot[2];
        return summary;
    }

    void write(std::size_t ordinal, unsigned kind, const TreeSummary& summary) {
        std::uint8_t* slot = slotAt(ordinal, kind);
        require(Err::Internal, slot[0] == 0, "duplicate replication trie summary");
        require(Err::Internal, summary.depth < 32, "invalid replication trie summary depth");
        // Set the validity byte last: a failed verification never caches a
        // partially written summary, even if an import throws during parsing.
        slot[1] = summary.leaf ? 1 : 0;
        slot[2] = static_cast<std::uint8_t>(summary.depth);
        std::copy(summary.representative.begin(), summary.representative.end(), slot + 4);
        slot[0] = 1;
        ++this->saved_;
    }

    std::uint64_t size() const { return this->saved_; }

private:
    std::uint8_t* slotAt(std::size_t ordinal, unsigned kind) const {
        require(Err::Internal, ordinal < this->count_ && kind < kinds && this->mapped_ != nullptr,
                "replication trie summary lookup out of range");
        return this->mapped_ + ordinal * entryBytes + kind * slotBytes;
    }

    AccountedScratchFile fd_;
    std::size_t count_ = 0;
    std::uint8_t* mapped_ = nullptr;
    std::uint64_t saved_ = 0;
};

// The semantic model verifier must compare edges grouped by *target* UUID.
// Scanning models produces them grouped by *source* UUID. Sort fixed-size
// (target, source) records in bounded chunks and merge the runs from one
// private, unlinked file. Large fan-in creates only one heap entry per run;
// small snapshots avoid temporary files altogether.
class SortedReferenceEdges {
public:
    using Edge = std::array<std::uint8_t, 32>;
    static constexpr std::size_t chunkEdges = 8192;
    static_assert(sizeof(Edge) == 32);

    explicit SortedReferenceEdges(const std::filesystem::path& scratch) : scratch_(scratch) {
        this->buffer_.reserve(chunkEdges);
    }
    ~SortedReferenceEdges() {
        if (this->mapped_ != nullptr) ::munmap(this->mapped_, this->records_ * sizeof(Edge));
    }
    SortedReferenceEdges(const SortedReferenceEdges&) = delete;
    SortedReferenceEdges& operator=(const SortedReferenceEdges&) = delete;

    void append(const Key& target, const Key& source) {
        require(Err::Internal, !this->finalized_, "cannot append to finalized reference sorter");
        Edge edge{};
        std::copy(target.begin(), target.end(), edge.begin());
        std::copy(source.begin(), source.end(), edge.begin() + 16);
        this->buffer_.push_back(edge);
        if (this->buffer_.size() == chunkEdges) flush();
    }

    void finalize() {
        require(Err::Internal, !this->finalized_, "reference sorter finalized twice");
        this->finalized_ = true;
        if (this->runs_.empty()) {
            std::sort(this->buffer_.begin(), this->buffer_.end());
            return;
        }
        flush();
        require(Err::LimitExceeded, this->records_ <= std::numeric_limits<std::size_t>::max() / sizeof(Edge),
                "sorted references exceed addressable memory");
        void* const mapped = ::mmap(nullptr, this->records_ * sizeof(Edge),
                                    PROT_READ, MAP_SHARED, this->fd_.fd(), 0);
        systemCall(mapped != MAP_FAILED, "map sorted model references");
        this->mapped_ = mapped;
        for (std::size_t index = 0; index < this->runs_.size(); ++index) {
            const Run& run = this->runs_[index];
            this->next_.push(Head{mappedEdge(run.start), index});
        }
    }

    std::optional<Edge> next() {
        require(Err::Internal, this->finalized_, "reference sorter must be finalized");
        if (this->runs_.empty()) {
            if (this->cursor_ == this->buffer_.size()) return std::nullopt;
            return this->buffer_[this->cursor_++];
        }
        if (this->next_.empty()) return std::nullopt;
        const Head first = this->next_.top();
        this->next_.pop();
        Run& run = this->runs_[first.run];
        ++run.used;
        if (run.used != run.count) {
            this->next_.push(Head{mappedEdge(run.start + run.used), first.run});
        }
        return first.edge;
    }

private:
    struct Run {
        std::size_t start = 0;
        std::size_t count = 0;
        std::size_t used = 0;
    };
    struct Head {
        Edge edge{};
        std::size_t run = 0;
    };
    struct GreaterHead {
        bool operator()(const Head& left, const Head& right) const {
            return left.edge > right.edge;
        }
    };

    Edge mappedEdge(std::size_t ordinal) const {
        const Edge* const entries = static_cast<const Edge*>(this->mapped_);
        return entries[ordinal];
    }

    void flush() {
        if (this->buffer_.empty()) return;
        std::sort(this->buffer_.begin(), this->buffer_.end());
        if (this->fd_.fd() < 0) this->fd_ = AccountedScratchFile(this->scratch_, "reference-edges");
        require(Err::LimitExceeded, this->records_ <= static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max() / sizeof(Edge)) -
                    this->buffer_.size(),
                "temporary model reference sorter exceeds file size limit");
        this->fd_.write(reinterpret_cast<const std::uint8_t*>(this->buffer_.data()),
                   this->buffer_.size() * sizeof(Edge),
                   static_cast<std::uint64_t>(this->records_) * sizeof(Edge));
        this->runs_.push_back(Run{this->records_, this->buffer_.size(), 0});
        this->records_ += this->buffer_.size();
        this->buffer_.clear();
    }

    std::filesystem::path scratch_;
    AccountedScratchFile fd_;
    void* mapped_ = nullptr;
    bool finalized_ = false;
    std::size_t records_ = 0;
    std::size_t cursor_ = 0;
    std::vector<Edge> buffer_;
    std::vector<Run> runs_;
    std::priority_queue<Head, std::vector<Head>, GreaterHead> next_;
};

// Preflight checksums use a fixed-size scratch buffer, even when the bundle
// contains a large value object. Typed graph validation still decodes those
// objects later; the first pass never needs to materialize their payloads.
Hash sha256FileRegion(int fd, std::uint64_t offset, std::uint64_t length) {
    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    require(Err::Internal, context != nullptr && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1,
            "initialize bundle SHA-256 failed");
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (length != 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, buffer.size()));
        readExact(fd, buffer.data(), chunk, offset);
        require(Err::Internal, EVP_DigestUpdate(context.get(), buffer.data(), chunk) == 1,
                "update bundle SHA-256 failed");
        offset += chunk;
        length -= chunk;
    }
    Hash result{};
    unsigned int digestSize = 0;
    require(Err::Internal, EVP_DigestFinal_ex(context.get(), result.data(), &digestSize) == 1 &&
            digestSize == result.size(), "finalize bundle SHA-256 failed");
    return result;
}

constexpr std::array<std::uint8_t, 5> packRecordMagic{'A', 'O', 'B', 'J', 1};

struct PackRecord {
    Hash hash{};
    std::uint64_t payloadSize = 0;
};

// A record is valid only when its header is well-formed, its payload lies
// completely before `end`, and the payload hashes to the recorded SHA-256.
// Payloads are hashed in bounded chunks, never materialized.
std::optional<PackRecord> validPackRecord(int fd, std::uint64_t position, std::uint64_t end) {
    if (end < position || end - position < headerSize) return std::nullopt;
    std::array<std::uint8_t, headerSize> header{};
    readExact(fd, header.data(), header.size(), position);
    if (!std::equal(packRecordMagic.begin(), packRecordMagic.end(), header.begin()))
        return std::nullopt;
    PackRecord record{};
    record.payloadSize = readU64(header.data() + 37);
    if (record.payloadSize > maxObjectBytes ||
        end - position - headerSize < record.payloadSize) return std::nullopt;
    std::copy_n(header.begin() + 5, record.hash.size(), record.hash.begin());
    if (sha256FileRegion(fd, position + headerSize, record.payloadSize) != record.hash)
        return std::nullopt;
    return record;
}

// Search [begin, end) for the start of any complete, checksum-valid record.
// Distinguishes a torn tail (nothing valid follows) from mid-pack corruption.
std::optional<std::uint64_t> findValidPackRecord(int fd, std::uint64_t begin,
                                                 std::uint64_t end) {
    constexpr std::size_t window = 1024U * 1024U;
    std::vector<std::uint8_t> buffer(window + packRecordMagic.size() - 1U);
    std::uint64_t position = begin;
    while (position < end && end - position >= headerSize) {
        // Candidate starts are position .. end - headerSize (inclusive).
        const std::uint64_t candidates = end - headerSize - position + 1U;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(window, candidates));
        const std::size_t bytes = count + packRecordMagic.size() - 1U;
        readExact(fd, buffer.data(), bytes, position);
        for (std::size_t offset = 0; offset < count; ++offset) {
            if (buffer[offset] != packRecordMagic[0]) continue;
            if (!std::equal(packRecordMagic.begin(), packRecordMagic.end(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(offset))) continue;
            if (validPackRecord(fd, position + offset, end)) return position + offset;
        }
        position += count;
    }
    return std::nullopt;
}

bool isLowerHexToken(std::string_view text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
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
    require(Err::Corruption, stream.good(), "cannot read active pack pointer");
    const std::string contents((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
    require(Err::Corruption, contents.size() == 39 && contents.substr(0, 6) == "PACK1 " &&
            contents.back() == '\n', "invalid active pack pointer");
    for (std::size_t i = 6; i < 38; ++i) {
        require(Err::Corruption, (contents[i] >= '0' && contents[i] <= '9') ||
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
    require(Err::InvalidInput, detail::validBranchLabel(name),
            "invalid branch name");
}

int nibbleHex(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    throw StorageError(Err::InvalidInput, "invalid SHA-256 hex digit");
}

// The branch ref namespace contains ONLY UUIDv7 filenames. User-visible names
// are metadata in those files, never filesystem components. The HEAD and the
// name are published together as one atomic record; renaming does not move it.
struct StoredBranch {
    BranchId id;
    std::string name;
    Hash head{}; // SHA HEAD for BRANCH-1, integrity digest for BRANCH-2
    std::optional<RevisionId> revisionV2;
    std::optional<BranchId> originBranchId; // Immutable creation provenance, not a revision parent.
    std::optional<RevisionId> forkRevisionId; // Immutable historical fork point; never a DAG edge.
};

std::vector<StoredBranch> readStoredBranches(const std::filesystem::path& repository) {
    const std::filesystem::path heads = repository / "refs" / "heads";
    require(Err::Corruption, !std::filesystem::is_symlink(heads) &&
                std::filesystem::is_directory(heads), "unsafe UUID branch directory");
    std::vector<StoredBranch> result;
    std::set<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(heads)) {
        const std::string filename = entry.path().filename().string();
        if (filename.starts_with(".stage-")) continue; // crash leftover, never published
        const std::optional<BranchId> parsed = BranchId::parse(filename);
        require(Err::Corruption, parsed.has_value() && filename == parsed->toString(),
                "unsupported branch-ref format: expected UUIDv7 filename (pre-release reset required)");
        require(Err::Corruption, !entry.is_symlink() && entry.is_regular_file(),
                "symlink or special UUID branch record");
        const int fd = ::open(entry.path().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && errno == ELOOP) throw StorageError(Err::Corruption, "symlinked UUID branch record");
        if (fd < 0) throwSystemError("open UUID branch record");
        std::string bytes;
        try {
            struct stat st{};
            systemCall(::fstat(fd, &st) == 0, "stat UUID branch record");
            require(Err::Corruption, S_ISREG(st.st_mode) && st.st_size >= 21 && st.st_size <= 320,
                    "invalid UUID branch record file type or length");
            bytes.resize(static_cast<std::size_t>(st.st_size));
            std::size_t offset = 0;
            while (offset != bytes.size()) {
                const ssize_t n = ::read(fd, bytes.data()+offset, bytes.size()-offset);
                if (n < 0 && errno == EINTR) continue;
                systemCall(n >= 0, "read UUID branch record");
                require(Err::Corruption, n > 0, "truncated UUID branch record");
                offset += static_cast<std::size_t>(n);
            }
            systemCall(::close(fd) == 0, "close UUID branch record");
        } catch (...) { ::close(fd); throw; }
        constexpr std::string_view prefixV1 = "AOWIS-BRANCH-1\n";
        constexpr std::string_view prefixV2 = "AOWIS-BRANCH-2\n";
        constexpr std::string_view prefixV3 = "AOWIS-BRANCH-3\n";
        constexpr std::string_view prefixV4 = "AOWIS-BRANCH-4\n";
        constexpr std::string_view prefixV5 = "AOWIS-BRANCH-5\n";
        const bool forkV5 = bytes.starts_with(prefixV5);
        const bool unbornV4 = bytes.starts_with(prefixV4);
        const bool originV3 = forkV5 || bytes.starts_with(prefixV3);
        const bool revisionV2 = originV3 || bytes.starts_with(prefixV2);
        require(Err::Corruption, unbornV4 || revisionV2 || bytes.starts_with(prefixV1),
                "invalid UUID branch record marker");
        const std::size_t prefixSize = forkV5 ? prefixV5.size() : unbornV4 ? prefixV4.size() : originV3 ? prefixV3.size()
            : revisionV2 ? prefixV2.size() : prefixV1.size();
        const std::size_t split = bytes.find('\n', prefixSize);
        require(Err::Corruption, split != std::string::npos, "invalid UUID branch record name");
        const std::string name = bytes.substr(prefixSize, split - prefixSize);
        try { checkBranch(name); }
        catch (const StorageError&) { throw StorageError(Err::Corruption, "invalid stored branch name"); }
        if (unbornV4) {
            require(Err::Corruption, bytes.substr(split + 1) == "-\n-\n" &&
                    names.insert(name).second, "invalid unborn UUIDv7 branch record");
            result.push_back(StoredBranch{*parsed, name, Hash{}, std::nullopt, std::nullopt, std::nullopt});
            continue;
        }
        std::optional<RevisionId> revisionId;
        std::size_t hashOffset = split + 1;
        if (revisionV2) {
            const std::size_t uuidEnd = bytes.find('\n', hashOffset);
            require(Err::Corruption, uuidEnd != std::string::npos, "missing branch revision UUID");
            const std::string_view uuidText(bytes.data() + hashOffset, uuidEnd - hashOffset);
            revisionId = RevisionId::parse(uuidText);
            require(Err::Corruption, revisionId && uuidText == revisionId->toString(),
                    "noncanonical UUIDv7 branch HEAD");
            hashOffset = uuidEnd + 1;
        }
        std::optional<BranchId> origin;
        if (originV3) {
            const std::size_t originEnd = bytes.find('\n', hashOffset);
            require(Err::Corruption, originEnd != std::string::npos,
                    "missing creation origin in UUID branch record");
            const std::string_view text(bytes.data() + hashOffset, originEnd - hashOffset);
            if (text != "-") {
                origin = BranchId::parse(text);
                require(Err::Corruption, origin && text == origin->toString() && *origin != *parsed,
                        "invalid or self-referential creation origin");
            }
            hashOffset = originEnd + 1;
        }
        std::optional<RevisionId> forkRevision;
        if (forkV5) {
            const std::size_t forkEnd = bytes.find('\n', hashOffset);
            require(Err::Corruption, forkEnd != std::string::npos, "missing immutable fork revision");
            const std::string_view text(bytes.data() + hashOffset, forkEnd - hashOffset);
            forkRevision = RevisionId::parse(text);
            require(Err::Corruption, origin && forkRevision && text == forkRevision->toString(),
                    "invalid immutable fork revision or missing source branch");
            hashOffset = forkEnd + 1;
        }
        require(Err::Corruption, bytes.size() == hashOffset + 65 && bytes.back() == '\n',
                "invalid UUID branch record HEAD");
        Hash head{};
        try { head = fromHex(std::string_view(bytes.data() + hashOffset, 64)); }
        catch (const StorageError&) { throw StorageError(Err::Corruption, "invalid UUID branch HEAD hash"); }
        require(Err::Corruption, !isZero(head) && names.insert(name).second,
                "zero HEAD or duplicate branch name");
        result.push_back(StoredBranch{*parsed, name, head, revisionId, origin, forkRevision});
    }
    std::sort(result.begin(), result.end(), [](const StoredBranch& a, const StoredBranch& b) {
        return a.name < b.name;
    });
    return result;
}

std::optional<StoredBranch> findStoredBranch(const std::filesystem::path& repository,
                                             std::string_view name) {
    checkBranch(name);
    for (const StoredBranch& branch : readStoredBranches(repository)) {
        if (branch.name == name) return branch;
    }
    return std::nullopt;
}

// Caller holds the repository-wide advisory lock. A fully synced private file
// is installed with no replacement for creation, rename(2) for HEAD/label
// updates. Name is DATA, not a path component. Every filesystem component is
// a fixed token, a generated UUIDv7, or a pre-existing repository path.
void publishStoredBranch(const std::filesystem::path& repository,
                         const StoredBranch& branch, bool create) {
    const std::filesystem::path heads = repository / "refs" / "heads";
    const std::filesystem::path destination = heads / branch.id.toString();
    // Preserve legacy branch encodings on read. New forks use BRANCH-5 to
    // retain their immutable source branch and selected revision across commits.
    const std::string payload = (!branch.revisionV2 && isZero(branch.head))
        ? std::string("AOWIS-BRANCH-4\n") + branch.name + "\n-\n-\n"
        : branch.revisionV2
        ? (branch.originBranchId
            ? (branch.forkRevisionId ? std::string("AOWIS-BRANCH-5\n") : std::string("AOWIS-BRANCH-3\n")) + branch.name + "\n" +
                branch.revisionV2->toString() + "\n" +
                branch.originBranchId->toString() + "\n" +
                (branch.forkRevisionId ? branch.forkRevisionId->toString() + "\n" : std::string()) +
                toHex(branch.head) + "\n"
            : std::string("AOWIS-BRANCH-2\n") + branch.name + "\n" +
                branch.revisionV2->toString() + "\n" + toHex(branch.head) + "\n")
        : std::string("AOWIS-BRANCH-1\n") + branch.name + "\n" +
            toHex(branch.head) + "\n";
    const BranchId stagingId = BranchId::generate();
    const std::filesystem::path temporary = heads / (".stage-" + stagingId.toString());
    int fd = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    systemCall(fd >= 0, "create UUID branch staging record");
    bool published = false;
    try {
        detail::faultPartialWrite(fd, reinterpret_cast<const std::uint8_t*>(payload.data()),
                                  payload.size(), 0, "ref.partial.temp");
        writeExact(fd, reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(), 0);
        detail::faultCheckpoint("ref.before.fsync");
        systemCall(::fsync(fd) == 0, "sync UUID branch staging record");
        detail::faultCheckpoint("ref.after.fsync");
        systemCall(::close(fd) == 0, "close UUID branch staging record");
        fd = -1;
        detail::faultCheckpoint("ref.before.rename");
        if (create) {
            systemCall(::link(temporary.c_str(), destination.c_str()) == 0,
                       "publish new UUID branch record without replacement");
        } else {
            // Do not follow a maliciously swapped symlink at the destination.
            struct stat st{};
            systemCall(::lstat(destination.c_str(), &st) == 0 && S_ISREG(st.st_mode),
                       "validate UUID branch record before replace");
            systemCall(::rename(temporary.c_str(), destination.c_str()) == 0,
                       "atomically update UUID branch record");
        }
        published = true;
        detail::faultCheckpoint("ref.after.rename");
        syncDirectory(heads);
        detail::faultCheckpoint("ref.after.dirsync");
    } catch (...) {
        if (fd >= 0) ::close(fd);
        (void)::unlink(temporary.c_str());
        throw;
    }
    if (published) (void)::unlink(temporary.c_str());
}

// Shared immutable logical verifier for untrusted bundle objects AND objects
// already persisted locally. A valid SHA-256 hash is not proof of a valid
// typed revision graph, trie, or reverse-reference index.
struct LogicalVerification {
    std::uint64_t reachableObjects = 0;
    std::uint64_t revisionCount = 0;
    // Locally resident historical revisions still need a hash set. Imported
    // revisions instead use the file-backed visitation marks by bundle ordinal.
    std::set<Hash> localRevisions;
    std::function<bool(const Hash&)> containsBundledRevision;
    // Trusted local revisions referenced by bundled revisions. Their own
    // ancestry was not walked; see ExternalVerificationMarks::trustedLocal.
    std::set<Hash> boundaryRevisions;
    std::uint64_t treeNodes = 0;
    std::uint64_t modelSnapshots = 0;

    bool containsRevision(const Hash& hash) const {
        return this->localRevisions.contains(hash) || this->boundaryRevisions.contains(hash) ||
               (this->containsBundledRevision && this->containsBundledRevision(hash));
    }
};

// Only imported bundle hashes use external visitation marks. Incremental
// bundles also reference existing local hashes, which use the map fallback.
struct ExternalVerificationMarks {
    std::function<std::optional<std::uint8_t>(const Hash&)> read;
    std::function<void(const Hash&, std::uint8_t)> write;
    std::function<std::uint64_t()> markedCount;
    std::function<std::optional<TreeSummary>(const Hash&, unsigned)> readTree;
    std::function<void(const Hash&, unsigned, const TreeSummary&)> writeTree;
    std::function<std::uint64_t()> treeCount;
    // The importer enumerates imported revision identities from the already
    // sorted bundle index, selecting only entries marked as revision objects.
    // No separate std::set<Hash> is needed for those entries.
    std::function<void(const std::function<void(const Hash&)>&)> forEachRevision;
    // Objects already stored in this repository passed verification when
    // they were committed or imported. Such a hash (absent from the bundle)
    // is a boundary: its node is read and checked against its position, but
    // the subtree or ancestry below it is not walked again. This keeps an
    // incremental import proportional to the delta, not to the history.
    std::function<bool(const Hash&)> trustedLocal;
};

struct LogicalVerificationProfile {
    double graphTraversalMs = 0;
    double modelReferenceMs = 0;
};

using ImportClock = std::chrono::steady_clock;
double elapsedImportMs(ImportClock::time_point start, ImportClock::time_point finish) {
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

LogicalVerification verifyLogicalGraph(
        const std::vector<Hash>& tips,
        const std::function<Bytes(const Hash&)>& objectByHash,
        const std::filesystem::path& scratch,
        const ExternalVerificationMarks& externalMarks = {},
        LogicalVerificationProfile* profile = nullptr) {
    enum class Kind {
        Revision, RawTree, ModelTree, ReferenceTree,
        RawValue, ModelValue, ReferenceValue
    };
    std::vector<std::pair<Hash, Kind>> pending;
    for (const Hash& tip : tips) {
        require(Err::Corruption, !isZero(tip), "zero revision tip is invalid");
        pending.emplace_back(tip, Kind::Revision);
    }
    std::map<Hash, std::uint8_t> checkedKinds;
    const std::function<std::uint8_t(const Hash&)> readKinds = [&](const Hash& hash) {
        if (externalMarks.read) {
            const std::optional<std::uint8_t> bits = externalMarks.read(hash);
            if (bits) return *bits;
        }
        const std::map<Hash, std::uint8_t>::const_iterator found = checkedKinds.find(hash);
        return found == checkedKinds.end() ? std::uint8_t{0} : found->second;
    };
    const std::function<void(const Hash&, Kind)> markKind = [&](const Hash& hash, Kind kind) {
        const unsigned bit = 1U << static_cast<unsigned>(kind);
        const std::uint8_t bits = static_cast<std::uint8_t>(
            static_cast<unsigned>(readKinds(hash)) | bit);
        if (externalMarks.read && externalMarks.write && externalMarks.read(hash)) {
            externalMarks.write(hash, bits);
        } else {
            checkedKinds[hash] = bits;
        }
    };
    std::set<Hash> localRevisions;
    std::set<Hash> boundaryRevisions;
    // Semantic (model/reverse-index) state. Trusted boundary revisions enter
    // it directly: their snapshots are verified, their ancestry is done.
    std::set<std::pair<Hash, Hash>> verifiedModelSnapshots;
    std::set<Hash> processedLocalModelRevisions;
    const std::function<bool(const Hash&)> trusted = [&](const Hash& hash) {
        return externalMarks.trustedLocal && externalMarks.trustedLocal(hash);
    };
    std::uint64_t verifiedRevisionCount = 0;
    std::map<std::pair<Hash, Kind>, TreeSummary> verifiedTrees;
    std::set<std::pair<Hash, Kind>> visitingTrees;
    // A trusted local subtree is internally canonical, so one node gives
    // the summary its parent needs: a leaf's key, or a branch's depth and
    // canonical prefix (all nibbles before the depth are shared, the parent
    // only compares positions up to its own, smaller depth).
    //
    // Value kinds: every model or reference leaf that differs from the
    // verified parent snapshot is decoded by the semantic phase below, so a
    // grafted subtree of the wrong kind is caught there. Raw trees have no
    // semantic phase. Values of a stored subtree are homogeneous ('V' raw or
    // model values vs. INR1 referrer lists), so one leaf decides its kind.
    const std::function<TreeSummary(const Hash&, Kind, unsigned)> trustedTreeSummary =
        [&](const Hash& hash, Kind kind, unsigned minimumDepth) {
            Bytes node = objectByHash(hash);
            TreeSummary result{};
            const std::function<void(const Bytes&)> checkNode = [](const Bytes& candidate) {
                require(Err::Corruption, (candidate.size() == 49 && candidate[0] == 'L') ||
                            (candidate.size() == 530 && candidate[0] == 'B' && candidate[1] < 32),
                        "referenced local object is not a tree node");
            };
            checkNode(node);
            if (node[0] == 'L') {
                result.leaf = true;
                std::copy_n(node.begin() + 1, 16, result.representative.begin());
            } else {
                result.depth = node[1];
                require(Err::Corruption, result.depth >= minimumDepth,
                        "invalid bundled compressed trie depth ordering");
                std::copy_n(node.begin() + 2, 16, result.representative.begin());
            }
            if (kind == Kind::RawTree) {
                while (node[0] == 'B') {
                    Hash child{};
                    for (unsigned slot = 0; slot < 16 && isZero(child); ++slot) {
                        child = readHash(node, 18 + 32U * slot);
                    }
                    require(Err::Corruption, !isZero(child), "referenced local branch is empty");
                    node = objectByHash(child);
                    checkNode(node);
                }
                const Bytes value = objectByHash(readHash(node, 17));
                require(Err::Corruption, !value.empty() && value[0] == 'V',
                        "referenced local subtree is not a raw value tree");
            }
            return result;
        };
    std::function<TreeSummary(const Hash&, Kind, unsigned)> verifyTree;
    verifyTree = [&](const Hash& hash, Kind kind, unsigned minimumDepth) -> TreeSummary {
        try {
            require(Err::Corruption, !isZero(hash), "zero bundled tree node");
            const std::pair<Hash, Kind> identity{hash, kind};
            const unsigned treeKind = static_cast<unsigned>(kind) -
                static_cast<unsigned>(Kind::RawTree);
            // Imported nodes live in the file-backed table. Locally resident
            // ancestors of incremental bundles use the ordinary map fallback.
            const std::optional<TreeSummary> cached = externalMarks.readTree
                ? externalMarks.readTree(hash, treeKind) : std::nullopt;
            if (cached) {
                require(Err::Corruption, cached->leaf || cached->depth >= minimumDepth,
                        "invalid bundled compressed trie depth ordering");
                return *cached;
            }
            const std::map<std::pair<Hash, Kind>, TreeSummary>::const_iterator previous =
                verifiedTrees.find(identity);
            if (previous != verifiedTrees.end()) {
                require(Err::Corruption, previous->second.leaf || previous->second.depth >= minimumDepth,
                        "invalid bundled compressed trie depth ordering");
                return previous->second;
            }
            if (trusted(hash)) {
                const TreeSummary result = trustedTreeSummary(hash, kind, minimumDepth);
                verifiedTrees.emplace(identity, result);
                markKind(hash, kind);
                return result;
            }
            require(Err::Corruption, visitingTrees.insert(identity).second, "cyclic bundled trie");
            const Bytes object = objectByHash(hash);
            require(Err::Corruption, !object.empty(), "invalid bundled tree node");
            TreeSummary result{};
            if (object[0] == 'L') {
                require(Err::Corruption, object.size() == 49, "invalid bundled leaf");
                result.leaf = true;
                std::copy_n(object.begin() + 1, 16, result.representative.begin());
                const Hash value = readHash(object, 17);
                require(Err::Corruption, !isZero(value), "bundled leaf contains zero value hash");
                const Kind valueKind = kind == Kind::RawTree ? Kind::RawValue :
                    kind == Kind::ModelTree ? Kind::ModelValue : Kind::ReferenceValue;
                pending.emplace_back(value, valueKind);
            } else if (object[0] == 'B') {
                require(Err::Corruption, object.size() == 530 && object[1] < 32,
                        "invalid bundled tree branch");
                result.depth = object[1];
                require(Err::Corruption, result.depth >= minimumDepth,
                        "invalid bundled compressed trie depth ordering");
                Key prefix{};
                std::copy_n(object.begin() + 2, 16, prefix.begin());
                // storeNode() clears all nibbles at or beyond the split depth.
                // Requiring that representation also excludes equivalent but
                // differently hashed encodings of one logical trie.
                for (unsigned position = result.depth; position < 32; ++position) {
                    require(Err::Corruption, nibble(prefix, position) == 0,
                            "noncanonical bundled trie prefix");
                }
                unsigned children = 0;
                for (unsigned slot = 0; slot < 16; ++slot) {
                    const Hash child = readHash(object, 18 + 32U * slot);
                    if (isZero(child)) continue;
                    ++children;
                    const TreeSummary descendant = verifyTree(child, kind, result.depth + 1U);
                    require(Err::Corruption, descendant.leaf || descendant.depth > result.depth,
                            "invalid bundled compressed trie depth ordering");
                    for (unsigned position = 0; position < result.depth; ++position) {
                        require(Err::Corruption, nibble(descendant.representative, position) ==
                                    nibble(prefix, position),
                                "bundled trie child violates compressed prefix");
                    }
                    require(Err::Corruption, nibble(descendant.representative, result.depth) == slot,
                            "bundled trie child occupies wrong slot");
                    if (children == 1) result.representative = descendant.representative;
                }
                require(Err::Corruption, children >= 2, "invalid bundled branch child count");
            } else {
                throw StorageError(Err::Corruption, "invalid bundled tree tag");
            }
            visitingTrees.erase(identity);
            if (externalMarks.writeTree && externalMarks.read &&
                externalMarks.read(hash).has_value()) {
                externalMarks.writeTree(hash, treeKind, result);
            } else {
                verifiedTrees.emplace(identity, result);
            }
            markKind(hash, kind);
            return result;
        } catch (...) {
            rethrowWithContext("tree " + toHex(hash) + ": ");
        }
    };
    const ImportClock::time_point graphStart = ImportClock::now();
    while (!pending.empty()) {
        const std::pair<Hash, Kind> next = pending.back();
        pending.pop_back();
        if (isZero(next.first)) continue;
        const unsigned bit = 1U << static_cast<unsigned>(next.second);
        if ((static_cast<unsigned>(readKinds(next.first)) & bit) != 0U) continue;
        if (next.second == Kind::RawTree || next.second == Kind::ModelTree ||
            next.second == Kind::ReferenceTree) {
            (void)verifyTree(next.first, next.second, 0);
            continue;
        }
        markKind(next.first, next.second);
        const Bytes object = objectByHash(next.first);
        if (next.second == Kind::Revision) {
            require(Err::Corruption, object.size() >= 67 && object[0] == 'S',
                    "invalid bundled revision " + toHex(next.first));
            const unsigned parents = object[66];
            require(Err::Corruption, object[65] <= 1 && parents <= 2 &&
                    object.size() == 67 + 32U * parents,
                    "invalid bundled revision metadata");
            const Hash root = readHash(object, 1);
            const Hash referenceRoot = readHash(object, 33);
            require(Err::Corruption, object[65] == 1 || isZero(referenceRoot),
                    "raw bundled revision has reference index");
            if (trusted(next.first)) {
                boundaryRevisions.insert(next.first);
                if (object[65] == 1) verifiedModelSnapshots.emplace(root, referenceRoot);
                processedLocalModelRevisions.insert(next.first);
                continue;
            }
            ++verifiedRevisionCount;
            // The bundle's visitation bitmap is already keyed by content
            // hash. Avoid retaining each imported revision a second time.
            // Incremental imports still track locally resident ancestors.
            if (!externalMarks.read || !externalMarks.read(next.first)) {
                localRevisions.insert(next.first);
            }
            pending.emplace_back(root, object[65] == 1 ? Kind::ModelTree : Kind::RawTree);
            pending.emplace_back(referenceRoot, Kind::ReferenceTree);
            for (unsigned index = 0; index < parents; ++index) {
                const Hash parent = readHash(object, 67 + index * 32);
                require(Err::Corruption, !isZero(parent), "zero parent in bundled revision");
                pending.emplace_back(parent, Kind::Revision);
            }
        } else if (next.second == Kind::ReferenceValue) {
            (void)decodeReferrers(object);
        } else {
            require(Err::Corruption, !object.empty() && object[0] == 'V',
                    "invalid bundled model/raw value");
            if (next.second == Kind::ModelValue) {
                (void)decodeModelRecord(Bytes(object.begin() + 1, object.end()));
            }
        }
    }
    const ImportClock::time_point graphFinish = ImportClock::now();
    if (profile != nullptr) {
        profile->graphTraversalMs = elapsedImportMs(graphStart, graphFinish);
    }
    const ImportClock::time_point modelStart = graphFinish;
    // Verify the semantic relationship between the two immutable indexes,
    // not just their independent Merkle structure. Hash-valid imports can
    // otherwise carry an omitted incoming edge, and a later ordinary
    // commitModel() deletion would trust that poisoned reverse index.
    //
    // Include every walked revision, including merge parents. Trusted local
    // boundary revisions are already verified snapshots. The same pair of
    // immutable roots is checked only once when revisions share a snapshot.
    // A verified canonical trie enumerates its leaf UUIDs in strict order
    // when children are visited from slot 0 to 15. Besides eliminating a
    // second model-key set, explicitly assert this invariant here.
    const std::function<void(const Hash&,
            const std::function<void(const Key&, const Hash&)>&)> visitLeaves =
        [&](const Hash& root,
            const std::function<void(const Key&, const Hash&)>& visitor) {
            std::vector<Hash> nodes;
            std::optional<Key> previous;
            if (!isZero(root)) nodes.push_back(root);
            while (!nodes.empty()) {
                const Hash current = nodes.back();
                nodes.pop_back();
                const Bytes node = objectByHash(current);
                if (node[0] == 'L') {
                    Key key{};
                    std::copy_n(node.begin() + 1, 16, key.begin());
                    require(Err::Corruption, !previous || *previous < key,
                            "bundled trie contains duplicate or unordered UUID");
                    previous = key;
                    visitor(key, readHash(node, 17));
                } else {
                    for (unsigned slot = 16; slot != 0; --slot) {
                        const Hash child = readHash(node, 18 + 32U * (slot - 1U));
                        if (!isZero(child)) nodes.push_back(child);
                    }
                }
            }
        };
    // Only ask for model existence once per distinct target, after sorting.
    // This search reads at most one 32-nibble path and does not retain a set
    // of all model UUIDs on the heap.
    const std::function<bool(const Hash&, const Key&)> containsModel =
        [&](const Hash& root, const Key& key) {
            Hash current = root;
            while (!isZero(current)) {
                const Bytes node = objectByHash(current);
                if (node[0] == 'L') {
                    return std::equal(key.begin(), key.end(), node.begin() + 1);
                }
                const unsigned slot = nibble(key, node[1]);
                current = readHash(node, 18 + 32U * slot);
            }
            return false;
        };
    // Enumerate only changes between two previously structurally validated
    // Merkle trees. Identical immutable subtrees need no further I/O. A split
    // or compressed prefix can change during an insertion/deletion; in that
    // case merge the two subtree leaf streams instead of assuming their
    // branches have matching shapes.
    using OptionalValueHash = std::optional<Hash>;
    using LeafChangeVisitor = std::function<void(
        const Key&, const OptionalValueHash&, const OptionalValueHash&)>;
    const std::function<void(const Hash&, const Hash&, const LeafChangeVisitor&)> diffLeaves =
        [&](const Hash& oldRoot, const Hash& newRoot,
            const LeafChangeVisitor& visitor) {
            const std::function<void(const Hash&, const Hash&)> walk =
                [&](const Hash& before, const Hash& after) {
                    if (before == after) return;
                    // Matching branches are structurally compatible, so only
                    // child hashes that actually changed need examination.
                    if (!isZero(before) && !isZero(after)) {
                        const Bytes oldNode = objectByHash(before);
                        const Bytes newNode = objectByHash(after);
                        if (oldNode[0] == 'B' && newNode[0] == 'B' &&
                            std::equal(oldNode.begin() + 1, oldNode.begin() + 18,
                                       newNode.begin() + 1)) {
                            for (unsigned slot = 0; slot < 16; ++slot) {
                                walk(readHash(oldNode, 18 + 32U * slot),
                                     readHash(newNode, 18 + 32U * slot));
                            }
                            return;
                        }
                    }
                    // Structural rewrites occur only at affected trie nodes.
                    // Fall back to an ordered merge of their leaf sets;
                    // neither side needs to be a branch or even nonempty.
                    std::vector<std::pair<Key, Hash>> beforeLeaves;
                    std::vector<std::pair<Key, Hash>> afterLeaves;
                    visitLeaves(before, [&](const Key& key, const Hash& value) {
                        beforeLeaves.emplace_back(key, value);
                    });
                    visitLeaves(after, [&](const Key& key, const Hash& value) {
                        afterLeaves.emplace_back(key, value);
                    });
                    std::size_t i = 0;
                    std::size_t j = 0;
                    while (i < beforeLeaves.size() || j < afterLeaves.size()) {
                        if (j == afterLeaves.size() ||
                            (i < beforeLeaves.size() &&
                             beforeLeaves[i].first < afterLeaves[j].first)) {
                            visitor(beforeLeaves[i].first, beforeLeaves[i].second,
                                    std::nullopt);
                            ++i;
                        } else if (i == beforeLeaves.size() ||
                                   afterLeaves[j].first < beforeLeaves[i].first) {
                            visitor(afterLeaves[j].first, std::nullopt,
                                    afterLeaves[j].second);
                            ++j;
                        } else {
                            if (beforeLeaves[i].second != afterLeaves[j].second) {
                                visitor(beforeLeaves[i].first, beforeLeaves[i].second,
                                        afterLeaves[j].second);
                            }
                            ++i;
                            ++j;
                        }
                    }
                };
            walk(oldRoot, newRoot);
        };

    // The edge delta of the model records must equal the edge delta of the
    // reverse index. This is sufficient when the parent snapshot has already
    // been verified, and also catches missing/extra index entries even if
    // every individual leaf and hash is well-formed.
    using ReferenceEdge = SortedReferenceEdges::Edge;
    using EdgeChanges = std::map<ReferenceEdge, int>;
    const std::function<void(EdgeChanges&, const Key&, const Key&, int)> changeEdge =
        [&](EdgeChanges& changes, const Key& target, const Key& source, int direction) {
            ReferenceEdge edge{};
            std::copy(target.begin(), target.end(), edge.begin());
            std::copy(source.begin(), source.end(), edge.begin() + 16);
            const int change = changes[edge] + direction;
            if (change == 0) changes.erase(edge);
            else changes[edge] = change;
        };

    // Validate ancestors before their descendants, independent of bundle hash
    // sort order. A revision with an indexed parent can reuse that parent's
    // established reference consistency, including merge second-parents.
    // The high bit is unused by the seven structural kinds. Imported
    // revisions reuse their file-backed mark rather than adding another
    // heap-allocated set for semantic verification.
    constexpr std::uint8_t modelRevisionChecked = 0x80U;
    const std::function<bool(const Hash&)> modelRevisionDone = [&](const Hash& id) {
        if (externalMarks.read) {
            const std::optional<std::uint8_t> bits = externalMarks.read(id);
            if (bits && ((*bits & modelRevisionChecked) != 0U)) return true;
        }
        return processedLocalModelRevisions.contains(id);
    };
    const std::function<void(const Hash&)> markModelRevisionDone = [&](const Hash& id) {
        if (externalMarks.read && externalMarks.write) {
            const std::optional<std::uint8_t> bits = externalMarks.read(id);
            if (bits) {
                externalMarks.write(id, static_cast<std::uint8_t>(*bits | modelRevisionChecked));
                return;
            }
        }
        processedLocalModelRevisions.insert(id);
    };
    const std::function<void(const Hash&)> checkVerifiedRevision = [&](const Hash& revisionId) {
        try {
            const Bytes encodedRevision = objectByHash(revisionId);
            const unsigned parentCount = encodedRevision[66];
            std::optional<std::pair<Hash, Hash>> parentSnapshot;
            for (unsigned i = 0; i < parentCount; ++i) {
                const Hash parentId = readHash(encodedRevision, 67 + 32U * i);
                // The caller visits parents in postorder, without recursion.
                const Bytes parent = objectByHash(parentId);
                if (parent[65] == 1 && !parentSnapshot) {
                    parentSnapshot = std::make_pair(readHash(parent, 1),
                                                    readHash(parent, 33));
                }
            }
            if (encodedRevision[65] == 0) {
                markModelRevisionDone(revisionId);
                return;
            }
            const Hash modelRoot = readHash(encodedRevision, 1);
            const Hash referenceRoot = readHash(encodedRevision, 33);
            const std::pair<Hash, Hash> snapshot{modelRoot, referenceRoot};
            if (verifiedModelSnapshots.contains(snapshot)) {
                markModelRevisionDone(revisionId);
                return;
            }

            if (parentSnapshot && verifiedModelSnapshots.contains(*parentSnapshot)) {
                EdgeChanges modelChanges;
                EdgeChanges indexChanges;
                std::vector<Key> deletedModels;
                const std::function<std::vector<Key>(const Hash&, bool)> references =
                    [&](const Hash& value, bool canonical) {
                        if (isZero(value)) return std::vector<Key>{};
                        const Bytes stored = objectByHash(value);
                        require(Err::Corruption, !stored.empty() && stored.front() == 'V',
                                "invalid bundled model value");
                        const Bytes envelope(stored.begin() + 1, stored.end());
                        const ModelRecord record = decodeModelRecord(envelope);
                        if (canonical) {
                            require(Err::Corruption, encodeModelRecord(record) == envelope,
                                    "noncanonical bundled model references");
                        }
                        return record.references;
                    };
                diffLeaves(parentSnapshot->first, modelRoot,
                           [&](const Key& source, const OptionalValueHash& oldValue,
                               const OptionalValueHash& newValue) {
                    require(Err::Corruption, source != Key{}, "bundled model tree contains zero UUID");
                    if (oldValue && !newValue) deletedModels.push_back(source);
                    if (oldValue) {
                        for (const Key& target : references(*oldValue, false)) {
                            changeEdge(modelChanges, target, source, -1);
                        }
                    }
                    if (newValue) {
                        for (const Key& target : references(*newValue, true)) {
                            require(Err::Corruption, target != Key{} && target != source,
                                    "invalid bundled model UUID reference");
                            changeEdge(modelChanges, target, source, 1);
                        }
                    }
                });
                diffLeaves(parentSnapshot->second, referenceRoot,
                           [&](const Key& target, const OptionalValueHash& oldValue,
                               const OptionalValueHash& newValue) {
                    require(Err::Corruption, target != Key{},
                            "bundled incoming-reference target is zero");
                    if (oldValue) {
                        for (const Key& source : decodeReferrers(objectByHash(*oldValue))) {
                            changeEdge(indexChanges, target, source, -1);
                        }
                    }
                    if (newValue) {
                        for (const Key& source : decodeReferrers(objectByHash(*newValue))) {
                            changeEdge(indexChanges, target, source, 1);
                        }
                    }
                });
                if (modelChanges != indexChanges) {
                    const bool onlyMissingFromUnchangedModel = modelChanges.empty() &&
                        !indexChanges.empty() &&
                        std::all_of(indexChanges.begin(), indexChanges.end(),
                            [](const std::pair<const ReferenceEdge, int>& edge) {
                                return edge.second < 0;
                            });
                    throw StorageError(Err::Corruption, onlyMissingFromUnchangedModel
                        ? "bundled reverse-reference index is missing model references"
                        : "bundled reverse-reference index does not match model records");
                }
                // A changed model may introduce a new dependency. An unchanged
                // model may still refer to a deleted target: checking the
                // resulting incoming index for every deleted UUID catches it.
                std::optional<Key> previousTarget;
                for (const std::pair<const ReferenceEdge, int>& edge : modelChanges) {
                    if (edge.second <= 0) continue;
                    Key target{};
                    std::copy_n(edge.first.begin(), 16, target.begin());
                    if (previousTarget && *previousTarget == target) continue;
                    require(Err::Corruption, containsModel(modelRoot, target),
                            "bundled model reference target does not exist");
                    previousTarget = target;
                }
                for (const Key& deleted : deletedModels) {
                    require(Err::Corruption, !containsModel(referenceRoot, deleted),
                            "bundled model reference target does not exist");
                }
            } else {
                // First indexed revision after a raw branch (or a history
                // without an indexed parent): establish a complete baseline.
                SortedReferenceEdges expected(scratch);
                visitLeaves(modelRoot, [&](const Key& source, const Hash& value) {
                    require(Err::Corruption, source != Key{}, "bundled model tree contains zero UUID");
                    const Bytes stored = objectByHash(value);
                    require(Err::Corruption, !stored.empty() && stored.front() == 'V',
                            "invalid bundled model value");
                    const Bytes envelope(stored.begin() + 1, stored.end());
                    const ModelRecord record = decodeModelRecord(envelope);
                    require(Err::Corruption, encodeModelRecord(record) == envelope,
                            "noncanonical bundled model references");
                    for (const Key& target : record.references) {
                        require(Err::Corruption, target != Key{} && target != source,
                                "invalid bundled model UUID reference");
                        expected.append(target, source);
                    }
                });
                expected.finalize();
                std::optional<SortedReferenceEdges::Edge> nextExpected = expected.next();
                std::optional<Key> checkedTarget;
                const std::function<void()> checkTarget = [&] {
                    if (!nextExpected) return;
                    Key target{};
                    std::copy_n(nextExpected->begin(), 16, target.begin());
                    if (checkedTarget && *checkedTarget == target) return;
                    require(Err::Corruption, containsModel(modelRoot, target),
                            "bundled model reference target does not exist");
                    checkedTarget = target;
                };
                visitLeaves(referenceRoot, [&](const Key& target, const Hash& value) {
                    require(Err::Corruption, target != Key{}, "bundled incoming-reference target is zero");
                    const std::vector<Key> actual = decodeReferrers(objectByHash(value));
                    for (const Key& source : actual) {
                        checkTarget();
                        SortedReferenceEdges::Edge matching{};
                        std::copy(target.begin(), target.end(), matching.begin());
                        std::copy(source.begin(), source.end(), matching.begin() + 16);
                        require(Err::Corruption, nextExpected && *nextExpected == matching,
                                "bundled reverse-reference index does not match model records");
                        nextExpected = expected.next();
                    }
                    if (nextExpected && std::equal(target.begin(), target.end(),
                                                   nextExpected->begin())) {
                        throw StorageError(Err::Corruption,
                            "bundled reverse-reference index does not match model records");
                    }
                });
                checkTarget();
                require(Err::Corruption, !nextExpected,
                        "bundled reverse-reference index is missing model references");
            }
            verifiedModelSnapshots.insert(snapshot);
            markModelRevisionDone(revisionId);
        } catch (...) {
            rethrowWithContext("model revision " + toHex(revisionId) + ": ");
        }
    };
    // Use an explicit postorder stack: histories can contain hundreds of
    // thousands of commits, so recursive parent walking risks stack overflow.
    const std::function<void(const Hash&)> checkModelRevision = [&](const Hash& tip) {
        std::vector<std::pair<Hash, bool>> work{{tip, false}};
        std::set<Hash> active;
        while (!work.empty()) {
            const std::pair<Hash, bool> frame = work.back();
            work.pop_back();
            if (modelRevisionDone(frame.first)) continue;
            if (frame.second) {
                checkVerifiedRevision(frame.first);
                active.erase(frame.first);
                continue;
            }
            require(Err::Corruption, active.insert(frame.first).second,
                    "cyclic bundled revision ancestry");
            work.emplace_back(frame.first, true);
            const Bytes revision = objectByHash(frame.first);
            for (unsigned i = 0; i < revision[66]; ++i) {
                const Hash parent = readHash(revision, 67 + 32U * i);
                if (!modelRevisionDone(parent)) {
                    work.emplace_back(parent, false);
                }
            }
        }
    };
    for (const Hash& revisionId : localRevisions) checkModelRevision(revisionId);
    if (externalMarks.forEachRevision) externalMarks.forEachRevision(checkModelRevision);

    if (profile != nullptr) {
        profile->modelReferenceMs = elapsedImportMs(modelStart, ImportClock::now());
    }
    LogicalVerification summary{};
    summary.reachableObjects = static_cast<std::uint64_t>(checkedKinds.size()) +
        (externalMarks.markedCount ? externalMarks.markedCount() : 0);
    summary.revisionCount = verifiedRevisionCount;
    summary.localRevisions = std::move(localRevisions);
    summary.boundaryRevisions = std::move(boundaryRevisions);
    if (externalMarks.read) {
        const std::function<std::optional<std::uint8_t>(const Hash&)> read = externalMarks.read;
        summary.containsBundledRevision = [read](const Hash& hash) {
            const std::optional<std::uint8_t> kinds = read(hash);
            return kinds && ((*kinds & 1U) != 0U);
        };
    }
    summary.treeNodes = static_cast<std::uint64_t>(verifiedTrees.size()) +
        (externalMarks.treeCount ? externalMarks.treeCount() : 0);
    summary.modelSnapshots = static_cast<std::uint64_t>(verifiedModelSnapshots.size());
    return summary;
}
} // namespace

void setInternalScratchLimit(std::uint64_t maximumBytes) {
    InternalScratchAccounting& accounting = scratchAccounting();
    const std::lock_guard<std::mutex> lock(accounting.mutex);
    if (accounting.reserved != 0)
        throw StorageError(Err::Conflict, "cannot change internal scratch limit while files are active");
    accounting.maximum = maximumBytes;
}

std::uint64_t internalScratchReservedBytes() {
    InternalScratchAccounting& accounting = scratchAccounting();
    const std::lock_guard<std::mutex> lock(accounting.mutex);
    return accounting.reserved;
}

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
    require(Err::InvalidInput, value.size() == 64, "SHA-256 hex must be 64 characters");
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
    require(Err::Internal, EVP_Digest(value.data(), value.size(), output.data(), &length,
                       EVP_sha256(), nullptr) == 1 && length == output.size(),
            "OpenSSL SHA-256 failed");
    return output;
}

Repository::Repository(std::filesystem::path directory)
    : directory_(std::move(directory)), scratch_(this->directory_ / "tmp") {
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
        prepareScratchDirectory();
        this->packToken_ = activePackToken(this->directory_);
        const int flags = O_RDWR | O_CLOEXEC | (this->packToken_.empty() ? O_CREAT : 0);
        this->packFd_ = ::open(packPath(this->directory_, this->packToken_, false).c_str(),
                               flags, 0644);
        systemCall(this->packFd_ >= 0, "open active content pack");
        this->indexFd_ = ::open(packPath(this->directory_, this->packToken_, true).c_str(),
                                flags | O_CREAT, 0644);
        systemCall(this->indexFd_ >= 0, "open active content index");
        refreshIndex();
        require(Err::Corruption, !std::filesystem::exists(this->directory_ / "refs" / "branch-ids"),
                "obsolete name-keyed branch identity directory; recreate pre-release database");
        (void)readStoredBranches(this->directory_); // Reject older name-keyed refs on open.
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

// Called with repo.lock held. Named scratch files exist only between
// mkostemp() and unlink() inside createScratchFile(); any such file found here
// was left by a crashed process. A concurrent creator tolerates ENOENT.
void Repository::prepareScratchDirectory() {
    std::filesystem::create_directories(this->scratch_);
    require(Err::Corruption, !std::filesystem::is_symlink(this->scratch_) &&
                std::filesystem::is_directory(this->scratch_),
            "repository scratch path is not a directory: " + this->scratch_.string());
    std::filesystem::permissions(this->scratch_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(this->scratch_)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(scratchPrefix) || entry.is_symlink() || !entry.is_regular_file()) continue;
        if (::unlink(entry.path().c_str()) != 0 && errno != ENOENT)
            throwSystemError("remove leftover scratch file " + name);
    }
}

ScratchFile Repository::createScratchFile() {
    return ScratchFile(aowis::dvmdb::createScratchFile(this->scratch_, "client"));
}

const std::filesystem::path& Repository::scratchDirectory() const noexcept {
    return this->scratch_;
}

ScratchFile::ScratchFile(int fd) noexcept : fd_(fd) {}
ScratchFile::~ScratchFile() {
    if (this->fd_ >= 0) ::close(this->fd_);
}
ScratchFile::ScratchFile(ScratchFile&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
ScratchFile& ScratchFile::operator=(ScratchFile&& other) noexcept {
    if (this != &other) {
        if (this->fd_ >= 0) ::close(this->fd_);
        this->fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}
int ScratchFile::fd() const noexcept { return this->fd_; }
int ScratchFile::release() noexcept {
    const int fd = this->fd_;
    this->fd_ = -1;
    return fd;
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
    require(Err::Corruption, data.st_size >= 0, "invalid negative pack size");
    const std::uint64_t size = static_cast<std::uint64_t>(data.st_size);
    require(Err::Corruption, size >= this->scanOffset_, "content pack shrank while repository was open");
    std::uint64_t position = this->scanOffset_;
    while (position < size) {
        const std::optional<PackRecord> record = validPackRecord(this->packFd_, position, size);
        if (!record) break;
        
        const std::array<std::uint8_t, indexEntrySize> entry =
            encodeIndexEntry(record->hash, position + headerSize, record->payloadSize);
        const std::uint64_t entryPosition = this->indexReadOffset_ + this->pendingIndex_.size();
        this->pendingIndex_.insert(this->pendingIndex_.end(), entry.begin(), entry.end());
        indexEntry(record->hash, entryPosition);
        position += headerSize + record->payloadSize;
    }
    const bool truncated = position != size;
    if (truncated) discardTornPackTail(position, size);
    this->scanOffset_ = position;
    if (!this->pendingIndex_.empty() || truncated) syncPack();
}

// [position, size) begins with an invalid record. After power loss a file
// system may persist an extended size without data (zeros) or only part of
// a full-length body; a killed writer leaves a short record. Published refs
// only name objects fsynced before the ref rename, so an invalid region that
// runs to EOF is unpublished and can be removed. Anything else fails closed.
void Repository::discardTornPackTail(std::uint64_t position, std::uint64_t size) {
    if (const std::optional<std::uint64_t> later =
            findValidPackRecord(this->packFd_, position + 1U, size)) {
        throw StorageError(Err::Corruption,
            "corrupt content pack record at offset " + std::to_string(position) +
            " is followed by a valid record at offset " + std::to_string(*later) +
            "; refusing automatic repair (restore from backup)");
    }
    // Defense in depth: never drop bytes a published branch still needs.
    // Each commit appends its revision object last, so damage that reaches a
    // published head is detected here instead of being silently truncated.
    for (const StoredBranch& branch : readStoredBranches(this->directory_)) {
        require(Err::Corruption, isZero(branch.head) || containsIndex(branch.head),
                "branch '" + branch.name + "' references damaged pack tail at offset " +
                std::to_string(position));
    }
    preserveDiscardedPackBytes(position, size);
    systemCall(::ftruncate(this->packFd_, static_cast<off_t>(position)) == 0,
               "discard torn trailing pack records");
    this->recoveredPackTailBytes_ += size - position;
}

void Repository::preserveDiscardedPackBytes(std::uint64_t position, std::uint64_t size) {
    const std::filesystem::path directory = this->directory_ / "recovered-pack-tails";
    if (!std::filesystem::exists(directory)) {
        std::filesystem::create_directory(directory);
        syncDirectory(this->directory_);
    }
    const std::string stem = "pack-" + (this->packToken_.empty()
        ? std::string("initial") : this->packToken_) + "-offset-" + std::to_string(position);
    int fd = -1;
    std::filesystem::path target;
    for (unsigned attempt = 0; fd < 0; ++attempt) {
        require(Err::Internal, attempt < 1000, "too many preserved copies of one torn pack tail");
        target = directory / (stem + (attempt == 0 ? std::string() :
                                      "-" + std::to_string(attempt)) + ".bin");
        fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) systemCall(false, "create torn pack tail copy");
    }
    try {
        std::vector<std::uint8_t> buffer(1024U * 1024U);
        std::uint64_t done = 0;
        while (done < size - position) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), size - position - done));
            readExact(this->packFd_, buffer.data(), chunk, position + done);
            writeExact(fd, buffer.data(), chunk, done);
            done += chunk;
        }
        systemCall(::fsync(fd) == 0, "sync torn pack tail copy");
        const int closing = fd;
        fd = -1;
        systemCall(::close(closing) == 0, "close torn pack tail copy");
        // The copy must be durable before the original bytes are truncated.
        syncDirectory(directory);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        throw;
    }
}

std::uint64_t Repository::recoveredPackTailBytes() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    return this->recoveredPackTailBytes_;
}

// Called only under exclusive maintenance ownership. Deletes complete or
// partial generations left by an interrupted compaction; never the active
// generation, recovered tails, refs or unrelated files.
std::uint64_t Repository::removeStalePackGenerations() {
    std::vector<std::filesystem::path> stale;
    for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(this->directory_)) {
        if (entry.is_symlink() || !entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        bool remove = false;
        if (name == "objects.pack" || name == "objects.idx") {
            remove = !this->packToken_.empty(); // retired initial generation
        }
        // objects.<32 hex>.pack / objects.<32 hex>.idx of another generation
        for (const std::string_view suffix : {std::string_view(".pack"), std::string_view(".idx")}) {
            if (name.size() == 8 + 32 + suffix.size() && name.starts_with("objects.") &&
                name.ends_with(suffix)) {
                const std::string_view token(name.data() + 8, 32);
                if (isLowerHexToken(token) && token != this->packToken_) remove = true;
            }
        }
        // .objects.current.<32 hex>.tmp: unpublished generation pointer
        if (name.size() == 17 + 32 + 4 && name.starts_with(".objects.current.") &&
            name.ends_with(".tmp") &&
            isLowerHexToken(std::string_view(name.data() + 17, 32))) remove = true;
        if (remove) stale.push_back(entry.path());
    }
    for (const std::filesystem::path& path : stale)
        systemCall(::unlink(path.c_str()) == 0, "remove stale pack generation file");
    if (!stale.empty()) syncDirectory(this->directory_);
    return static_cast<std::uint64_t>(stale.size());
}

// The physical lookup accelerator only stores 8 bytes per occupied slot.
// Values in the checksummed sidecar remain authoritative: a fingerprint match
// always reads and compares the entire SHA-256 hash. A zero ordinal marks an
// empty slot; no tombstones are needed because removal rebuilds the index.
std::uint32_t Repository::indexFingerprint(const Hash& hash) const noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (std::uint8_t byte : hash) value = (value ^ byte) * 1099511628211ULL;
    return static_cast<std::uint32_t>(value ^ (value >> 32U));
}

void Repository::reserveIndexEntries(std::uint64_t entries) {
    require(Err::LimitExceeded, entries <= std::numeric_limits<std::uint32_t>::max(),
            "physical index exceeds 32-bit sidecar ordinal capacity");
    // Keep load <= 70%, including before inserting the next entry.
    const std::uint64_t required = (entries * 10U) / 7U + 2U;
    std::size_t capacity = this->indexSlots_.empty() ? 16U : this->indexSlots_.size();
    while (static_cast<std::uint64_t>(capacity) < required) {
        require(Err::LimitExceeded, capacity <= std::numeric_limits<std::size_t>::max() / 2U,
                "physical index capacity overflow");
        capacity *= 2U;
    }
    if (capacity == this->indexSlots_.size()) return;
    std::vector<std::uint64_t> slots(capacity, 0);
    for (std::uint64_t value : this->indexSlots_) {
        if (value == 0) continue;
        std::size_t bucket = static_cast<std::size_t>(value >> 32U) & (capacity - 1U);
        while (slots[bucket] != 0) bucket = (bucket + 1U) & (capacity - 1U);
        slots[bucket] = value;
    }
    this->indexSlots_.swap(slots);
}

void Repository::clearIndex() noexcept {
    this->indexSlots_.clear();
    this->indexCache_.clear();
    this->indexCount_ = 0;
}

void Repository::readIndexedEntry(std::uint32_t ordinal, std::uint8_t* destination) const {
    require(Err::Corruption, ordinal != 0, "invalid empty sidecar ordinal");
    const std::uint64_t position = indexHeaderSize +
        (static_cast<std::uint64_t>(ordinal) - 1U) * indexEntrySize;
    if (position < this->indexReadOffset_) {
        readExact(this->indexFd_, destination, indexEntrySize, position);
    } else {
        const std::uint64_t offset = position - this->indexReadOffset_;
        require(Err::Corruption, offset <= this->pendingIndex_.size() &&
                indexEntrySize <= this->pendingIndex_.size() - offset,
                "physical index points outside pending sidecar entries");
        std::copy_n(this->pendingIndex_.data() + static_cast<std::size_t>(offset),
                    indexEntrySize, destination);
    }
    require(Err::Corruption, indexChecksum(destination) == readU32(destination + 48),
            "physical index sidecar entry checksum mismatch");
}

Hash Repository::indexedHash(std::uint32_t ordinal) const {
    std::array<std::uint8_t, indexEntrySize> entry{};
    readIndexedEntry(ordinal, entry.data());
    Hash hash{};
    std::copy_n(entry.begin(), hash.size(), hash.begin());
    return hash;
}

std::optional<Repository::Location> Repository::indexLocation(const Hash& hash) const {
    if (this->indexSlots_.empty()) return std::nullopt;
    const std::uint32_t fingerprint = indexFingerprint(hash);
    if (!this->indexCache_.empty()) {
        const CachedIndexEntry& cached = this->indexCache_[fingerprint & 4095U];
        if (cached.location.offset != 0 && cached.hash == hash) return cached.location;
    }
    std::size_t bucket = static_cast<std::size_t>(fingerprint) & (this->indexSlots_.size() - 1U);
    while (this->indexSlots_[bucket] != 0) {
        const std::uint64_t value = this->indexSlots_[bucket];
        if (static_cast<std::uint32_t>(value >> 32U) == fingerprint) {
            std::array<std::uint8_t, indexEntrySize> entry{};
            readIndexedEntry(static_cast<std::uint32_t>(value), entry.data());
            if (std::equal(hash.begin(), hash.end(), entry.begin())) {
                const Location location{readU64(entry.data() + 32), readU64(entry.data() + 40)};
                if (this->indexCache_.empty()) this->indexCache_.resize(4096);
                CachedIndexEntry& cached = this->indexCache_[fingerprint & 4095U];
                cached.hash = hash;
                cached.location = location;
                return location;
            }
        }
        bucket = (bucket + 1U) & (this->indexSlots_.size() - 1U);
    }
    return std::nullopt;
}

bool Repository::containsIndex(const Hash& hash) const {
    return indexLocation(hash).has_value();
}

void Repository::indexEntry(const Hash& hash, std::uint64_t position) {
    require(Err::Corruption, position >= indexHeaderSize &&
            (position - indexHeaderSize) % indexEntrySize == 0,
            "misaligned sidecar entry location");
    const std::uint64_t ordinal = (position - indexHeaderSize) / indexEntrySize + 1U;
    require(Err::LimitExceeded, ordinal <= std::numeric_limits<std::uint32_t>::max(),
            "physical index exceeds 32-bit sidecar ordinal capacity");
    reserveIndexEntries(this->indexCount_ + 1U);
    const std::uint32_t fingerprint = indexFingerprint(hash);
    std::size_t bucket = static_cast<std::size_t>(fingerprint) & (this->indexSlots_.size() - 1U);
    while (this->indexSlots_[bucket] != 0) {
        const std::uint64_t value = this->indexSlots_[bucket];
        if (static_cast<std::uint32_t>(value >> 32U) == fingerprint &&
            indexedHash(static_cast<std::uint32_t>(value)) == hash) {
            this->indexSlots_[bucket] = (static_cast<std::uint64_t>(fingerprint) << 32U) | ordinal;
            if (!this->indexCache_.empty()) {
                CachedIndexEntry& cached = this->indexCache_[fingerprint & 4095U];
                if (cached.hash == hash) cached.location.offset = 0;
            }
            return;
        }
        bucket = (bucket + 1U) & (this->indexSlots_.size() - 1U);
    }
    this->indexSlots_[bucket] = (static_cast<std::uint64_t>(fingerprint) << 32U) | ordinal;
    ++this->indexCount_;
}

// The sidecar is derived data. If missing, torn, or internally inconsistent,
// rebuild it from cryptographically verified pack records without changing
// any model, tree, or revision hash.
void Repository::rebuildIndex() {
    clearIndex();
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
    require(Err::Unavailable, !this->needsReopen_, "repository must be reopened after interrupted compaction");
    // A preceding operation could have written objects before throwing;
    // make its orphan records discoverable by other processes too.
    if (!this->pendingIndex_.empty()) syncPack();
    struct stat packData{};
    struct stat indexData{};
    systemCall(::fstat(this->packFd_, &packData) == 0, "stat content pack");
    systemCall(::fstat(this->indexFd_, &indexData) == 0, "stat content index");
    require(Err::Corruption, packData.st_size >= 0 && indexData.st_size >= 0, "invalid storage size");
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
    reserveIndexEntries(this->indexCount_ + (indexBytes - position) / indexEntrySize);
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
            this->indexReadOffset_ = position + (i + 1U) * indexEntrySize;
            indexEntry(hash, position + i * indexEntrySize);
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
    require(Err::Corruption, !isZero(hash), "attempted to load empty tree hash");
    const std::optional<Location> location = indexLocation(hash);
    require(Err::Corruption, location.has_value(), "referenced content is missing from pack");
    Bytes payload(static_cast<std::size_t>(location->size));
    if (!payload.empty()) readExact(this->packFd_, payload.data(), payload.size(), location->offset);
    require(Err::Corruption, sha256(payload) == hash, "content checksum mismatch during read");
    return payload;
}

Hash Repository::store(const Bytes& bytes) {
    require(Err::LimitExceeded, bytes.size() <= maxObjectBytes, "content object exceeds size limit");
    Hash hash = sha256(bytes);
    if (containsIndex(hash)) return hash;
    std::array<std::uint8_t, headerSize> header{};
    header[0] = 'A'; header[1] = 'O'; header[2] = 'B'; header[3] = 'J'; header[4] = 1;
    std::copy(hash.begin(), hash.end(), header.begin() + 5);
    storeU64(header.data() + 37, static_cast<std::uint64_t>(bytes.size()));
    std::uint64_t offset = this->scanOffset_;
    detail::faultCheckpoint("pack.before.header");
    writeExact(this->packFd_, header.data(), header.size(), offset);
    detail::faultCheckpoint("pack.after.header");
    if (!bytes.empty()) {
        detail::faultPartialWrite(this->packFd_, bytes.data(), bytes.size(),
                                  offset + headerSize, "pack.partial.body");
        writeExact(this->packFd_, bytes.data(), bytes.size(), offset + headerSize);
    }
    detail::faultCheckpoint("pack.after.body");
    const std::array<std::uint8_t, indexEntrySize> entry =
        encodeIndexEntry(hash, offset + headerSize, bytes.size());
    const std::uint64_t entryPosition = this->indexReadOffset_ + this->pendingIndex_.size();
    this->pendingIndex_.insert(this->pendingIndex_.end(), entry.begin(), entry.end());
    indexEntry(hash, entryPosition);
    this->scanOffset_ += headerSize + bytes.size();
    return hash;
}

// Reuse the same content-addressed append path for both value categories.
Hash Repository::store(Bytes&& bytes) {
    return store(static_cast<const Bytes&>(bytes));
}

void Repository::syncPack() {
    // Branch durability depends on the pack, not on this reconstructible
    // sidecar. The index may lag across a crash; refreshIndex hashes the
    // uncovered durable pack suffix to rebuild it on the next open.
    detail::faultCheckpoint("pack.before.fsync");
    systemCall(::fsync(this->packFd_) == 0, "fsync content pack");
    detail::faultCheckpoint("pack.after.fsync");
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
    require(Err::Corruption, !bytes.empty(), "empty tree node");
    TreeNode result{};
    if (bytes[0] == 'L') {
        require(Err::Corruption, bytes.size() == 1 + 16 + 32, "invalid leaf node size");
        result.leaf = true;
        std::copy_n(bytes.begin() + 1, 16, result.key.begin());
        result.value = readHash(bytes, 17);
        require(Err::Corruption, !isZero(result.value), "leaf references zero content hash");
    } else if (bytes[0] == 'B') {
        require(Err::Corruption, bytes.size() == 2 + 16 + 16 * 32, "invalid branch node size");
        result.leaf = false;
        result.depth = bytes[1];
        std::copy_n(bytes.begin() + 2, 16, result.prefix.begin());
        require(Err::Corruption, result.depth < 32, "invalid trie branch depth");
        unsigned count = 0;
        for (std::size_t i = 0; i < 16; ++i) {
            result.children[i] = readHash(bytes, 18 + i * 32);
            if (!isZero(result.children[i])) ++count;
        }
        require(Err::Corruption, count >= 2, "trie branch must have at least two children");
    } else {
        throw StorageError(Err::Corruption, "invalid tree node tag");
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
        require(Err::Internal, updates[begin].second.has_value(), "buildTree expects present values");
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
    require(Err::Internal, divergence < 32, "duplicate UUID in sorted batch");
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
    require(Err::Corruption, node.depth >= minDepth, "invalid compressed tree ordering");

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

// Emits every leaf of `root` as an addition (added == true) or a removal.
void Repository::diffWholeSubtree(const Hash& root, bool added, std::vector<Difference>& output,
                                  std::size_t limit) {
    if (isZero(root)) return;
    const TreeNode node = readNode(root);
    if (node.leaf) {
        output.push_back(added ? Difference{node.key, std::nullopt, node.value}
                               : Difference{node.key, node.value, std::nullopt});
        require(Err::LimitExceeded, output.size() <= limit, "difference exceeds the requested limit");
        return;
    }
    for (const Hash& child : node.children) diffWholeSubtree(child, added, output, limit);
}

// Structural three-case diff of two canonical compressed tries. Work is
// proportional to the changed paths: a key inserted above an existing
// compressed prefix descends into that subtree instead of listing it. At most
// `limit` differences are produced; more raise LimitExceeded immediately.
void Repository::diffHashes(const Hash& from, const Hash& to,
                            std::vector<Difference>& output, std::size_t limit) {
    if (from == to) return;
    if (isZero(from)) { diffWholeSubtree(to, true, output, limit); return; }
    if (isZero(to)) { diffWholeSubtree(from, false, output, limit); return; }
    const TreeNode left = readNode(from);
    const TreeNode right = readNode(to);
    if (left.leaf && right.leaf) {
        if (left.key == right.key) {
            output.push_back(Difference{left.key, left.value, right.value});
        } else {
            output.push_back(Difference{left.key, left.value, std::nullopt});
            require(Err::LimitExceeded, output.size() <= limit, "difference exceeds the requested limit");
            output.push_back(Difference{right.key, std::nullopt, right.value});
        }
        require(Err::LimitExceeded, output.size() <= limit, "difference exceeds the requested limit");
        return;
    }
    // A leaf behaves like a node whose whole key is its prefix (depth 32).
    const unsigned leftDepth = left.leaf ? 32U : left.depth;
    const unsigned rightDepth = right.leaf ? 32U : right.depth;
    const Key& leftPrefix = left.leaf ? left.key : left.prefix;
    const Key& rightPrefix = right.leaf ? right.key : right.prefix;
    const unsigned shared = std::min(leftDepth, rightDepth);
    for (unsigned position = 0; position < shared; ++position) {
        if (nibble(leftPrefix, position) != nibble(rightPrefix, position)) {
            // Disjoint key ranges: everything left was removed, everything right added.
            diffWholeSubtree(from, false, output, limit);
            diffWholeSubtree(to, true, output, limit);
            return;
        }
    }
    if (leftDepth == rightDepth) { // two branches with the same split
        for (std::size_t slot = 0; slot < 16; ++slot)
            diffHashes(left.children[slot], right.children[slot], output, limit);
        return;
    }
    if (leftDepth < rightDepth) { // `to` lies entirely in one child slot of `from`
        const unsigned slot = nibble(rightPrefix, leftDepth);
        for (unsigned child = 0; child < 16; ++child)
            diffHashes(left.children[child], child == slot ? to : Hash{}, output, limit);
        return;
    }
    const unsigned slot = nibble(leftPrefix, rightDepth); // `from` lies in one slot of `to`
    for (unsigned child = 0; child < 16; ++child)
        diffHashes(child == slot ? from : Hash{}, right.children[child], output, limit);
}

Revision Repository::readRevision(const Hash& hash) {
    const Bytes payload = load(hash);
    // Native prototype format 2. No pre-release migration is supported.
    // S | content root | reverse-reference root | indexed flag | parent count | parents
    require(Err::Corruption, payload.size() >= 67 && payload[0] == 'S', "invalid native revision object");
    const std::uint8_t flags = payload[65];
    const std::size_t parentCount = payload[66];
    require(Err::Corruption, flags <= 1 && parentCount <= 2 && payload.size() == 67 + 32 * parentCount,
            "invalid native revision metadata");
    Revision revision{};
    revision.id = hash;
    revision.root = readHash(payload, 1);
    revision.referencesRoot = readHash(payload, 33);
    revision.modelIndexed = (flags == 1);
    require(Err::Corruption, revision.modelIndexed || isZero(revision.referencesRoot),
            "raw revision contains a model reference index");
    for (std::size_t i = 0; i < parentCount; ++i) {
        revision.parents.push_back(readHash(payload, 67 + i * 32));
    }
    return revision;
}

bool Repository::revisionReachableFrom(const Hash& tip, const Hash& candidate) {
    require(Err::InvalidInput, !isZero(candidate), "zero revision hash is invalid");
    std::vector<Hash> pending{tip};
    std::unordered_set<Hash, HashHasher> seen;
    while (!pending.empty()) {
        const Hash current = pending.back();
        pending.pop_back();
        require(Err::Corruption, !isZero(current), "invalid zero revision parent");
        if (!seen.insert(current).second) continue;
        // Verify the record even for the requested candidate: a supplied
        // hash of an arbitrary content object is not a valid revision.
        const Revision revision = readRevision(current);
        if (current == candidate) return true;
        for (const Hash& parent : revision.parents) pending.push_back(parent);
    }
    return false;
}

Hash Repository::branchHead(std::string_view name) {
    const std::optional<StoredBranch> branch = findStoredBranch(this->directory_, name);
    require(Err::NotFound, branch.has_value(), "branch does not exist: " + std::string(name));
    require(Err::Unavailable, !branch->revisionV2, "UUIDv7 branch requires v2 revision API");
    return branch->head;
}

void Repository::publishRef(std::string_view name, const Hash& commitHash, bool mustBeAbsent,
                            std::optional<BranchId> requestedId) {
    checkBranch(name);
    const std::optional<StoredBranch> existing = findStoredBranch(this->directory_, name);
    require(Err::Conflict, !(mustBeAbsent && existing), "branch already exists");
    require(Err::NotFound, mustBeAbsent || existing.has_value(), "branch does not exist");
    require(Err::Unavailable, !existing || !existing->revisionV2, "UUIDv7 branch requires v2 revision API");
    const BranchId id = existing ? existing->id : requestedId.value_or(BranchId::generate());
    require(Err::Conflict, !existing || !requestedId || existing->id == *requestedId,
            "branch UUIDv7 conflicts with requested identity");
    publishStoredBranch(this->directory_, StoredBranch{id, std::string(name), commitHash, std::nullopt,
                                                           existing ? existing->originBranchId : std::nullopt,
                                                           std::nullopt},
                        !existing.has_value());
}

std::optional<BranchId> Repository::branchIdLocked(std::string_view name) {
    const std::optional<StoredBranch> branch = findStoredBranch(this->directory_, name);
    return branch ? std::optional<BranchId>(branch->id) : std::nullopt;
}

std::optional<BranchId> Repository::branchId(std::string_view name) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    require(Err::NotFound, findStoredBranch(this->directory_, name).has_value(),
            "branch does not exist");
    return branchIdLocked(name);
}

void Repository::adoptBranchId(std::string_view name, const BranchId& identity,
                               const Hash& expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    require(Err::Conflict, branchHead(name) == expectedHead &&
                branchIdLocked(name) == std::optional<BranchId>(identity),
            "branch already has a different UUIDv7 identity or HEAD");
    // Pre-release format requires UUIDv7 from creation; no legacy conversion.
}

void Repository::renameBranch(std::string_view previous, std::string_view renamed,
                              const Hash& expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(previous);
    checkBranch(renamed);
    require(Err::Conflict, previous != renamed, "branch rename must change the name");
    const std::optional<StoredBranch> source = findStoredBranch(this->directory_, previous);
    require(Err::NotFound, source.has_value(), "source branch not found");
    require(Err::Unavailable, !source->revisionV2, "UUIDv7 branch requires v2 rename API");
    require(Err::Conflict, source->head == expectedHead, "branch HEAD advanced before rename");
    require(Err::Conflict, !findStoredBranch(this->directory_, renamed), "new branch name already exists");
    publishStoredBranch(this->directory_, StoredBranch{source->id, std::string(renamed), source->head,
                                                           std::nullopt, source->originBranchId, source->forkRevisionId}, false);
}

void Repository::createBranch(std::string_view name, std::optional<std::string_view> from) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(name);
    require(Err::Conflict, !findStoredBranch(this->directory_, name), "branch already exists");
    for (const StoredBranch& existing : readStoredBranches(this->directory_))
        require(Err::Unavailable, !existing.revisionV2, "cannot mix SHA and UUID revision HEADs");
    Hash initial{};
    if (from) {
        initial = branchHead(*from);
    } else {
        Bytes revision{'S'};
        appendHash(revision, Hash{});
        appendHash(revision, Hash{});
        revision.push_back(0);
        revision.push_back(0);
        initial = store(std::move(revision));
        syncPack();
    }
    publishRef(name, initial, true);
}

// UUIDv7 ancestry uses both merge parents. Revision IDs are never inferred from
// record digests; every visited UUID is authenticated through the resolver.
bool Repository::revisionReachableFromV2(const ProjectId& project, const RevisionId& tip,
                                        const RevisionId& candidate) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    std::set<RevisionId> visited;
    std::vector<RevisionId> pending{tip};
    while (!pending.empty()) {
        const RevisionId current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        require(Err::LimitExceeded, visited.size() <= 250000, "v2 ancestry traversal limit exceeded");
        const std::optional<ResolvedRevisionV2> record = resolver.resolve(current, reader);
        require(Err::NotFound, record.has_value(), "v2 ancestry revision not found");
        if (current == candidate) return true;
        for (const RevisionParentV2& parent : record->record.parents) pending.push_back(parent.id);
    }
    return false;
}

RevisionId Repository::createBranchAtRevisionV2(const ProjectId& project,
        std::string_view name, std::string_view sourceBranch, const RevisionId& revision) {
    return createBranchAtRevisionWithIdentityV2(project, name, sourceBranch, revision).revisionId;
}

BranchCreationResultV2 Repository::createBranchAtRevisionWithIdentityV2(
        const ProjectId& project, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return createBranchAtRevisionLockedV2(project, name, sourceBranch, revision,
                                          BranchId::generate());
}

BranchCreationResultV2 Repository::createBranchAtRevisionLockedV2(
        const ProjectId& project, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision, const BranchId& createdId) {
    checkBranch(name);
    require(Err::Conflict, !findStoredBranch(this->directory_, name), "branch already exists");
    const RevisionRecordV2 source = branchRevisionV2Locked(project, sourceBranch);
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    std::set<RevisionId> visited;
    std::vector<RevisionId> pending{source.id};
    std::optional<Hash> selectedHash;
    while (!pending.empty()) {
        const RevisionId current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        require(Err::LimitExceeded, visited.size() <= 250000, "v2 ancestry traversal limit exceeded");
        const std::optional<ResolvedRevisionV2> record = resolver.resolve(current, reader);
        require(Err::Corruption, record.has_value(), "v2 branch ancestry is missing a revision");
        if (current == revision) {
            selectedHash = record->recordHash;
            break;
        }
        for (const RevisionParentV2& parent : record->record.parents) pending.push_back(parent.id);
    }
    require(Err::InvalidInput, selectedHash.has_value(),
            "revision is not reachable from source branch");
    const BranchId sourceId = findStoredBranch(this->directory_, sourceBranch)->id;
    publishStoredBranch(this->directory_, StoredBranch{createdId,
            std::string(name), *selectedHash, revision, sourceId, revision}, true);
    return BranchCreationResultV2{createdId, revision, sourceId, revision};
}

MergeResultV2 Repository::mergeBranchesV2(const ProjectId& project,
        std::string_view target, std::string_view source,
        std::optional<RevisionId> expectedTarget, std::optional<RevisionId> expectedSource,
        const MergeOptions& options) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    require(Err::InvalidInput, !options.expectedTargetHead && !options.expectedSourceHead,
            "v2 merge expects UUIDv7 HEAD pins, not SHA-256 revision pins");
    const RevisionRecordV2 targetHead = branchRevisionV2Locked(project, target);
    const RevisionRecordV2 sourceHead = branchRevisionV2Locked(project, source);
    require(Err::Conflict, !expectedTarget || *expectedTarget == targetHead.id,
            "target HEAD advanced before v2 merge");
    require(Err::Conflict, !expectedSource || *expectedSource == sourceHead.id,
            "source HEAD advanced before v2 merge");
    const std::function<void()> checkSelections = [&options]() {
        if (!options.checkSelections) return;
        const std::optional<std::string> error = options.checkSelections();
        require(Err::InvalidInput, !error, error.value_or("unused merge resolution choices"));
    };
    MergeResultV2 result;
    result.targetBefore = targetHead.id;
    result.sourceHead = sourceHead.id;
    result.resultHead = targetHead.id;
    if (targetHead.id == sourceHead.id) {
        checkSelections();
        result.base = targetHead.id;
        return result;
    }

    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    std::map<RevisionId, RevisionRecordV2> cached;
    const std::function<const RevisionRecordV2&(const RevisionId&)> fetch =
        [&resolver, &reader, &cached](const RevisionId& id) -> const RevisionRecordV2& {
            const std::map<RevisionId, RevisionRecordV2>::const_iterator found = cached.find(id);
            if (found != cached.end()) return found->second;
            const std::optional<ResolvedRevisionV2> loaded = resolver.resolve(id, reader);
            require(Err::Corruption, loaded.has_value(), "v2 merge ancestry is missing a revision");
            return cached.emplace(id, loaded->record).first->second;
        };
    const std::function<void(std::vector<RevisionId>&, std::set<RevisionId>&)> traverse =
        [&fetch](std::vector<RevisionId>& pending, std::set<RevisionId>& visited) {
            while (!pending.empty()) {
                const RevisionId current = pending.back();
                pending.pop_back();
                if (!visited.insert(current).second) continue;
                require(Err::LimitExceeded, visited.size() <= 250000,
                        "v2 merge ancestry limit exceeded");
                for (const RevisionParentV2& parent : fetch(current).parents)
                    pending.push_back(parent.id);
            }
        };
    std::set<RevisionId> targetAncestors;
    std::vector<RevisionId> pending{targetHead.id};
    traverse(pending, targetAncestors);
    if (targetAncestors.contains(sourceHead.id)) {
        checkSelections();
        result.base = sourceHead.id;  // source already merged, possibly through second parent
        return result;
    }
    std::set<RevisionId> sourceAncestors;
    pending.push_back(sourceHead.id);
    traverse(pending, sourceAncestors);
    if (sourceAncestors.contains(targetHead.id)) {
        checkSelections();
        result.base = targetHead.id;
            if (options.validate) {
                // A domain validator expects typed model records; a raw
                // snapshot must never bypass it through a fast-forward.
                if (!sourceHead.modelIndexed) {
                    result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                        std::nullopt, std::nullopt,
                        "cannot validate a non-model-indexed source snapshot"});
                    result.status = MergeStatus::Conflicts;
                    return result;
                }
                const std::function<std::optional<ModelRecord>(const Key&)> readSource =
                    [this, &sourceHead](const Key& key) -> std::optional<ModelRecord> {
                        const std::optional<Hash> hash = lookup(sourceHead.root, key);
                        if (!hash) return std::nullopt;
                        const Bytes value = load(*hash);
                        require(Err::Corruption, !value.empty() && value.front() == 'V',
                                "invalid model envelope in fast-forward validation");
                        return decodeModelRecord(Bytes(value.begin() + 1, value.end()));
                    };
                const MergeCandidate candidate{
                    readSource,
                    [this, &sourceHead](
                        const std::function<void(const Key&, const ModelRecord&)>& visitor) {
                        std::vector<std::pair<Key, Hash>> hashes;
                        listHashes(sourceHead.root, hashes);
                        for (const std::pair<Key, Hash>& entry : hashes) {
                            const Bytes value = load(entry.second);
                            require(Err::Corruption, !value.empty() && value.front() == 'V',
                                    "invalid model envelope in fast-forward validation");
                            visitor(entry.first,
                                    decodeModelRecord(Bytes(value.begin() + 1, value.end())));
                        }
                    }
                };
                if (const std::optional<std::string> invalid = options.validate(candidate)) {
                    result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                        std::nullopt, std::nullopt, *invalid});
                    result.status = MergeStatus::Conflicts;
                    return result;
                }
            }
        const std::optional<StoredBranch> stored = findStoredBranch(this->directory_, target);
        require(Err::Internal, stored && stored->revisionV2 == result.targetBefore,
                "v2 target branch disappeared before fast forward");
        publishStoredBranch(this->directory_, StoredBranch{stored->id, stored->name,
                hashRevisionRecordV2(sourceHead), sourceHead.id, stored->originBranchId, stored->forkRevisionId}, false);
        result.status = MergeStatus::FastForward;
        result.resultHead = sourceHead.id;
        return result;
    }
    std::set<RevisionId> common;
    for (const RevisionId& id : sourceAncestors)
        if (targetAncestors.contains(id)) common.insert(id);
    require(Err::Conflict, !common.empty(), "v2 branches have no common ancestor");
    // A criss-cross history may contain multiple incomparable merge bases.
    // Reject it rather than arbitrarily choosing one and losing edits.
    std::set<RevisionId> dominated;
    for (const RevisionId& candidate : common) {
        pending.clear();
        for (const RevisionParentV2& parent : fetch(candidate).parents) pending.push_back(parent.id);
        std::set<RevisionId> visited;
        traverse(pending, visited);
        for (const RevisionId& ancestor : visited)
            if (common.contains(ancestor)) dominated.insert(ancestor);
    }
    std::vector<RevisionId> lowest;
    for (const RevisionId& candidate : common)
        if (!dominated.contains(candidate)) lowest.push_back(candidate);
    require(Err::Conflict, lowest.size() == 1,
            "v2 criss-cross merge requires recursive merge-base resolution");
    const RevisionRecordV2& base = fetch(lowest.front());
    result.base = base.id;
    require(Err::InvalidInput, targetHead.modelIndexed == sourceHead.modelIndexed &&
            (base.modelIndexed == targetHead.modelIndexed ||
             (!base.modelIndexed && isZero(base.root) && targetHead.modelIndexed)),
            "incompatible v2 raw and model histories");

    std::vector<Difference> targetDiff;
    std::vector<Difference> sourceDiff;
    diffHashes(base.root, targetHead.root, targetDiff);
    diffHashes(base.root, sourceHead.root, sourceDiff);
    struct Values {
        std::optional<Hash> base;
        std::optional<Hash> target;
        std::optional<Hash> source;
    };
    std::map<Key, Values> differences;
    for (const Difference& change : targetDiff) {
        differences.emplace(change.key, Values{change.before, change.after, change.before});
    }
    for (const Difference& change : sourceDiff) {
        std::map<Key, Values>::iterator existing = differences.find(change.key);
        if (existing == differences.end()) {
            differences.emplace(change.key,
                                Values{change.before, change.before, change.after});
        } else {
            existing->second.source = change.after;
        }
    }
    std::map<Key, std::optional<Hash>> selected;
    // Resolutions are checked without writing any pack objects. The
    // resolver sees authenticated canonical model records at all three
    // immutable snapshots and must not access this Repository recursively.
    std::map<Key, ModelRecord> resolved;
    for (const std::pair<const Key, Values>& item : differences) {
        const Values& values = item.second;
        if (values.target == values.source) continue;
        if (values.target != values.base && values.source != values.base) {
            if (options.chooseConflict && targetHead.modelIndexed) {
                const MergeConflict conflict{item.first, values.base, values.target,
                    values.source, "concurrent changes to the same UUID"};
                const std::optional<MergeChoiceSide> choice = options.chooseConflict(conflict);
                if (choice) {
                    // Also include target-side selections in the candidate
                    // preview, so reference/semantic validation cannot be
                    // skipped when the chosen object is already at target.
                    selected.emplace(item.first, *choice == MergeChoiceSide::Source
                        ? values.source : values.target);
                    continue;
                }
            }
            if (options.resolve && targetHead.modelIndexed && base.modelIndexed &&
                values.base && values.target && values.source) {
                const auto decodeAt = [this](const Hash& hash) {
                    const Bytes envelope = load(hash);
                    require(Err::Corruption, !envelope.empty() && envelope.front() == 'V',
                            "invalid model envelope in merge conflict");
                    return decodeModelRecord(Bytes(envelope.begin() + 1, envelope.end()));
                };
                const ModelRecord original = decodeAt(*values.base);
                const ModelRecord left = decodeAt(*values.target);
                const ModelRecord right = decodeAt(*values.source);
                if (original.kind == left.kind && left.kind == right.kind) {
                    MergeResolution resolution = options.resolve(item.first, original, left, right);
                    if (resolution.record && resolution.conflictingFields.empty() &&
                        resolution.record->kind == left.kind) {
                        resolved.emplace(item.first, std::move(*resolution.record));
                        continue;
                    }
                    if (!resolution.conflictingFields.empty()) {
                        std::string detail = "conflicting model fields: ";
                        for (const std::string& field : resolution.conflictingFields) {
                            if (detail.back() != ' ') detail += ", ";
                            detail += field;
                        }
                        result.conflicts.push_back(MergeConflict{item.first, values.base,
                            values.target, values.source, std::move(detail)});
                        continue;
                    }
                }
            }
            if (options.resolveConcurrentAddition && targetHead.modelIndexed &&
                !values.base && values.target && values.source) {
                const auto decodeAt = [this](const Hash& hash) {
                    const Bytes envelope = load(hash);
                    require(Err::Corruption, !envelope.empty() && envelope.front() == 'V',
                            "invalid model envelope in concurrent addition");
                    return decodeModelRecord(Bytes(envelope.begin() + 1, envelope.end()));
                };
                const ModelRecord left = decodeAt(*values.target);
                const ModelRecord right = decodeAt(*values.source);
                MergeResolution resolution = options.resolveConcurrentAddition(
                    item.first, left, right);
                if (resolution.record && resolution.conflictingFields.empty() &&
                    resolution.record->kind == left.kind && left.kind == right.kind) {
                    resolved.emplace(item.first, std::move(*resolution.record));
                    continue;
                }
            }
            result.conflicts.push_back(MergeConflict{item.first, values.base,
                values.target, values.source, "both branches changed the same UUID"});
        } else if (values.target == values.base) {
            selected.emplace(item.first, values.source);
        }
    }
    // Validate explicit choices even when other conflicts remain unresolved.
    // Otherwise a misspelled/unused choice could be silently ignored by
    // an early conflict return, only to change behavior on a later retry.
    checkSelections();
    if (!result.conflicts.empty()) {
        result.status = MergeStatus::Conflicts;
        return result;
    }

    std::vector<ModelChange> modelChanges;
    std::vector<Change> rawChanges;
    // Keep the selected records for dependency preflight, so a target can
    // be checked without decoding unrelated entities.
    std::map<Key, std::optional<ModelRecord>> selectedRecords;
    for (const std::pair<const Key, std::optional<Hash>>& item : selected) {
        if (!item.second) {
            if (targetHead.modelIndexed) selectedRecords.emplace(item.first, std::nullopt);
            else rawChanges.push_back(Change{item.first, std::nullopt});
            continue;
        }
        const Bytes value = load(*item.second);
        require(Err::Corruption, !value.empty() && value.front() == 'V', "invalid merged value envelope");
        const Bytes payload(value.begin() + 1, value.end());
        if (targetHead.modelIndexed) {
            ModelRecord record = decodeModelRecord(payload);
            selectedRecords.emplace(item.first, std::move(record));
        } else {
            rawChanges.push_back(Change{item.first, payload});
        }
    }
    for (std::pair<const Key, ModelRecord>& item : resolved)
        selectedRecords.emplace(item.first, std::move(item.second));
    if (targetHead.modelIndexed && options.reconcile) {
        // Readers inspect authenticated, immutable branch tips; candidate
        // modifications remain memory-only until ALL preflights complete.
        const auto readSnapshot = [this](const Hash& root, const Key& key)
            -> std::optional<ModelRecord> {
            const std::optional<Hash> hash = lookup(root, key);
            if (!hash) return std::nullopt;
            const Bytes value = load(*hash);
            require(Err::Corruption, !value.empty() && value.front() == 'V',
                    "invalid model envelope in merge reconciliation");
            return decodeModelRecord(Bytes(value.begin() + 1, value.end()));
        };
        const std::function<std::optional<ModelRecord>(const Key&)> readTarget =
            [&readSnapshot, &targetHead](const Key& key) {
                return readSnapshot(targetHead.root, key);
            };
        const std::function<std::optional<ModelRecord>(const Key&)> readSource =
            [&readSnapshot, &sourceHead](const Key& key) {
                return readSnapshot(sourceHead.root, key);
            };
        const std::function<std::optional<ModelRecord>(const Key&)> readMerged =
            [&selectedRecords, &readTarget](const Key& key) {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator found =
                    selectedRecords.find(key);
                return found == selectedRecords.end() ? readTarget(key) : found->second;
            };
        const MergeCandidate candidate{
            readMerged,
            [this, &targetHead, &selectedRecords, &readMerged](
                const std::function<void(const Key&, const ModelRecord&)>& visitor) {
                std::vector<std::pair<Key, Hash>> hashes;
                listHashes(targetHead.root, hashes);
                std::map<Key, std::optional<ModelRecord>>::const_iterator pending =
                    selectedRecords.begin();
                for (const std::pair<Key, Hash>& entry : hashes) {
                    while (pending != selectedRecords.end() && pending->first < entry.first) {
                        if (pending->second) visitor(pending->first, *pending->second);
                        ++pending;
                    }
                    if (pending != selectedRecords.end() && pending->first == entry.first) {
                        if (pending->second) visitor(entry.first, *pending->second);
                        ++pending;
                    } else {
                        const std::optional<ModelRecord> record = readMerged(entry.first);
                        require(Err::Corruption, record.has_value(), "missing record in merge reconciliation");
                        visitor(entry.first, *record);
                    }
                }
                for (; pending != selectedRecords.end(); ++pending)
                    if (pending->second) visitor(pending->first, *pending->second);
            }
        };
        const std::function<void(const Key&, std::optional<ModelRecord>)> set =
            [&selectedRecords](const Key& key, std::optional<ModelRecord> record) {
                selectedRecords.insert_or_assign(key, std::move(record));
            };
        if (const std::optional<std::string> error =
            options.reconcile(candidate, set, readTarget, readSource)) {
            result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                std::nullopt, std::nullopt, *error});
            result.status = MergeStatus::Conflicts;
            return result;
        }
    }
    if (targetHead.modelIndexed) {
        // Content-hash conflicts are not enough: an unrelated deletion may
        // invalidate references created independently on the other branch.
        // Preflight before storing any new objects or publishing a ref.
        for (const std::pair<const Key, std::optional<ModelRecord>>& item : selectedRecords) {
            if (!item.second) continue;
            for (const Key& reference : item.second->references) {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator pendingTarget =
                    selectedRecords.find(reference);
                const bool exists = pendingTarget != selectedRecords.end()
                    ? pendingTarget->second.has_value()
                    : lookup(targetHead.root, reference).has_value();
                if (!exists || reference == Key{} || reference == item.first) {
                    const std::map<Key, Values>::const_iterator values = differences.find(item.first);
                    result.conflicts.push_back(MergeConflict{item.first,
                        values == differences.end() ? std::nullopt : values->second.base,
                        values == differences.end() ? std::nullopt : values->second.target,
                        values == differences.end() ? std::nullopt : values->second.source,
                        "merged record has an invalid reference target"});
                    break;
                }
            }
        }
        for (const std::pair<const Key, std::optional<ModelRecord>>& item : selectedRecords) {
            if (item.second) continue;
            const std::vector<Key> inbound = readReferrers(targetHead.referencesRoot, item.first);
            for (const Key& referrer : inbound) {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator updated =
                    selectedRecords.find(referrer);
                if (updated != selectedRecords.end() &&
                    (!updated->second || std::find(updated->second->references.begin(),
                        updated->second->references.end(), item.first) ==
                        updated->second->references.end())) continue;
                const std::map<Key, Values>::const_iterator values = differences.find(item.first);
                result.conflicts.push_back(MergeConflict{item.first,
                    values == differences.end() ? std::nullopt : values->second.base,
                    values == differences.end() ? std::nullopt : values->second.target,
                    values == differences.end() ? std::nullopt : values->second.source,
                    "merged deletion leaves a surviving incoming reference"});
                break;
            }
        }
        if (!result.conflicts.empty()) {
            result.status = MergeStatus::Conflicts;
            return result;
        }
        if (options.validate) {
            // Copy-on-write view of the proposed snapshot. Changes override
            // the current target tree; no callback can mutate the repository.
            const auto lookupRecord = [this, &targetHead, &selectedRecords](const Key& key)
                -> std::optional<ModelRecord> {
                const auto pending = selectedRecords.find(key);
                if (pending != selectedRecords.end()) return pending->second;
                const std::optional<Hash> hash = lookup(targetHead.root, key);
                if (!hash) return std::nullopt;
                const Bytes value = load(*hash);
                require(Err::Corruption, !value.empty() && value.front() == 'V',
                        "invalid model envelope in merge validation");
                return decodeModelRecord(Bytes(value.begin() + 1, value.end()));
            };
            const MergeCandidate candidate{
                lookupRecord,
                [this, &targetHead, &selectedRecords, &lookupRecord](
                    const std::function<void(const Key&, const ModelRecord&)>& visitor) {
                    std::vector<std::pair<Key, Hash>> hashes;
                    listHashes(targetHead.root, hashes);
                    auto pending = selectedRecords.begin();
                    for (const std::pair<Key, Hash>& entry : hashes) {
                        while (pending != selectedRecords.end() && pending->first < entry.first) {
                            if (pending->second) visitor(pending->first, *pending->second);
                            ++pending;
                        }
                        if (pending != selectedRecords.end() && pending->first == entry.first) {
                            if (pending->second) visitor(entry.first, *pending->second);
                            ++pending;
                        } else {
                            const std::optional<ModelRecord> record = lookupRecord(entry.first);
                            require(Err::Corruption, record.has_value(), "missing model in merge preview");
                            visitor(entry.first, *record);
                        }
                    }
                    for (; pending != selectedRecords.end(); ++pending)
                        if (pending->second) visitor(pending->first, *pending->second);
                }
            };
            if (const std::optional<std::string> invalid = options.validate(candidate)) {
                result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                    std::nullopt, std::nullopt, *invalid});
                result.status = MergeStatus::Conflicts;
                return result;
            }
        }
        modelChanges.reserve(selectedRecords.size());
        for (std::pair<const Key, std::optional<ModelRecord>>& item : selectedRecords) {
            modelChanges.push_back(ModelChange{item.first, std::move(item.second)});
        }
    }
    Hash newRoot{};
    Hash referencesRoot{};
    if (targetHead.modelIndexed) {
        PreparedModelCommit prepared;
        try {
            prepared = prepareModelCommitLocked(targetHead.root,
                                                targetHead.referencesRoot, std::move(modelChanges));
        } catch (const StorageError& error) {
            if (error.kind() != Err::InvalidInput) throw;
            result.conflicts.push_back(MergeConflict{Key{}, std::nullopt, std::nullopt,
                                                     std::nullopt, error.what()});
            result.status = MergeStatus::Conflicts;
            return result;
        }
        newRoot = applyEncodedChangesLocked(targetHead.root, prepared.encoded);
        referencesRoot = prepared.referencesRoot;
    } else {
        newRoot = applyEncodedChangesLocked(targetHead.root, rawChanges);
    }
    const RevisionId merged = writeRevisionV2Locked(project, newRoot, referencesRoot,
                      targetHead.modelIndexed, {targetHead.id, sourceHead.id});
    const std::optional<ResolvedRevisionV2> saved = resolver.resolve(merged, reader);
    require(Err::Internal, saved.has_value(), "v2 merge record was not persisted");
    const std::optional<StoredBranch> stored = findStoredBranch(this->directory_, target);
    require(Err::Internal, stored && stored->revisionV2 == result.targetBefore,
            "v2 target branch disappeared before merge publication");
    publishStoredBranch(this->directory_, StoredBranch{stored->id, stored->name,
                                                        saved->recordHash, merged,
                                                        stored->originBranchId, stored->forkRevisionId}, false);
    result.status = MergeStatus::Merged;
    result.resultHead = merged;
    return result;
}

RevisionId Repository::writeRevisionV2(const ProjectId& project, const Hash& root,
                                       const Hash& referencesRoot, bool modelIndexed,
                                       const std::vector<RevisionId>& parents) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return writeRevisionV2Locked(project, root, referencesRoot, modelIndexed, parents);
}

RevisionId Repository::writeRevisionV2Locked(const ProjectId& project, const Hash& root,
                                             const Hash& referencesRoot, bool modelIndexed,
                                             const std::vector<RevisionId>& parents) {
    require(Err::InvalidInput, modelIndexed || isZero(referencesRoot),
            "raw v2 revision cannot contain a reference index");
    require(Err::InvalidInput, parents.size() <= 2, "v2 revision has too many parents");
    // A hash provided by a caller is never accepted as a claim that a tree
    // exists. Validate the actual content object and its node format.
    if (!isZero(root)) (void)readNode(root);
    if (!isZero(referencesRoot)) (void)readNode(referencesRoot);
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 readObject = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    RevisionRecordV2 record{RevisionId::generate(), project, root, referencesRoot, modelIndexed, {}};
    for (const RevisionId& parentId : parents) {
        const std::optional<ResolvedRevisionV2> parent = resolver.resolve(parentId, readObject);
        require(Err::NotFound, parent.has_value(), "v2 parent revision UUID was not found");
        record.parents.push_back(RevisionParentV2{parentId, parent->recordHash});
    }
    const Bytes encoded = encodeRevisionRecordV2(record);
    const Hash packedHash = store(encoded);
    // Pack fsync precedes UUID binding publication. A crash may leave an
    // unreachable immutable pack object; it must never leave a dangling UUID.
    syncPack();
    const Hash publishedHash = resolver.publish(encoded, readObject);
    require(Err::Internal, publishedHash == packedHash, "v2 pack hash differs from resolver");
    return record.id;
}

std::optional<Bytes> Repository::readRevisionV2(const ProjectId& project, const RevisionId& id) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    // Reading an unknown UUID must not initialize (and potentially pin) an
    // empty repository to an arbitrary caller-supplied project identity.
    const std::filesystem::path namespacePath = this->directory_ / "revision-v2";
    if (!std::filesystem::exists(namespacePath) && !std::filesystem::is_symlink(namespacePath))
        return std::nullopt;
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 readObject = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> resolved = resolver.resolve(id, readObject);
    if (!resolved) return std::nullopt;
    return load(resolved->recordHash);
}

// The v2 ref stores UUID identity *and* the expected packed record digest.
// Resolve and compare both under the repository lock before reading/mutating.
RevisionRecordV2 Repository::branchRevisionV2Locked(const ProjectId& project,
                                                   std::string_view branch) {
    const std::optional<StoredBranch> stored = findStoredBranch(this->directory_, branch);
    require(Err::NotFound, stored.has_value(), "branch not found");
    require(Err::Unavailable, stored->revisionV2.has_value(), "branch has SHA-based HEAD");
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> resolved = resolver.resolve(*stored->revisionV2, reader);
    require(Err::Corruption, resolved.has_value() && resolved->recordHash == stored->head,
            "branch UUIDv7 HEAD does not match immutable revision SHA-256");
    return resolved->record;
}

void Repository::createUnbornBranchV2(const ProjectId& project, std::string_view name) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(name);
    require(Err::Conflict, readStoredBranches(this->directory_).empty(),
            "unborn branch requires a repository without branches");
    // The project ownership is pinned by the first immutable revision, as in
    // createBranchV2. No UUID revision or pack object is written here.
    (void)project;
    publishStoredBranch(this->directory_, StoredBranch{
        BranchId::generate(), std::string(name), Hash{}, std::nullopt, std::nullopt, std::nullopt}, true);
}

void Repository::provisionUnbornBranchV2(const ProjectId& project, std::string_view name,
                                         const BranchId& identity) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(name);
    (void)project;
    const std::vector<StoredBranch> branches = readStoredBranches(this->directory_);
    if (branches.size() == 1 && branches.front().id == identity &&
        branches.front().name == name && !branches.front().revisionV2 &&
        isZero(branches.front().head) && !branches.front().originBranchId &&
        !branches.front().forkRevisionId)
        return;
    require(Err::Conflict, branches.empty(),
            "unborn checkout requires an empty target project or the identical unborn branch");
    publishStoredBranch(this->directory_, StoredBranch{
        identity, std::string(name), Hash{}, std::nullopt, std::nullopt, std::nullopt}, true);
}

std::optional<RevisionId> Repository::optionalHeadV2(
        const ProjectId& project, std::string_view branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const std::optional<StoredBranch> stored = findStoredBranch(this->directory_, branch);
    require(Err::NotFound, stored.has_value(), "branch not found");
    if (!stored->revisionV2 && isZero(stored->head)) return std::nullopt;
    return branchRevisionV2Locked(project, branch).id;
}

RevisionId Repository::headV2(const ProjectId& project, std::string_view branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return branchRevisionV2Locked(project, branch).id;
}

// A direct resolved node has a verified project UUID, immutable record digest
// and authenticated parent bindings. Full-DAG callers traverse both parents.
static RevisionGraphRevisionV2 graphRevisionV2(const ResolvedRevisionV2& resolved) {
    RevisionGraphRevisionV2 result{resolved.record.id, resolved.recordHash,
        resolved.record.root, resolved.record.referencesRoot,
        resolved.record.modelIndexed, {}};
    for (const RevisionParentV2& parent : resolved.record.parents)
        result.parents.push_back({parent.id, parent.expectedRecordHash});
    return result;
}

std::vector<RevisionGraphRevisionV2> Repository::historyV2(
    const ProjectId& project, std::string_view branch, std::size_t maxCount,
    std::optional<RevisionId> after) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    require(Err::InvalidInput, maxCount >= 1 && maxCount <= 1000,
            "v2 history page size must be between 1 and 1000");
    const std::optional<StoredBranch> stored = findStoredBranch(this->directory_, branch);
    require(Err::NotFound, stored.has_value(), "branch not found");
    if (!stored->revisionV2 && isZero(stored->head)) {
        require(Err::InvalidInput, !after,
                "unborn branch cannot have a history cursor");
        return {};
    }
    const RevisionRecordV2 tip = branchRevisionV2Locked(project, branch);
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    // A cursor is a revision UUID, never a filename. Walking the captured
    // first-parent ancestry validates that it really belongs to this branch.
    RevisionId current = tip.id;
    std::size_t traversed = 0;
    if (after) {
        bool found = false;
        for (;;) {
            require(Err::LimitExceeded, ++traversed <= 250000,
                    "v2 first-parent ancestry traversal budget exceeded");
            const std::optional<ResolvedRevisionV2> resolved = resolver.resolveDirect(current, reader);
            require(Err::Corruption, resolved.has_value(), "missing v2 first-parent revision");
            if (current == *after) {
                found = true;
                if (resolved->record.parents.empty()) return {};
                current = resolved->record.parents.front().id;
                break;
            }
            if (resolved->record.parents.empty()) break;
            current = resolved->record.parents.front().id;
        }
        require(Err::InvalidInput, found, "v2 history cursor is not on branch first-parent chain");
    }
    std::vector<RevisionGraphRevisionV2> result;
    result.reserve(maxCount);
    for (;;) {
        require(Err::LimitExceeded, ++traversed <= 250000,
                "v2 first-parent ancestry traversal budget exceeded");
        const std::optional<ResolvedRevisionV2> resolved = resolver.resolveDirect(current, reader);
        require(Err::Corruption, resolved.has_value(), "missing v2 first-parent revision");
        result.push_back(graphRevisionV2(*resolved));
        if (result.size() == maxCount || resolved->record.parents.empty()) break;
        current = resolved->record.parents.front().id;
    }
    return result;
}

RevisionGraphPageV2 Repository::revisionGraphV2(
    const ProjectId& project, std::size_t maxCount,
    std::optional<RevisionGraphCursorV2> cursor) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    constexpr std::size_t maxRevisions = 250000;
    constexpr std::size_t maxBranches = 1024;
    require(Err::InvalidInput, maxCount >= 1 && maxCount <= 1000,
            "v2 graph page size must be between 1 and 1000");
    RevisionGraphPageV2 page;
    std::size_t skip = 0;
    if (cursor) {
        require(Err::InvalidInput, cursor->heads.size() <= maxBranches &&
                cursor->skip <= maxRevisions, "invalid v2 graph cursor bounds");
        skip = cursor->skip;
        page.heads = std::move(cursor->heads);
        std::string previous;
        std::set<BranchId> identities;
        for (const RevisionGraphHeadV2& head : page.heads) {
            checkBranch(head.name);
            require(Err::InvalidInput, (previous.empty() || previous < head.name) &&
                    (head.revision.has_value() == (head.recordHash != Hash{})) &&
                    identities.insert(head.branchId).second,
                    "invalid v2 graph cursor heads");
            previous = head.name;
        }
    } else {
        for (const StoredBranch& branch : readStoredBranches(this->directory_)) {
            require(Err::Unavailable, branch.revisionV2.has_value() || isZero(branch.head),
                    "v2 graph requires UUIDv7 revision HEADs");
            page.heads.push_back({branch.id, branch.name, branch.revisionV2,
                                  branch.head, branch.originBranchId, branch.forkRevisionId});
            require(Err::LimitExceeded, page.heads.size() <= maxBranches,
                    "v2 graph exceeds branch snapshot limit");
        }
    }
    if (page.heads.empty()) return page;
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    // Authenticate every captured HEAD independently of current moving refs.
    // An obsolete branch may have disappeared, but its pinned immutable
    // revision remains traversable until compaction; do not reread HEADs.
    std::map<RevisionId, ResolvedRevisionV2> cached;
    const auto get = [&](const RevisionId& id) -> const ResolvedRevisionV2& {
        const auto found = cached.find(id);
        if (found != cached.end()) return found->second;
        const std::optional<ResolvedRevisionV2> value = resolver.resolveDirect(id, reader);
        require(Err::Corruption, value.has_value(), "missing v2 graph revision");
        return cached.emplace(id, *value).first->second;
    };
    for (const RevisionGraphHeadV2& head : page.heads) {
        if (head.revision)
            require(Err::Corruption, get(*head.revision).recordHash == head.recordHash,
                    "v2 graph HEAD UUID/SHA mismatch");
    }
    std::vector<RevisionId> pending;
    pending.reserve(page.heads.size());
    for (auto it = page.heads.rbegin(); it != page.heads.rend(); ++it) {
        if (it->revision) pending.push_back(*it->revision);
    }
    std::set<RevisionId> seen;
    std::size_t visited = 0;
    while (!pending.empty()) {
        const RevisionId id = pending.back();
        pending.pop_back();
        if (!seen.insert(id).second) continue;
        require(Err::LimitExceeded, visited < maxRevisions,
                "v2 graph exceeds revision traversal budget");
        const ResolvedRevisionV2& resolved = get(id);
        for (auto it = resolved.record.parents.rbegin(); it != resolved.record.parents.rend(); ++it)
            pending.push_back(it->id);
        if (visited++ < skip) continue;
        if (page.revisions.size() == maxCount) {
            page.next = RevisionGraphCursorV2{page.heads, skip + maxCount};
            break;
        }
        page.revisions.push_back(graphRevisionV2(resolved));
    }
    require(Err::InvalidInput, !cursor || visited > skip,
            "v2 graph cursor is beyond captured history");
    return page;
}

void Repository::renameBranchV2(const ProjectId& project, std::string_view previous,
                                std::string_view renamed, const RevisionId& expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(renamed);
    require(Err::Conflict, previous != renamed, "branch rename must change the name");
    const RevisionRecordV2 previousRevision = branchRevisionV2Locked(project, previous);
    require(Err::Conflict, previousRevision.id == expectedHead,
            "branch HEAD advanced before rename");
    require(Err::Conflict, !findStoredBranch(this->directory_, renamed),
            "new branch name already exists");
    const std::optional<StoredBranch> existing = findStoredBranch(this->directory_, previous);
    require(Err::Internal, existing && existing->revisionV2, "v2 branch disappeared");
    publishStoredBranch(this->directory_, StoredBranch{existing->id, std::string(renamed),
                                 existing->head, existing->revisionV2, existing->originBranchId, existing->forkRevisionId}, false);
}

RevisionId Repository::createBranchV2(const ProjectId& project, std::string_view name,
                                       std::optional<std::string_view> from) {
    return createBranchWithIdentityV2(project, name, from).revisionId;
}

BranchCreationResultV2 Repository::createBranchWithIdentityV2(
        const ProjectId& project, std::string_view name,
        std::optional<std::string_view> from) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return createBranchLockedV2(project, name, from, BranchId::generate());
}

BranchCreationResultV2 Repository::createBranchLockedV2(
        const ProjectId& project, std::string_view name,
        std::optional<std::string_view> from, const BranchId& createdId) {
    checkBranch(name);
    require(Err::Conflict, !findStoredBranch(this->directory_, name), "branch already exists");
    const std::vector<StoredBranch> branches = readStoredBranches(this->directory_);
    for (const StoredBranch& existing : branches)
        require(Err::Unavailable, existing.revisionV2.has_value(), "cannot mix SHA and UUID revision HEADs");
    RevisionId id = RevisionId::generate();
    Hash checksum{};
    if (from) {
        const RevisionRecordV2 source = branchRevisionV2Locked(project, *from);
        id = source.id;
        checksum = hashRevisionRecordV2(source);
    } else {
        require(Err::InvalidInput, branches.empty(),
                "new branch requires a source once project has a root revision");
        id = writeRevisionV2Locked(project, Hash{}, Hash{}, false, {});
        RevisionResolverV2 resolver(this->directory_, project);
        const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
            if (!containsIndex(digest)) return std::nullopt;
            return load(digest);
        };
        const std::optional<ResolvedRevisionV2> root = resolver.resolve(id, reader);
        require(Err::Internal, root.has_value(), "new root revision did not resolve");
        checksum = root->recordHash;
    }
    const std::optional<BranchId> sourceId = from
        ? std::optional<BranchId>(findStoredBranch(this->directory_, *from)->id) : std::nullopt;
    const std::optional<RevisionId> forkId = from
        ? std::optional<RevisionId>(id) : std::nullopt;
    publishStoredBranch(this->directory_, StoredBranch{createdId, std::string(name),
                                                        checksum, id, sourceId, forkId}, true);
    return BranchCreationResultV2{createdId, id, sourceId, forkId};
}

std::optional<RevisionGraphHeadV2> Repository::branchByIdV2(const ProjectId& project,
                                                           const BranchId& branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    for (const StoredBranch& stored : readStoredBranches(this->directory_)) {
        if (stored.id != branch) continue;
        require(Err::Unavailable, stored.revisionV2.has_value() || isZero(stored.head),
                "UUIDv7 branch lookup requires a v2 branch");
        std::optional<RevisionId> revision;
        Hash recordHash{};
        if (stored.revisionV2) {
            // Same integrity check as every other HEAD read: the stored
            // digest must be the immutable record of the stored revision.
            revision = branchRevisionV2Locked(project, stored.name).id;
            recordHash = stored.head;
        }
        return RevisionGraphHeadV2{stored.id, stored.name, revision, recordHash,
                                   stored.originBranchId, stored.forkRevisionId};
    }
    return std::nullopt;
}

BranchCreationResultV2 Repository::createBranchFromIdV2(
        const ProjectId& project, std::string_view name, const BranchId& source,
        std::optional<RevisionId> revision, const BranchId& created) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(name);
    std::optional<StoredBranch> sourceBranch;
    for (StoredBranch& stored : readStoredBranches(this->directory_)) {
        if (stored.id == created) {
            // A retried request (e.g. a transport that resent the POST after
            // a lost response) finds its own branch: answer as the original
            // creation did. Any other use of the identity is a conflict.
            require(Err::Conflict, stored.name == name && stored.originBranchId == source &&
                        (!revision || stored.forkRevisionId == revision) &&
                        stored.forkRevisionId.has_value(),
                    "branch UUIDv7 already identifies a different branch");
            return BranchCreationResultV2{stored.id, *stored.forkRevisionId,
                                          stored.originBranchId, stored.forkRevisionId};
        }
        if (stored.id == source) sourceBranch = std::move(stored);
    }
    require(Err::NotFound, sourceBranch.has_value(), "source branch UUIDv7 not found");
    if (revision)
        return createBranchAtRevisionLockedV2(project, name, sourceBranch->name, *revision, created);
    return createBranchLockedV2(project, name, sourceBranch->name, created);
}

bool Repository::hasBranch(std::string_view branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    return findStoredBranch(this->directory_, branch).has_value();
}

std::vector<std::string> Repository::listBranches() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    std::vector<std::string> result;
    for (const StoredBranch& branch : readStoredBranches(this->directory_)) result.push_back(branch.name);
    return result;
}

Revision Repository::head(std::string_view branch) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return readRevision(branchHead(branch));
}

Revision Repository::revision(const Hash& revisionId) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return clientRevision(revisionId);
}

// Revision identities supplied by callers (HTTP clients, CLI arguments) may be
// unknown or may name a tree/value object. Both are NotFound, never storage
// corruption; a checksum failure of an existing object remains Corruption.
Revision Repository::clientRevision(const Hash& revisionId) {
    require(Err::InvalidInput, !isZero(revisionId), "zero revision hash is invalid");
    require(Err::NotFound, containsIndex(revisionId), "revision not found");
    const Bytes payload = load(revisionId);
    require(Err::NotFound, payload.size() >= 67 && payload[0] == 'S' && payload[65] <= 1 &&
                payload[66] <= 2 && payload.size() == 67U + 32U * payload[66],
            "object is not a revision");
    return readRevision(revisionId);
}

std::vector<Revision> Repository::history(std::string_view branch, std::size_t maxCount,
                                          std::optional<Hash> after) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    require(Err::InvalidInput, maxCount <= 1000, "history page exceeds 1000 revisions");
    Hash current = branchHead(branch);
    if (after) {
        require(Err::InvalidInput, !isZero(*after), "zero history cursor is invalid");
        // A cursor from another branch (or a side parent of a merge) must
        // not silently switch the history being paged.
        for (;;) {
            const Revision item = readRevision(current);
            if (item.id == *after) {
                current = item.parents.empty() ? Hash{} : item.parents.front();
                break;
            }
            require(Err::InvalidInput, !item.parents.empty(), "history cursor not on branch first-parent chain");
            current = item.parents.front();
        }
    }
    std::vector<Revision> result;
    result.reserve(maxCount);
    while (!isZero(current) && result.size() < maxCount) {
        const Revision item = readRevision(current);
        result.push_back(item);
        current = item.parents.empty() ? Hash{} : item.parents.front();
    }
    return result;
}

RevisionGraphPage Repository::revisionGraph(std::size_t maxCount,
                                                std::optional<RevisionGraphCursor> cursor) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    // A deliberate work budget protects the HTTP worker (and the native GUI)
    // from unbounded traversal. Exceeding it is explicit, never a truncated DAG.
    constexpr std::size_t maxRevisions = 250000;
    constexpr std::size_t maxBranches = 1024;
    require(Err::InvalidInput, maxCount >= 1 && maxCount <= 1000,
            "graph page size must be between 1 and 1000");

    RevisionGraphPage page;
    std::size_t skip = 0;
    if (cursor) {
        require(Err::InvalidInput, cursor->heads.size() <= maxBranches &&
                cursor->skip <= maxRevisions, "invalid graph cursor bounds");
        skip = cursor->skip;
        page.heads = std::move(cursor->heads);
        std::string previous;
        for (const RevisionGraphHead& head : page.heads) {
            checkBranch(head.name);
            require(Err::InvalidInput, !isZero(head.revision) &&
                    (previous.empty() || previous < head.name), "invalid graph cursor heads");
            previous = head.name;
        }
    } else {
        for (const StoredBranch& branch : readStoredBranches(this->directory_)) {
            page.heads.push_back({branch.name, branch.head});
            require(Err::LimitExceeded, page.heads.size() <= maxBranches,
                    "graph exceeds branch snapshot limit");
        }
    }

    // Deterministic iterative DFS, walking *both* merge parents. Process the
    // branch refs in sorted order and parents in stored order. Replaying the
    // immutable snapshot until skip reconstructs the visited set across pages,
    // so a shared ancestor is returned exactly once without server sessions.
    // Order is stable DFS, not a global topological sort between roots.
    std::vector<Hash> pending;
    pending.reserve(page.heads.size());
    for (auto it = page.heads.rbegin(); it != page.heads.rend(); ++it)
        pending.push_back(it->revision);
    std::set<Hash> seen;
    std::size_t visited = 0;
    while (!pending.empty()) {
        const Hash id = pending.back();
        pending.pop_back();
        if (!seen.insert(id).second) continue;
        require(Err::LimitExceeded, visited < maxRevisions,
                "graph exceeds revision traversal budget");
        // A stored HEAD set remains valid after normal branch advances,
        // because revision objects are immutable. Offline compaction can
        // invalidate an old cursor; such reads fail rather than lie.
        const Revision item = readRevision(id);
        for (auto it = item.parents.rbegin(); it != item.parents.rend(); ++it)
            pending.push_back(*it);
        if (visited++ < skip) continue;
        if (page.revisions.size() == maxCount) {
            page.next = RevisionGraphCursor{page.heads, skip + maxCount};
            break;
        }
        page.revisions.push_back(item);
    }
    require(Err::InvalidInput, !cursor || visited > skip,
            "graph cursor is beyond the captured history");
    return page;
}

bool Repository::revisionOnBranch(std::string_view branch, const Hash& revisionId) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    (void)clientRevision(revisionId); // NotFound before walking any history
    return revisionReachableFrom(branchHead(branch), revisionId);
}

void Repository::createBranchAtRevision(std::string_view name, std::string_view sourceBranch,
                                        const Hash& revisionId) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(name);
    require(Err::Conflict, !findStoredBranch(this->directory_, name), "branch already exists");
    require(Err::InvalidInput, revisionReachableFrom(branchHead(sourceBranch), revisionId),
            "revision is not reachable from source branch");
    publishRef(name, revisionId, true);
}

MergeResult Repository::mergeBranches(std::string_view target, std::string_view source,
                                      const MergeOptions& options) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision targetHead = readRevision(branchHead(target));
    const Revision sourceHead = readRevision(branchHead(source));
    require(Err::Conflict, !options.expectedTargetHead || *options.expectedTargetHead == targetHead.id,
            "target branch advanced since merge resolution was prepared");
    require(Err::Conflict, !options.expectedSourceHead || *options.expectedSourceHead == sourceHead.id,
            "source branch advanced since merge resolution was prepared");
    const std::function<void()> checkSelections = [&options]() {
        if (!options.checkSelections) return;
        const std::optional<std::string> problem = options.checkSelections();
        require(Err::InvalidInput, !problem, problem.value_or("unconsumed merge resolution choices"));
    };
    MergeResult result{};
    result.targetBefore = targetHead.id;
    result.sourceHead = sourceHead.id;
    result.revision = targetHead;
    if (targetHead.id == sourceHead.id) {
        checkSelections();
        result.base = targetHead.id;
        return result;
    }

    // Track only target revision identities, not full Revision objects.
    // Short-circuit up-to-date merges as soon as the source tip is found,
    // including when it is a *second* parent of an earlier merge.
    std::unordered_set<Hash, HashHasher> targetAncestors;
    std::vector<Hash> pending{targetHead.id};
    while (!pending.empty()) {
        const Hash current = pending.back();
        pending.pop_back();
        require(Err::Corruption, !isZero(current), "zero revision parent in merge ancestry");
        if (!targetAncestors.insert(current).second) continue;
        if (current == sourceHead.id) {
            checkSelections();
            result.base = sourceHead.id;
            return result; // source already incorporated
        }
        const Revision record = readRevision(current);
        for (const Hash& parent : record.parents) pending.push_back(parent);
    }

    // The first target-common revision encountered on each source path
    // dominates all of its own ancestors. Stop walking that path there:
    // descending into the shared history only to rediscover older common
    // revisions is unnecessary. This bounds source-side work by the
    // divergent graph plus the common frontier in typical histories.
    std::unordered_set<Hash, HashHasher> sourceVisited;
    std::unordered_set<Hash, HashHasher> commonFrontier;
    pending.push_back(sourceHead.id);
    while (!pending.empty()) {
        const Hash current = pending.back();
        pending.pop_back();
        require(Err::Corruption, !isZero(current), "zero revision parent in merge ancestry");
        if (!sourceVisited.insert(current).second) continue;
        if (current == targetHead.id) {
            // The target is an ancestor of the source. When the caller has
            // supplied domain validation, the source snapshot is the entire
            // proposed result and must be checked BEFORE publishing its ref.
            // This does not create a revision or modify the source branch.
            checkSelections();
            result.base = targetHead.id;
            if (options.validate) {
                // A domain validator expects typed model records; a raw
                // snapshot must never bypass it through a fast-forward.
                if (!sourceHead.modelIndexed) {
                    result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                        std::nullopt, std::nullopt,
                        "cannot validate a non-model-indexed source snapshot"});
                    result.status = MergeStatus::Conflicts;
                    return result;
                }
                const std::function<std::optional<ModelRecord>(const Key&)> readSource =
                    [this, &sourceHead](const Key& key) -> std::optional<ModelRecord> {
                        const std::optional<Hash> hash = lookup(sourceHead.root, key);
                        if (!hash) return std::nullopt;
                        const Bytes value = load(*hash);
                        require(Err::Corruption, !value.empty() && value.front() == 'V',
                                "invalid model envelope in fast-forward validation");
                        return decodeModelRecord(Bytes(value.begin() + 1, value.end()));
                    };
                const MergeCandidate candidate{
                    readSource,
                    [this, &sourceHead](
                        const std::function<void(const Key&, const ModelRecord&)>& visitor) {
                        std::vector<std::pair<Key, Hash>> hashes;
                        listHashes(sourceHead.root, hashes);
                        for (const std::pair<Key, Hash>& entry : hashes) {
                            const Bytes value = load(entry.second);
                            require(Err::Corruption, !value.empty() && value.front() == 'V',
                                    "invalid model envelope in fast-forward validation");
                            visitor(entry.first,
                                    decodeModelRecord(Bytes(value.begin() + 1, value.end())));
                        }
                    }
                };
                if (const std::optional<std::string> invalid = options.validate(candidate)) {
                    result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                        std::nullopt, std::nullopt, *invalid});
                    result.status = MergeStatus::Conflicts;
                    return result;
                }
            }
            publishRef(target, sourceHead.id, false);
            result.status = MergeStatus::FastForward;
            result.revision = sourceHead;
            return result;
        }
        if (targetAncestors.contains(current)) {
            commonFrontier.insert(current);
            continue;
        }
        const Revision record = readRevision(current);
        for (const Hash& parent : record.parents) pending.push_back(parent);
    }
    require(Err::Conflict, !commonFrontier.empty(), "branches have no common revision ancestor");

    // Different merge paths can expose common-frontier nodes which are
    // themselves ancestors of other frontier nodes (criss-cross DAGs).
    // Remove dominated candidates, visiting any ancestor at most once.
    // The overwhelmingly common single-frontier case performs no extra
    // history traversal at all.
    std::unordered_set<Hash, HashHasher> dominated;
    if (commonFrontier.size() > 1) {
        std::unordered_set<Hash, HashHasher> visitedAncestors;
        for (const Hash& candidate : commonFrontier) {
            const Revision record = readRevision(candidate);
            std::vector<Hash> ancestry = record.parents;
            while (!ancestry.empty()) {
                const Hash current = ancestry.back();
                ancestry.pop_back();
                if (!visitedAncestors.insert(current).second) continue;
                if (commonFrontier.contains(current)) dominated.insert(current);
                const Revision ancestor = readRevision(current);
                for (const Hash& parent : ancestor.parents) ancestry.push_back(parent);
            }
        }
    }
    std::vector<Hash> lowest;
    for (const Hash& candidate : commonFrontier) {
        if (!dominated.contains(candidate)) lowest.push_back(candidate);
    }
    require(Err::Conflict, lowest.size() == 1, "multiple independent merge bases require recursive merging");
    result.base = lowest.front();
    const Revision base = readRevision(result.base);
    require(Err::InvalidInput, targetHead.modelIndexed == sourceHead.modelIndexed &&
            (base.modelIndexed == targetHead.modelIndexed ||
             (!base.modelIndexed && isZero(base.root) && targetHead.modelIndexed)),
            "cannot merge incompatible raw and model-indexed histories");

    std::vector<Difference> targetDiff;
    std::vector<Difference> sourceDiff;
    diffHashes(base.root, targetHead.root, targetDiff);
    diffHashes(base.root, sourceHead.root, sourceDiff);
    struct Values {
        std::optional<Hash> base;
        std::optional<Hash> target;
        std::optional<Hash> source;
    };
    std::map<Key, Values> differences;
    for (const Difference& change : targetDiff) {
        differences.emplace(change.key, Values{change.before, change.after, change.before});
    }
    for (const Difference& change : sourceDiff) {
        std::map<Key, Values>::iterator existing = differences.find(change.key);
        if (existing == differences.end()) {
            differences.emplace(change.key,
                                Values{change.before, change.before, change.after});
        } else {
            existing->second.source = change.after;
        }
    }
    std::map<Key, std::optional<Hash>> selected;
    // Resolutions are checked without writing any pack objects. The
    // resolver sees authenticated canonical model records at all three
    // immutable snapshots and must not access this Repository recursively.
    std::map<Key, ModelRecord> resolved;
    for (const std::pair<const Key, Values>& item : differences) {
        const Values& values = item.second;
        if (values.target == values.source) continue;
        if (values.target != values.base && values.source != values.base) {
            if (options.chooseConflict && targetHead.modelIndexed) {
                const MergeConflict conflict{item.first, values.base, values.target,
                    values.source, "concurrent changes to the same UUID"};
                const std::optional<MergeChoiceSide> choice = options.chooseConflict(conflict);
                if (choice) {
                    // Also include target-side selections in the candidate
                    // preview, so reference/semantic validation cannot be
                    // skipped when the chosen object is already at target.
                    selected.emplace(item.first, *choice == MergeChoiceSide::Source
                        ? values.source : values.target);
                    continue;
                }
            }
            if (options.resolve && targetHead.modelIndexed && base.modelIndexed &&
                values.base && values.target && values.source) {
                const auto decodeAt = [this](const Hash& hash) {
                    const Bytes envelope = load(hash);
                    require(Err::Corruption, !envelope.empty() && envelope.front() == 'V',
                            "invalid model envelope in merge conflict");
                    return decodeModelRecord(Bytes(envelope.begin() + 1, envelope.end()));
                };
                const ModelRecord original = decodeAt(*values.base);
                const ModelRecord left = decodeAt(*values.target);
                const ModelRecord right = decodeAt(*values.source);
                if (original.kind == left.kind && left.kind == right.kind) {
                    MergeResolution resolution = options.resolve(item.first, original, left, right);
                    if (resolution.record && resolution.conflictingFields.empty() &&
                        resolution.record->kind == left.kind) {
                        resolved.emplace(item.first, std::move(*resolution.record));
                        continue;
                    }
                    if (!resolution.conflictingFields.empty()) {
                        std::string detail = "conflicting model fields: ";
                        for (const std::string& field : resolution.conflictingFields) {
                            if (detail.back() != ' ') detail += ", ";
                            detail += field;
                        }
                        result.conflicts.push_back(MergeConflict{item.first, values.base,
                            values.target, values.source, std::move(detail)});
                        continue;
                    }
                }
            }
            if (options.resolveConcurrentAddition && targetHead.modelIndexed &&
                !values.base && values.target && values.source) {
                const auto decodeAt = [this](const Hash& hash) {
                    const Bytes envelope = load(hash);
                    require(Err::Corruption, !envelope.empty() && envelope.front() == 'V',
                            "invalid model envelope in concurrent addition");
                    return decodeModelRecord(Bytes(envelope.begin() + 1, envelope.end()));
                };
                const ModelRecord left = decodeAt(*values.target);
                const ModelRecord right = decodeAt(*values.source);
                MergeResolution resolution = options.resolveConcurrentAddition(
                    item.first, left, right);
                if (resolution.record && resolution.conflictingFields.empty() &&
                    resolution.record->kind == left.kind && left.kind == right.kind) {
                    resolved.emplace(item.first, std::move(*resolution.record));
                    continue;
                }
            }
            result.conflicts.push_back(MergeConflict{item.first, values.base,
                values.target, values.source, "both branches changed the same UUID"});
        } else if (values.target == values.base) {
            selected.emplace(item.first, values.source);
        }
    }
    // Validate explicit choices even when other conflicts remain unresolved.
    // Otherwise a misspelled/unused choice could be silently ignored by
    // an early conflict return, only to change behavior on a later retry.
    checkSelections();
    if (!result.conflicts.empty()) {
        result.status = MergeStatus::Conflicts;
        return result;
    }

    std::vector<ModelChange> modelChanges;
    std::vector<Change> rawChanges;
    // Keep the selected records for dependency preflight, so a target can
    // be checked without decoding unrelated entities.
    std::map<Key, std::optional<ModelRecord>> selectedRecords;
    for (const std::pair<const Key, std::optional<Hash>>& item : selected) {
        if (!item.second) {
            if (targetHead.modelIndexed) selectedRecords.emplace(item.first, std::nullopt);
            else rawChanges.push_back(Change{item.first, std::nullopt});
            continue;
        }
        const Bytes value = load(*item.second);
        require(Err::Corruption, !value.empty() && value.front() == 'V', "invalid merged value envelope");
        const Bytes payload(value.begin() + 1, value.end());
        if (targetHead.modelIndexed) {
            ModelRecord record = decodeModelRecord(payload);
            selectedRecords.emplace(item.first, std::move(record));
        } else {
            rawChanges.push_back(Change{item.first, payload});
        }
    }
    for (std::pair<const Key, ModelRecord>& item : resolved)
        selectedRecords.emplace(item.first, std::move(item.second));
    if (targetHead.modelIndexed && options.reconcile) {
        // Readers inspect authenticated, immutable branch tips; candidate
        // modifications remain memory-only until ALL preflights complete.
        const auto readSnapshot = [this](const Hash& root, const Key& key)
            -> std::optional<ModelRecord> {
            const std::optional<Hash> hash = lookup(root, key);
            if (!hash) return std::nullopt;
            const Bytes value = load(*hash);
            require(Err::Corruption, !value.empty() && value.front() == 'V',
                    "invalid model envelope in merge reconciliation");
            return decodeModelRecord(Bytes(value.begin() + 1, value.end()));
        };
        const std::function<std::optional<ModelRecord>(const Key&)> readTarget =
            [&readSnapshot, &targetHead](const Key& key) {
                return readSnapshot(targetHead.root, key);
            };
        const std::function<std::optional<ModelRecord>(const Key&)> readSource =
            [&readSnapshot, &sourceHead](const Key& key) {
                return readSnapshot(sourceHead.root, key);
            };
        const std::function<std::optional<ModelRecord>(const Key&)> readMerged =
            [&selectedRecords, &readTarget](const Key& key) {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator found =
                    selectedRecords.find(key);
                return found == selectedRecords.end() ? readTarget(key) : found->second;
            };
        const MergeCandidate candidate{
            readMerged,
            [this, &targetHead, &selectedRecords, &readMerged](
                const std::function<void(const Key&, const ModelRecord&)>& visitor) {
                std::vector<std::pair<Key, Hash>> hashes;
                listHashes(targetHead.root, hashes);
                std::map<Key, std::optional<ModelRecord>>::const_iterator pending =
                    selectedRecords.begin();
                for (const std::pair<Key, Hash>& entry : hashes) {
                    while (pending != selectedRecords.end() && pending->first < entry.first) {
                        if (pending->second) visitor(pending->first, *pending->second);
                        ++pending;
                    }
                    if (pending != selectedRecords.end() && pending->first == entry.first) {
                        if (pending->second) visitor(entry.first, *pending->second);
                        ++pending;
                    } else {
                        const std::optional<ModelRecord> record = readMerged(entry.first);
                        require(Err::Corruption, record.has_value(), "missing record in merge reconciliation");
                        visitor(entry.first, *record);
                    }
                }
                for (; pending != selectedRecords.end(); ++pending)
                    if (pending->second) visitor(pending->first, *pending->second);
            }
        };
        const std::function<void(const Key&, std::optional<ModelRecord>)> set =
            [&selectedRecords](const Key& key, std::optional<ModelRecord> record) {
                selectedRecords.insert_or_assign(key, std::move(record));
            };
        if (const std::optional<std::string> error =
            options.reconcile(candidate, set, readTarget, readSource)) {
            result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                std::nullopt, std::nullopt, *error});
            result.status = MergeStatus::Conflicts;
            return result;
        }
    }
    if (targetHead.modelIndexed) {
        // Content-hash conflicts are not enough: an unrelated deletion may
        // invalidate references created independently on the other branch.
        // Preflight before storing any new objects or publishing a ref.
        for (const std::pair<const Key, std::optional<ModelRecord>>& item : selectedRecords) {
            if (!item.second) continue;
            for (const Key& reference : item.second->references) {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator pendingTarget =
                    selectedRecords.find(reference);
                const bool exists = pendingTarget != selectedRecords.end()
                    ? pendingTarget->second.has_value()
                    : lookup(targetHead.root, reference).has_value();
                if (!exists || reference == Key{} || reference == item.first) {
                    const std::map<Key, Values>::const_iterator values = differences.find(item.first);
                    result.conflicts.push_back(MergeConflict{item.first,
                        values == differences.end() ? std::nullopt : values->second.base,
                        values == differences.end() ? std::nullopt : values->second.target,
                        values == differences.end() ? std::nullopt : values->second.source,
                        "merged record has an invalid reference target"});
                    break;
                }
            }
        }
        for (const std::pair<const Key, std::optional<ModelRecord>>& item : selectedRecords) {
            if (item.second) continue;
            const std::vector<Key> inbound = readReferrers(targetHead.referencesRoot, item.first);
            for (const Key& referrer : inbound) {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator updated =
                    selectedRecords.find(referrer);
                if (updated != selectedRecords.end() &&
                    (!updated->second || std::find(updated->second->references.begin(),
                        updated->second->references.end(), item.first) ==
                        updated->second->references.end())) continue;
                const std::map<Key, Values>::const_iterator values = differences.find(item.first);
                result.conflicts.push_back(MergeConflict{item.first,
                    values == differences.end() ? std::nullopt : values->second.base,
                    values == differences.end() ? std::nullopt : values->second.target,
                    values == differences.end() ? std::nullopt : values->second.source,
                    "merged deletion leaves a surviving incoming reference"});
                break;
            }
        }
        if (!result.conflicts.empty()) {
            result.status = MergeStatus::Conflicts;
            return result;
        }
        if (options.validate) {
            // Copy-on-write view of the proposed snapshot. Changes override
            // the current target tree; no callback can mutate the repository.
            const auto lookupRecord = [this, &targetHead, &selectedRecords](const Key& key)
                -> std::optional<ModelRecord> {
                const auto pending = selectedRecords.find(key);
                if (pending != selectedRecords.end()) return pending->second;
                const std::optional<Hash> hash = lookup(targetHead.root, key);
                if (!hash) return std::nullopt;
                const Bytes value = load(*hash);
                require(Err::Corruption, !value.empty() && value.front() == 'V',
                        "invalid model envelope in merge validation");
                return decodeModelRecord(Bytes(value.begin() + 1, value.end()));
            };
            const MergeCandidate candidate{
                lookupRecord,
                [this, &targetHead, &selectedRecords, &lookupRecord](
                    const std::function<void(const Key&, const ModelRecord&)>& visitor) {
                    std::vector<std::pair<Key, Hash>> hashes;
                    listHashes(targetHead.root, hashes);
                    auto pending = selectedRecords.begin();
                    for (const std::pair<Key, Hash>& entry : hashes) {
                        while (pending != selectedRecords.end() && pending->first < entry.first) {
                            if (pending->second) visitor(pending->first, *pending->second);
                            ++pending;
                        }
                        if (pending != selectedRecords.end() && pending->first == entry.first) {
                            if (pending->second) visitor(entry.first, *pending->second);
                            ++pending;
                        } else {
                            const std::optional<ModelRecord> record = lookupRecord(entry.first);
                            require(Err::Corruption, record.has_value(), "missing model in merge preview");
                            visitor(entry.first, *record);
                        }
                    }
                    for (; pending != selectedRecords.end(); ++pending)
                        if (pending->second) visitor(pending->first, *pending->second);
                }
            };
            if (const std::optional<std::string> invalid = options.validate(candidate)) {
                result.conflicts.push_back(MergeConflict{Key{}, std::nullopt,
                    std::nullopt, std::nullopt, *invalid});
                result.status = MergeStatus::Conflicts;
                return result;
            }
        }
        modelChanges.reserve(selectedRecords.size());
        for (std::pair<const Key, std::optional<ModelRecord>>& item : selectedRecords) {
            modelChanges.push_back(ModelChange{item.first, std::move(item.second)});
        }
    }
    // A merge with no primary changes is still a two-parent revision: the
    // second parent records that the source history was incorporated.
    if (targetHead.modelIndexed) {
        result.revision = commitModelLocked(target, std::move(modelChanges),
                                            targetHead.id, sourceHead.id);
    } else {
        result.revision = commitLocked(target, rawChanges, std::nullopt, sourceHead.id);
    }
    result.status = MergeStatus::Merged;
    return result;
}

Revision Repository::commit(std::string_view branch, const std::vector<Change>& changes) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return commitLocked(branch, changes);
}

Hash Repository::applyEncodedChangesLocked(const Hash& original,
                                            const std::vector<Change>& changes) {
    std::vector<const Change*> ordered;
    ordered.reserve(changes.size());
    for (const Change& change : changes) ordered.push_back(&change);
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const Change* left, const Change* right) { return left->key < right->key; });
    std::vector<TreeUpdate> updates;
    updates.reserve(ordered.size());
    for (std::size_t i = 0; i < ordered.size();) {
        std::size_t next = i + 1;
        while (next < ordered.size() && ordered[i]->key == ordered[next]->key) ++next;
        const Change& change = *ordered[next - 1];
        std::optional<Hash> value;
        if (change.value) {
            Bytes encoded{'V'};
            encoded.insert(encoded.end(), change.value->begin(), change.value->end());
            value = store(std::move(encoded));
        }
        updates.emplace_back(change.key, value);
        i = next;
    }
    return applyTree(original, updates, 0, updates.size(), 0);
}

Revision Repository::commitLocked(std::string_view branch, const std::vector<Change>& changes,
                                  std::optional<Hash> referencesRoot,
                                  std::optional<Hash> mergeParent) {
    const Hash parent = branchHead(branch);
    const Revision previous = readRevision(parent);
    require(Err::InvalidInput, !previous.modelIndexed || referencesRoot.has_value(),
            "raw commits cannot modify a model-indexed branch");
    require(Err::InvalidInput, !referencesRoot.has_value() || previous.modelIndexed || isZero(previous.root),
            "cannot enable model indexing on a populated raw branch");
    // Sort pointers into the input rather than copying every encoded payload
    // into a second map. Stable ordering preserves raw commit's last-write-
    // wins behavior for repeated keys.
    const Hash newRoot = applyEncodedChangesLocked(previous.root, changes);
    Bytes encoded{'S'};
    appendHash(encoded, newRoot);
    appendHash(encoded, referencesRoot.value_or(Hash{}));
    encoded.push_back(referencesRoot.has_value() ? 1 : 0);
    encoded.push_back(mergeParent ? 2 : 1);
    appendHash(encoded, parent);
    if (mergeParent) {
        require(Err::Internal, !isZero(*mergeParent) && *mergeParent != parent, "invalid merge parent");
        appendHash(encoded, *mergeParent);
    }
    const Hash newRevision = store(std::move(encoded));
    // Publication order is critical: a crash may leave unused packed objects,
    // but must never leave a published ref pointing to unsynced content.
    syncPack();
    publishRef(branch, newRevision, false);
    return readRevision(newRevision);
}


RevisionId Repository::commitV2(const ProjectId& project, std::string_view branch,
                                const std::vector<Change>& changes,
                                std::optional<RevisionId> expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const std::optional<StoredBranch> starting = findStoredBranch(this->directory_, branch);
    require(Err::NotFound, starting.has_value(), "branch not found");
    const bool unborn = !starting->revisionV2 && isZero(starting->head);
    const std::optional<RevisionRecordV2> previous = unborn ? std::nullopt :
        std::optional<RevisionRecordV2>(branchRevisionV2Locked(project, branch));
    require(Err::Conflict, !expectedHead || (previous && previous->id == *expectedHead),
            "branch HEAD changed before commit");
    require(Err::InvalidInput, !previous || !previous->modelIndexed,
            "raw edits cannot modify a model-indexed revision");
    const Hash newRoot = applyEncodedChangesLocked(previous ? previous->root : Hash{}, changes);
    const RevisionId newId = writeRevisionV2Locked(project, newRoot, Hash{}, false, previous ? std::vector<RevisionId>{previous->id} : std::vector<RevisionId>{});
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> updated = resolver.resolve(newId, reader);
    require(Err::Internal, updated.has_value(), "new revision failed post-write verification");
    const std::optional<StoredBranch> stored = findStoredBranch(this->directory_, branch);
    require(Err::Internal, stored && stored->id == starting->id && stored->revisionV2 == (previous ? std::optional<RevisionId>(previous->id) : std::nullopt) && (previous || isZero(stored->head)),
            "branch changed during v2 commit");
    publishStoredBranch(this->directory_, StoredBranch{stored->id, stored->name,
                            updated->recordHash, newId, stored->originBranchId, stored->forkRevisionId}, false);
    return newId;
}

std::vector<Key> Repository::readReferrers(const Hash& root, const Key& target) {
    const std::optional<Hash> indexValue = lookup(root, target);
    if (!indexValue) return {};
    return decodeReferrers(load(*indexValue));
}

Revision Repository::commitModel(std::string_view branch, std::vector<ModelChange> changes,
                                 std::optional<Hash> expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return commitModelLocked(branch, std::move(changes), expectedHead, std::nullopt);
}

RevisionId Repository::commitModelV2(const ProjectId& project, std::string_view branch,
                                      std::vector<ModelChange> changes,
                                      std::optional<RevisionId> expectedHead) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const std::optional<StoredBranch> starting = findStoredBranch(this->directory_, branch);
    require(Err::NotFound, starting.has_value(), "branch not found");
    const bool unborn = !starting->revisionV2 && isZero(starting->head);
    const std::optional<RevisionRecordV2> previous = unborn ? std::nullopt :
        std::optional<RevisionRecordV2>(branchRevisionV2Locked(project, branch));
    require(Err::Conflict, !expectedHead || (previous && previous->id == *expectedHead),
            "branch HEAD changed since model edit was prepared");
    require(Err::InvalidInput, !previous || previous->modelIndexed || isZero(previous->root),
            "cannot enable model indexing on a populated raw branch");
    const PreparedModelCommit prepared = prepareModelCommitLocked(
        previous ? previous->root : Hash{}, previous ? previous->referencesRoot : Hash{}, std::move(changes));
    const Hash newRoot = applyEncodedChangesLocked(previous ? previous->root : Hash{}, prepared.encoded);
    const RevisionId revisionId = writeRevisionV2Locked(
        project, newRoot, prepared.referencesRoot, true, previous ? std::vector<RevisionId>{previous->id} : std::vector<RevisionId>{});
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> saved = resolver.resolve(revisionId, reader);
    require(Err::Internal, saved.has_value(), "model revision did not resolve after write");
    const std::optional<StoredBranch> branchRecord = findStoredBranch(this->directory_, branch);
    require(Err::Internal, branchRecord && branchRecord->id == starting->id &&
            branchRecord->revisionV2 == (previous ? std::optional<RevisionId>(previous->id) : std::nullopt) &&
            (previous || isZero(branchRecord->head)),
            "model branch HEAD changed during commit");
    publishStoredBranch(this->directory_, StoredBranch{
        branchRecord->id, branchRecord->name, saved->recordHash, revisionId,
        branchRecord->originBranchId, branchRecord->forkRevisionId}, false);
    return revisionId;
}

std::optional<ModelRecord> Repository::getModelAtRevisionV2(const ProjectId& project,
                                                const RevisionId& revisionId, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const std::filesystem::path namespacePath = this->directory_ / "revision-v2";
    if (!std::filesystem::exists(namespacePath) && !std::filesystem::is_symlink(namespacePath))
        return std::nullopt;
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> record = resolver.resolve(revisionId, reader);
    require(Err::NotFound, record.has_value(), "revision UUID not found");
    require(Err::InvalidInput, record->record.modelIndexed,
            "model records require a model-indexed revision");
    const std::optional<Hash> value = lookup(record->record.root, key);
    if (!value) return std::nullopt;
    const Bytes stored = load(*value);
    require(Err::Corruption, !stored.empty() && stored.front() == 'V',
            "invalid model object envelope");
    return decodeModelRecord(Bytes(stored.begin() + 1, stored.end()));
}

std::vector<Key> Repository::referencingModelAtRevisionV2(const ProjectId& project,
                                                         const RevisionId& revisionId, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const std::filesystem::path namespacePath = this->directory_ / "revision-v2";
    if (!std::filesystem::exists(namespacePath) && !std::filesystem::is_symlink(namespacePath))
        throw StorageError(StorageErrorKind::NotFound, "revision UUID not found");
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> snapshot = resolver.resolve(revisionId, reader);
    require(Err::NotFound, snapshot.has_value(), "revision UUID not found");
    require(Err::InvalidInput, snapshot->record.modelIndexed,
            "reverse references require a model-indexed revision");
    return readReferrers(snapshot->record.referencesRoot, key);
}

Repository::PreparedModelCommit Repository::prepareModelCommitLocked(
        const Hash& root, const Hash& referencesRoot, std::vector<ModelChange> changes) {
    // Canonicalize the owned change list in place. Large initial imports no
    // longer allocate another full model payload for an ordered map copy.
    std::sort(changes.begin(), changes.end(),
        [](const ModelChange& left, const ModelChange& right) { return left.key < right.key; });
    for (std::size_t i = 0; i < changes.size(); ++i) {
        ModelChange& change = changes[i];
        require(Err::InvalidInput, change.key != Key{}, "null model UUID is not allowed");
        require(Err::InvalidInput, i == 0 || changes[i - 1].key != change.key,
                "duplicate model UUID in commit");
        if (change.record) {
            ModelRecord& canonical = *change.record;
            require(Err::InvalidInput, canonical.kind != 0, "model kind must be nonzero");
            std::sort(canonical.references.begin(), canonical.references.end());
            canonical.references.erase(std::unique(canonical.references.begin(),
                        canonical.references.end()), canonical.references.end());
        }
    }

    // Evaluate old and new outgoing edges without traversing unrelated model
    // objects. An update only touches its own incident reverse-index entries.
    std::map<Key, std::set<Key>> removedEdges;
    std::map<Key, std::set<Key>> addedEdges;
    for (const ModelChange& item : changes) {
        const std::optional<Hash> existing = lookup(root, item.key);
        std::vector<Key> oldReferences;
        if (existing) {
            const Bytes stored = load(*existing);
            require(Err::Corruption, !stored.empty() && stored.front() == 'V', "invalid model value object");
            const ModelRecord old = decodeModelRecord(Bytes(stored.begin() + 1, stored.end()));
            oldReferences = old.references;
        }
        const std::vector<Key> newReferences = item.record ? item.record->references : std::vector<Key>{};
        for (const Key& referenced : oldReferences) {
            if (!std::binary_search(newReferences.begin(), newReferences.end(), referenced)) {
                removedEdges[referenced].insert(item.key);
            }
        }
        for (const Key& referenced : newReferences) {
            require(Err::InvalidInput, referenced != Key{} && referenced != item.key,
                    "invalid model UUID reference");
            if (!std::binary_search(oldReferences.begin(), oldReferences.end(), referenced)) {
                addedEdges[referenced].insert(item.key);
            }
            const std::vector<ModelChange>::const_iterator pending =
                std::lower_bound(changes.begin(), changes.end(), referenced,
                    [](const ModelChange& candidate, const Key& key) {
                        return candidate.key < key;
                    });
            if (pending != changes.end() && pending->key == referenced) {
                require(Err::InvalidInput, pending->record.has_value(),
                        "model reference target deleted by same commit");
            } else {
                const std::optional<Hash> destination = lookup(root, referenced);
                require(Err::InvalidInput, destination.has_value(), "model reference target does not exist");
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
        const std::vector<Key> original = readReferrers(referencesRoot, target);
        std::set<Key> inbound(original.begin(), original.end());
        const std::map<Key, std::set<Key>>::const_iterator remove = removedEdges.find(target);
        if (remove != removedEdges.end()) {
            for (const Key& source : remove->second) inbound.erase(source);
        }
        const std::map<Key, std::set<Key>>::const_iterator add = addedEdges.find(target);
        if (add != addedEdges.end()) inbound.insert(add->second.begin(), add->second.end());
        nextIncoming.emplace_back(target, std::move(inbound));
    }
    for (const ModelChange& item : changes) {
        if (item.record) continue;
        const std::vector<std::pair<Key, std::set<Key>>>::const_iterator changed =
            std::lower_bound(nextIncoming.begin(), nextIncoming.end(), item.key,
                [](const std::pair<Key, std::set<Key>>& candidate, const Key& key) {
                    return candidate.first < key;
                });
        if (changed != nextIncoming.end() && changed->first == item.key) {
            require(Err::InvalidInput, changed->second.empty(), "cannot delete referenced model UUID");
        } else {
            require(Err::InvalidInput, readReferrers(referencesRoot, item.key).empty(),
                    "cannot delete referenced model UUID");
        }
    }

    // Preflight serialization and size limits before writing ANY pack data.
    // Invalid requests must not leave even unreachable index objects behind.
    std::vector<Change> encoded;
    encoded.reserve(changes.size());
    for (ModelChange& item : changes) {
        if (item.record) {
            Bytes value = encodeModelRecord(*item.record);
            require(Err::LimitExceeded, value.size() < maxObjectBytes, "encoded model value exceeds pack limit");
            encoded.push_back(Change{item.key, std::move(value)});
            // All validation has finished. Release each canonical source
            // payload as soon as its encoded representation is available.
            item.record.reset();
        } else {
            encoded.push_back(Change{item.key, std::nullopt});
        }
    }
    std::vector<std::pair<Key, std::optional<Bytes>>> pendingIndex;
    pendingIndex.reserve(nextIncoming.size());
    for (const std::pair<Key, std::set<Key>>& item : nextIncoming) {
        if (item.second.empty()) {
            pendingIndex.emplace_back(item.first, std::nullopt);
        } else {
            Bytes value = encodeReferrers(item.second);
            require(Err::LimitExceeded, value.size() <= maxObjectBytes, "incoming model index exceeds pack limit");
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
    const Hash updatedReferences = applyTree(referencesRoot, indexUpdates,
                                              0, indexUpdates.size(), 0);
    return PreparedModelCommit{std::move(encoded), updatedReferences};
}

Revision Repository::commitModelLocked(std::string_view branch, std::vector<ModelChange> changes,
                                       std::optional<Hash> expectedHead,
                                       std::optional<Hash> mergeParent) {
    const Revision previous = readRevision(branchHead(branch));
    require(Err::Conflict, !expectedHead || previous.id == *expectedHead,
            "branch head changed since the model update was prepared");
    require(Err::InvalidInput, previous.modelIndexed || isZero(previous.root),
            "cannot commit model records to a populated raw branch");

    const PreparedModelCommit prepared = prepareModelCommitLocked(
        previous.root, previous.referencesRoot, std::move(changes));
    return commitLocked(branch, prepared.encoded, prepared.referencesRoot, mergeParent);
}

std::vector<Key> Repository::referencingModel(std::string_view branch, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision revision = readRevision(branchHead(branch));
    require(Err::InvalidInput, revision.modelIndexed, "reverse references are unavailable on raw branch");
    return readReferrers(revision.referencesRoot, key);
}

std::optional<Bytes> Repository::getAtRevision(const Hash& revisionId, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision snapshot = clientRevision(revisionId);
    const std::optional<Hash> object = lookup(snapshot.root, key);
    if (!object) return std::nullopt;
    const Bytes encoded = load(*object);
    require(Err::Corruption, !encoded.empty() && encoded.front() == 'V', "invalid value object");
    return Bytes(encoded.begin() + 1, encoded.end());
}

std::optional<ModelRecord> Repository::getModelAtRevision(const Hash& revisionId, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision snapshot = clientRevision(revisionId);
    require(Err::InvalidInput, snapshot.modelIndexed, "model records require a model-indexed revision");
    const std::optional<Hash> object = lookup(snapshot.root, key);
    if (!object) return std::nullopt;
    const Bytes encoded = load(*object);
    require(Err::Corruption, !encoded.empty() && encoded.front() == 'V', "invalid value object");
    return decodeModelRecord(Bytes(encoded.begin() + 1, encoded.end()));
}

std::vector<Key> Repository::referencingModelAtRevision(const Hash& revisionId, const Key& key) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision snapshot = clientRevision(revisionId);
    require(Err::InvalidInput, snapshot.modelIndexed, "reverse references are unavailable on raw revision");
    return readReferrers(snapshot.referencesRoot, key);
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
    require(Err::Corruption, !encoded.empty() && encoded.front() == 'V', "invalid value object");
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
        require(Err::Corruption, !encoded.empty() && encoded.front() == 'V', "invalid value object");
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

std::vector<Difference> Repository::diffRevisions(const Hash& fromRevision,
                                                  const Hash& toRevision) {
    return diffRevisions(fromRevision, toRevision, std::numeric_limits<std::size_t>::max());
}

std::vector<Difference> Repository::diffRevisions(const Hash& fromRevision, const Hash& toRevision,
                                                  std::size_t maxDifferences) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    const Revision before = clientRevision(fromRevision);
    const Revision after = clientRevision(toRevision);
    std::vector<Difference> result;
    diffHashes(before.root, after.root, result, maxDifferences);
    std::sort(result.begin(), result.end(), [](const Difference& first, const Difference& second) {
        return first.key < second.key;
    });
    return result;
}


std::vector<Difference> Repository::diffRevisionsV2(const ProjectId& project,
        const RevisionId& fromRevision, const RevisionId& toRevision,
        std::size_t maxDifferences) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& digest) -> std::optional<Bytes> {
        if (!containsIndex(digest)) return std::nullopt;
        return load(digest);
    };
    const std::optional<ResolvedRevisionV2> from = resolver.resolve(fromRevision, reader);
    const std::optional<ResolvedRevisionV2> to = resolver.resolve(toRevision, reader);
    require(Err::NotFound, from && to, "UUIDv7 diff revision not found in project");
    std::vector<Difference> result;
    diffHashes(from->record.root, to->record.root, result, maxDifferences);
    std::sort(result.begin(), result.end(), [](const Difference& first, const Difference& second) {
        return first.key < second.key;
    });
    return result;
}

namespace {
// Before v2 HEAD and retention cutover, old SHA-only reachability cannot
// prove an indexed v2 revision survives compaction or is logically valid.
// Audit the published UUIDv7 namespace independently of branch refs. This is
// intentionally conservative: every published binding is immutable and must
// survive pack compaction, even if not yet reachable from a branch HEAD (e.g.
// crash after publishing the binding but before publishing a ref).
struct V2Audit {
    std::map<Hash, Bytes> logicalRevisions;
    std::map<Hash, RevisionId> publishedIds;
    std::vector<Hash> recordedHashes;
};

std::optional<ProjectId> persistedV2Project(const std::filesystem::path& directory) {
    const std::filesystem::path namespacePath = directory / "revision-v2";
    if (!std::filesystem::exists(namespacePath)) return std::nullopt;
    require(Err::Corruption, !std::filesystem::is_symlink(namespacePath) &&
            std::filesystem::is_directory(namespacePath), "unsafe v2 revision directory");
    const std::filesystem::path manifest = namespacePath / "project";
    const int fd = ::open(manifest.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        require(Err::Corruption, !std::filesystem::exists(namespacePath / "ids"),
                "missing v2 project identity manifest");
        return std::nullopt;
    }
    if (fd < 0) throwSystemError("open v2 project identity manifest");
    std::array<std::uint8_t, 20> bytes{};
    struct stat st{};
    const bool valid = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 20;
    if (valid) {
        try { readExact(fd, bytes.data(), bytes.size(), 0); }
        catch (...) { (void)::close(fd); throw; }
    }
    (void)::close(fd);
    require(Err::Corruption, valid && bytes[0] == 'P' && bytes[1] == 'R' &&
            bytes[2] == 'J' && bytes[3] == 2, "invalid v2 project identity manifest");
    UuidV7 uuid{};
    std::copy_n(bytes.begin() + 4, uuid.bytes.size(), uuid.bytes.begin());
    const std::optional<ProjectId> project = ProjectId::parse(uuid.toString());
    require(Err::Corruption, project.has_value(), "invalid v2 project UUID");
    return project;
}

V2Audit auditPublishedV2(const std::filesystem::path& directory,
                         const RevisionObjectReaderV2& readObject) {
    V2Audit result{};
    const std::optional<ProjectId> project = persistedV2Project(directory);
    if (!project) return result;
    RevisionResolverV2 resolver(directory, *project);
    const std::filesystem::path identities = directory / "revision-v2" / "ids";
    require(Err::Corruption, !std::filesystem::is_symlink(identities) &&
            std::filesystem::is_directory(identities), "invalid v2 identities directory");
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(identities)) {
        const std::string filename = entry.path().filename().string();
        if (filename.starts_with(".stage-")) {
            const std::optional<RevisionId> staged = RevisionId::parse(filename.substr(7));
            require(Err::Corruption, staged.has_value() &&
                    staged->toString() == filename.substr(7) &&
                    entry.is_regular_file() && !entry.is_symlink(),
                    "unsafe v2 staging entry");
            continue; // abandoned private staging file, never published
        }
        require(Err::Corruption, entry.is_regular_file() && !entry.is_symlink(),
                "unsafe published v2 identity entry");
        const std::optional<RevisionId> id = RevisionId::parse(filename);
        require(Err::Corruption, id.has_value() && id->toString() == filename,
                "non-UUIDv7 revision identity filename");
        const std::optional<ResolvedRevisionV2> resolved = resolver.resolveDirect(*id, readObject);
        require(Err::Corruption, resolved.has_value(), "missing published v2 revision");
        // Authenticate the real packed object BEFORE exposing a logical view
        // to the common Merkle/model verifier. Parent digests come only from
        // the canonical record and were checked against UUID bindings above.
        const RevisionRecordV2& record = resolved->record;
        Bytes logical(67 + 32 * record.parents.size(), 0);
        logical[0] = 'S';
        std::copy(record.root.begin(), record.root.end(), logical.begin() + 1);
        std::copy(record.referencesRoot.begin(), record.referencesRoot.end(), logical.begin() + 33);
        logical[65] = record.modelIndexed ? 1 : 0;
        logical[66] = static_cast<std::uint8_t>(record.parents.size());
        for (std::size_t index = 0; index < record.parents.size(); ++index)
            std::copy(record.parents[index].expectedRecordHash.begin(),
                      record.parents[index].expectedRecordHash.end(),
                      logical.begin() + static_cast<std::ptrdiff_t>(67 + 32 * index));
        require(Err::Corruption, result.logicalRevisions.emplace(resolved->recordHash,
                                                                  std::move(logical)).second,
                "multiple revision UUIDs resolve to the same record hash");
        result.publishedIds.emplace(resolved->recordHash, record.id);
        result.recordedHashes.push_back(resolved->recordHash);
    }
    return result;
}
} // namespace

void Repository::verifyPack() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    verifyPackLocked();
}

void Repository::verifyPackLocked() {
    require(Err::Unavailable, !this->needsReopen_, "repository must be reopened after interrupted compaction");
    refreshIndex();
    std::uint64_t position = 0;
    std::uint64_t checked = 0;
    while (position < this->scanOffset_) {
        require(Err::Corruption, this->scanOffset_ - position >= headerSize, "truncated pack header");
        std::array<std::uint8_t, headerSize> header{};
        readExact(this->packFd_, header.data(), header.size(), position);
        require(Err::Corruption, header[0] == 'A' && header[1] == 'O' && header[2] == 'B' &&
                header[3] == 'J' && header[4] == 1, "corrupt pack record header");
        Hash expected{};
        std::copy_n(header.begin() + 5, 32, expected.begin());
        const std::uint64_t length = readU64(header.data() + 37);
        require(Err::Corruption, this->scanOffset_ - position >= headerSize &&
                length <= maxObjectBytes && length <= this->scanOffset_ - position - headerSize,
                "invalid content pack object boundary");
        Bytes payload(static_cast<std::size_t>(length));
        if (!payload.empty()) {
            readExact(this->packFd_, payload.data(), payload.size(), position + headerSize);
        }
        require(Err::Corruption, sha256(payload) == expected, "content pack checksum mismatch");
        const std::optional<Location> entry = indexLocation(expected);
        require(Err::Corruption, entry.has_value() && entry->offset == position + headerSize &&
                entry->size == length, "index differs from content pack");
        ++checked;
        position += headerSize + length;
    }
    require(Err::Corruption, position == this->scanOffset_ && checked == this->indexCount_,
            "pack verification found inconsistent object count");
}

VerificationReport Repository::verifyRepository() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    // Verify all physical objects, including unreachable ones, first.
    // Neither stage mutates branch heads or rewrites invalid object graphs.
    verifyPackLocked();
    require(Err::Unavailable, !this->needsReopen_, "repository must be reopened after interrupted compaction");
    refreshIndex();

    const RevisionObjectReaderV2 loadV2 = [this](const Hash& hash) -> std::optional<Bytes> {
        if (!containsIndex(hash)) return std::nullopt;
        return load(hash);
    };
    const V2Audit v2 = auditPublishedV2(this->directory_, loadV2);
    std::vector<Hash> tips = v2.recordedHashes;
    const std::vector<StoredBranch> branches = readStoredBranches(this->directory_);
    for (const StoredBranch& branch : branches) {
        if (branch.revisionV2) {
            const std::map<Hash, RevisionId>::const_iterator published =
                v2.publishedIds.find(branch.head);
            require(Err::Corruption, published != v2.publishedIds.end() &&
                    published->second == *branch.revisionV2,
                    "v2 branch HEAD UUID/SHA does not match the published revision");
        }
        if (!isZero(branch.head)) tips.push_back(branch.head);
    }
    const std::function<Bytes(const Hash&)> loadObject =
        [this, &v2](const Hash& hash) {
            const std::map<Hash, Bytes>::const_iterator logical = v2.logicalRevisions.find(hash);
            return logical == v2.logicalRevisions.end() ? load(hash) : logical->second;
        };
    LogicalVerification graph{};
    try {
        graph = verifyLogicalGraph(tips, loadObject, this->scratch_);
    } catch (...) {
        rethrowWithContext("logical repository verification failed: ");
    }
    VerificationReport result{};
    result.branches = static_cast<std::uint64_t>(branches.size());
    result.revisions = graph.revisionCount;
    result.reachableObjects = graph.reachableObjects;
    result.treeNodes = graph.treeNodes;
    result.modelSnapshots = graph.modelSnapshots;
    result.physicalObjects = static_cast<std::uint64_t>(this->indexCount_);
    return result;
}

std::uint64_t Repository::objectCount() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return static_cast<std::uint64_t>(this->indexCount_);
}

std::uint64_t Repository::packSize() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    return this->scanOffset_;
}

// UUIDv7 revision bundles carry immutable content and stable object identity,
// never host pack/index offsets. Legacy full/delta: AOWREV21/AOWREV22.
// Provenance-aware full/delta: AOWREV23/AOWREV24 (32 extra header bytes).
// Both use a caller-owned regular file as the on-disk payload stage. Per-object
// indexes, visited hashes and verification marks live in repository-local
// scratch files, avoiding payload-sized heap maps and a 64 MiB RAM ceiling.
namespace {
constexpr std::array<std::uint8_t, 8> revisionBundleMagicV2{
    'A', 'O', 'W', 'R', 'E', 'V', '2', '1'
};
constexpr std::array<std::uint8_t, 8> revisionDeltaMagicV2{
    'A', 'O', 'W', 'R', 'E', 'V', '2', '2'
};
constexpr std::array<std::uint8_t, 8> revisionBundleProvenanceMagicV2{
    'A', 'O', 'W', 'R', 'E', 'V', '2', '3'
};
constexpr std::array<std::uint8_t, 8> revisionDeltaProvenanceMagicV2{
    'A', 'O', 'W', 'R', 'E', 'V', '2', '4'
};
constexpr std::uint64_t revisionBundleHeaderV2 = 96;
constexpr std::uint64_t revisionDeltaHeaderV2 = 144;
constexpr std::uint64_t revisionBundleProvenanceHeaderV2 = 128;
constexpr std::uint64_t revisionDeltaProvenanceHeaderV2 = 176;
// Bound file/CPU/disk work even when an offline caller leaves BundleLimits
// unspecified. Operators can impose lower limits through BundleLimits.
constexpr std::uint64_t revisionBundleMaxBytesV2 = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t revisionBundleMaxObjectsV2 = 1000000;

Bytes logicalRevisionFromV2(const Bytes& canonical, const ProjectId& project) {
    const RevisionRecordV2 record = decodeRevisionRecordV2(canonical);
    require(Err::Corruption, record.project == project,
            "v2 bundle revision belongs to a different project");
    Bytes logical(67 + 32 * record.parents.size(), 0);
    logical[0] = 'S';
    std::copy(record.root.begin(), record.root.end(), logical.begin() + 1);
    std::copy(record.referencesRoot.begin(), record.referencesRoot.end(), logical.begin() + 33);
    logical[65] = record.modelIndexed ? 1 : 0;
    logical[66] = static_cast<std::uint8_t>(record.parents.size());
    for (std::size_t index = 0; index < record.parents.size(); ++index) {
        std::copy(record.parents[index].expectedRecordHash.begin(),
                  record.parents[index].expectedRecordHash.end(),
                  logical.begin() + static_cast<std::ptrdiff_t>(67 + index * 32));
    }
    return logical;
}

void checkRevisionBundleLimitV2(std::uint64_t size, std::uint64_t objects,
                                const BundleLimits& limits) {
    require(Err::LimitExceeded, size <= revisionBundleMaxBytesV2 &&
            (limits.maxBytes == 0 || size <= limits.maxBytes),
            "v2 replication bundle byte budget exceeded");
    require(Err::LimitExceeded, objects <= revisionBundleMaxObjectsV2 &&
            (limits.maxObjects == 0 || objects <= limits.maxObjects),
            "v2 replication bundle object budget exceeded");
}
} // namespace

void Repository::exportRevisionBundleV2(const ProjectId& project, std::string_view branch,
                                        int fd, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportRevisionBundleV2Locked(project, branch, std::nullopt, fd, limits);
}

void Repository::exportIncrementalRevisionBundleV2(const ProjectId& project,
        std::string_view branch, const RevisionId& recipientBase, int fd, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportRevisionBundleV2Locked(project, branch, recipientBase, fd, limits);
}

void Repository::exportRevisionBundleV2Locked(const ProjectId& project,
        std::string_view branch, std::optional<RevisionId> base, int fd,
        BundleLimits limits) {
    struct stat destination{};
    systemCall(fd >= 0 && ::fstat(fd, &destination) == 0, "stat v2 bundle output");
    require(Err::InvalidInput, S_ISREG(destination.st_mode) && destination.st_size == 0,
            "v2 bundle export requires an empty regular file");
    const std::optional<StoredBranch> ref = findStoredBranch(this->directory_, branch);
    require(Err::NotFound, ref.has_value(), "v2 bundle branch was not found");
    require(Err::Unavailable, ref->revisionV2.has_value(),
            "v2 replication requires UUIDv7 branch HEAD");
    const RevisionRecordV2 tip = branchRevisionV2Locked(project, branch);
    const Hash tipHash = hashRevisionRecordV2(tip);
    require(Err::Corruption, tipHash == ref->head && tip.id == *ref->revisionV2,
            "v2 bundle HEAD UUID/SHA mismatch");
    RevisionResolverV2 resolver(this->directory_, project);
    const RevisionObjectReaderV2 reader = [this](const Hash& hash) -> std::optional<Bytes> {
        if (!containsIndex(hash)) return std::nullopt;
        return load(hash);
    };
    Hash baseHash{};
    if (base) {
        const std::optional<ResolvedRevisionV2> resolved = resolver.resolve(*base, reader);
        require(Err::NotFound, resolved.has_value(), "incremental recipient base is not published");
        baseHash = resolved->recordHash;
    }
    enum class Kind { Revision, Tree, Value };
    // Read and authenticate the whole base graph; it becomes the exclusion
    // set for the delta, but no raw payload is retained in process memory.
    DiskBundleHashes baseObjects(this->scratch_);
    const auto collect = [&](const Hash& root, DiskBundleHashes& visited,
                             const DiskBundleHashes* excluded,
                             bool& reachedBase) {
        std::vector<std::pair<Hash, Kind>> pending{{root, Kind::Revision}};
        while (!pending.empty()) {
            const std::pair<Hash, Kind> next = pending.back();
            pending.pop_back();
            if (isZero(next.first)) continue;
            if (base && next.second == Kind::Revision && next.first == baseHash)
                reachedBase = true;
            if ((excluded && excluded->contains(next.first)) ||
                !visited.insert(next.first, revisionBundleMaxObjectsV2)) continue;
            const Bytes bytes = load(next.first);
            require(Err::Corruption, sha256(bytes) == next.first,
                    "v2 export pack object checksum mismatch");
            require(Err::LimitExceeded, limits.maxObjectBytes == 0 ||
                    bytes.size() <= limits.maxObjectBytes,
                    "v2 bundle object is larger than configured limit");
            if (next.second == Kind::Revision) {
                const RevisionRecordV2 record = decodeRevisionRecordV2(bytes);
                require(Err::Corruption, record.project == project,
                        "v2 export revision belongs to another project");
                const std::optional<ResolvedRevisionV2> published =
                    resolver.resolveDirect(record.id, reader);
                require(Err::Corruption, published && published->recordHash == next.first,
                        "v2 export contains inconsistent revision binding");
                pending.emplace_back(record.root, Kind::Tree);
                pending.emplace_back(record.referencesRoot, Kind::Tree);
                for (const RevisionParentV2& parent : record.parents)
                    pending.emplace_back(parent.expectedRecordHash, Kind::Revision);
            } else if (next.second == Kind::Tree) {
                const TreeNode node = readNode(next.first);
                if (node.leaf) pending.emplace_back(node.value, Kind::Value);
                else for (const Hash& child : node.children)
                    pending.emplace_back(child, Kind::Tree);
            }
        }
    };
    bool ignoredBase = false;
    if (base) collect(baseHash, baseObjects, nullptr, ignoredBase);
    DiskBundleHashes objects(this->scratch_);
    bool reachedBase = false;
    collect(tipHash, objects, base ? &baseObjects : nullptr, reachedBase);
    require(Err::Conflict, !base || reachedBase || tipHash == baseHash,
            "incremental recipient base is not an ancestor of source HEAD");
    SortedBundleHashes ordered(this->scratch_);
    objects.forEach([&](const Hash& hash) { ordered.append(hash); });
    ordered.finalize();
    require(Err::Corruption, !ref->forkRevisionId || ref->originBranchId,
            "branch fork revision has no origin branch identity");
    const bool hasProvenance = ref->originBranchId.has_value() || ref->forkRevisionId.has_value();
    const std::uint64_t headerSize = hasProvenance
        ? (base ? revisionDeltaProvenanceHeaderV2 : revisionBundleProvenanceHeaderV2)
        : (base ? revisionDeltaHeaderV2 : revisionBundleHeaderV2);
    std::uint64_t total = headerSize;
    // Preflight byte limits before exposing any bundle content.
    while (const std::optional<Hash> hash = ordered.next()) {
        const Bytes bytes = load(*hash);
        require(Err::LimitExceeded, bytes.size() <= maxObjectBytes &&
                (limits.maxObjectBytes == 0 || bytes.size() <= limits.maxObjectBytes),
                "v2 export object exceeds configured size limit");
        require(Err::LimitExceeded, total <= revisionBundleMaxBytesV2 &&
                bytes.size() + bundleEntryBytes <= revisionBundleMaxBytesV2 - total,
                "v2 export bundle exceeds hard size limit");
        total += bundleEntryBytes + bytes.size();
        checkRevisionBundleLimitV2(total, objects.size(), limits);
    }
    // The verifier checks canonical trie/model/reference semantics of the
    // full source graph. It can load from the pack without a payload map.
    const std::function<Bytes(const Hash&)> verifiedObject = [&](const Hash& hash) {
        const Bytes bytes = load(hash);
        if (bytes.size() >= 2 && bytes[0] == 'T' && bytes[1] == 2)
            return logicalRevisionFromV2(bytes, project);
        return bytes;
    };
    (void)verifyLogicalGraph({tipHash}, verifiedObject, this->scratch_);
    // Reset the sorter: its iterator is one-shot. Rebuild from the private
    // hash table, which remains disk-resident, and stream into caller's file.
    SortedBundleHashes sorted(this->scratch_);
    objects.forEach([&](const Hash& hash) { sorted.append(hash); });
    sorted.finalize();
    Bytes header(static_cast<std::size_t>(headerSize), 0);
    const std::array<std::uint8_t, 8>& magic = hasProvenance
        ? (base ? revisionDeltaProvenanceMagicV2 : revisionBundleProvenanceMagicV2)
        : (base ? revisionDeltaMagicV2 : revisionBundleMagicV2);
    std::copy(magic.begin(), magic.end(), header.begin());
    std::copy(project.uuid().bytes.begin(), project.uuid().bytes.end(), header.begin() + 8);
    std::copy(ref->id.uuid().bytes.begin(), ref->id.uuid().bytes.end(), header.begin() + 24);
    std::copy(tip.id.uuid().bytes.begin(), tip.id.uuid().bytes.end(), header.begin() + 40);
    std::copy(tipHash.begin(), tipHash.end(), header.begin() + 56);
    storeU64(header.data() + 88, objects.size());
    if (base) {
        std::copy(base->uuid().bytes.begin(), base->uuid().bytes.end(), header.begin() + 96);
        std::copy(baseHash.begin(), baseHash.end(), header.begin() + 112);
    }
    if (hasProvenance) {
        const std::size_t at = base ? revisionDeltaHeaderV2 : revisionBundleHeaderV2;
        if (ref->originBranchId) {
            std::copy(ref->originBranchId->uuid().bytes.begin(),
                      ref->originBranchId->uuid().bytes.end(), header.begin() + at);
        }
        if (ref->forkRevisionId) {
            std::copy(ref->forkRevisionId->uuid().bytes.begin(),
                      ref->forkRevisionId->uuid().bytes.end(), header.begin() + at + 16);
        }
    }
    try {
        writeExact(fd, header.data(), header.size(), 0);
        std::uint64_t offset = headerSize;
        while (const std::optional<Hash> hash = sorted.next()) {
            const Bytes bytes = load(*hash);
            std::array<std::uint8_t, bundleEntryBytes> entry{};
            std::copy(hash->begin(), hash->end(), entry.begin());
            storeU64(entry.data() + 32, bytes.size());
            writeExact(fd, entry.data(), entry.size(), offset);
            offset += entry.size();
            if (!bytes.empty()) {
                writeExact(fd, bytes.data(), bytes.size(), offset);
                offset += bytes.size();
            }
        }
        systemCall(::fsync(fd) == 0, "sync UUIDv7 revision bundle");
    } catch (...) {
        (void)::ftruncate(fd, 0);
        throw;
    }
}

RevisionBundleImportV2 Repository::importRevisionBundleV2(const ProjectId& expectedProject,
        int fd, std::string_view localBranch, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(localBranch);
    struct stat source{};
    systemCall(fd >= 0 && ::fstat(fd, &source) == 0, "stat v2 revision bundle");
    require(Err::InvalidInput, S_ISREG(source.st_mode) && source.st_size >= 0 &&
            static_cast<std::uint64_t>(source.st_size) >= revisionBundleHeaderV2,
            "invalid v2 replication bundle file");
    const std::uint64_t size = static_cast<std::uint64_t>(source.st_size);
    checkRevisionBundleLimitV2(size, 0, limits);
    std::array<std::uint8_t, 8> magic{};
    readExact(fd, magic.data(), magic.size(), 0);
    const bool incremental = magic == revisionDeltaMagicV2 ||
                             magic == revisionDeltaProvenanceMagicV2;
    const bool hasProvenance = magic == revisionBundleProvenanceMagicV2 ||
                               magic == revisionDeltaProvenanceMagicV2;
    require(Err::InvalidInput, incremental || hasProvenance || magic == revisionBundleMagicV2,
            "unsupported UUIDv7 revision replication format");
    const std::uint64_t headerSize = hasProvenance
        ? (incremental ? revisionDeltaProvenanceHeaderV2 : revisionBundleProvenanceHeaderV2)
        : (incremental ? revisionDeltaHeaderV2 : revisionBundleHeaderV2);
    require(Err::InvalidInput, size >= headerSize, "truncated v2 replication header");
    Bytes header(static_cast<std::size_t>(headerSize));
    readExact(fd, header.data(), header.size(), 0);
    const auto parseUuid = [&](std::size_t at) -> std::string {
        std::array<std::uint8_t, 16> uuid{};
        std::copy_n(header.begin() + static_cast<std::ptrdiff_t>(at), uuid.size(), uuid.begin());
        return UuidV7{uuid}.toString();
    };
    const std::optional<ProjectId> project = ProjectId::parse(parseUuid(8));
    const std::optional<BranchId> branchId = BranchId::parse(parseUuid(24));
    const std::optional<RevisionId> incoming = RevisionId::parse(parseUuid(40));
    require(Err::InvalidInput, project && branchId && incoming,
            "bundle has invalid UUIDv7 identity");
    require(Err::Conflict, *project == expectedProject,
            "v2 bundle project does not match explicitly expected project UUID");
    std::optional<BranchId> originBranchId;
    std::optional<RevisionId> forkRevisionId;
    if (hasProvenance) {
        const std::size_t at = incremental ? revisionDeltaHeaderV2 : revisionBundleHeaderV2;
        const auto isNullUuid = [&](std::size_t offset) {
            return std::all_of(header.begin() + static_cast<std::ptrdiff_t>(offset),
                               header.begin() + static_cast<std::ptrdiff_t>(offset + 16),
                               [](std::uint8_t value) { return value == 0; });
        };
        if (!isNullUuid(at)) originBranchId = BranchId::parse(parseUuid(at));
        if (!isNullUuid(at + 16)) forkRevisionId = RevisionId::parse(parseUuid(at + 16));
        require(Err::InvalidInput, originBranchId.has_value() &&
                (!forkRevisionId || originBranchId),
                "invalid UUIDv7 branch provenance in replication bundle");
    }
    Hash tipHash{};
    std::copy_n(header.begin() + 56, 32, tipHash.begin());
    const std::uint64_t count = readU64(header.data() + 88);
    require(Err::InvalidInput, tipHash != Hash{} && (incremental || count > 0) &&
            count <= (size - headerSize) / bundleEntryBytes,
            "invalid v2 bundle tip digest or count");
    checkRevisionBundleLimitV2(size, count, limits);
    const std::optional<ProjectId> existingProject = persistedV2Project(this->directory_);
    require(Err::Conflict, !existingProject || *existingProject == *project,
            "cannot import revisions into a different project identity");
    const std::vector<StoredBranch> branches = readStoredBranches(this->directory_);
    const StoredBranch* existing = nullptr;
    for (const StoredBranch& branch : branches) {
        require(Err::Unavailable, branch.revisionV2.has_value(),
                "v2 replication cannot mix SHA-based branch HEADs");
        require(Err::Conflict, branch.id != *branchId || branch.name == localBranch,
                "incoming branch UUID is assigned to a different name");
        if (branch.name == localBranch) existing = &branch;
    }
    require(Err::Conflict, !existing || existing->id == *branchId,
            "v2 replication would rebind branch UUID");
    if (existing && hasProvenance) {
        require(Err::Conflict, existing->originBranchId == originBranchId &&
                existing->forkRevisionId == forkRevisionId,
                "v2 replication branch fork provenance conflicts with local branch");
    }
    std::optional<RevisionId> base;
    Hash baseHash{};
    if (incremental) {
        base = RevisionId::parse(parseUuid(96));
        require(Err::InvalidInput, base.has_value(), "invalid incremental base UUID");
        std::copy_n(header.begin() + 112, 32, baseHash.begin());
        require(Err::InvalidInput, baseHash != Hash{}, "invalid incremental base digest");
        // A delta is valid for exactly one recipient HEAD. Allow a same-tip
        // replay, but never apply a stale or forked delta to another HEAD.
        require(Err::Conflict, existing && existing->revisionV2 &&
                ((existing->revisionV2 == base && existing->head == baseHash) ||
                 (existing->revisionV2 == incoming && existing->head == tipHash)),
                "incremental recipient HEAD differs from pinned base");
        RevisionResolverV2 resolver(this->directory_, *project);
        const RevisionObjectReaderV2 local = [this](const Hash& hash) -> std::optional<Bytes> {
            if (!containsIndex(hash)) return std::nullopt;
            return load(hash);
        };
        const std::optional<ResolvedRevisionV2> pinned = resolver.resolve(*base, local);
        require(Err::Conflict, pinned && pinned->recordHash == baseHash,
                "incremental base UUID/SHA is missing or changed");
    }
    // Only the sorted SHA/offset index and a small UUID→offset table are kept.
    // All immutable payloads stay in the caller-owned disk file and are
    // reread with SHA verification when accessed for structural checks.
    BundleLocationIndex entries(count, this->scratch_);
    std::map<RevisionId, std::pair<Hash, BundleLocation>> revisions;
    Hash previous{};
    std::uint64_t offset = headerSize;
    for (std::uint64_t ordinal = 0; ordinal < count; ++ordinal) {
        require(Err::InvalidInput, size - offset >= bundleEntryBytes,
                "truncated v2 bundle object entry");
        std::array<std::uint8_t, bundleEntryBytes> entry{};
        readExact(fd, entry.data(), entry.size(), offset);
        offset += entry.size();
        Hash hash{};
        std::copy_n(entry.begin(), 32, hash.begin());
        const std::uint64_t length = readU64(entry.data() + 32);
        require(Err::InvalidInput, hash != Hash{} && (ordinal == 0 || previous < hash) &&
                length <= maxObjectBytes && length <= size - offset,
                "invalid v2 bundle object order or size");
        require(Err::LimitExceeded, limits.maxObjectBytes == 0 ||
                length <= limits.maxObjectBytes,
                "v2 bundle object exceeds configured limit");
        require(Err::InvalidInput, sha256FileRegion(fd, offset, length) == hash,
                "v2 bundle object checksum mismatch");
        const BundleLocation location{offset, length};
        entries.append(hash, location);
        if (length >= 2) {
            std::array<std::uint8_t, 2> prefix{};
            readExact(fd, prefix.data(), prefix.size(), offset);
            if (prefix[0] == 'T' && prefix[1] == 2) {
                Bytes bytes(static_cast<std::size_t>(length));
                readExact(fd, bytes.data(), bytes.size(), offset);
                RevisionRecordV2 record = decodeRevisionRecordV2(bytes);
                require(Err::InvalidInput, record.project == *project &&
                        revisions.emplace(record.id, std::make_pair(hash, location)).second,
                        "duplicate or cross-project v2 revision in bundle");
            }
        }
        offset += length;
        previous = hash;
    }
    require(Err::InvalidInput, offset == size, "v2 bundle has trailing bytes");
    entries.finalize();
    const auto readBundle = [&](const Hash& hash) -> Bytes {
        const std::optional<BundleLocation> location = entries.find(hash);
        require(Err::Corruption, location.has_value(), "missing transferred v2 object");
        Bytes data(static_cast<std::size_t>(location->size));
        if (!data.empty()) readExact(fd, data.data(), data.size(), location->offset);
        require(Err::InvalidInput, sha256(data) == hash,
                "v2 replication bundle changed during import");
        return data;
    };
    const auto readLocal = [&](const Hash& hash) -> Bytes {
        require(Err::Corruption, containsIndex(hash), "missing local incremental object");
        const Bytes bytes = load(hash);
        require(Err::Corruption, sha256(bytes) == hash,
                "local incremental pack object checksum mismatch");
        return bytes;
    };
    const auto bundled = [&](const Hash& hash) -> Bytes {
        Bytes bytes = entries.find(hash) ? readBundle(hash) :
                      (incremental ? readLocal(hash) : Bytes{});
        require(Err::Corruption, !bytes.empty(), "v2 bundle omits reachable object");
        if (bytes.size() >= 2 && bytes[0] == 'T' && bytes[1] == 2)
            return logicalRevisionFromV2(bytes, *project);
        return bytes;
    };
    const std::map<RevisionId, std::pair<Hash, BundleLocation>>::const_iterator tip =
        revisions.find(*incoming);
    require(Err::InvalidInput, (tip != revisions.end() && tip->second.first == tipHash) ||
            (incremental && *incoming == *base && tipHash == baseHash && count == 0),
            "v2 bundle does not authenticate its HEAD revision");
    const RevisionRecordLookupV2 getRevision = [&](const RevisionId& id) -> std::optional<Bytes> {
        const std::map<RevisionId, std::pair<Hash, BundleLocation>>::const_iterator found =
            revisions.find(id);
        if (found != revisions.end()) return readBundle(found->second.first);
        if (!incremental) return std::nullopt;
        RevisionResolverV2 resolver(this->directory_, *project);
        const RevisionObjectReaderV2 local = [this](const Hash& hash) -> std::optional<Bytes> {
            if (!containsIndex(hash)) return std::nullopt;
            return load(hash);
        };
        const std::optional<ResolvedRevisionV2> resolved = resolver.resolveDirect(id, local);
        if (!resolved) return std::nullopt;
        return encodeRevisionRecordV2(resolved->record);
    };
    try {
        (void)verifyRevisionAncestryV2(
            tip != revisions.end() ? readBundle(tip->second.first) : *getRevision(*incoming),
            getRevision, revisionBundleMaxObjectsV2);
    } catch (const StorageError& error) {
        if (error.kind() == Err::Corruption)
            throw StorageError(Err::InvalidInput,
                               std::string("invalid v2 bundle ancestry: ") + error.what());
        throw;
    }
    if (forkRevisionId) {
        // Provenance cannot identify an unrelated revision: the immutable
        // fork point must be a genuine ancestor of this branch's HEAD.
        std::set<RevisionId> seen;
        std::vector<RevisionId> pending{*incoming};
        bool foundFork = false;
        while (!pending.empty()) {
            const RevisionId id = pending.back();
            pending.pop_back();
            if (!seen.insert(id).second) continue;
            require(Err::LimitExceeded, seen.size() <= revisionBundleMaxObjectsV2,
                    "v2 bundle fork provenance exceeds ancestry budget");
            if (id == *forkRevisionId) {
                foundFork = true;
                break;
            }
            const std::optional<Bytes> encoded = getRevision(id);
            require(Err::InvalidInput, encoded.has_value(),
                    "v2 bundle fork ancestry has an unavailable revision");
            const RevisionRecordV2 record = decodeRevisionRecordV2(*encoded);
            for (const RevisionParentV2& parent : record.parents)
                pending.push_back(parent.id);
        }
        require(Err::InvalidInput, foundFork,
                "v2 bundle fork revision is not an ancestor of branch HEAD");
    }
    if (incremental) {
        // A matching recipient base in the header is not enough: an
        // untrusted sender could supply an unrelated tip and move the branch
        // sideways. Require the pinned base in the authenticated parent DAG.
        std::set<RevisionId> seen;
        std::vector<RevisionId> pending{*incoming};
        bool foundBase = false;
        while (!pending.empty()) {
            const RevisionId id = pending.back();
            pending.pop_back();
            if (!seen.insert(id).second) continue;
            require(Err::LimitExceeded, seen.size() <= revisionBundleMaxObjectsV2,
                    "incremental v2 ancestry exceeds traversal budget");
            if (id == *base) {
                foundBase = true;
                break;
            }
            const std::optional<Bytes> encoded = getRevision(id);
            require(Err::InvalidInput, encoded.has_value(),
                    "incremental v2 history has an unavailable ancestor");
            const RevisionRecordV2 revision = decodeRevisionRecordV2(*encoded);
            for (const RevisionParentV2& parent : revision.parents)
                pending.push_back(parent.id);
        }
        require(Err::InvalidInput, foundBase,
                "incremental v2 recipient base is not an ancestor of incoming HEAD");
    }
    BundleVisitMarks marks(entries.size(), this->scratch_);
    BundleTreeSummaries summaries(entries.size(), this->scratch_);
    const ExternalVerificationMarks external{
        [&](const Hash& hash) -> std::optional<std::uint8_t> {
            const std::optional<std::size_t> index = entries.ordinal(hash);
            if (!index) return std::nullopt;
            return marks.at(*index);
        },
        [&](const Hash& hash, std::uint8_t bits) {
            const std::optional<std::size_t> index = entries.ordinal(hash);
            require(Err::Internal, index.has_value(), "missing bundle verification mark");
            marks.mark(*index, bits);
        },
        [&]() { return marks.markedCount(); },
        [&](const Hash& hash, unsigned kind) -> std::optional<TreeSummary> {
            const std::optional<std::size_t> index = entries.ordinal(hash);
            return index ? summaries.read(*index, kind) : std::nullopt;
        },
        [&](const Hash& hash, unsigned kind, const TreeSummary& value) {
            const std::optional<std::size_t> index = entries.ordinal(hash);
            require(Err::Internal, index.has_value(), "missing tree-summary entry");
            summaries.write(*index, kind, value);
        },
        [&]() { return summaries.size(); },
        [&](const std::function<void(const Hash&)>& visitor) {
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if ((marks.at(index) & 1U) != 0U) visitor(entries.at(index).first);
            }
        },
        [&](const Hash& hash) {
            return incremental && !entries.find(hash) && containsIndex(hash);
        }
    };
    try {
        (void)verifyLogicalGraph({tipHash}, bundled, this->scratch_, external);
        for (std::size_t index = 0; index < entries.size(); ++index)
            require(Err::InvalidInput, marks.at(index) != 0,
                    "v2 bundle includes unreachable or redundant objects");
    } catch (const StorageError& error) {
        if (error.kind() == Err::Corruption)
            throw StorageError(Err::InvalidInput,
                               std::string("invalid v2 bundle graph: ") + error.what());
        throw;
    }
    if (incremental && existing->revisionV2 == incoming) {
        return RevisionBundleImportV2{BundleImportStatus::UpToDate, *incoming, 0};
    }
    BundleImportStatus status = BundleImportStatus::Created;
    if (existing) {
        if (existing->revisionV2 == incoming) {
            require(Err::Conflict, existing->head == tipHash,
                    "existing branch HEAD UUID/SHA differs from incoming");
            return RevisionBundleImportV2{BundleImportStatus::UpToDate, *incoming, 0};
        }
        if (revisions.contains(*existing->revisionV2)) {
            require(Err::Conflict, revisions.at(*existing->revisionV2).first == existing->head,
                    "existing branch ancestry checksum conflicts with incoming");
            status = BundleImportStatus::FastForward;
        } else if (incremental) {
            require(Err::Conflict, existing->revisionV2 == base && existing->head == baseHash,
                    "stale incremental recipient HEAD");
            status = BundleImportStatus::FastForward;
        } else {
            RevisionResolverV2 resolver(this->directory_, *project);
            const RevisionObjectReaderV2 local = [this](const Hash& hash) -> std::optional<Bytes> {
                if (!containsIndex(hash)) return std::nullopt;
                return load(hash);
            };
            std::set<RevisionId> seen;
            std::vector<RevisionId> pending{*existing->revisionV2};
            bool ancestor = false;
            while (!pending.empty()) {
                const RevisionId current = pending.back();
                pending.pop_back();
                if (!seen.insert(current).second) continue;
                require(Err::LimitExceeded, seen.size() <= revisionBundleMaxObjectsV2,
                        "v2 replication ancestry budget exceeded");
                const std::optional<ResolvedRevisionV2> revision =
                    resolver.resolveDirect(current, local);
                require(Err::Corruption, revision.has_value(),
                        "missing local v2 revision ancestor");
                if (current == *incoming) {
                    require(Err::Conflict, revision->recordHash == tipHash,
                            "local revision digest conflicts with incoming");
                    ancestor = true;
                    break;
                }
                for (const RevisionParentV2& parent : revision->record.parents)
                    pending.push_back(parent.id);
            }
            require(Err::Conflict, ancestor,
                    "v2 replication would replace divergent branch history");
            return RevisionBundleImportV2{BundleImportStatus::UpToDate,
                                           *existing->revisionV2, 0};
        }
    }
    // Only after all external data, ancestry and object semantics validate:
    // append immutable objects; fsync; publish UUID mappings parent-first;
    // finally atomically replace the branch HEAD record.
    std::uint64_t imported = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::pair<Hash, BundleLocation> object = entries.at(index);
        if (containsIndex(object.first)) {
            require(Err::Corruption, sha256(load(object.first)) == object.first,
                    "local immutable object checksum mismatch");
            continue;
        }
        require(Err::Internal, store(readBundle(object.first)) == object.first,
                "v2 imported object digest changed during storage");
        ++imported;
    }
    syncPack();
    RevisionResolverV2 resolver(this->directory_, *project);
    const RevisionObjectReaderV2 local = [this](const Hash& hash) -> std::optional<Bytes> {
        if (!containsIndex(hash)) return std::nullopt;
        return load(hash);
    };
    std::set<RevisionId> published;
    std::vector<std::pair<RevisionId, bool>> pending{{*incoming, false}};
    while (!pending.empty()) {
        const std::pair<RevisionId, bool> step = pending.back();
        pending.pop_back();
        if (published.contains(step.first)) continue;
        const std::map<RevisionId, std::pair<Hash, BundleLocation>>::const_iterator found =
            revisions.find(step.first);
        if (found == revisions.end()) {
            require(Err::Corruption, incremental &&
                    resolver.resolveDirect(step.first, local).has_value(),
                    "v2 imported ancestry refers to an unpublished local revision");
            published.insert(step.first);
            continue;
        }
        if (step.second) {
            require(Err::Corruption, resolver.publish(readBundle(found->second.first), local) ==
                    found->second.first, "v2 imported revision digest changed");
            published.insert(step.first);
        } else {
            pending.emplace_back(step.first, true);
            const RevisionRecordV2 revision =
                decodeRevisionRecordV2(readBundle(found->second.first));
            for (const RevisionParentV2& parent : revision.parents)
                if (!published.contains(parent.id)) pending.emplace_back(parent.id, false);
        }
    }
    publishStoredBranch(this->directory_, StoredBranch{*branchId, std::string(localBranch),
                                                        tipHash, *incoming,
                                                        existing ? existing->originBranchId : originBranchId,
                                                        existing ? existing->forkRevisionId : forkRevisionId},
                        !existing);
    return RevisionBundleImportV2{status, *incoming, imported};
}

// physical pack/index pages. Separate repositories retain identical identities.
void Repository::exportBranchBundle(std::string_view branch,
                                    const std::filesystem::path& bundlePath, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, &bundlePath, -1, std::nullopt, limits, true);
}

void Repository::exportBranchBundle(std::string_view branch, int fd, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, nullptr, fd, std::nullopt, limits, true);
}

void Repository::exportIncrementalBranchBundle(std::string_view branch, const Hash& recipientBase,
                                               int fd, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, nullptr, fd, recipientBase, limits, true);
}

void Repository::exportIncrementalBranchBundle(std::string_view branch,
                                               const Hash& recipientBase,
                                               const std::filesystem::path& bundlePath, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, &bundlePath, -1, recipientBase, limits, true);
}

void Repository::exportIdentifiedBranchBundle(std::string_view branch,
                                             const std::filesystem::path& bundlePath,
                                             BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, &bundlePath, -1, std::nullopt, limits, true);
}

void Repository::exportIdentifiedBranchBundle(std::string_view branch, int fd,
                                             BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, nullptr, fd, std::nullopt, limits, true);
}

void Repository::exportIdentifiedIncrementalBranchBundle(
        std::string_view branch, const Hash& recipientBase,
        const std::filesystem::path& bundlePath, BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, &bundlePath, -1, recipientBase, limits, true);
}

void Repository::exportIdentifiedIncrementalBranchBundle(
        std::string_view branch, const Hash& recipientBase, int fd,
        BundleLimits limits) {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    exportBundleLocked(branch, nullptr, fd, recipientBase, limits, true);
}

// Objects of `after` that are not part of the tree `before`, found by walking
// both tries in parallel. Identical subtree hashes are skipped without I/O;
// where an insertion or deletion changed the trie shape, `before` is aligned
// to the level of `after`. Skipping happens only for hashes proven present
// in `before`, so a misaligned comparison costs bytes, never correctness.
void Repository::collectTreeDelta(const Hash& before, const Hash& after, std::set<Hash>& output,
                                  std::uint64_t maxObjects) {
    // No pruning at nodes already emitted for another revision: that walk
    // relied on ITS parent's tree, which may in turn rely on this one.
    if (isZero(after) || before == after) return;
    const TreeNode node = readNode(after);
    const unsigned depth = node.leaf ? 32U : node.depth;
    const Key& prefix = node.leaf ? node.key : node.prefix;
    Hash aligned = before;
    std::optional<TreeNode> old;
    while (!isZero(aligned)) {
        old = readNode(aligned);
        if (old->leaf || old->depth >= depth) break;
        aligned = old->children[nibble(prefix, old->depth)]; // descend to after's level
        old.reset();
    }
    if (aligned == after) return;
    insertBundleObject(output, after, maxObjects);
    if (node.leaf) {
        if (!old || !old->leaf || old->value != node.value)
            insertBundleObject(output, node.value, maxObjects);
        return;
    }
    for (unsigned slot = 0; slot < 16; ++slot) {
        Hash oldChild{};
        if (old && !old->leaf && old->depth == node.depth) {
            oldChild = old->children[slot];
        } else if (old) { // `old` is deeper: it lies within one slot of `after`
            const Key& oldPrefix = old->leaf ? old->key : old->prefix;
            if (nibble(oldPrefix, node.depth) == slot) oldChild = aligned;
        }
        collectTreeDelta(oldChild, node.children[slot], output, maxObjects);
    }
}

// Incremental bundle content: the revisions reachable from `tip` but not from
// `base`, plus for each of them the tree objects that differ from one of its
// parents. Work is proportional to the new revisions and their changes, not
// to the size of the trees: only the base's revision ancestry is read.
// The recipient holds the base's complete graph. Each new revision's walk is
// complete relative to its chosen parent, so by induction over the ancestry
// every object of a new revision is emitted or held by the recipient.
std::set<Hash> Repository::collectDelta(const Hash& tip, const Hash& base,
                                        std::uint64_t maxObjects) {
    std::unordered_set<Hash, HashHasher> known;
    std::vector<Hash> pending{base};
    while (!pending.empty()) {
        const Hash current = pending.back();
        pending.pop_back();
        if (!known.insert(current).second) continue;
        for (const Hash& parent : readRevision(current).parents) pending.push_back(parent);
    }
    std::set<Hash> output;
    std::unordered_set<Hash, HashHasher> visited;
    pending.assign(1, tip);
    while (!pending.empty()) {
        const Hash current = pending.back();
        pending.pop_back();
        if (known.contains(current) || !visited.insert(current).second) continue;
        const Revision revision = readRevision(current);
        insertBundleObject(output, current, maxObjects);
        // Prefer a parent the recipient already holds: its trees need no
        // transfer and the new revision's diff against it is the smallest.
        Revision parent{};
        for (const Hash& candidate : revision.parents) {
            if (known.contains(candidate)) { parent = readRevision(candidate); break; }
        }
        if (isZero(parent.id) && !revision.parents.empty()) parent = readRevision(revision.parents.front());
        collectTreeDelta(parent.root, revision.root, output, maxObjects);
        collectTreeDelta(parent.referencesRoot, revision.referencesRoot, output, maxObjects);
        for (const Hash& candidate : revision.parents) pending.push_back(candidate);
    }
    return output;
}

// Filesystem-path exports write into a private file in the destination
// directory, fsync it, then atomically install the finished file without
// replacing an existing name. Caller-owned descriptor exports retain their
// previous contract: the caller handles publication and keeps ownership.
void Repository::exportBundleLocked(std::string_view branch,
                                    const std::filesystem::path* bundlePath, int targetFd,
                                    std::optional<Hash> recipientBase, BundleLimits limits,
                                    bool includeBranchIdentity) {
    require(Err::Internal, includeBranchIdentity,
            "SHA-only replication export is unsupported under BRANCH-1");

    const Hash tip = branchHead(branch);
    const std::optional<BranchId> identity = includeBranchIdentity
        ? branchIdLocked(branch) : std::nullopt;
    require(Err::Conflict, !includeBranchIdentity || identity.has_value(),
            "cannot export branch identity: legacy branch has no authoritative UUIDv7");
    // Full-history visited state lives in a private, disk-backed hash table.
    // Keep incremental trie-delta enumeration unchanged: its small-delta
    // performance must not pay the full-history scratch/index overhead.
    const auto collectObjects = [&](const Hash& revisionTip, DiskBundleHashes& objects) {
        enum class Kind { Revision, Tree, Value };
        std::vector<std::pair<Hash, Kind>> pending{{revisionTip, Kind::Revision}};
        while (!pending.empty()) {
            const std::pair<Hash, Kind> entry = pending.back();
            pending.pop_back();
            if (isZero(entry.first) || !objects.insert(entry.first, limits.maxObjects)) continue;
            if (entry.second == Kind::Revision) {
                const Revision revision = readRevision(entry.first);
                pending.emplace_back(revision.root, Kind::Tree);
                pending.emplace_back(revision.referencesRoot, Kind::Tree);
                for (const Hash& parent : revision.parents)
                    pending.emplace_back(parent, Kind::Revision);
            } else if (entry.second == Kind::Tree) {
                const TreeNode node = readNode(entry.first);
                if (node.leaf) {
                    pending.emplace_back(node.value, Kind::Value);
                } else {
                    for (const Hash& child : node.children)
                        pending.emplace_back(child, Kind::Tree);
                }
            } else {
                (void)load(entry.first); // Verify every immutable object on export.
            }
        }
    };
    // Validate the request before creating an output file. A parent revision
    // can be shared even if it is not on the first-parent history.
    if (recipientBase) {
        require(Err::Conflict, !isZero(*recipientBase) && revisionReachableFrom(tip, *recipientBase),
                "incremental export base is not an ancestor of the source branch");
    }
    std::optional<std::set<Hash>> delta;
    std::unique_ptr<SortedBundleHashes> sortedFull;
    std::uint64_t objectCount = 0;
    if (recipientBase) {
        delta = collectDelta(tip, *recipientBase, limits.maxObjects);
        objectCount = delta->size();
    } else {
        DiskBundleHashes objects(this->scratch_);
        collectObjects(tip, objects);
        objectCount = objects.size();
        require(Err::InvalidInput, objectCount != 0, "cannot export an empty revision graph");
        sortedFull = std::make_unique<SortedBundleHashes>(this->scratch_);
        objects.forEach([&](const Hash& hash) { sortedFull->append(hash); });
        sortedFull->finalize();
    }
    // The count limit was enforced at each unique discovery, before opening
    // any output file. No partial bundle can be published on rejection.
    int fd = targetFd;
    int directoryFd = -1;
    std::string temporaryName; // empty for an anonymous O_TMPFILE export
    std::string destinationName;
    if (bundlePath != nullptr) {
        const std::filesystem::path parent = bundlePath->parent_path().empty()
            ? std::filesystem::path(".") : bundlePath->parent_path();
        destinationName = bundlePath->filename().string();
        require(Err::InvalidInput, !destinationName.empty() && destinationName != "." &&
                destinationName != "..", "invalid replication bundle destination");
        directoryFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        systemCall(directoryFd >= 0, "open replication bundle destination directory");
        try {
            // On supporting filesystems a crash before publication leaves no
            // directory entry. Named fallback files are deliberately hidden;
            // they can be removed after a crashed exporter has exited.
            if (!detail::forceNamedScratchFiles()) {
                fd = ::openat(directoryFd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
                if (fd < 0 && errno != EOPNOTSUPP && errno != EISDIR &&
                    errno != EINVAL && errno != ENOTSUP)
                    throwSystemError("create anonymous replication bundle");
            }
            if (fd < 0) {
                std::string name = (parent / ".aowis-export-XXXXXX").string();
                fd = ::mkostemp(name.data(), O_CLOEXEC);
                systemCall(fd >= 0, "create private replication bundle");
                temporaryName = std::filesystem::path(name).filename().string();
            }
        } catch (...) {
            ::close(directoryFd);
            throw;
        }
    } else {
        struct stat target{};
        systemCall(fd >= 0 && ::fstat(fd, &target) == 0, "stat replication bundle target");
        require(Err::InvalidInput, S_ISREG(target.st_mode) && target.st_size == 0,
                "replication bundle target must be an empty regular file");
    }
    try {
        const std::uint64_t headerSize = recipientBase
            ? (includeBranchIdentity ? identifiedIncrementalHeaderBytes : incrementalBundleHeaderBytes)
            : (includeBranchIdentity ? identifiedBundleHeaderBytes : bundleHeaderBytes);
        Bytes header(static_cast<std::size_t>(headerSize));
        const std::array<std::uint8_t, 8>& magic = includeBranchIdentity
            ? (recipientBase ? identifiedIncrementalBundleMagic : identifiedBundleMagic)
            : (recipientBase ? incrementalBundleMagic : bundleMagic);
        std::copy(magic.begin(), magic.end(), header.begin());
        std::copy(tip.begin(), tip.end(), header.begin() + 8);
        storeU64(header.data() + 40, objectCount);
        if (recipientBase) {
            std::copy(recipientBase->begin(), recipientBase->end(), header.begin() + 48);
        }
        if (identity) {
            const std::size_t position = recipientBase ? 80 : 48;
            std::copy(identity->uuid().bytes.begin(), identity->uuid().bytes.end(),
                      header.begin() + static_cast<std::ptrdiff_t>(position));
        }
        writeExact(fd, header.data(), header.size(), 0);
        std::uint64_t offset = headerSize;
        require(Err::LimitExceeded, limits.maxBytes == 0 || offset <= limits.maxBytes,
                "bundle transfer limit: byte count exceeded");
        const auto appendObject = [&](const Hash& hash) {
            const Bytes bytes = load(hash);
            require(Err::LimitExceeded, limits.maxObjectBytes == 0 || bytes.size() <= limits.maxObjectBytes,
                    "bundle transfer limit: individual object exceeded");
            require(Err::LimitExceeded, limits.maxBytes == 0 ||
                    (offset <= limits.maxBytes &&
                     bytes.size() <= limits.maxBytes - offset &&
                     bundleEntryBytes <= limits.maxBytes - offset - bytes.size()),
                    "bundle transfer limit: byte count exceeded");
            std::array<std::uint8_t, bundleEntryBytes> entry{};
            std::copy(hash.begin(), hash.end(), entry.begin());
            storeU64(entry.data() + 32, static_cast<std::uint64_t>(bytes.size()));
            writeExact(fd, entry.data(), entry.size(), offset);
            offset += bundleEntryBytes;
            if (!bytes.empty()) {
                detail::faultPartialWrite(fd, bytes.data(), bytes.size(), offset,
                                          "export.partial.object");
                writeExact(fd, bytes.data(), bytes.size(), offset);
            }
            offset += bytes.size();
        };
        if (delta) {
            for (const Hash& hash : *delta) appendObject(hash);
        } else {
            while (const std::optional<Hash> hash = sortedFull->next()) appendObject(*hash);
        }
        systemCall(::fsync(fd) == 0, "sync replication bundle");
        if (bundlePath == nullptr) return; // caller-owned descriptor stays open
        detail::faultCheckpoint("export.before.publish");
        // linkat is no-replace: two concurrent exporters cannot overwrite
        // one another, and neither can replace a symlink or existing file.
        // The completed file is the only content that can become visible at
        // the requested name, even if the process dies immediately afterward.
        if (temporaryName.empty()) {
            systemCall(::linkat(fd, "", directoryFd, destinationName.c_str(), AT_EMPTY_PATH) == 0,
                       "publish anonymous replication bundle (destination must not exist)");
        } else {
            systemCall(::linkat(directoryFd, temporaryName.c_str(), directoryFd,
                                destinationName.c_str(), 0) == 0,
                       "publish replication bundle (destination must not exist)");
        }
        detail::faultCheckpoint("export.after.publish");
        if (!temporaryName.empty()) {
            systemCall(::unlinkat(directoryFd, temporaryName.c_str(), 0) == 0,
                       "remove private replication bundle name");
            temporaryName.clear();
        }
        systemCall(::fsync(directoryFd) == 0, "sync replication bundle directory");
        detail::faultCheckpoint("export.after.dirsync");
        // Close only after publication. A close failure does not remove the
        // already completed/published file (the caller can retry by name).
        const int closing = fd;
        fd = -1;
        systemCall(::close(closing) == 0, "close replication bundle");
        const int closingDirectory = directoryFd;
        directoryFd = -1;
        systemCall(::close(closingDirectory) == 0, "close replication bundle directory");
    } catch (...) {
        if (bundlePath == nullptr) {
            // Never leave a partial bundle behind. The original failure is
            // reported; a failing truncate cannot add information to it.
            if (::ftruncate(targetFd, 0) != 0) errno = 0;
            throw;
        }
        if (fd >= 0) ::close(fd);
        if (directoryFd >= 0) {
            if (!temporaryName.empty())
                (void)::unlinkat(directoryFd, temporaryName.c_str(), 0);
            ::close(directoryFd);
        }
        throw;
    }
}

BundleImportResult Repository::importBranchBundle(const std::filesystem::path& bundlePath,
                                                   std::string_view localBranch, BundleLimits limits,
                                                   BundleImportProfile* profile) {
    const int fd = ::open(bundlePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    systemCall(fd >= 0, "open replication bundle");
    try {
        const BundleImportResult result = importBranchBundle(fd, localBranch, limits, profile);
        ::close(fd);
        return result;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

BundleImportResult Repository::importBranchBundle(int fd, std::string_view localBranch,
                                                   BundleLimits limits,
                                                   BundleImportProfile* profile) {
    const ImportClock::time_point importStart = ImportClock::now();
    if (profile != nullptr) *profile = BundleImportProfile{};
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    refreshIndex();
    checkBranch(localBranch);
    {
        struct stat metadata{};
        systemCall(::fstat(fd, &metadata) == 0, "stat replication bundle");
        require(Err::InvalidInput, S_ISREG(metadata.st_mode) && metadata.st_size >= 0 &&
                static_cast<std::uint64_t>(metadata.st_size) >= bundleHeaderBytes,
                "invalid replication bundle file");
        const std::uint64_t fileSize = static_cast<std::uint64_t>(metadata.st_size);
        require(Err::LimitExceeded, limits.maxBytes == 0 || fileSize <= limits.maxBytes,
                "bundle transfer limit: byte count exceeded");
        std::array<std::uint8_t, bundleHeaderBytes> header{};
        readExact(fd, header.data(), header.size(), 0);
        const bool fullIdentified = std::equal(identifiedBundleMagic.begin(),
                                               identifiedBundleMagic.end(), header.begin());
        const bool deltaIdentified = std::equal(identifiedIncrementalBundleMagic.begin(),
                                                identifiedIncrementalBundleMagic.end(), header.begin());
        const bool identified = fullIdentified || deltaIdentified;
        const bool incremental = deltaIdentified;
        require(Err::InvalidInput, identified,
                "unsupported replication bundle format: UUIDv7 branch identity required");
        const std::uint64_t headerBytes = incremental
            ? (identified ? identifiedIncrementalHeaderBytes : incrementalBundleHeaderBytes)
            : (identified ? identifiedBundleHeaderBytes : bundleHeaderBytes);
        require(Err::InvalidInput, fileSize >= headerBytes, "truncated replication bundle header");
        Hash recipientBase{};
        if (incremental) {
            readExact(fd, recipientBase.data(), recipientBase.size(), bundleHeaderBytes);
            require(Err::InvalidInput, !isZero(recipientBase), "incremental bundle has zero base revision");
            const bool known = containsIndex(recipientBase);
            require(Err::Conflict, known, "incremental bundle base revision is missing locally");
            (void)readRevision(recipientBase); // Must be a real revision, not another object.
        }
        std::optional<BranchId> incomingBranchId;
        if (identified) {
            std::array<std::uint8_t, 16> idBytes{};
            readExact(fd, idBytes.data(), idBytes.size(), incremental ? 80 : 48);
            const UuidV7 decoded{idBytes};
            incomingBranchId = BranchId::parse(decoded.toString());
            require(Err::InvalidInput, incomingBranchId.has_value(),
                    "replication bundle contains an invalid UUIDv7 branch identity");
        }
        Hash incoming{};
        std::copy_n(header.begin() + 8, 32, incoming.begin());
        require(Err::InvalidInput, !isZero(incoming), "replication bundle has zero revision head");
        const std::uint64_t count = readU64(header.data() + 40);
        require(Err::InvalidInput, (incremental || count != 0) &&
                count <= (fileSize - headerBytes) / bundleEntryBytes,
                "invalid replication bundle object count");
        require(Err::LimitExceeded, limits.maxObjects == 0 || count <= limits.maxObjects,
                "bundle transfer limit: object count exceeded");
        BundleLocationIndex entries(count, this->scratch_);
        std::uint64_t offset = headerBytes;
        Hash previous{};
        for (std::uint64_t index = 0; index < count; ++index) {
            require(Err::InvalidInput, fileSize - offset >= bundleEntryBytes,
                    "truncated replication bundle entry");
            std::array<std::uint8_t, bundleEntryBytes> entry{};
            readExact(fd, entry.data(), entry.size(), offset);
            offset += bundleEntryBytes;
            Hash hash{};
            std::copy_n(entry.begin(), 32, hash.begin());
            const std::uint64_t size = readU64(entry.data() + 32);
            require(Err::InvalidInput, !isZero(hash) && (index == 0 || previous < hash),
                    "replication bundle object hashes must be unique and sorted");
            require(Err::InvalidInput, size <= maxObjectBytes && size <= fileSize - offset,
                    "replication bundle object exceeds bounds");
            require(Err::LimitExceeded, limits.maxObjectBytes == 0 || size <= limits.maxObjectBytes,
                    "bundle transfer limit: individual object exceeded");
            require(Err::InvalidInput, sha256FileRegion(fd, offset, size) == hash,
                    "replication bundle SHA-256 mismatch");
            entries.append(hash, BundleLocation{offset, size});
            offset += size;
            previous = hash;
        }
        require(Err::InvalidInput, offset == fileSize, "replication bundle contains trailing bytes");
        entries.finalize();
        if (profile != nullptr) {
            profile->scanMs = elapsedImportMs(importStart, ImportClock::now());
        }
        const ImportClock::time_point graphStart = ImportClock::now();
        const auto containsLocally = [&](const Hash& hash) {
            return containsIndex(hash);
        };
        require(Err::InvalidInput, entries.find(incoming).has_value() ||
                    (incremental && containsLocally(incoming)),
                "replication bundle lacks tip revision");
        const auto bundleObject = [&](const Hash& hash) -> Bytes {
            const std::optional<BundleLocation> location = entries.find(hash);
            if (!location) {
                require(Err::InvalidInput, incremental && containsLocally(hash),
                        "incremental replication bundle is missing a referenced object");
                Bytes local;
                try {
                    local = load(hash);
                    require(Err::Corruption, sha256(local) == hash,
                            "local replication object SHA-256 mismatch");
                } catch (...) {
                    // Damage in THIS repository is not a defect of the bundle.
                    try { throw; }
                    catch (const StorageError& error) {
                        throw LocalStorageFailure(error.kind(), "local object " + toHex(hash) +
                                                  ": " + error.what());
                    }
                }
                if (profile != nullptr) {
                    ++profile->localObjectReads;
                    profile->localObjectBytes += local.size();
                }
                return local;
            }
            Bytes object(static_cast<std::size_t>(location->size));
            if (!object.empty()) readExact(fd, object.data(), object.size(), location->offset);
            require(Err::InvalidInput, sha256(object) == hash, "replication bundle changed during import");
            if (profile != nullptr) {
                ++profile->bundleObjectReads;
                profile->bundleObjectBytes += object.size();
            }
            return object;
        };
        // A content hash authenticates bytes, not trie semantics. Verify each
        // immutable subtree bottom-up, so a subtree shared between revisions
        // is checked once per value type. The summary guarantees that ALL its
        // keys have the prefix preceding the root branch depth (or that it is
        // a single leaf). Parent checks can therefore use one representative
        // key without revisiting descendants in each ancestor context.
        BundleVisitMarks visitMarks(entries.size(), this->scratch_);
        BundleTreeSummaries treeSummaries(entries.size(), this->scratch_);
        const ExternalVerificationMarks externalMarks{
            [&](const Hash& hash) -> std::optional<std::uint8_t> {
                const std::optional<std::size_t> ordinal = entries.ordinal(hash);
                if (!ordinal) return std::nullopt;
                return visitMarks.at(*ordinal);
            },
            [&](const Hash& hash, std::uint8_t bits) {
                const std::optional<std::size_t> ordinal = entries.ordinal(hash);
                require(Err::Internal, ordinal.has_value(), "missing hash in bundle visitation index");
                visitMarks.mark(*ordinal, bits);
            },
            [&]() { return visitMarks.markedCount(); },
            [&](const Hash& hash, unsigned kind) -> std::optional<TreeSummary> {
                const std::optional<std::size_t> ordinal = entries.ordinal(hash);
                if (!ordinal) return std::nullopt;
                return treeSummaries.read(*ordinal, kind);
            },
            [&](const Hash& hash, unsigned kind, const TreeSummary& summary) {
                const std::optional<std::size_t> ordinal = entries.ordinal(hash);
                require(Err::Internal, ordinal.has_value(), "missing hash in bundle trie summary index");
                treeSummaries.write(*ordinal, kind, summary);
            },
            [&]() { return treeSummaries.size(); },
            [&](const std::function<void(const Hash&)>& visitor) {
                // Ordinal traversal uses the mapped, sorted index and a
                // single byte per object. No per-revision hash container.
                for (std::size_t ordinal = 0; ordinal < entries.size(); ++ordinal) {
                    if ((visitMarks.at(ordinal) & 1U) != 0U) {
                        visitor(entries.at(ordinal).first);
                    }
                }
            },
            [&](const Hash& hash) {
                return incremental && !entries.find(hash).has_value() && containsLocally(hash);
            }
        };
        LogicalVerificationProfile graphProfile{};
        LogicalVerification verification{};
        try {
            verification = verifyLogicalGraph({incoming}, bundleObject, this->scratch_, externalMarks,
                                              profile != nullptr ? &graphProfile : nullptr);
        } catch (...) {
            rethrowBundleFailure();
        }
        if (profile != nullptr) {
            // Includes setup of visit-mark and trie-summary scratch tables.
            // The separately reported graph/model stages cover the verifier
            // itself; any remaining setup work stays in graphTraversalMs.
            profile->graphTraversalMs = elapsedImportMs(graphStart, ImportClock::now()) -
                                         graphProfile.modelReferenceMs;
            profile->modelReferenceMs = graphProfile.modelReferenceMs;
        }
        const ImportClock::time_point reachStart = ImportClock::now();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            require(Err::InvalidInput, visitMarks.at(index) != 0,
                    "replication bundle contains unreachable objects");
        }
        // Boundary revisions were not walked. Usually the base (and a
        // fast-forwarded head) is itself a boundary; otherwise search the
        // local ancestry below the boundaries, once for all candidates.
        std::optional<std::unordered_set<Hash, HashHasher>> boundaryAncestry;
        const std::function<bool(const Hash&)> includesRevision = [&](const Hash& candidate) {
            if (verification.containsRevision(candidate)) return true;
            if (!boundaryAncestry) {
                boundaryAncestry.emplace();
                std::vector<Hash> pending(verification.boundaryRevisions.begin(),
                                          verification.boundaryRevisions.end());
                try {
                    while (!pending.empty()) {
                        const Hash current = pending.back();
                        pending.pop_back();
                        if (!boundaryAncestry->insert(current).second) continue;
                        for (const Hash& parent : readRevision(current).parents) pending.push_back(parent);
                    }
                } catch (const StorageError& error) {
                    throw StorageError(error.kind(), "local revision ancestry: " +
                                       std::string(error.what()));
                }
            }
            return boundaryAncestry->contains(candidate);
        };
        if (incremental) {
            require(Err::InvalidInput, includesRevision(recipientBase),
                    "incremental bundle base is not an ancestor of incoming revision");
        }
        if (profile != nullptr) {
            profile->reachabilityMs = elapsedImportMs(reachStart, ImportClock::now());
        }
        const ImportClock::time_point branchStart = ImportClock::now();
        const bool exists = findStoredBranch(this->directory_, localBranch).has_value();
        Hash previousHead{};
        BundleImportStatus status = BundleImportStatus::Created;
        if (exists) {
            previousHead = branchHead(localBranch);
            if (previousHead == incoming ||
                // A previous import may already have advanced past this tip.
                (containsIndex(incoming) &&
                 revisionReachableFrom(previousHead, incoming))) {
                status = BundleImportStatus::UpToDate;
            } else {
                require(Err::Conflict, includesRevision(previousHead),
                        "replication would overwrite divergent branch history");
                status = BundleImportStatus::FastForward;
            }
        }
        if (profile != nullptr) {
            profile->branchCheckMs = elapsedImportMs(branchStart, ImportClock::now());
        }
        // Pre-release format: an imported branch MUST carry the same UUIDv7
        // on every replica. SHA-only legacy bundle import is intentionally rejected.
        require(Err::InvalidInput, incomingBranchId.has_value(),
                "legacy bundle has no UUIDv7 branch identity; unsupported pre-release format");
        const std::optional<BranchId> localId = branchIdLocked(localBranch);
        require(Err::Conflict, !localId || *localId == *incomingBranchId,
                "replication would rebind a different branch UUIDv7");
        for (const StoredBranch& other : readStoredBranches(this->directory_)) {
            require(Err::Conflict, other.name == localBranch || other.id != *incomingBranchId,
                    "branch UUIDv7 already belongs to another local name");
        }
        if (status == BundleImportStatus::UpToDate) {
            if (profile != nullptr) {
                profile->totalMs = elapsedImportMs(importStart, ImportClock::now());
            }
            return BundleImportResult{status, previousHead, 0};
        }
        const ImportClock::time_point writeStart = ImportClock::now();
        std::uint64_t imported = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const std::pair<Hash, BundleLocation> entry = entries.at(index);
            const bool present = containsIndex(entry.first);
            if (present) continue;
            Bytes payload = bundleObject(entry.first);
            require(Err::Internal, store(std::move(payload)) == entry.first,
                    "replication content hash changed before publication");
            ++imported;
            detail::faultCheckpoint("import.after.object");
        }
        if (profile != nullptr) {
            profile->objectWriteMs = elapsedImportMs(writeStart, ImportClock::now());
        }
        const ImportClock::time_point publishStart = ImportClock::now();
        syncPack(); // durable objects before an atomic branch-ref update
        detail::faultCheckpoint("import.after.sync");
        publishRef(localBranch, incoming, !exists, incomingBranchId);
        if (profile != nullptr) {
            profile->publicationMs = elapsedImportMs(publishStart, ImportClock::now());
            profile->totalMs = elapsedImportMs(importStart, ImportClock::now());
        }
        return BundleImportResult{status, incoming, imported};
    }
}

CompactionStats Repository::compact() {
    std::lock_guard<std::mutex> guarded(this->mutex_);
    ScopedFileLock locked(this->lockFd_);
    ScopedMaintenanceExclusive alone(this->lifetimeFd_);
    require(Err::Unavailable, !this->needsReopen_, "repository must be reopened after interrupted compaction");
    refreshIndex();
    const RevisionObjectReaderV2 loadV2 = [this](const Hash& hash) -> std::optional<Bytes> {
        if (!containsIndex(hash)) return std::nullopt;
        return load(hash);
    };
    const V2Audit v2 = auditPublishedV2(this->directory_, loadV2);

    CompactionStats stats{};
    stats.staleFilesRemoved = removeStalePackGenerations();
    stats.objectsBefore = static_cast<std::uint64_t>(this->indexCount_);
    stats.packBytesBefore = this->scanOffset_;
    struct stat oldIndex{};
    systemCall(::fstat(this->indexFd_, &oldIndex) == 0, "stat content index");
    require(Err::Corruption, oldIndex.st_size >= 0, "invalid index size");
    stats.indexBytesBefore = static_cast<std::uint64_t>(oldIndex.st_size);

    // The retention policy is intentionally conservative: preserve EVERY
    // revision on the parent chain of EVERY branch head, plus both trees and
    // every leaf payload (including reverse-reference index values).
    enum class ReachableKind { Revision, RevisionV2, Tree, Value };
    std::vector<std::pair<Hash, ReachableKind>> pending;
    for (const StoredBranch& branch : readStoredBranches(this->directory_)) {
        if (branch.revisionV2) {
            const std::map<Hash, RevisionId>::const_iterator published =
                v2.publishedIds.find(branch.head);
            require(Err::Corruption, published != v2.publishedIds.end() &&
                    published->second == *branch.revisionV2,
                    "v2 branch HEAD UUID/SHA does not match the published revision");
        }
        if (!isZero(branch.head))
            pending.emplace_back(branch.head,
                                 branch.revisionV2 ? ReachableKind::RevisionV2 : ReachableKind::Revision);
    }
    // Retain even unreferenced but *published* UUID bindings; no retained
    // UUID may end up pointing to a compacted-away immutable object.
    for (const Hash& hash : v2.recordedHashes)
        pending.emplace_back(hash, ReachableKind::RevisionV2);
    std::unordered_set<Hash, HashHasher> reachable;
    reachable.reserve(static_cast<std::size_t>(stats.objectsBefore));
    while (!pending.empty()) {
        const std::pair<Hash, ReachableKind> next = pending.back();
        pending.pop_back();
        if (isZero(next.first) || !reachable.insert(next.first).second) continue;
        if (next.second == ReachableKind::RevisionV2) {
            const std::map<Hash, Bytes>::const_iterator found = v2.logicalRevisions.find(next.first);
            require(Err::Corruption, found != v2.logicalRevisions.end(),
                    "unpublished UUIDv7 revision encountered during compaction");
            const Bytes& encoded = found->second;
            const Hash root = readHash(encoded, 1);
            const Hash referenceRoot = readHash(encoded, 33);
            pending.emplace_back(root, ReachableKind::Tree);
            pending.emplace_back(referenceRoot, ReachableKind::Tree);
            for (std::size_t i = 0; i < encoded[66]; ++i)
                pending.emplace_back(readHash(encoded, 67 + 32 * i), ReachableKind::RevisionV2);
        } else if (next.second == ReachableKind::Revision) {
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
    require(Err::Corruption, reachable.size() <= stats.objectsBefore, "invalid reachable object count");
    std::array<std::uint8_t, 16> random{};
    require(Err::Internal, RAND_bytes(random.data(), static_cast<int>(random.size())) == 1,
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
            require(Err::Corruption, header[0] == 'A' && header[1] == 'O' && header[2] == 'B' &&
                    header[3] == 'J' && header[4] == 1, "invalid existing pack header");
            const std::uint64_t size = readU64(header.data() + 37);
            require(Err::Corruption, size <= maxObjectBytes &&
                    this->scanOffset_ - oldPosition >= headerSize &&
                    size <= this->scanOffset_ - oldPosition - headerSize,
                    "invalid existing pack record boundary");
            Hash hash{};
            std::copy_n(header.begin() + 5, 32, hash.begin());
            if (reachable.contains(hash)) {
                const std::optional<Location> entry = indexLocation(hash);
                require(Err::Corruption, entry.has_value() &&
                        entry->offset == oldPosition + headerSize &&
                        entry->size == size, "pack and index mismatch during compaction");
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
        require(Err::Corruption, stats.objectsAfter == reachable.size(),
                "not all reachable objects were found in the pack");
        stats.packBytesAfter = newPosition;
        stats.indexBytesAfter = indexPosition;
        systemCall(::fsync(newPackFd) == 0, "sync compacted pack");
        systemCall(::fsync(newIndexFd) == 0, "sync compacted index");
        syncDirectory(this->directory_);
        detail::faultCheckpoint("compact.pack.before.pointer");

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
        detail::faultCheckpoint("compact.pack.after.pointer");
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
        clearIndex();
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
        } else {
            // The active pointer may now select a different immutable pack.
            // Continuing on the previous open descriptors could commit refs
            // to objects invisible after restart. Require a new handle.
            this->needsReopen_ = true;
        }
        throw;
    }
    return stats;
}

}  // namespace aowis::dvmdb
