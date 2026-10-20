#include "aowis/native/pack_experiment.h"
#include "aowis/native/native_store.h"

#include <algorithm>
#include <cerrno>
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace aowis::native {
namespace {
constexpr std::uint64_t originalHeaderSize = 45;
constexpr std::uint64_t blockHeaderSize = 44;
constexpr std::uint64_t candidateEntrySize = 48;
constexpr std::uint64_t originalEntrySize = 52;
constexpr std::uint64_t maxPayload = 64ULL * 1024ULL * 1024ULL;
constexpr std::array<std::uint8_t, 8> candidateMagic{'A','O','B','I','D','X','0','1'};

void check(bool valid, std::string_view description) {
    if (!valid) throw std::runtime_error(std::string(description));
}
std::uint32_t get32(const std::uint8_t* data) {
    std::uint32_t number = 0;
    for (unsigned i = 0; i < 4; ++i) number = (number << 8U) | data[i];
    return number;
}
std::uint64_t get64(const std::uint8_t* data) {
    std::uint64_t number = 0;
    for (unsigned i = 0; i < 8; ++i) number = (number << 8U) | data[i];
    return number;
}
void put32(std::uint8_t* target, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) target[i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
}
void put64(std::uint8_t* target, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) target[i] = static_cast<std::uint8_t>(value >> (56 - i * 8));
}
void readAt(int fd, std::uint8_t* target, std::size_t count, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < count) {
        const ssize_t readCount = ::pread(fd, target + done, count - done,
                                          static_cast<off_t>(offset + done));
        if (readCount < 0 && errno == EINTR) continue;
        check(readCount > 0, "read of pack/index failed or truncated");
        done += static_cast<std::size_t>(readCount);
    }
}
void writeAt(int fd, const std::uint8_t* data, std::size_t count, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < count) {
        const ssize_t wrote = ::pwrite(fd, data + done, count - done,
                                       static_cast<off_t>(offset + done));
        if (wrote < 0 && errno == EINTR) continue;
        check(wrote > 0, "write of candidate pack/index failed");
        done += static_cast<std::size_t>(wrote);
    }
}
std::uint64_t fileSize(int fd) {
    struct stat info{};
    check(::fstat(fd, &info) == 0 && info.st_size >= 0, "cannot stat pack/index");
    return static_cast<std::uint64_t>(info.st_size);
}
class File {
public:
    File(const std::filesystem::path& path, int flags) {
        this->fd_ = ::open(path.c_str(), flags | O_CLOEXEC, 0600);
        check(this->fd_ >= 0, "cannot open pack/index file");
    }
    ~File() { if (this->fd_ >= 0) ::close(this->fd_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int fd() const { return this->fd_; }
private:
    int fd_ = -1;
};
class ScopedLock {
public:
    explicit ScopedLock(int fd) : fd_(fd) { check(::flock(this->fd_, LOCK_EX) == 0, "cannot lock repository"); }
    ~ScopedLock() { ::flock(this->fd_, LOCK_UN); }
private:
    int fd_;
};

std::filesystem::path originalPackPath(const std::filesystem::path& repo) {
    const std::filesystem::path marker = repo / "objects.current";
    if (!std::filesystem::exists(marker)) return repo / "objects.pack";
    std::ifstream stream(marker, std::ios::binary);
    check(stream.good(), "cannot read objects.current");
    std::string line;
    std::getline(stream, line);
    check(line.size() == 38 && line.substr(0, 6) == "PACK1 ", "invalid pack marker");
    for (std::size_t i = 6; i < line.size(); ++i) {
        check((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f'),
              "invalid pack generation");
    }
    return repo / ("objects." + line.substr(6) + ".pack");
}

struct Entry {
    Hash hash{};
    std::uint64_t sourceOffset = 0;
    std::uint64_t candidateBlock = 0;
    std::uint32_t slot = 0;
    std::uint32_t size = 0;
};

void flushBlock(int packFd, int indexFd, Bytes& body,
                std::vector<Entry>& blockEntries,
                std::uint64_t& blockPosition, std::uint64_t& indexPosition,
                PackExperimentStats& stats) {
    if (blockEntries.empty()) return;
    check(body.size() <= std::numeric_limits<std::uint32_t>::max(), "candidate block too large");
    std::array<std::uint8_t, blockHeaderSize> header{};
    header[0] = 'A'; header[1] = 'O'; header[2] = 'B'; header[3] = 'K';
    put32(header.data() + 4, static_cast<std::uint32_t>(blockEntries.size()));
    put32(header.data() + 8, static_cast<std::uint32_t>(body.size()));
    const Hash blockHash = sha256(body);
    std::copy(blockHash.begin(), blockHash.end(), header.begin() + 12);
    writeAt(packFd, header.data(), header.size(), blockPosition);
    writeAt(packFd, body.data(), body.size(), blockPosition + blockHeaderSize);
    for (Entry& entry : blockEntries) {
        std::array<std::uint8_t, candidateEntrySize> encoded{};
        std::copy(entry.hash.begin(), entry.hash.end(), encoded.begin());
        put64(encoded.data() + 32, blockPosition);
        put32(encoded.data() + 40, entry.slot);
        put32(encoded.data() + 44, entry.size);
        writeAt(indexFd, encoded.data(), encoded.size(), indexPosition);
        indexPosition += candidateEntrySize;
        entry.candidateBlock = blockPosition;
    }
    blockPosition += blockHeaderSize + body.size();
    ++stats.blocks;
    body.clear();
    blockEntries.clear();
}

void verifyData(int packFd, int indexFd, std::uint64_t expectedCount) {
    const std::uint64_t packSize = fileSize(packFd);
    const std::uint64_t indexSize = fileSize(indexFd);
    check(indexSize >= candidateMagic.size() &&
          (indexSize - candidateMagic.size()) % candidateEntrySize == 0,
          "invalid candidate index size");
    std::array<std::uint8_t, 8> magic{};
    readAt(indexFd, magic.data(), magic.size(), 0);
    check(magic == candidateMagic, "candidate index magic mismatch");
    std::uint64_t blockPosition = 0;
    std::uint64_t indexPosition = candidateMagic.size();
    std::uint64_t objects = 0;
    while (blockPosition < packSize) {
        check(packSize - blockPosition >= blockHeaderSize, "truncated candidate block");
        std::array<std::uint8_t, blockHeaderSize> header{};
        readAt(packFd, header.data(), header.size(), blockPosition);
        check(std::memcmp(header.data(), "AOBK", 4) == 0, "invalid candidate block magic");
        const std::uint32_t count = get32(header.data() + 4);
        const std::uint32_t bytes = get32(header.data() + 8);
        check(count != 0 && bytes <= packSize - blockPosition - blockHeaderSize,
              "invalid candidate block boundary");
        Bytes body(bytes);
        readAt(packFd, body.data(), body.size(), blockPosition + blockHeaderSize);
        const Hash digest = sha256(body);
        check(std::equal(digest.begin(), digest.end(), header.begin() + 12),
              "candidate block digest mismatch");
        std::size_t offset = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            check(body.size() - offset >= 4, "truncated candidate item length");
            const std::uint32_t size = get32(body.data() + offset);
            check(size <= maxPayload && size <= body.size() - offset - 4,
                  "truncated candidate item body");
            check(indexSize - indexPosition >= candidateEntrySize,
                  "candidate index has fewer objects than pack");
            std::array<std::uint8_t, candidateEntrySize> idx{};
            readAt(indexFd, idx.data(), idx.size(), indexPosition);
            check(get64(idx.data() + 32) == blockPosition && get32(idx.data() + 40) == offset &&
                  get32(idx.data() + 44) == size, "candidate index location differs from pack");
            const Bytes payload(body.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                                body.begin() + static_cast<std::ptrdiff_t>(offset + 4 + size));
            const Hash expected = sha256(payload);
            check(std::equal(expected.begin(), expected.end(), idx.begin()),
                  "candidate object SHA-256 mismatch");
            offset += 4 + size;
            indexPosition += candidateEntrySize;
            ++objects;
        }
        check(offset == body.size(), "extra bytes in candidate block");
        blockPosition += blockHeaderSize + bytes;
    }
    check(blockPosition == packSize && indexPosition == indexSize && objects == expectedCount,
          "candidate pack object count mismatch");
}

Bytes readSourceValue(int fd, const Entry& entry) {
    Bytes data(entry.size);
    if (!data.empty()) readAt(fd, data.data(), data.size(), entry.sourceOffset);
    check(sha256(data) == entry.hash, "source random-read SHA-256 mismatch");
    return data;
}
Bytes readCandidateValue(int fd, const Entry& entry) {
    // The checked index contains the slot and size. Read only the payload:
    // validating SHA-256 authenticates the returned bytes and catches an
    // incorrect index location. The 4-byte record framing is checked during
    // full pack verification and index reconstruction, not on every lookup.
    const std::uint64_t offset = entry.candidateBlock + blockHeaderSize + entry.slot + 4;
    Bytes data(entry.size);
    if (!data.empty()) readAt(fd, data.data(), data.size(), offset);
    check(sha256(data) == entry.hash, "candidate random-read SHA-256 mismatch");
    return data;
}

} // namespace

PackExperimentStats runPackExperiment(const std::filesystem::path& repositoryDirectory,
                                      const std::filesystem::path& outputDirectory,
                                      std::uint32_t targetBlockBytes) {
    check(targetBlockBytes >= 1024 && targetBlockBytes <= 8 * 1024 * 1024,
          "target block size must be 1024..8388608 bytes");
    // Prevent compaction while the source pack is being read. Do not modify it.
    File maintenance(repositoryDirectory / "maintenance.lock", O_RDWR);
    check(::flock(maintenance.fd(), LOCK_SH) == 0, "cannot lock source repository lifetime");
    File repoLock(repositoryDirectory / "repo.lock", O_RDWR);
    ScopedLock lock(repoLock.fd());
    const std::filesystem::path inputPath = originalPackPath(repositoryDirectory);
    File source(inputPath, O_RDONLY);
    const std::uint64_t sourceSize = fileSize(source.fd());
    const std::filesystem::path originalSidecar = inputPath.parent_path() /
        (inputPath.filename().string().substr(0, inputPath.filename().string().size() - 5) + ".idx");
    check(std::filesystem::exists(originalSidecar), "original index is missing");
    const std::uint64_t sourceIndexSize = std::filesystem::file_size(originalSidecar);
    check(!std::filesystem::exists(outputDirectory), "experiment output path already exists");
    std::filesystem::create_directories(outputDirectory);
    File result(outputDirectory / "candidate.pack", O_RDWR | O_CREAT | O_EXCL);
    File resultIndex(outputDirectory / "candidate.idx", O_RDWR | O_CREAT | O_EXCL);
    writeAt(resultIndex.fd(), candidateMagic.data(), candidateMagic.size(), 0);
    PackExperimentStats stats{};
    stats.originalPackBytes = sourceSize;
    stats.originalIndexBytes = sourceIndexSize;
    const auto buildBegin = std::chrono::steady_clock::now();
    std::vector<Entry> entries;
    Bytes body;
    std::vector<Entry> blockEntries;
    std::uint64_t scan = 0;
    std::uint64_t candidatePos = 0;
    std::uint64_t indexPos = candidateMagic.size();
    while (scan < sourceSize) {
        check(sourceSize - scan >= originalHeaderSize, "truncated source pack header");
        std::array<std::uint8_t, originalHeaderSize> header{};
        readAt(source.fd(), header.data(), header.size(), scan);
        check(std::memcmp(header.data(), "AOBJ\1", 5) == 0, "source is not Stage 7 pack format");
        const std::uint64_t length = get64(header.data() + 37);
        check(length <= maxPayload && length <= sourceSize - scan - originalHeaderSize,
              "invalid source object length");
        Bytes payload(static_cast<std::size_t>(length));
        if (!payload.empty()) readAt(source.fd(), payload.data(), payload.size(), scan + originalHeaderSize);
        const Hash hash = sha256(payload);
        check(std::equal(hash.begin(), hash.end(), header.begin() + 5),
              "source content corruption during candidate build");
        check(payload.size() + 4 <= std::numeric_limits<std::uint32_t>::max(),
              "source object cannot fit a block");
        if (!blockEntries.empty() && body.size() + payload.size() + 4 > targetBlockBytes) {
            flushBlock(result.fd(), resultIndex.fd(), body, blockEntries, candidatePos, indexPos, stats);
        }
        const std::size_t offset = body.size();
        const std::size_t end = offset + 4 + payload.size();
        body.resize(end);
        put32(body.data() + offset, static_cast<std::uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), body.begin() + static_cast<std::ptrdiff_t>(offset + 4));
        Entry entry{};
        entry.hash = hash;
        entry.sourceOffset = scan + originalHeaderSize;
        entry.slot = static_cast<std::uint32_t>(offset);
        entry.size = static_cast<std::uint32_t>(payload.size());
        entries.push_back(entry);
        blockEntries.push_back(entry);
        scan += originalHeaderSize + length;
    }
    flushBlock(result.fd(), resultIndex.fd(), body, blockEntries, candidatePos, indexPos, stats);
    // Candidate offsets are deterministic from block boundaries, not from source offsets.
    // Read the finalized physical index in its original record order.
    check(fileSize(resultIndex.fd()) == candidateMagic.size() + entries.size() * candidateEntrySize,
          "candidate index size mismatch");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        std::array<std::uint8_t, candidateEntrySize> rec{};
        readAt(resultIndex.fd(), rec.data(), rec.size(), candidateMagic.size() + i * candidateEntrySize);
        check(std::equal(entries[i].hash.begin(), entries[i].hash.end(), rec.begin()),
              "candidate index order mismatch");
        entries[i].candidateBlock = get64(rec.data() + 32);
        entries[i].slot = get32(rec.data() + 40);
    }
    stats.objects = entries.size();
    stats.candidatePackBytes = candidatePos;
    stats.candidateIndexBytes = indexPos;
    stats.buildMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - buildBegin).count();
    const auto verifyBegin = std::chrono::steady_clock::now();
    verifyData(result.fd(), resultIndex.fd(), entries.size());
    stats.fullVerifyMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - verifyBegin).count();
    // Repeatable warm-cache sampling, not a claim about independent cold-cache I/O.
    stats.sampleReads = std::min<std::uint64_t>(10000, entries.size());
    std::vector<std::size_t> sample;
    sample.reserve(static_cast<std::size_t>(stats.sampleReads));
    for (std::uint64_t i = 0; i < stats.sampleReads; ++i) {
        sample.push_back(static_cast<std::size_t>((i * 2654435761ULL + 1013904223ULL) % entries.size()));
    }
    // Run the same sampled offsets on both stores with alternating order.
    // Discard the first pair as a warm-up and report medians rather than one
    // potentially biased Stage-7-first timing. These are PRELOCATED, warm-cache
    // payload reads with SHA-256 checks, not complete indexed hash lookups.
    auto timedReads = [&](bool candidate) {
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i : sample) {
            if (candidate) (void)readCandidateValue(result.fd(), entries[i]);
            else (void)readSourceValue(source.fd(), entries[i]);
        }
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
    };
    std::vector<double> originalTimes;
    std::vector<double> candidateTimes;
    constexpr unsigned pairedTrials = 7;
    for (unsigned trial = 0; trial < pairedTrials; ++trial) {
        double sourceTime = 0.0;
        double candidateTime = 0.0;
        if (trial % 2 == 0) {
            sourceTime = timedReads(false);
            candidateTime = timedReads(true);
        } else {
            candidateTime = timedReads(true);
            sourceTime = timedReads(false);
        }
        if (trial != 0) {
            originalTimes.push_back(sourceTime);
            candidateTimes.push_back(candidateTime);
        }
    }
    auto median = [](std::vector<double>& samples) {
        std::sort(samples.begin(), samples.end());
        const std::size_t middle = samples.size() / 2;
        return (samples[middle - 1] + samples[middle]) / 2.0;
    };
    stats.sampleTrials = static_cast<std::uint32_t>(originalTimes.size());
    stats.sourceReadMs = median(originalTimes);
    stats.candidateReadMs = median(candidateTimes);
    return stats;
}

