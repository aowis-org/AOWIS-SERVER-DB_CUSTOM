// Review finding F6: scratch files live in <repository>/tmp (the pack's
// filesystem), never in TMPDIR. Linked against the fault-test engine build,
// which can force the named mkostemp() fallback used without O_TMPFILE.
#include "aowis/native/native_store.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using aowis::native::BundleLimits;
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::Repository;
using aowis::native::ScratchFile;
using aowis::native::StorageError;
using aowis::native::StorageErrorKind;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-native-scratch-" + std::string(label) + "-" + std::to_string(::getpid()) +
             "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++counter));
        std::filesystem::create_directories(this->path);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

// Sets an environment variable for one scope and restores the old value.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
        const char* previous = ::getenv(name);
        if (previous != nullptr) this->previous_ = std::string(previous);
        expect(::setenv(name, value.c_str(), 1) == 0, "setenv failed");
    }
    ~ScopedEnvironment() {
        if (this->previous_) ::setenv(this->name_, this->previous_->c_str(), 1);
        else ::unsetenv(this->name_);
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
private:
    const char* name_;
    std::optional<std::string> previous_;
};

Key key(std::uint8_t last) {
    Key result{};
    result.back() = last;
    return result;
}

void expectKind(StorageErrorKind expected, const std::string& label,
                const std::function<void()>& action) {
    try {
        action();
    } catch (const StorageError& error) {
        expect(error.kind() == expected, label + ": expected " + StorageError::kindName(expected) +
               ", got " + StorageError::kindName(error.kind()) + " (" + error.what() + ")");
        return;
    }
    throw std::runtime_error(label + ": no exception");
}

std::uintmax_t fileSize(int fd) {
    struct stat data{};
    expect(::fstat(fd, &data) == 0, "fstat failed");
    return static_cast<std::uintmax_t>(data.st_size);
}

std::size_t entries(const std::filesystem::path& directory) {
    std::size_t count = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        (void)entry;
        ++count;
    }
    return count;
}

// A model history whose verification uses every scratch-backed structure:
// bundle index, visit marks, trie summaries and (beyond 8192 edges) the
// external reference sort.
Hash buildModelHistory(Repository& repository) {
    repository.createBranch("main");
    std::vector<ModelChange> targets;
    for (unsigned i = 1; i <= 64; ++i)
        targets.push_back(ModelChange{key(static_cast<std::uint8_t>(i)), ModelRecord{1, {}, Bytes{1}}});
    repository.commitModel("main", targets);
    std::vector<Key> referenced;
    for (unsigned i = 1; i <= 64; ++i) referenced.push_back(key(static_cast<std::uint8_t>(i)));
    std::vector<ModelChange> sources;
    for (unsigned i = 0; i < 160; ++i) {
        Key source{};
        source[0] = 0x80;
        source[15] = static_cast<std::uint8_t>(i);
        sources.push_back(ModelChange{source, ModelRecord{1, referenced, Bytes{2}}});
    }
    return repository.commitModel("main", sources).id; // 160 x 64 = 10240 edges
}

void testLocationAndPermissions() {
    TestDirectory directory("location");
    const std::filesystem::path path = directory.path / "repo";
    {
        Repository repository(path);
        expect(repository.scratchDirectory() == path / "tmp", "scratch directory not inside repository");
        expect(std::filesystem::is_directory(path / "tmp"), "scratch directory not created");
    }
    std::filesystem::permissions(path / "tmp", std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace);
    Repository reopened(path);
    expect(std::filesystem::status(path / "tmp").permissions() == std::filesystem::perms::owner_all,
           "scratch directory permissions not restricted to the owner");
}

void testIndependentOfTmpdir(bool namedFallback) {
    TestDirectory directory(namedFallback ? "named" : "anonymous");
    ScopedEnvironment tmpdir("TMPDIR", (directory.path / "no-such-tmpdir").string());
    std::optional<ScopedEnvironment> named;
    if (namedFallback) named.emplace("AOWIS_NATIVE_TEST_NAMED_SCRATCH", "1");
    Repository source(directory.path / "source");
    const Hash head = buildModelHistory(source);
    source.exportBranchBundle("main", directory.path / "full.bundle");
    Repository target(directory.path / "target");
    expect(target.importBranchBundle(directory.path / "full.bundle", "main").head == head,
           "import without usable TMPDIR failed");
    const aowis::native::VerificationReport report = target.verifyRepository();
    expect(report.modelSnapshots >= 2, "verification did not inspect model snapshots");
    expect(entries(target.scratchDirectory()) == 0, "scratch files left behind after import");
    expect(entries(source.scratchDirectory()) == 0, "scratch files left behind after export");
}

