#include "aowis/native/block_store_prototype.h"
#include "aowis/native/pack_experiment.h"
#include "native_test_faults.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace aowis::native {
namespace {
constexpr std::uint64_t blockHeaderBytes = 44;
constexpr std::uint64_t indexEntryBytes = 48;
constexpr std::uint64_t indexHeaderBytes = 8;
constexpr std::uint64_t manifestBytes = 72;
constexpr std::uint64_t maxPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::array<std::uint8_t, 8> indexMagic{'A','O','B','I','D','X','0','1'};
constexpr std::array<std::uint8_t, 8> manifestMagic{'A','O','B','C','M','T','0','1'};

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void store32(std::uint8_t* pointer, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) pointer[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}
void store64(std::uint8_t* pointer, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) pointer[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
}
std::uint32_t load32(const std::uint8_t* pointer) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | pointer[i];
    return value;
}
std::uint64_t load64(const std::uint8_t* pointer) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | pointer[i];
    return value;
}
void readExact(int fd, std::uint8_t* result, std::size_t count, std::uint64_t offset) {
    std::size_t done = 0;
    while (done != count) {
        const ssize_t n = ::pread(fd, result + done, count - done, static_cast<off_t>(offset + done));
        if (n == -1 && errno == EINTR) continue;
        check(n > 0, "block pack/index truncated or unreadable");
        done += static_cast<std::size_t>(n);
    }
}
void writeExact(int fd, const std::uint8_t* source, std::size_t count, std::uint64_t offset) {
    std::size_t done = 0;
    while (done != count) {
        const ssize_t n = ::pwrite(fd, source + done, count - done, static_cast<off_t>(offset + done));
        if (n == -1 && errno == EINTR) continue;
        check(n > 0, "cannot write block pack/index");
        done += static_cast<std::size_t>(n);
    }
}
std::uint64_t sizeOf(int fd) {
    struct stat status{};
    check(::fstat(fd, &status) == 0 && status.st_size >= 0, "cannot stat block pack/index");
    return static_cast<std::uint64_t>(status.st_size);
}
void flush(int fd) { check(::fsync(fd) == 0, "fsync failed"); }
void syncDirectory(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(fd >= 0, "cannot open block-store directory");
    const int result = ::fsync(fd);
    ::close(fd);
    check(result == 0, "cannot fsync block-store directory");
}
void closeFd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

// Independent full scan: pack is authoritative; index is purely derived.
// A committed block's SHA-256 and every object's SHA-256 must be verifiable.
template <typename OnObject>
std::uint64_t walkPack(int fd, std::uint64_t limit, OnObject callback) {
    std::uint64_t cursor = 0;
    std::uint64_t blocks = 0;
    while (cursor < limit) {
        check(limit - cursor >= blockHeaderBytes, "truncated committed block header");
        std::array<std::uint8_t, blockHeaderBytes> head{};
        readExact(fd, head.data(), head.size(), cursor);
        check(std::memcmp(head.data(), "AOBK", 4) == 0, "invalid committed block magic");
        const std::uint32_t count = load32(head.data() + 4);
        const std::uint32_t length = load32(head.data() + 8);
        check(count > 0 && length <= limit - cursor - blockHeaderBytes &&
              length <= 8U * 1024U * 1024U + maxPayloadBytes + 4U,
              "invalid committed block boundary");
        Bytes body(length);
        if (length) readExact(fd, body.data(), body.size(), cursor + blockHeaderBytes);
        const Hash blockHash = sha256(body);
        check(std::equal(blockHash.begin(), blockHash.end(), head.begin() + 12),
              "committed block checksum mismatch");
        std::size_t offset = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            check(offset <= body.size() && body.size() - offset >= 4,
                  "truncated committed object size");
            const std::uint32_t size = load32(body.data() + offset);
            check(size <= maxPayloadBytes && size <= body.size() - offset - 4,
                  "invalid committed object size");
            Bytes payload(body.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                          body.begin() + static_cast<std::ptrdiff_t>(offset + 4 + size));
            callback(sha256(payload), cursor, static_cast<std::uint32_t>(offset), size);
            offset += 4 + size;
        }
        check(offset == body.size(), "unparsed bytes in committed block");
        cursor += blockHeaderBytes + length;
        ++blocks;
    }
    return blocks;
}
}