void verifyPackExperiment(const std::filesystem::path& outputDirectory) {
    File candidate(outputDirectory / "candidate.pack", O_RDONLY);
    File index(outputDirectory / "candidate.idx", O_RDONLY);
    check(fileSize(index.fd()) >= candidateMagic.size(), "candidate index is truncated");
    const std::uint64_t entryCount = (fileSize(index.fd()) - candidateMagic.size()) / candidateEntrySize;
    verifyData(candidate.fd(), index.fd(), entryCount);
}


void rebuildPackExperimentIndex(const std::filesystem::path& outputDirectory) {
    File pack(outputDirectory / "candidate.pack", O_RDONLY);
    File index(outputDirectory / "candidate.idx", O_CREAT | O_EXCL | O_RDWR);
    writeAt(index.fd(), candidateMagic.data(), candidateMagic.size(), 0);
    const std::uint64_t packBytes = fileSize(pack.fd());
    std::uint64_t position = 0;
    std::uint64_t indexPosition = candidateMagic.size();
    while (position < packBytes) {
        check(packBytes - position >= blockHeaderSize, "truncated candidate block during rebuild");
        std::array<std::uint8_t, blockHeaderSize> header{};
        readAt(pack.fd(), header.data(), header.size(), position);
        check(std::memcmp(header.data(), "AOBK", 4) == 0, "invalid candidate block during rebuild");
        const std::uint32_t count = get32(header.data() + 4);
        const std::uint32_t length = get32(header.data() + 8);
        check(count != 0 && length <= packBytes - position - blockHeaderSize,
              "invalid candidate block bounds during rebuild");
        Bytes body(length);
        readAt(pack.fd(), body.data(), body.size(), position + blockHeaderSize);
        const Hash digest = sha256(body);
        check(std::equal(digest.begin(), digest.end(), header.begin() + 12),
              "candidate block digest mismatch during index rebuild");
        std::size_t offset = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            check(offset <= body.size() && body.size() - offset >= 4,
                  "candidate item truncated during index rebuild");
            const std::uint32_t size = get32(body.data() + offset);
            check(size <= maxPayload && size <= body.size() - offset - 4,
                  "candidate item size invalid during index rebuild");
            Bytes payload(body.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                          body.begin() + static_cast<std::ptrdiff_t>(offset + 4 + size));
            const Hash identity = sha256(payload);
            std::array<std::uint8_t, candidateEntrySize> entry{};
            std::copy(identity.begin(), identity.end(), entry.begin());
            put64(entry.data() + 32, position);
            put32(entry.data() + 40, static_cast<std::uint32_t>(offset));
            put32(entry.data() + 44, size);
            writeAt(index.fd(), entry.data(), entry.size(), indexPosition);
            indexPosition += candidateEntrySize;
            offset += 4 + size;
        }
        check(offset == body.size(), "unindexed candidate block content");
        position += blockHeaderSize + length;
    }
    check(position == packBytes, "candidate rebuild didn't reach pack end");
    check(::fsync(index.fd()) == 0, "cannot sync rebuilt candidate index");
    verifyData(pack.fd(), index.fd(), (indexPosition - candidateMagic.size()) / candidateEntrySize);
}

} // namespace aowis::native
