#include "aowis/dvmdb/native_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::BundleLimits;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::Repository;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::Change;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <class Callable>
void rejectsLimit(Callable operation) {
    try {
        operation();
    } catch (const StorageError& error) {
        check(error.kind() == StorageErrorKind::LimitExceeded,
              "wrong error classification for transfer limit");
        check(std::string_view(error.what()).find("bundle transfer limit:") != std::string_view::npos,
              "wrong rejection reason for transfer limit");
        return;
    }
    throw std::runtime_error("transfer violating configured limit was accepted");
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char path[] = "/tmp/aowis-dvmdb-bundle-limits-XXXXXX";
        const char* created = ::mkdtemp(path);
        check(created != nullptr, "temporary directory creation failed");
        this->root_ = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->root_, ignored);
    }
    const std::filesystem::path& path() const { return this->root_; }
private:
    std::filesystem::path root_;
};

// The object limit counts distinct SHA-256 object identities, not graph
// edges. A tree shared between many revisions is counted only once.
std::uint64_t bundleObjectCount(const std::filesystem::path& bundle) {
    std::ifstream input(bundle, std::ios::binary);
    check(input.good(), "cannot read bundle object count");
    input.seekg(40);
    std::uint64_t count = 0;
    for (unsigned index = 0; index < 8; ++index) {
        const int value = input.get();
        check(value >= 0, "truncated bundle count header");
        count = (count << 8U) | static_cast<std::uint8_t>(value);
    }
    return count;
}

std::vector<char> contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot open bundle file");
    return std::vector<char>(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
}

Key testKey(unsigned sequence) {
    Key result{};
    result[12] = static_cast<std::uint8_t>(sequence >> 24U);
    result[13] = static_cast<std::uint8_t>(sequence >> 16U);
    result[14] = static_cast<std::uint8_t>(sequence >> 8U);
    result[15] = static_cast<std::uint8_t>(sequence);
    return result;
}

