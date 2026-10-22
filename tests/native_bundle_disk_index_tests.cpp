#include "aowis/native/native_store.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {
using aowis::native::BundleImportStatus;
using aowis::native::BundleImportResult;
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::PhysicalStore;
using aowis::native::Repository;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char pattern[] = "/tmp/aowis-db-bundle-disk-index-XXXXXX";
        const char* result = ::mkdtemp(pattern);
        check(result != nullptr, "create unique test directory");
        this->directory_ = result;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->directory_, ignored);
    }
    const std::filesystem::path& path() const { return this->directory_; }
private:
    std::filesystem::path directory_;
};

class ScopedTemporaryIndexDirectory {
public:
    explicit ScopedTemporaryIndexDirectory(const std::filesystem::path& directory) {
        const char* original = ::getenv("TMPDIR");
        if (original != nullptr) this->previous_ = original;
        std::filesystem::create_directories(directory);
        this->directory_ = directory;
        check(::setenv("TMPDIR", this->directory_.c_str(), 1) == 0,
              "set temporary index directory");
    }
    ~ScopedTemporaryIndexDirectory() {
        if (this->previous_) ::setenv("TMPDIR", this->previous_->c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
    void checkEmpty() const {
        check(std::filesystem::is_empty(this->directory_),
              "replication import leaked a temporary index file");
    }
private:
    std::optional<std::string> previous_;
    std::filesystem::path directory_;
};

void corruptTail(const std::filesystem::path& source,
                 const std::filesystem::path& destination) {
    std::filesystem::copy_file(source, destination);
    std::fstream stream(destination, std::ios::binary | std::ios::in | std::ios::out);
    check(stream.good(), "open corrupted bundle fixture");
    stream.seekg(-1, std::ios::end);
    char character = 0;
    stream.read(&character, 1);
    check(stream.good(), "read corrupted bundle fixture");
    character ^= 0x40;
    stream.seekp(-1, std::ios::end);
    stream.write(&character, 1);
    stream.flush();
    check(stream.good(), "write corrupted bundle fixture");
}

void rejectWithoutPublication(Repository& receiver, const std::filesystem::path& bundle,
                              std::string_view branch) {
    const std::uint64_t objectsBefore = receiver.objectCount();
    try {
        (void)receiver.importBranchBundle(bundle, branch);
    } catch (const std::runtime_error&) {
        check(receiver.objectCount() == objectsBefore,
              "invalid transfer wrote objects before passing preflight");
        check(receiver.listBranches().empty(),
              "invalid transfer published a branch");
        return;
    }
    throw std::runtime_error("corrupted transfer accepted");
}

void exercise(PhysicalStore sourceBackend, PhysicalStore destinationBackend) {
    TemporaryRoot temporary;
    ScopedTemporaryIndexDirectory temporaryIndex(temporary.path() / "scratch");
    Repository source(temporary.path() / "source", sourceBackend);
    Repository destination(temporary.path() / "destination", destinationBackend);
    Repository rejected(temporary.path() / "rejected", destinationBackend);
    source.createBranch("main");

    // Many small changes generate many revision, trie and immutable payload
    // objects, including repeated keys in distinct historical snapshots.
    for (unsigned index = 0; index < 320; ++index) {
        Key key{};
        key[14] = static_cast<std::uint8_t>((index % 160) >> 8);
        key[15] = static_cast<std::uint8_t>(index % 160);
        Bytes payload(97 + (index % 23) * 73, static_cast<std::uint8_t>(index));
        (void)source.commit("main", {{key, std::move(payload)}});
    }
    // Include a much larger single object to exercise chunked hashing.
    Key largeKey{};
    largeKey[0] = 0xf1;
    (void)source.commit("main", {{largeKey, Bytes(2U * 1024U * 1024U, 0x4d)}});
    const Hash firstTip = source.head("main").id;
    const std::filesystem::path full = temporary.path() / "full.aowbundle";
    source.exportBranchBundle("main", full);
    check(std::filesystem::file_size(full) > 2U * 1024U * 1024U,
          "full bundle fixture should include large object");
    check(source.objectCount() > 600, "insufficient historical objects for index exercise");

    const BundleImportResult created = destination.importBranchBundle(full, "remote");
    check(created.status == BundleImportStatus::Created && created.head == firstTip &&
          created.objectsImported > 600, "full file-backed import was incomplete");
    temporaryIndex.checkEmpty();
    (void)destination.verifyRepository();

    const std::filesystem::path corrupted = temporary.path() / "corrupt.aowbundle";
    corruptTail(full, corrupted);
    rejectWithoutPublication(rejected, corrupted, "remote");
    temporaryIndex.checkEmpty();

    for (unsigned index = 0; index < 90; ++index) {
        Key key{};
        key[13] = 0x01;
        key[15] = static_cast<std::uint8_t>(index);
        (void)source.commit("main", {{key, Bytes(90 + index, static_cast<std::uint8_t>(index))}});
    }
    const Hash secondTip = source.head("main").id;
    const std::filesystem::path incremental = temporary.path() / "incremental.aowbundle";
    source.exportIncrementalBranchBundle("main", firstTip, incremental);
    const BundleImportResult updated = destination.importBranchBundle(incremental, "remote");
    check(updated.status == BundleImportStatus::FastForward && updated.head == secondTip,
          "incremental file-backed index import failed");
    check(destination.importBranchBundle(incremental, "remote").status ==
          BundleImportStatus::UpToDate, "incremental retry lost idempotence");
    temporaryIndex.checkEmpty();
    (void)destination.verifyRepository();

    // A delta from a tip to itself has zero entries. The index must be able
    // to represent it without creating or mapping a zero-length file.
    const std::filesystem::path noOp = temporary.path() / "noop.aowbundle";
    source.exportIncrementalBranchBundle("main", secondTip, noOp);
    check(destination.importBranchBundle(noOp, "remote").status ==
          BundleImportStatus::UpToDate, "empty incremental transfer was rejected");
    temporaryIndex.checkEmpty();

    // A valid transfer is still rejected when it would overwrite local work;
    // the finalized temporary index must be released on this failure path.
    Key localKey{};
    localKey[0] = 0xed;
    const Hash localTip = destination.commit("remote", {{localKey, Bytes{0x91}}}).id;
    const std::uint64_t beforeDivergence = destination.objectCount();
    try {
        (void)destination.importBranchBundle(incremental, "remote");
        throw std::runtime_error("divergent imported branch was incorrectly accepted");
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()).find("divergent") != std::string_view::npos,
              "divergence test rejected for the wrong reason");
    }
    check(destination.head("remote").id == localTip &&
          destination.objectCount() == beforeDivergence,
          "rejected divergent transfer changed the destination");
    temporaryIndex.checkEmpty();
}
} // namespace

int main() {
    try {
        const std::array<PhysicalStore, 2> backends{
            PhysicalStore::Stage7Pack, PhysicalStore::ExperimentalBlocks};
        for (PhysicalStore source : backends)
            for (PhysicalStore destination : backends)
                exercise(source, destination);
        std::cout << "AOWIS native bundle disk-index tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native bundle disk-index tests failed: " << error.what() << '\n';
        return 1;
    }
}