std::filesystem::path BlockStorePrototype::activeDirectory(
    const std::filesystem::path& root) {
    const std::filesystem::path pointer = root / "candidate.active";
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(pointer, error);
    if (status.type() == std::filesystem::file_type::not_found) return root;
    check(!error && status.type() == std::filesystem::file_type::regular,
          "invalid block generation pointer");
    std::ifstream stream(pointer, std::ios::binary);
    check(stream.good(), "cannot open block generation pointer");
    const std::string value(std::istreambuf_iterator<char>{stream},
                            std::istreambuf_iterator<char>{});
    check(value.size() == 41 && value.substr(0, 8) == "AOBGEN1 " &&
          value.back() == '\n', "invalid block generation pointer");
    for (std::size_t i = 8; i < 40; ++i) {
        check((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'),
              "invalid block generation identifier");
    }
    const std::filesystem::path selected = root /
        ("generation-" + value.substr(8, 32));
    check(!std::filesystem::is_symlink(selected) &&
          std::filesystem::is_directory(selected),
          "published block generation is missing");
    return selected;
}

void BlockStorePrototype::createEmpty(const std::filesystem::path& directory) {
    check(!std::filesystem::exists(directory), "block directory already exists");
    check(std::filesystem::create_directory(directory), "cannot create block directory");
    int pack = ::open((directory / "candidate.pack").c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(pack >= 0, "cannot create empty block pack");
    flush(pack);
    closeFd(pack);
    const int index = ::open((directory / "candidate.idx").c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(index >= 0, "cannot create empty block index");
    try {
        writeExact(index, indexMagic.data(), indexMagic.size(), 0);
        flush(index);
    } catch (...) {
        ::close(index);
        throw;
    }
    ::close(index);
    syncDirectory(directory);
}

std::size_t BlockStorePrototype::HashHasher::operator()(const Hash& hash) const noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (std::uint8_t byte : hash) value = (value ^ byte) * 1099511628211ULL;
    return static_cast<std::size_t>(value);
}

BlockStorePrototype::BlockStorePrototype(const std::filesystem::path& directory,
                                         std::uint32_t targetBlockBytes)
    : directory_(directory), blockTarget_(targetBlockBytes) {
    check(targetBlockBytes >= 1024 && targetBlockBytes <= 8U * 1024U * 1024U,
          "invalid prototype block target");
    try {
        check(std::filesystem::is_directory(directory_), "candidate directory missing");
        this->lockFd_ = ::open((directory_ / "candidate.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        check(this->lockFd_ >= 0, "cannot open candidate lock");
        check(::flock(this->lockFd_, LOCK_EX | LOCK_NB) == 0,
              "candidate already opened in another process or handle");
        this->activeDir_ = activeDirectory(this->directory_);
        this->packFd_ = ::open((activeDir_ / "candidate.pack").c_str(), O_RDWR | O_CLOEXEC);
        check(this->packFd_ >= 0, "cannot open candidate pack");
        this->indexFd_ = ::open((activeDir_ / "candidate.idx").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        check(this->indexFd_ >= 0, "cannot open candidate index");
        const std::filesystem::path marker = activeDir_ / "candidate.current";
        if (!std::filesystem::exists(marker)) {
            if (sizeOf(this->indexFd_) == 0) this->rebuildIndex(sizeOf(this->packFd_));
            // First activation of an offline experiment: authenticate EVERYTHING.
            verifyPackExperiment(activeDir_);
            this->packBytes_ = sizeOf(this->packFd_);
            this->indexBytes_ = sizeOf(this->indexFd_);
            check((this->indexBytes_ - indexHeaderBytes) % indexEntryBytes == 0,
                  "invalid initial candidate index length");
            flush(this->packFd_);
            flush(this->indexFd_);
            this->writeManifest(this->packBytes_, this->indexBytes_,
                                (this->indexBytes_ - indexHeaderBytes) / indexEntryBytes, 0);
        } else {
            const int markerFd = ::open(marker.c_str(), O_RDONLY | O_CLOEXEC);
            check(markerFd >= 0, "cannot open committed marker");
            std::array<std::uint8_t, manifestBytes> data{};
            const std::uint64_t length = sizeOf(markerFd);
            if (length == data.size()) readExact(markerFd, data.data(), data.size(), 0);
            ::close(markerFd);
            check(length == data.size() &&
                  std::equal(manifestMagic.begin(), manifestMagic.end(), data.begin()),
                  "bad committed marker header");
            const Bytes markerBody(data.begin(), data.begin() + 40);
            const Hash expected = sha256(markerBody);
            check(std::equal(expected.begin(), expected.end(), data.begin() + 40),
                  "committed marker SHA-256 mismatch");
            this->packBytes_ = load64(data.data() + 8);
            this->indexBytes_ = load64(data.data() + 16);
            const std::uint64_t count = load64(data.data() + 24);
            this->generation_ = load64(data.data() + 32);
            check(this->indexBytes_ >= indexHeaderBytes &&
                  (this->indexBytes_ - indexHeaderBytes) % indexEntryBytes == 0 &&
                  count == (this->indexBytes_ - indexHeaderBytes) / indexEntryBytes,
                  "invalid committed index size/count");
            check(sizeOf(this->packFd_) >= this->packBytes_,
                  "published block pack is truncated");
            // Material beyond published offsets is uncommitted (e.g. process crash).
            if (sizeOf(this->packFd_) != this->packBytes_) {
                check(::ftruncate(this->packFd_, static_cast<off_t>(this->packBytes_)) == 0,
                      "cannot discard unpublished pack tail");
                flush(this->packFd_);
            }
            if (sizeOf(this->indexFd_) > this->indexBytes_) {
                check(::ftruncate(this->indexFd_, static_cast<off_t>(this->indexBytes_)) == 0,
                      "cannot discard unpublished index tail");
                flush(this->indexFd_);
            }
            if (sizeOf(this->indexFd_) < this->indexBytes_) this->rebuildIndex(this->packBytes_);
        }
        try {
            this->refreshLocations();
        } catch (const std::runtime_error&) {
            // Sidecar corruption is repairable only by rebuilding from the
            // authenticated committed pack, never by guessing index entries.
            this->rebuildIndex(this->packBytes_);
            this->refreshLocations();
        }
    } catch (...) {
        closeFd(this->indexFd_);
        closeFd(this->packFd_);
        closeFd(this->lockFd_);
        throw;
    }
}

BlockStorePrototype::~BlockStorePrototype() {
    closeFd(this->indexFd_);
    closeFd(this->packFd_);
    closeFd(this->lockFd_);
}

void BlockStorePrototype::writeManifest(std::uint64_t packBytes,
                                        std::uint64_t indexBytes,
                                        std::uint64_t objects,
                                        std::uint64_t generation) {
    std::array<std::uint8_t, manifestBytes> marker{};
    std::copy(manifestMagic.begin(), manifestMagic.end(), marker.begin());
    store64(marker.data() + 8, packBytes);
    store64(marker.data() + 16, indexBytes);
    store64(marker.data() + 24, objects);
    store64(marker.data() + 32, generation);
    const Hash digest = sha256(Bytes(marker.begin(), marker.begin() + 40));
    std::copy(digest.begin(), digest.end(), marker.begin() + 40);
    const std::filesystem::path temp = activeDir_ / "candidate.current.pending";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    check(fd >= 0, "cannot open pending candidate marker");
    try {
        detail::faultPartialWrite(fd, marker.data(), marker.size(), 0,
                                  "block.partial.manifest");
        writeExact(fd, marker.data(), marker.size(), 0);
        flush(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    detail::faultCheckpoint("block.before.manifest.rename");
    check(::rename(temp.c_str(), (activeDir_ / "candidate.current").c_str()) == 0,
          "cannot publish candidate commit marker");
    detail::faultCheckpoint("block.after.manifest.rename");
    syncDirectory(activeDir_);
    detail::faultCheckpoint("block.after.manifest.dirsync");
}

void BlockStorePrototype::rebuildIndex(std::uint64_t committedPackBytes) {
    // Use a temporary index: a failure while verifying the pack leaves the
    // prior index untouched. The commit marker remains the source of truth.
    const auto temp = activeDir_ / "candidate.idx.rebuilding";
    const int fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    check(fd >= 0, "cannot open index rebuild file");
    try {
        writeExact(fd, indexMagic.data(), indexMagic.size(), 0);
        std::uint64_t position = indexHeaderBytes;
        (void)walkPack(this->packFd_, committedPackBytes,
            [&](const Hash& hash, std::uint64_t block, std::uint32_t slot, std::uint32_t size) {
                std::array<std::uint8_t, indexEntryBytes> encoded{};
                std::copy(hash.begin(), hash.end(), encoded.begin());
                store64(encoded.data() + 32, block);
                store32(encoded.data() + 40, slot);
                store32(encoded.data() + 44, size);
                writeExact(fd, encoded.data(), encoded.size(), position);
                position += indexEntryBytes;
            });
        check(this->indexBytes_ == 0 || position == this->indexBytes_,
              "rebuilt index object count differs from committed marker");
        flush(fd);
        ::close(fd);
    } catch (...) {
        ::close(fd);
        ::unlink(temp.c_str());
        throw;
    }
    closeFd(this->indexFd_);
    check(::rename(temp.c_str(), (activeDir_ / "candidate.idx").c_str()) == 0,
          "cannot replace derived candidate index");
    syncDirectory(activeDir_);
    this->indexFd_ = ::open((activeDir_ / "candidate.idx").c_str(), O_RDWR | O_CLOEXEC);
    check(this->indexFd_ >= 0, "cannot reopen rebuilt index");
}

void BlockStorePrototype::refreshLocations() {
    this->locations_.clear();
    this->blocks_ = 0;
    const std::uint64_t size = sizeOf(this->indexFd_);
    check(size == this->indexBytes_ && size >= indexHeaderBytes, "committed candidate index size mismatch");
    std::array<std::uint8_t, indexHeaderBytes> magic{};
    readExact(this->indexFd_, magic.data(), magic.size(), 0);
    check(magic == indexMagic, "invalid candidate index magic");
    const std::uint64_t entryCount = (size - indexHeaderBytes) / indexEntryBytes;
    check(entryCount <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()),
          "candidate index has too many entries");
    this->locations_.reserve(static_cast<std::size_t>(entryCount));
    // The index is written in pack order. Batch decoding avoids a separate
    // pread syscall for every tiny object on large repositories.
    constexpr std::size_t entriesPerRead = 4096;
    std::array<std::uint8_t, entriesPerRead * indexEntryBytes> buffer{};
    std::uint64_t precedingBlock = 0;
    bool first = true;
    for (std::uint64_t offset = indexHeaderBytes; offset < size;) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(entriesPerRead, (size - offset) / indexEntryBytes));
        readExact(this->indexFd_, buffer.data(), count * indexEntryBytes, offset);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* bytes = buffer.data() + i * indexEntryBytes;
            Hash hash{};
            std::copy_n(bytes, hash.size(), hash.begin());
            Location loc{load64(bytes + 32), load32(bytes + 40), load32(bytes + 44)};
            check(loc.block <= this->packBytes_ && this->packBytes_ - loc.block >= blockHeaderBytes &&
                  loc.slot <= this->packBytes_ - loc.block - blockHeaderBytes &&
                  loc.size <= this->packBytes_ - loc.block - blockHeaderBytes - loc.slot &&
                  loc.size <= maxPayloadBytes,
                  "invalid indexed block location");
            check(first || loc.block >= precedingBlock, "candidate index block order is invalid");
            if (first || loc.block != precedingBlock) ++this->blocks_;
            first = false;
            precedingBlock = loc.block;
            check(this->locations_.emplace(hash, loc).second,
                  "duplicate content hash in committed index");
        }
        offset += count * indexEntryBytes;
    }
    check(this->locations_.size() == entryCount,
          "committed candidate index contains duplicate entries");
}

std::vector<Hash> BlockStorePrototype::putBatch(const std::vector<Bytes>& values) {
    std::vector<Hash> results;
    results.reserve(values.size());
    std::vector<HashedBytes> distinct;
    distinct.reserve(values.size());
    std::unordered_set<Hash, HashHasher> seen;
    seen.reserve(values.size());
    for (const Bytes& data : values) {
        check(data.size() <= maxPayloadBytes, "object exceeds 64 MiB");
        const Hash hash = sha256(data);
        results.push_back(hash);
        if (seen.insert(hash).second) distinct.emplace_back(hash, &data);
    }
    // The physical writer accepts pre-deduplicated hashes. Its other caller is
    // Repository's SHA-256-keyed staging map, which is unique by construction.
    this->putHashedBatch(distinct);
    return results;
}

void BlockStorePrototype::putHashedBatch(const std::vector<HashedBytes>& values) {
    check(!this->needsReopen_, "block store requires reopening after a failed commit");
    // The input is already a stable, unique vector of hash/payload references.
    // Keep only positions for newly inserted objects: copying their hashes into
    // a second pending vector and then into a third publication vector consumes
    // tens of MiB for large model imports.
    std::vector<std::size_t> pendingIndices;
    pendingIndices.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const Bytes& data = *values[i].second;
        check(data.size() <= maxPayloadBytes, "object exceeds 64 MiB");
        if (!this->locations_.contains(values[i].first)) pendingIndices.push_back(i);
    }
    if (pendingIndices.empty()) return;
    // Keep only physical locations until publication, not another array of
    // full content hashes. No in-memory location is published before the marker.
    std::vector<Location> pendingLocations(pendingIndices.size());
    // From the first physical append onward, any error leaves this handle
    // unusable. Reopening resolves the authoritative published marker and
    // discards its orphan tails; no later call may accidentally publish them.
    this->needsReopen_ = true;
    std::uint64_t packCursor = this->packBytes_;
    std::uint64_t indexCursor = this->indexBytes_;
    Bytes body;
    body.reserve(this->blockTarget_);
    std::vector<std::size_t> inBlock;
    std::vector<std::uint8_t> indexBuffer;
    indexBuffer.reserve((this->blockTarget_ / 32U + 1U) * indexEntryBytes);
    std::uint64_t newBlocks = 0;
    const auto flushBlock = [&] {
        if (inBlock.empty()) return;
        std::array<std::uint8_t, blockHeaderBytes> head{};
        std::copy_n("AOBK", 4, head.begin());
        check(body.size() <= std::numeric_limits<std::uint32_t>::max(), "block is too large");
        store32(head.data() + 4, static_cast<std::uint32_t>(inBlock.size()));
        store32(head.data() + 8, static_cast<std::uint32_t>(body.size()));
        const Hash digest = sha256(body);
        std::copy(digest.begin(), digest.end(), head.begin() + 12);
        detail::faultCheckpoint("block.before.header");
        writeExact(this->packFd_, head.data(), head.size(), packCursor);
        detail::faultCheckpoint("block.after.header");
        detail::faultPartialWrite(this->packFd_, body.data(), body.size(),
                                  packCursor + blockHeaderBytes, "block.partial.body");
        writeExact(this->packFd_, body.data(), body.size(), packCursor + blockHeaderBytes);
        detail::faultCheckpoint("block.after.body");
        indexBuffer.resize(inBlock.size() * indexEntryBytes);
        std::size_t slot = 0;
        for (std::size_t i = 0; i < inBlock.size(); ++i) {
            const std::size_t pendingIndex = inBlock[i];
            const HashedBytes& entry = values[pendingIndices[pendingIndex]];
            std::uint8_t* encoded = indexBuffer.data() + i * indexEntryBytes;
            std::copy(entry.first.begin(), entry.first.end(), encoded);
            store64(encoded + 32, packCursor);
            store32(encoded + 40, static_cast<std::uint32_t>(slot));
            store32(encoded + 44, static_cast<std::uint32_t>(entry.second->size()));
            pendingLocations[pendingIndex] = Location{packCursor,
                static_cast<std::uint32_t>(slot), static_cast<std::uint32_t>(entry.second->size())};
            slot += 4 + entry.second->size();
        }
        writeExact(this->indexFd_, indexBuffer.data(), indexBuffer.size(), indexCursor);
        indexCursor += indexBuffer.size();
        packCursor += blockHeaderBytes + body.size();
        ++newBlocks;
        inBlock.clear();
        body.clear();
    };
    for (std::size_t i = 0; i < pendingIndices.size(); ++i) {
        const Bytes& data = *values[pendingIndices[i]].second;
        const std::size_t size = data.size();
        if (!inBlock.empty() && body.size() + size + 4 > this->blockTarget_) flushBlock();
        const std::size_t slot = body.size();
        check(slot <= std::numeric_limits<std::uint32_t>::max(), "block slot exceeds 32 bits");
        body.resize(slot + 4 + size);
        store32(body.data() + slot, static_cast<std::uint32_t>(size));
        std::copy(data.begin(), data.end(), body.begin() + static_cast<std::ptrdiff_t>(slot + 4));
        inBlock.push_back(i);
    }
    flushBlock();
    detail::faultCheckpoint("block.before.pack.fsync");
    flush(this->packFd_);
    detail::faultCheckpoint("block.after.pack.fsync");
    flush(this->indexFd_);
    detail::faultCheckpoint("block.after.index.fsync");
    // Publish bytes durably before exposing their locations or any revision ref.
    writeManifest(packCursor, indexCursor,
                        static_cast<std::uint64_t>(this->locations_.size()) + pendingIndices.size(),
                        this->generation_ + 1);
    this->packBytes_ = packCursor;
    this->indexBytes_ = indexCursor;
    ++this->generation_;
    for (std::size_t i = 0; i < pendingIndices.size(); ++i) {
        this->locations_.emplace(values[pendingIndices[i]].first, pendingLocations[i]);
    }
    this->blocks_ += newBlocks;
    this->needsReopen_ = false;
}

std::optional<Bytes> BlockStorePrototype::get(const Hash& hash) const {
    check(!this->needsReopen_, "block store requires reopening after a failed commit");
    const auto it = this->locations_.find(hash);
    if (it == this->locations_.end()) return std::nullopt;
    const Location& loc = it->second;
    Bytes payload(loc.size);
    if (!payload.empty()) readExact(this->packFd_, payload.data(), payload.size(),
                                    loc.block + blockHeaderBytes + loc.slot + 4);
    check(sha256(payload) == hash, "indexed block object SHA-256 mismatch");
    return payload;
}

bool BlockStorePrototype::contains(const Hash& hash) const {
    check(!this->needsReopen_, "block store requires reopening after a failed commit");
    return this->locations_.contains(hash);
}
std::vector<Hash> BlockStorePrototype::hashes() const {
    check(!this->needsReopen_, "block store requires reopening after a failed commit");
    std::vector<Hash> result;
    result.reserve(this->locations_.size());
    for (const auto& item : this->locations_) result.push_back(item.first);
    std::sort(result.begin(), result.end());
    return result;
}
BlockStorePrototype::Stats BlockStorePrototype::stats() const {
    check(!this->needsReopen_, "block store requires reopening after a failed commit");
    return {static_cast<std::uint64_t>(this->locations_.size()), this->packBytes_,
            this->indexBytes_, this->blocks_};
}
void BlockStorePrototype::verify() const {
    check(!this->needsReopen_, "block store requires reopening after a failed commit");
    verifyPackExperiment(this->activeDir_);
    check(sizeOf(this->packFd_) == this->packBytes_ &&
          sizeOf(this->indexFd_) == this->indexBytes_,
          "physical block pack differs from published sizes");
}
} // namespace aowis::native