void testTraversalBounds() {
    TemporaryRoot temporary;
    Repository source(temporary.path() / "source");
    Repository destination(temporary.path() / "destination");
    source.createBranch("main");
    // Repeat a value across keys and revisions to exercise hash sharing.
    const Bytes repeated(180, 0x57);
    (void)source.commit("main", {{testKey(1), repeated}});
    const Hash base = source.head("main").id;
    const std::filesystem::path basePath = temporary.path() / "base.bundle";
    source.exportBranchBundle("main", basePath);
    check(destination.importBranchBundle(basePath, "main").head == base,
          "failed to seed incremental recipient");

    source.createBranch("side", "main");
    for (unsigned index = 2; index < 16; ++index) {
        (void)source.commit("main", {{testKey(index), repeated}});
    }
    for (unsigned index = 40; index < 46; ++index) {
        (void)source.commit("side", {{testKey(index), repeated}});
    }
    check(source.mergeBranches("main", "side").status == MergeStatus::Merged,
          "fixture did not create a two-parent merge history");
    const Hash tip = source.head("main").id;
    const std::filesystem::path full = temporary.path() / "full.bundle";
    const std::filesystem::path delta = temporary.path() / "delta.bundle";
    source.exportBranchBundle("main", full);
    source.exportIncrementalBranchBundle("main", base, delta);
    const std::uint64_t fullCount = bundleObjectCount(full);
    const std::uint64_t deltaCount = bundleObjectCount(delta);
    check(fullCount > deltaCount && deltaCount > 1, "invalid shared-history fixture");

    // Exactly the number of unique objects must succeed. A one-object-smaller
    // limit must reject BEFORE a path or an empty descriptor is opened.
    const std::filesystem::path boundedFull = temporary.path() / "bounded-full.bundle";
    source.exportBranchBundle("main", boundedFull, BundleLimits{0, fullCount, 0});
    check(contents(boundedFull) == contents(full), "exact-bound full export differs");
    const std::filesystem::path boundedDelta = temporary.path() / "bounded-delta.bundle";
    source.exportIncrementalBranchBundle("main", base, boundedDelta,
                                         BundleLimits{0, deltaCount, 0});
    check(contents(boundedDelta) == contents(delta), "exact-bound incremental export differs");

    const std::filesystem::path overFull = temporary.path() / "over-full.bundle";
    rejectsLimit([&] {
        source.exportBranchBundle("main", overFull, BundleLimits{0, fullCount - 1, 0});
    });
    check(!std::filesystem::exists(overFull), "rejected full export left output");
    const std::filesystem::path overDelta = temporary.path() / "over-delta.bundle";
    rejectsLimit([&] {
        source.exportIncrementalBranchBundle("main", base, overDelta,
                                             BundleLimits{0, deltaCount - 1, 0});
    });
    check(!std::filesystem::exists(overDelta), "rejected incremental export left output");

    // Do not replace a caller's existing file when the object count is too big.
    const std::filesystem::path existing = temporary.path() / "existing.bundle";
    {
        std::ofstream output(existing, std::ios::binary);
        output << "keep original contents";
    }
    const std::vector<char> previous = contents(existing);
    rejectsLimit([&] {
        source.exportBranchBundle("main", existing, BundleLimits{0, 1, 0});
    });
    check(contents(existing) == previous, "failed export replaced existing file");

    // Both descriptor variants leave caller-owned regular files empty on
    // an object-count rejection. The descriptors remain open and reusable.
    for (unsigned incremental = 0; incremental < 2; ++incremental) {
        const std::filesystem::path target = temporary.path() /
            (incremental ? "incremental-fd.bundle" : "full-fd.bundle");
        const int fd = ::open(target.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        check(fd >= 0, "cannot create descriptor export fixture");
        try {
            rejectsLimit([&] {
                if (incremental) {
                    source.exportIncrementalBranchBundle("main", base, fd, BundleLimits{0, 1, 0});
                } else {
                    source.exportBranchBundle("main", fd, BundleLimits{0, 1, 0});
                }
            });
            struct stat metadata{};
            check(::fstat(fd, &metadata) == 0 && metadata.st_size == 0,
                  "object-limited descriptor export wrote data");
        } catch (...) {
            ::close(fd);
            throw;
        }
        check(::close(fd) == 0, "close descriptor export fixture");
    }

    check(destination.importBranchBundle(boundedDelta, "main").head == tip,
          "exact-bound incremental export failed to import");
    (void)destination.verifyRepository();
    Repository independent(temporary.path() / "independent");
    check(independent.importBranchBundle(boundedFull, "main").head == tip,
          "exact-bound full export failed to import");
    (void)independent.verifyRepository();
}

// Larger than an external-sort chunk and several times the initial visited
// table capacity. Exercises table growth, multi-run sorting, hash sharing,
// canonical output order, repeated exports, import and rejection cleanup.
void testDiskBackedFullExport() {
    TemporaryRoot temporary;
    Repository source(temporary.path() / "source");
    source.createBranch("main");
    std::vector<Change> changes;
    changes.reserve(1300);
    for (unsigned index = 0; index < 1300; ++index) {
        Bytes value(40);
        value[0] = static_cast<std::uint8_t>(index >> 8U);
        value[1] = static_cast<std::uint8_t>(index);
        value[2] = static_cast<std::uint8_t>(index ^ 0x55U);
        changes.push_back(Change{testKey(index + 1), value});
    }
    const Hash head = source.commit("main", changes).id;
    const std::filesystem::path first = temporary.path() / "sorted-first.bundle";
    const std::filesystem::path second = temporary.path() / "sorted-second.bundle";
    source.exportBranchBundle("main", first);
    source.exportBranchBundle("main", second);
    check(contents(first) == contents(second), "disk-backed exports were nondeterministic");
    const std::uint64_t count = bundleObjectCount(first);
    check(count > 2048, "fixture did not cross the external sorter run boundary");

    // A bundle must retain the original sorted-by-hash ABI, not the order of
    // hash-table slots or the order in which graph traversal discovered them.
    std::ifstream input(first, std::ios::binary);
    check(input.good(), "cannot inspect disk-backed export");
    input.seekg(48);
    Hash previous{};
    for (std::uint64_t ordinal = 0; ordinal < count; ++ordinal) {
        Hash hash{};
        input.read(reinterpret_cast<char*>(hash.data()), static_cast<std::streamsize>(hash.size()));
        check(input.good() && previous < hash, "full export contains unordered or duplicate hashes");
        std::uint64_t size = 0;
        for (unsigned index = 0; index < 8; ++index) {
            const int byte = input.get();
            check(byte >= 0, "export has truncated object size");
            size = (size << 8U) | static_cast<std::uint8_t>(byte);
        }
        input.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        check(input.good(), "export has a truncated object payload");
        previous = hash;
    }
    check(input.peek() == std::char_traits<char>::eof(), "export has trailing data");

    Repository recipient(temporary.path() / "recipient");
    check(recipient.importBranchBundle(first, "main").head == head,
          "disk-backed full export could not be imported");
    (void)recipient.verifyRepository();

    const std::filesystem::path exact = temporary.path() / "exact.bundle";
    source.exportBranchBundle("main", exact, BundleLimits{0, count, 0});
    check(contents(first) == contents(exact), "exact limit changed external sort output");
    const std::filesystem::path failed = temporary.path() / "rejected.bundle";
    rejectsLimit([&] { source.exportBranchBundle("main", failed, BundleLimits{0, count - 1, 0}); });
    check(!std::filesystem::exists(failed), "over-limit external sort created output");
    check(std::filesystem::is_empty(source.scratchDirectory()),
          "external export left visible repository scratch files");
}

void exercise() {
    TemporaryRoot temporary;
    Repository source(temporary.path() / "sender");
    Repository destination(temporary.path() / "receiver");
    source.createBranch("main");
    Key key{};
    key[15] = 42;
    const Hash tip = source.commit("main", {{key, Bytes(1024, 0x6f)}}).id;
    const std::filesystem::path bundle = temporary.path() / "full.aowbundle";
    source.exportBranchBundle("main", bundle);
    const std::uint64_t length = std::filesystem::file_size(bundle);
    check(length > 1100, "unexpectedly small bundle fixture");
    const std::uint64_t objectsBefore = destination.objectCount();

    rejectsLimit([&] {
        (void)destination.importBranchBundle(bundle, "tracking", BundleLimits{length - 1, 0, 0});
    });
    rejectsLimit([&] {
        (void)destination.importBranchBundle(bundle, "tracking", BundleLimits{0, 1, 0});
    });
    rejectsLimit([&] {
        (void)destination.importBranchBundle(bundle, "tracking", BundleLimits{0, 0, 16});
    });
    check(destination.listBranches().empty() && destination.objectCount() == objectsBefore,
          "rejected transfer modified destination repository");

    const std::filesystem::path rejectedBytes = temporary.path() / "rejected-bytes.aowbundle";
    rejectsLimit([&] {
        source.exportBranchBundle("main", rejectedBytes, BundleLimits{length - 1, 0, 0});
    });
    check(!std::filesystem::exists(rejectedBytes), "failed export left partial output");
    const std::filesystem::path rejectedObjects = temporary.path() / "rejected-objects.aowbundle";
    rejectsLimit([&] {
        source.exportBranchBundle("main", rejectedObjects, BundleLimits{0, 1, 0});
    });
    check(!std::filesystem::exists(rejectedObjects), "object-limited export created an output");
    const std::filesystem::path rejectedIndividual = temporary.path() / "rejected-individual.aowbundle";
    rejectsLimit([&] {
        source.exportBranchBundle("main", rejectedIndividual, BundleLimits{0, 0, 16});
    });
    check(!std::filesystem::exists(rejectedIndividual), "individual-limited export left output");

    const BundleLimits exact{length, 10000, 2048};
    const std::filesystem::path exactExport = temporary.path() / "bounded.aowbundle";
    source.exportBranchBundle("main", exactExport, exact);
    check(std::filesystem::file_size(exactExport) == length, "bounded export altered bundle bytes");
    const aowis::dvmdb::BundleImportResult imported =
        destination.importBranchBundle(exactExport, "tracking", exact);
    check(imported.status == BundleImportStatus::Created && imported.head == tip,
          "valid bounded transfer rejected");
    check(destination.importBranchBundle(exactExport, "tracking", exact).status ==
              BundleImportStatus::UpToDate, "bounded retry was not idempotent");
    (void)destination.verifyRepository();
}
} // namespace

int main() {
    try {
        exercise();
        testTraversalBounds();
        testDiskBackedFullExport();
        std::cout << "AOWIS native bundle limit tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native bundle limit tests failed: " << error.what() << '\n';
        return 1;
    }
}