void testLeftoverCleanup() {
    TestDirectory directory("leftovers");
    const std::filesystem::path path = directory.path / "repo";
    { Repository repository(path); }
    { std::ofstream(path / "tmp" / "aowis-scratch-bundle-index-Ab12Cd") << "crashed"; }
    { std::ofstream(path / "tmp" / "aowis-scratch-reference-edges-Zz99Yy") << "crashed"; }
    { std::ofstream(path / "tmp" / "keep-me.txt") << "unrelated"; }
    std::filesystem::create_directory(path / "tmp" / "aowis-scratch-directory");
    { Repository repository(path); }
    expect(!std::filesystem::exists(path / "tmp" / "aowis-scratch-bundle-index-Ab12Cd") &&
           !std::filesystem::exists(path / "tmp" / "aowis-scratch-reference-edges-Zz99Yy"),
           "leftover scratch files were not removed on open");
    expect(std::filesystem::exists(path / "tmp" / "keep-me.txt"), "unrelated file removed");
    expect(std::filesystem::is_directory(path / "tmp" / "aowis-scratch-directory"),
           "directory with scratch prefix removed");
}

void testUnsafeScratchPath() {
    TestDirectory directory("unsafe");
    const std::filesystem::path path = directory.path / "repo";
    { Repository repository(path); }
    std::filesystem::remove(path / "tmp");
    std::filesystem::create_directory(directory.path / "elsewhere");
    std::filesystem::create_directory_symlink(directory.path / "elsewhere", path / "tmp");
    expectKind(StorageErrorKind::Corruption, "symlinked scratch directory",
               [&] { Repository repository(path); });
    std::filesystem::remove(path / "tmp");
    { std::ofstream(path / "tmp") << "not a directory"; }
    bool refused = false;
    try { Repository repository(path); } catch (const StorageError&) { refused = true; }
    catch (const std::filesystem::filesystem_error&) { refused = true; }
    expect(refused, "regular file at scratch path accepted");
}

void testScratchFileApi() {
    TestDirectory directory("api");
    Repository repository(directory.path / "repo");
    ScratchFile file = repository.createScratchFile();
    expect(file.fd() >= 0, "scratch file has no descriptor");
    expect(entries(repository.scratchDirectory()) == 0, "anonymous scratch file has a visible name");
    const std::string text = "scratch";
    expect(::pwrite(file.fd(), text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()),
           "scratch file not writable");
    ScratchFile moved(std::move(file));
    expect(file.fd() == -1 && moved.fd() >= 0, "move did not transfer ownership");
    ScratchFile assigned;
    assigned = std::move(moved);
    expect(moved.fd() == -1 && fileSize(assigned.fd()) == text.size(), "move assignment lost data");
    const int released = assigned.release();
    expect(assigned.fd() == -1 && released >= 0, "release did not hand over the descriptor");
    ::close(released);
    {
        ScopedEnvironment named("AOWIS_NATIVE_TEST_NAMED_SCRATCH", "1");
        ScratchFile fallback = repository.createScratchFile();
        expect(fallback.fd() >= 0 && entries(repository.scratchDirectory()) == 0,
               "named fallback scratch file was not unlinked");
    }
}

void testDescriptorTransfer() {
    TestDirectory directory("descriptor");
    Repository source(directory.path / "source");
    const Hash head = buildModelHistory(source);
    const Hash base = source.history("main", 2).back().id;

    ScratchFile full = source.createScratchFile();
    source.exportBranchBundle("main", full.fd());
    expect(fileSize(full.fd()) > 0, "export to descriptor wrote nothing");
    Repository target(directory.path / "target");
    expect(target.importBranchBundle(full.fd(), "main").head == head, "import from descriptor failed");
    expect(target.importBranchBundle(full.fd(), "main").status ==
           aowis::native::BundleImportStatus::UpToDate, "descriptor import not repeatable");

    Repository partial(directory.path / "partial");
    ScratchFile older = source.createScratchFile();
    source.exportIncrementalBranchBundle("main", base, older.fd());
    expectKind(StorageErrorKind::Conflict, "incremental descriptor import without base",
               [&] { (void)partial.importBranchBundle(older.fd(), "main"); });

    expectKind(StorageErrorKind::InvalidInput, "export into a non-empty descriptor",
               [&] { source.exportBranchBundle("main", full.fd()); });
    ScratchFile limited = source.createScratchFile();
    expectKind(StorageErrorKind::LimitExceeded, "export over the byte limit",
               [&] { source.exportBranchBundle("main", limited.fd(), BundleLimits{4096, 0, 0}); });
    expect(fileSize(limited.fd()) == 0, "failed descriptor export left a partial bundle");
    int pipeEnds[2]{};
    expect(::pipe(pipeEnds) == 0, "pipe failed");
    expectKind(StorageErrorKind::InvalidInput, "import from a pipe",
               [&] { (void)target.importBranchBundle(pipeEnds[0], "other"); });
    expectKind(StorageErrorKind::InvalidInput, "export into a pipe",
               [&] { source.exportBranchBundle("main", pipeEnds[1]); });
    ::close(pipeEnds[0]);
    ::close(pipeEnds[1]);
}

