#include "aowis/native/native_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <map>
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
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelRecord;
using aowis::native::ModelChange;
using aowis::native::Repository;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char pattern[] = "/tmp/aowis-dvmdb-bundle-disk-index-XXXXXX";
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

std::uint64_t readU64(const Bytes& bytes, std::size_t position) {
    check(position <= bytes.size() && bytes.size() - position >= 8,
          "invalid bundle test fixture integer offset");
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value = (value << 8U) | bytes[position + index];
    return value;
}

void appendU64(Bytes& bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (56U - 8U * index)));
}

// Forge a bundle with an additional VALID, correctly hashed but unreachable
// payload. The reachability mask must reject it without publishing anything.
void appendUnreachableObject(const std::filesystem::path& source,
                             const std::filesystem::path& destination) {
    std::ifstream input(source, std::ios::binary);
    check(input.good(), "open valid bundle for orphan injection");
    const Bytes original((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    check(original.size() >= 48 && original[0] == 'A' && original[7] == '1',
          "expected full bundle fixture");
    const std::uint64_t count = readU64(original, 40);
    std::map<Hash, Bytes> objects;
    std::size_t offset = 48;
    for (std::uint64_t index = 0; index < count; ++index) {
        check(offset <= original.size() && original.size() - offset >= 40,
              "truncated valid bundle fixture");
        Hash hash{};
        std::copy_n(original.begin() + static_cast<std::ptrdiff_t>(offset),
                    hash.size(), hash.begin());
        const std::uint64_t size = readU64(original, offset + 32);
        offset += 40;
        check(size <= original.size() - offset, "invalid original bundle entry length");
        objects.emplace(hash, Bytes(original.begin() + static_cast<std::ptrdiff_t>(offset),
                                    original.begin() + static_cast<std::ptrdiff_t>(offset + size)));
        offset += static_cast<std::size_t>(size);
    }
    check(offset == original.size(), "trailing bytes in valid bundle fixture");
    const Bytes orphan{'V', 'o', 'r', 'p', 'h', 'a', 'n', 0x7d};
    check(objects.emplace(aowis::native::sha256(orphan), orphan).second,
          "unexpected orphan hash collision");
    Bytes output(original.begin(), original.begin() + 40);
    appendU64(output, static_cast<std::uint64_t>(objects.size()));
    for (const std::pair<const Hash, Bytes>& entry : objects) {
        output.insert(output.end(), entry.first.begin(), entry.first.end());
        appendU64(output, entry.second.size());
        output.insert(output.end(), entry.second.begin(), entry.second.end());
    }
    std::ofstream file(destination, std::ios::binary);
    check(file.good(), "open forged bundle output");
    file.write(reinterpret_cast<const char*>(output.data()),
               static_cast<std::streamsize>(output.size()));
    check(file.good(), "write forged bundle output");
}

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
                              std::string_view branch,
                              std::string_view expectedReason = {}) {
    const std::uint64_t objectsBefore = receiver.objectCount();
    try {
        (void)receiver.importBranchBundle(bundle, branch);
    } catch (const std::runtime_error& error) {
        check(expectedReason.empty() ||
              std::string_view(error.what()).find(expectedReason) != std::string_view::npos,
              "forged bundle rejected for an unexpected reason");
        check(receiver.objectCount() == objectsBefore,
              "invalid transfer wrote objects before passing preflight");
        check(receiver.listBranches().empty(),
              "invalid transfer published a branch");
        return;
    }
    throw std::runtime_error("corrupted transfer accepted");
}

// The same immutable trie hash may occur in a raw ancestor and a later
// model-indexed revision. Summaries MUST remain distinct by tree kind, since
// both kinds validate leaf values differently. This also forces retrieval of
// cached summaries while walking the two historical revisions.
void exerciseSharedTreesAcrossKinds() {
    TemporaryRoot temporary;
    ScopedTemporaryIndexDirectory temporaryIndex(temporary.path() / "scratch");
    Repository source(temporary.path() / "source");
    Repository destination(temporary.path() / "destination");
    source.createBranch("main");
    Key first{};
    Key second{};
    first.back() = 0x23;
    second.back() = 0x34;
    const ModelRecord one{1, {}, Bytes{'a', 'b', 'c'}};
    const ModelRecord two{2, {}, Bytes{'d', 'e', 'f'}};
    const aowis::native::Revision raw = source.commit("main", {
        Change{first, aowis::native::encodeModelRecord(one)},
        Change{second, aowis::native::encodeModelRecord(two)}});
    (void)source.commit("main", {Change{first, std::nullopt},
                                  Change{second, std::nullopt}});
    const aowis::native::Revision model = source.commitModel("main", {
        ModelChange{first, one}, ModelChange{second, two}});
    check(raw.root == model.root && raw.root != Hash{},
          "shared-tree fixture did not reuse identical immutable trie objects");
    check(!raw.modelIndexed && model.modelIndexed,
          "shared-tree fixture must cross raw/model revision kinds");
    const std::filesystem::path bundle = temporary.path() / "shared-kinds.aowbundle";
    source.exportBranchBundle("main", bundle);
    const BundleImportResult imported = destination.importBranchBundle(bundle, "main");
    check(imported.status == BundleImportStatus::Created && imported.head == model.id,
          "shared raw/model tree transfer failed");
    check(destination.revision(raw.id).root == destination.head("main").root,
          "imported shared trie roots differ");
    check(destination.getAtRevision(raw.id, first) ==
          std::optional<Bytes>(aowis::native::encodeModelRecord(one)),
          "raw historical record differs after shared-tree import");
    const std::optional<ModelRecord> restored =
        destination.getModelAtRevision(model.id, second);
    check(restored.has_value() && restored->kind == two.kind &&
          restored->references == two.references && restored->payload == two.payload,
          "model historical record differs after shared-tree import");
    const aowis::native::VerificationReport originalReport = source.verifyRepository();
    const aowis::native::VerificationReport importedReport = destination.verifyRepository();
    check(originalReport.revisions == importedReport.revisions &&
          originalReport.treeNodes == importedReport.treeNodes &&
          originalReport.modelSnapshots == importedReport.modelSnapshots,
          "file-backed import altered logical integrity statistics");
    temporaryIndex.checkEmpty();
}

void exercise() {
    TemporaryRoot temporary;
    ScopedTemporaryIndexDirectory temporaryIndex(temporary.path() / "scratch");
    Repository source(temporary.path() / "source");
    Repository destination(temporary.path() / "destination");
    Repository rejected(temporary.path() / "rejected");
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
    // A broad batch adds thousands of distinct objects without requiring
    // thousands of fsyncs; checks the mapped mark table under hash churn.
    std::vector<Change> batch;
    for (unsigned index = 0; index < 1200; ++index) {
        Key key{};
        key[0] = 0xf2;
        key[14] = static_cast<std::uint8_t>(index >> 8U);
        key[15] = static_cast<std::uint8_t>(index);
        batch.push_back(Change{key, Bytes(32 + index % 17, static_cast<std::uint8_t>(index))});
    }
    (void)source.commit("main", batch);
    // Include a much larger single object to exercise chunked hashing.
    Key largeKey{};
    largeKey[0] = 0xf1;
    (void)source.commit("main", {{largeKey, Bytes(2U * 1024U * 1024U, 0x4d)}});
    const Hash firstTip = source.head("main").id;
    const std::filesystem::path full = temporary.path() / "full.aowbundle";
    source.exportBranchBundle("main", full);
    check(std::filesystem::file_size(full) > 2U * 1024U * 1024U,
          "full bundle fixture should include large object");
    check(source.objectCount() > 3000, "insufficient objects for mapped visitation exercise");

    const BundleImportResult created = destination.importBranchBundle(full, "remote");
    check(created.status == BundleImportStatus::Created && created.head == firstTip &&
          created.objectsImported > 600, "full file-backed import was incomplete");
    temporaryIndex.checkEmpty();
    const aowis::native::VerificationReport originalReport = source.verifyRepository();
    const aowis::native::VerificationReport importedReport = destination.verifyRepository();
    check(originalReport.revisions == importedReport.revisions &&
          originalReport.treeNodes == importedReport.treeNodes &&
          originalReport.modelSnapshots == importedReport.modelSnapshots,
          "file-backed trie cache changed logical verification statistics");

    const std::filesystem::path orphan = temporary.path() / "orphan.aowbundle";
    appendUnreachableObject(full, orphan);
    rejectWithoutPublication(rejected, orphan, "remote", "unreachable objects");
    temporaryIndex.checkEmpty();

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

int main(int argc, char** argv) {
    try {
        const bool sharedOnly = argc == 2 &&
            std::string_view(argv[1]) == "--shared-kinds-only";
        check(argc == 1 || sharedOnly, "invalid bundle disk-index test arguments");
        if (!sharedOnly) exercise();
        exerciseSharedTreesAcrossKinds();
        std::cout << "AOWIS native bundle disk-index tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native bundle disk-index tests failed: " << error.what() << '\n';
        return 1;
    }
}