void testInternalScratchBudget() {
    TestDirectory directory("quota");
    Repository source(directory.path / "source");
    const Hash head = buildModelHistory(source);
    const std::filesystem::path bundle = directory.path / "baseline.bundle";
    source.exportBranchBundle("main", bundle);
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "full export leaked internal scratch accounting");

    // Full export needs a 32 KiB initial hash table and additional sorter
    // storage; reject before writing into an output file.
    aowis::native::setInternalScratchLimit(1024);
    ScratchFile output = source.createScratchFile();
    expectKind(StorageErrorKind::Busy, "full export internal scratch budget",
               [&] { source.exportBranchBundle("main", output.fd()); });
    expect(fileSize(output.fd()) == 0, "scratch rejection left a partial bundle");
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "failed export did not release internal scratch accounting");

    Repository target(directory.path / "target");
    expectKind(StorageErrorKind::Busy, "import internal scratch budget",
               [&] { (void)target.importBranchBundle(bundle, "main"); });
    expect(target.listBranches().empty(), "failed import published a branch");
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "failed import did not release internal scratch accounting");

    // Budget can be reconfigured after operations finish. All paths work
    // again without recreating the repositories or changing stored history.
    aowis::native::setInternalScratchLimit(256ULL * 1024ULL * 1024ULL);
    source.exportBranchBundle("main", output.fd());
    expect(fileSize(output.fd()) > 0, "export did not resume after raising quota");
    expect(target.importBranchBundle(bundle, "main").head == head,
           "import did not resume after raising quota");
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "successful imports or exports leaked scratch accounting");
    target.verifyRepository();
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "repository verification leaked internal scratch accounting");
    aowis::native::setInternalScratchLimit(0);
}

// Exercise actual partial scratch writes, not just admission-limit rejection.
// The test-only writer persists half a hash-table entry before simulating
// ENOSPC, forcing RAII cleanup and correct repository reopening.
void testPartialScratchWrite() {
    TestDirectory directory("partial-write");
    const std::filesystem::path repo = directory.path / "source";
    const std::filesystem::path bundle = directory.path / "good.bundle";
    Repository source(repo);
    const Hash head = buildModelHistory(source);
    ScratchFile output = source.createScratchFile();
    {
        ScopedEnvironment point("AOWIS_NATIVE_TEST_FAULT", "scratch.partial.write");
        ScopedEnvironment action("AOWIS_NATIVE_TEST_ACTION", "error");
        expectKind(StorageErrorKind::ResourceExhausted, "partial scratch export write",
                   [&] { source.exportBranchBundle("main", output.fd()); });
    }
    expect(fileSize(output.fd()) == 0, "partial scratch export wrote output");
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "partial scratch export leaked quota reservations");
    source.exportBranchBundle("main", bundle);
    const std::filesystem::path targetPath = directory.path / "target";
    {
        Repository target(targetPath);
        {
            ScopedEnvironment point("AOWIS_NATIVE_TEST_FAULT", "scratch.partial.write");
            ScopedEnvironment action("AOWIS_NATIVE_TEST_ACTION", "error");
            expectKind(StorageErrorKind::ResourceExhausted, "partial scratch import write",
                       [&] { (void)target.importBranchBundle(bundle, "main"); });
        }
        expect(target.listBranches().empty(), "partial scratch import published branch");
        expect(aowis::native::internalScratchReservedBytes() == 0,
               "partial scratch import leaked quota reservations");
    }
    Repository reopened(targetPath);
    expect(reopened.listBranches().empty(), "partial scratch import published on reopen");
    reopened.verifyRepository();
    expect(reopened.importBranchBundle(bundle, "main").head == head,
           "import retry after partial scratch failed");
    reopened.verifyRepository();
    expect(aowis::native::internalScratchReservedBytes() == 0,
           "scratch quota remained reserved after retry");
}

} // namespace

int main() {
    try {
        testLocationAndPermissions();
        testIndependentOfTmpdir(false);
        testIndependentOfTmpdir(true);
        testLeftoverCleanup();
        testUnsafeScratchPath();
        testScratchFileApi();
        testDescriptorTransfer();
        testInternalScratchBudget();
        testPartialScratchWrite();
        std::cout << "Repository scratch storage tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Repository scratch storage test failed: " << error.what() << '\n';
        return 1;
    }
}
