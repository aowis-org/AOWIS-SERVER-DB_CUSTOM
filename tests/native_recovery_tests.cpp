#include "aowis/native/native_store.h"

#include <array>
#include <fcntl.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::Repository;
using aowis::native::StorageError;
using aowis::native::StorageErrorKind;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-native-recovery-" + std::string(label) + "-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++counter));
        std::filesystem::create_directories(this->path);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

Key key(std::uint8_t last) {
    Key result{};
    result.back() = last;
    return result;
}

Bytes value(std::uint8_t number) {
    return Bytes(1024, number);
}

void baseline(const std::filesystem::path& path, bool model) {
    Repository repository(path);
    repository.createBranch("main");
    if (model) {
        repository.commitModel("main", {
            ModelChange{key(1), ModelRecord{1, {}, value(11)}},
            ModelChange{key(2), ModelRecord{1, {key(1)}, value(12)}}
        });
    } else {
        repository.commit("main", {Change{key(1), value(11)}});
    }
    expect(repository.verifyRepository().branches == 1, "invalid fixture repository");
}

void recover(const std::filesystem::path& path,
             const Hash& original, bool afterPublish, bool model, bool compaction) {
    Repository repository(path);
    const Hash current = repository.head("main").id;
    if (compaction || !afterPublish) {
        expect(current == original, "unpublished operation changed branch head");
    } else {
        expect(current != original, "published operation did not update branch head");
    }
    if (model) {
        expect(repository.getModel("main", key(1)).has_value(),
               "model disappeared after recovery");
        expect(repository.getModel("main", key(2)).has_value(),
               "dependent model disappeared after recovery");
        expect(repository.referencingModel("main", key(1)) == std::vector<Key>{key(2)},
               "reverse-reference index inconsistent after recovery");
    } else {
        const std::optional<Bytes> data = repository.get("main", key(1));
        expect(data.has_value() && *data == value(11),
               "baseline record corrupted during recovery");
    }
    const aowis::native::VerificationReport report = repository.verifyRepository();
    expect(report.branches == 1 && report.revisions >= 2,
           "full verification failed to inspect recovered history");
    // A recovered repository must remain writable (not merely readable).
    if (model) {
        repository.commitModel("main", {
            ModelChange{key(3), ModelRecord{1, {}, value(33)}}});
    } else {
        repository.commit("main", {Change{key(3), value(33)}});
    }
    repository.verifyRepository();
}

// The production library is not linked into this test executable. Only this
// test target uses a duplicate, test-instrumented engine build.
void runFault(std::string_view point, bool crash,
              bool model, bool compaction, bool afterPublish) {
    TestDirectory directory(point);
    baseline(directory.path / "repo", model);
    Hash original{};
    {
        Repository repository(directory.path / "repo");
        original = repository.head("main").id;
    }
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        const std::string selected(point);
        ::setenv("AOWIS_NATIVE_TEST_FAULT", selected.c_str(), 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", crash ? "crash" : "error", 1);
        try {
            Repository repository(directory.path / "repo");
            if (compaction) {
                (void)repository.compact();
            } else if (model) {
                repository.commitModel("main", {
                    ModelChange{key(1), ModelRecord{1, {}, value(21)}}
                });
            } else {
                repository.commit("main", {Change{key(2), value(22)}});
            }
        } catch (const std::exception&) {
            ::_exit(crash ? 95 : 94);
        }
        ::_exit(96); // fault was not reached
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child, "waitpid failed");
    const int expected = crash ? 93 : 94;
    expect(WIFEXITED(status) && WEXITSTATUS(status) == expected,
           "fault checkpoint not reached: " + std::string(point) +
           "; child status=" + std::to_string(status));
    try {
        recover(directory.path / "repo", original, afterPublish, model, compaction);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(point) + ": " + error.what());
    }
}

void runImportFault(std::string_view point, bool crash,
                    bool afterPublish) {
    TestDirectory directory("import");
    const std::filesystem::path from = directory.path / "from";
    const std::filesystem::path to = directory.path / "to";
    const std::filesystem::path initial = directory.path / "initial.bundle";
    const std::filesystem::path incremental = directory.path / "incremental.bundle";
    Hash initialHead{};
    {
        Repository origin(from);
        origin.createBranch("main");
        origin.commitModel("main", {ModelChange{key(1), ModelRecord{1, {}, value(11)}}});
        initialHead = origin.head("main").id;
        origin.exportBranchBundle("main", initial);
        origin.commitModel("main", {
            ModelChange{key(2), ModelRecord{1, {key(1)}, value(22)}}});
        origin.exportIncrementalBranchBundle("main", initialHead, incremental);
    }
    {
        Repository destination(to);
        const aowis::native::BundleImportResult imported =
            destination.importBranchBundle(initial, "main");
        expect(imported.head == initialHead, "incorrect initial import");
    }
    const pid_t child = ::fork();
    expect(child >= 0, "import fork failed");
    if (child == 0) {
        const std::string selected(point);
        ::setenv("AOWIS_NATIVE_TEST_FAULT", selected.c_str(), 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", crash ? "crash" : "error", 1);
        try {
            Repository destination(to);
            (void)destination.importBranchBundle(incremental, "main");
        } catch (const std::exception&) { ::_exit(crash ? 95 : 94); }
        ::_exit(96);
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child, "import waitpid failed");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == (crash ? 93 : 94),
           "import did not reach fault point: " + std::string(point));
    Repository recovered(to);
    const Hash current = recovered.head("main").id;
    if (afterPublish) {
        expect(current != initialHead, "published import did not advance head");
        expect(recovered.getModel("main", key(2)).has_value(),
               "published import lost dependent model");
        expect(recovered.referencingModel("main", key(1)) == std::vector<Key>{key(2)},
               "published import lost reverse reference");
    } else {
        expect(current == initialHead, "failed import published branch head");
        expect(!recovered.getModel("main", key(2)).has_value(),
               "failed import exposed new model");
    }
    recovered.verifyRepository();
    if (!afterPublish) {
        const aowis::native::BundleImportResult retried =
            recovered.importBranchBundle(incremental, "main");
        expect(retried.head != initialHead, "retry did not complete import");
    }
    recovered.verifyRepository();
}

void testBackupAndSidecars() {
    TestDirectory directory("backup");
    const std::filesystem::path origin = directory.path / "origin";
    const std::filesystem::path backup = directory.path / "backup";
    baseline(origin, true);
    // Backups are made with all repository handles closed. Copying while the
    // engine is live is NOT promised to yield a consistent filesystem snapshot.
    std::filesystem::copy(origin, backup, std::filesystem::copy_options::recursive);
    {
        Repository restored(backup);
        restored.verifyRepository();
    }
    // Index sidecars are reconstructible. Simulate a crash that leaves only an
    // incomplete index suffix; object packs and commit markers remain intact.
    const std::filesystem::path index = origin / "objects.idx";
    const std::uintmax_t oldSize = std::filesystem::file_size(index);
    expect(oldSize > 16, "fixture lacks an index");
    std::filesystem::resize_file(index, oldSize - 7);
    {
        Repository restored(origin);
        restored.verifyRepository();
        restored.commitModel("main", {
            ModelChange{key(3), ModelRecord{1, {}, value(33)}}});
        restored.verifyRepository();
    }
}

void verifyLocking() {
    TestDirectory directory("locking");
    baseline(directory.path / "repo", false);
    int ready[2]{};
    int finish[2]{};
    expect(::pipe(ready) == 0 && ::pipe(finish) == 0, "pipe failed");
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        try {
            Repository repository(directory.path / "repo");
            const char yes = 'Y';
            if (::write(ready[1], &yes, 1) != 1) ::_exit(20);
            char signal = 0;
            if (::read(finish[0], &signal, 1) != 1) ::_exit(21);
            ::_exit(0);
        } catch (...) { ::_exit(22); }
    }
    ::close(ready[1]);
    ::close(finish[0]);
    char confirmed = 0;
    expect(::read(ready[0], &confirmed, 1) == 1 && confirmed == 'Y',
           "child could not open independent repository handle");
    bool refused = false;
    {
        Repository repository(directory.path / "repo");
        try { (void)repository.compact(); }
        catch (const std::exception&) { refused = true; }
    }
    const char done = 'D';
    expect(::write(finish[1], &done, 1) == 1, "release of child failed");
    int status = 0;
    expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "child failed during locking test");
    ::close(ready[0]);
    ::close(finish[1]);
    expect(refused, "compaction permitted a concurrent repository handle");
    Repository repository(directory.path / "repo");
    repository.verifyRepository();
}

void testInterruptedCompactionHandle(bool afterSwitch) {
    TestDirectory directory("compaction-handle");
    const std::filesystem::path path = directory.path / "repo";
    baseline(path, false);
    {
        Repository repository(path);
        const char* point = (afterSwitch ? "compact.pack.after.pointer" : "compact.pack.before.pointer");
        ::setenv("AOWIS_NATIVE_TEST_FAULT", point, 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", "error", 1);
        bool failed = false;
        try { (void)repository.compact(); }
        catch (const std::exception&) { failed = true; }
        ::unsetenv("AOWIS_NATIVE_TEST_FAULT");
        ::unsetenv("AOWIS_NATIVE_TEST_ACTION");
        expect(failed, "compaction did not observe injected failure");
        bool writable = true;
        try { repository.commit("main", {Change{key(5), value(55)}}); }
        catch (const std::exception&) { writable = false; }
        expect(writable != afterSwitch,
               "stale repository handle was writable after generation switch");
    }
    Repository reopened(path);
    reopened.verifyRepository();
    reopened.commit("main", {Change{key(6), value(66)}});
    reopened.verifyRepository();
}

// ---------------------------------------------------------------------------
// Torn pack tails (review finding F1). After power loss a file system may keep
// an extended file size while losing the data (zero-filled tail) or persist
// only part of a full-length body. Neither may make the repository
// unopenable, because no published ref can name an unsynced object.

void appendToPack(const std::filesystem::path& repository, const Bytes& bytes) {
    std::ofstream output(repository / "objects.pack", std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    expect(output.good(), "cannot append test bytes to pack");
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "cannot read " + path.string());
    return Bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void appendU64(Bytes& bytes, std::uint64_t number) {
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>(number >> (56U - 8U * i)));
}

// Well-formed header, wrong SHA-256: the shape of a body that never reached disk.
Bytes garbageRecord(std::uint64_t payloadSize, std::uint8_t fill) {
    Bytes record{'A', 'O', 'B', 'J', 1};
    record.insert(record.end(), 32, 0x5a);
    appendU64(record, payloadSize);
    record.insert(record.end(), static_cast<std::size_t>(payloadSize), fill);
    return record;
}

Bytes validRecord(const Bytes& payload) {
    const Hash hash = aowis::native::sha256(payload);
    Bytes record{'A', 'O', 'B', 'J', 1};
    record.insert(record.end(), hash.begin(), hash.end());
    appendU64(record, payload.size());
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

std::vector<std::filesystem::path> recoveredCopies(const std::filesystem::path& repository) {
    std::vector<std::filesystem::path> copies;
    const std::filesystem::path directory = repository / "recovered-pack-tails";
    if (!std::filesystem::exists(directory)) return copies;
    for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(directory)) copies.push_back(entry.path());
    return copies;
}

void testTornPackTail(std::string_view label, const Bytes& tail) {
    TestDirectory directory(label);
    const std::filesystem::path repo = directory.path / "repo";
    baseline(repo, true);
    Hash head{};
    {
        Repository repository(repo);
        head = repository.head("main").id;
    }
    const std::uintmax_t packBefore = std::filesystem::file_size(repo / "objects.pack");
    appendToPack(repo, tail);
    try {
        {
            Repository repository(repo);
            expect(repository.recoveredPackTailBytes() == tail.size(),
                   "recovered byte count differs from torn tail size");
            expect(std::filesystem::file_size(repo / "objects.pack") == packBefore,
                   "torn tail was not truncated to the last valid record");
            expect(repository.head("main").id == head, "torn tail recovery changed head");
            expect(repository.referencingModel("main", key(1)) == std::vector<Key>{key(2)},
                   "torn tail recovery damaged reverse references");
            repository.verifyRepository();
            repository.commitModel("main", {ModelChange{key(3), ModelRecord{1, {}, value(33)}}});
            repository.verifyRepository();
        }
        const std::vector<std::filesystem::path> copies = recoveredCopies(repo);
        expect(copies.size() == 1, "expected exactly one preserved torn tail copy");
        expect(readFile(copies.front()) == tail, "preserved torn tail differs from discarded bytes");
        Repository reopened(repo);
        expect(reopened.recoveredPackTailBytes() == 0, "clean reopen reported tail recovery");
        expect(reopened.getModel("main", key(3)).has_value(), "post-recovery commit lost");
        reopened.verifyRepository();
    } catch (const std::exception& error) {
        throw std::runtime_error("torn tail " + std::string(label) + ": " + error.what());
    }
}

void testTornPackTails() {
    testTornPackTail("short-header", Bytes{'A', 'O', 'B'});
    testTornPackTail("zero-filled", Bytes(45 + 100, 0));
    testTornPackTail("zero-short-header", Bytes(20, 0));
    testTornPackTail("garbage-body", garbageRecord(16, 0));
    testTornPackTail("short-body", [] {
        Bytes bytes = garbageRecord(4096, 0x33);
        bytes.resize(bytes.size() - 1000);
        return bytes;
    }());
    {
        Bytes several = garbageRecord(4096, 0x77);
        const Bytes second = garbageRecord(10, 0);
        several.insert(several.end(), second.begin(), second.end());
        several.insert(several.end(), 7, 'Z');
        testTornPackTail("several-records", several);
    }
    {
        // Larger than the 1 MiB search window, with a record magic straddling
        // the window boundary that does not start a valid record.
        Bytes large(3U * 1024U * 1024U + 17U, 0);
        const std::size_t straddle = 1024U * 1024U - 2U;
        const Bytes magic{'A', 'O', 'B', 'J', 1};
        std::copy(magic.begin(), magic.end(), large.begin() + static_cast<std::ptrdiff_t>(straddle));
        testTornPackTail("large-zero-filled", large);
    }
}

// Mid-pack damage is NOT a torn tail: a valid record after the damage proves
// the region was once durable. Fail closed and leave the pack untouched.
void testCorruptionBeforeValidRecord(std::string_view label, std::size_t gap) {
    TestDirectory directory(label);
    const std::filesystem::path repo = directory.path / "repo";
    baseline(repo, true);
    Bytes damage = garbageRecord(16, 0);
    damage.insert(damage.end(), gap, 0);
    const Bytes later = validRecord(value(77));
    damage.insert(damage.end(), later.begin(), later.end());
    appendToPack(repo, damage);
    const std::uintmax_t size = std::filesystem::file_size(repo / "objects.pack");
    bool refused = false;
    try { Repository repository(repo); }
    catch (const std::exception& error) {
        refused = std::string_view(error.what()).find("followed by a valid record") !=
                  std::string_view::npos;
    }
    expect(refused, std::string(label) + ": mid-pack corruption was not refused");
    expect(std::filesystem::file_size(repo / "objects.pack") == size,
           std::string(label) + ": refused open modified the pack");
    expect(recoveredCopies(repo).empty(), std::string(label) + ": refused open preserved a tail");
}

// Bit rot inside the newest PUBLISHED revision record must never be
// mistaken for a torn tail, even when nothing valid follows it.
void testDamagedPublishedHeadIsNotTruncated() {
    TestDirectory directory("damaged-head");
    const std::filesystem::path repo = directory.path / "repo";
    baseline(repo, true);
    std::filesystem::remove(repo / "objects.idx"); // force a full pack rescan
    const std::filesystem::path pack = repo / "objects.pack";
    const std::uintmax_t size = std::filesystem::file_size(pack);
    {
        std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekg(-1, std::ios::end);
        char byte = 0;
        stream.read(&byte, 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
        stream.seekp(-1, std::ios::end);
        stream.write(&byte, 1);
        expect(stream.good(), "cannot corrupt head revision record");
    }
    bool refused = false;
    try { Repository repository(repo); }
    catch (const std::exception& error) {
        refused = std::string_view(error.what()).find("references content inside the damaged") !=
                  std::string_view::npos;
    }
    expect(refused, "damaged published head was truncated as a torn tail");
    expect(std::filesystem::file_size(pack) == size, "damaged published head modified the pack");
}

// ---------------------------------------------------------------------------
// Stale compaction generations (review finding F11).

std::size_t countFiles(const std::filesystem::path& directory, std::string_view prefix,
                       std::string_view suffix) {
    std::size_t count = 0;
    for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(suffix)) ++count;
    }
    return count;
}

void crashCompaction(const std::filesystem::path& repo, const char* point) {
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        ::setenv("AOWIS_NATIVE_TEST_FAULT", point, 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", "crash", 1);
        try {
            Repository repository(repo);
            (void)repository.compact();
        } catch (...) { ::_exit(95); }
        ::_exit(96);
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 93, std::string("compaction did not crash at ") + point);
}

void testStaleGenerationCleanup() {
    TestDirectory directory("stale-generations");
    const std::filesystem::path repo = directory.path / "repo";
    baseline(repo, false);

    // Crash after writing a complete new generation, before publishing it.
    crashCompaction(repo, "compact.pack.before.pointer");
    expect(countFiles(repo, "objects.", ".pack") == 2, "fixture lacks an orphan generation");
    const std::string pointerTemp = ".objects.current.0123456789abcdef0123456789abcdef.tmp";
    { std::ofstream(repo / pointerTemp) << "PACK1 0123456789abcdef0123456789abcdef\n"; }
    { std::ofstream(repo / "objects.notes") << "unrelated administrator file\n"; }
    {
        Repository repository(repo);
        const aowis::native::CompactionStats stats = repository.compact();
        expect(stats.staleFilesRemoved == 3,
               "expected orphan pack, orphan index and pointer temp to be removed, got " +
               std::to_string(stats.staleFilesRemoved));
        repository.verifyRepository();
    }
    expect(countFiles(repo, "objects.", ".pack") == 1 &&
           countFiles(repo, "objects.", ".idx") == 1, "stale generation survived compaction");
    expect(!std::filesystem::exists(repo / pointerTemp), "pointer temp survived compaction");
    expect(std::filesystem::exists(repo / "objects.notes"), "compaction removed an unrelated file");

    // Crash after publishing the pointer, before retiring the old generation.
    crashCompaction(repo, "compact.pack.after.pointer");
    expect(countFiles(repo, "objects.", ".pack") == 2, "fixture lacks a retired generation");
    {
        Repository repository(repo);
        repository.verifyRepository();
        const aowis::native::CompactionStats stats = repository.compact();
        expect(stats.staleFilesRemoved == 2, "retired generation was not removed");
        repository.commit("main", {Change{key(7), value(77)}});
        repository.verifyRepository();
    }
    expect(countFiles(repo, "objects.", ".pack") == 1, "more than one generation remains");
    Repository reopened(repo);
    reopened.verifyRepository();
}

// ---------------------------------------------------------------------------
// A failure immediately before atomic publication must leave the requested
// path absent; the temporary descriptor and any named fallback are cleaned.

void testExportFailureBeforePublish() {
    TestDirectory directory("export-before-publish");
    const std::filesystem::path repo = directory.path / "repo";
    const std::filesystem::path bundle = directory.path / "main.aowbundle";
    baseline(repo, true);
    Repository repository(repo);
    ::setenv("AOWIS_NATIVE_TEST_FAULT", "export.before.publish", 1);
    ::setenv("AOWIS_NATIVE_TEST_ACTION", "error", 1);
    bool failed = false;
    try { repository.exportBranchBundle("main", bundle); }
    catch (const std::exception&) { failed = true; }
    ::unsetenv("AOWIS_NATIVE_TEST_FAULT");
    ::unsetenv("AOWIS_NATIVE_TEST_ACTION");
    expect(failed, "export did not observe injected failure before publication");
    expect(!std::filesystem::exists(bundle), "failed export left a bundle behind");
    repository.exportBranchBundle("main", bundle);
    Repository copy(directory.path / "copy");
    expect(copy.importBranchBundle(bundle, "main").head == repository.head("main").id,
           "export after a failed attempt is not importable");
}

// ENOSPC after part of a bundle payload reaches disk must not expose a
// partial bundle under the requested name. Descriptor exports are caller-owned
// and can still remain partial after a process crash.
void testPartialBundleOutput(bool crash, bool descriptor) {
    TestDirectory directory("partial-export");
    const std::filesystem::path repo = directory.path / "repo";
    const std::filesystem::path bundle = directory.path / "partial.bundle";
    baseline(repo, false);
    Hash original{};
    {
        Repository repository(repo);
        original = repository.head("main").id;
    }
    const pid_t child = ::fork();
    expect(child >= 0, "partial export fork failed");
    if (child == 0) {
        ::setenv("AOWIS_NATIVE_TEST_FAULT", "export.partial.object", 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", crash ? "crash" : "error", 1);
        try {
            Repository repository(repo);
            if (descriptor) {
                const int fd = ::open(bundle.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (fd < 0) ::_exit(97);
                try { repository.exportBranchBundle("main", fd); }
                catch (...) {
                    if (::lseek(fd, 0, SEEK_END) != 0) ::_exit(98);
                    ::close(fd);
                    throw;
                }
                ::close(fd);
            } else {
                repository.exportBranchBundle("main", bundle);
            }
        } catch (const StorageError& error) {
            ::_exit(!crash && error.kind() == StorageErrorKind::ResourceExhausted ? 94 : 95);
        } catch (...) { ::_exit(96); }
        ::_exit(97);
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child, "partial export waitpid failed");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == (crash ? 93 : 94),
           "partial export failure did not reach injected payload write");
    if (!descriptor) {
        expect(!std::filesystem::exists(bundle),
               "failed or crashed named export exposed a partial final bundle");
    } else {
        expect(std::filesystem::exists(bundle), "interrupted descriptor export fixture missing");
        // Normal exception cleanup truncates a caller-owned descriptor. An
        // abrupt process death cannot execute that cleanup.
        expect((std::filesystem::file_size(bundle) == 0) == !crash,
               "partial descriptor output cleanup mismatch");
        if (crash) {
            Repository destination(directory.path / "destination");
            bool rejected = false;
            try { (void)destination.importBranchBundle(bundle, "main"); }
            catch (const StorageError&) { rejected = true; }
            expect(rejected, "interrupted descriptor bundle was accepted as valid");
            expect(destination.listBranches().empty(),
                   "interrupted descriptor bundle published a branch");
            destination.verifyRepository();
        }
    }
    Repository reopened(repo);
    expect(reopened.head("main").id == original,
           "failed export changed source branch head");
    reopened.verifyRepository();
    const std::filesystem::path retry = directory.path / "retry.bundle";
    reopened.exportBranchBundle("main", retry);
    Repository restored(directory.path / "restored");
    expect(restored.importBranchBundle(retry, "main").head == original,
           "export retry failed after interruption");
    restored.verifyRepository();
}

// Both the anonymous-file and the named fallback paths must keep the final
// output invisible until a complete file has been fsynced. Once the name is
// published a crash or injected error may make the caller uncertain whether
// export succeeded, but the published bundle must be complete and importable.
void testAtomicNamedBundlePublication() {
    for (bool namedFallback : {false, true}) {
        for (std::string_view point : {"export.partial.object", "export.before.publish",
                                       "export.after.publish", "export.after.dirsync"}) {
            for (bool crash : {false, true}) {
                TestDirectory directory("atomic-bundle");
                const std::filesystem::path repo = directory.path / "repo";
                const std::filesystem::path bundle = directory.path / "main.bundle";
                baseline(repo, false);
                Hash expected{};
                {
                    Repository source(repo);
                    expected = source.head("main").id;
                }
                const pid_t child = ::fork();
                expect(child >= 0, "atomic export fork failed");
                if (child == 0) {
                    const std::string checkpoint(point);
                    ::setenv("AOWIS_NATIVE_TEST_FAULT", checkpoint.c_str(), 1);
                    ::setenv("AOWIS_NATIVE_TEST_ACTION", crash ? "crash" : "error", 1);
                    if (namedFallback) ::setenv("AOWIS_NATIVE_TEST_NAMED_SCRATCH", "1", 1);
                    try {
                        Repository source(repo);
                        source.exportBranchBundle("main", bundle);
                    } catch (const StorageError& error) {
                        ::_exit(!crash && error.kind() == StorageErrorKind::ResourceExhausted ? 94 : 95);
                    } catch (...) { ::_exit(96); }
                    ::_exit(97);
                }
                int status = 0;
                expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                       WEXITSTATUS(status) == (crash ? 93 : 94),
                       "atomic named export did not reach injected failure");
                const bool published = point == "export.after.publish" ||
                                       point == "export.after.dirsync";
                expect(std::filesystem::exists(bundle) == published,
                       "atomic named export visibility differs from publication point");
                if (published) {
                    Repository receiver(directory.path / "receiver");
                    expect(receiver.importBranchBundle(bundle, "main").head == expected,
                           "atomically published bundle cannot be imported");
                    receiver.verifyRepository();
                    // Re-export must never overwrite an already published file.
                    const std::uint64_t previousBytes = std::filesystem::file_size(bundle);
                    Repository source(repo);
                    bool rejected = false;
                    try { source.exportBranchBundle("main", bundle); }
                    catch (const StorageError&) { rejected = true; }
                    expect(rejected && std::filesystem::file_size(bundle) == previousBytes,
                           "export unexpectedly replaced existing bundle");
                } else {
                    Repository source(repo);
                    const std::filesystem::path retry = directory.path / "retry.bundle";
                    source.exportBranchBundle("main", retry);
                    Repository receiver(directory.path / "receiver");
                    expect(receiver.importBranchBundle(retry, "main").head == expected,
                           "export cannot be retried after aborted publication");
                }
                // A normal failure cleans up private fallback files. A crash
                // can orphan one; operators can remove it once no writer owns it.
                if (!crash) {
                    expect(countFiles(directory.path, ".aowis-export-", "") == 0,
                           "exception left a named export staging file behind");
                }
            }
        }
    }
    // A pre-existing symlink must never be followed or replaced.
    TestDirectory directory("bundle-symlink");
    const std::filesystem::path repo = directory.path / "repo";
    const std::filesystem::path target = directory.path / "other";
    const std::filesystem::path bundle = directory.path / "export.bundle";
    baseline(repo, false);
    { std::ofstream output(target); output << "unchanged\n"; }
    std::filesystem::create_symlink(target, bundle);
    Repository source(repo);
    bool rejected = false;
    try { source.exportBranchBundle("main", bundle); }
    catch (const StorageError&) { rejected = true; }
    expect(rejected && std::filesystem::is_symlink(bundle),
           "export unexpectedly replaced existing symlink");
    std::ifstream input(target);
    std::string contents;
    std::getline(input, contents);
    expect(contents == "unchanged", "export followed existing destination symlink");
}

void runBackend() {
    const std::vector<std::string_view> packPre{
        "pack.before.header", "pack.after.header", "pack.partial.body",
        "pack.after.body",
        "pack.before.fsync", "pack.after.fsync"
    };
    const std::vector<std::string_view>& checkpoints = packPre;
    for (std::string_view point : checkpoints) {
        for (bool crash : {true, false}) runFault(point, crash, false, false, false);
    }
    for (std::string_view point : {
        "ref.partial.temp", "ref.before.fsync", "ref.after.fsync", "ref.before.rename",
        "ref.after.rename", "ref.after.dirsync"}) {
        for (bool crash : {true, false}) {
            const bool after = point == "ref.after.rename" || point == "ref.after.dirsync";
            runFault(point, crash, false, false, after);
        }
    }
    // Repeat representative checkpoints with model-indexed revisions.
    for (std::string_view point : {"ref.before.rename", "ref.after.rename"}) {
        runFault(point, true, true, false, point == "ref.after.rename");
    }
    for (bool crash : {true, false}) {
        runFault("compact.pack.before.pointer",
                 crash, false, true, false);
        runFault("compact.pack.after.pointer",
                 crash, false, true, false);
    }
    for (std::string_view point : {"ref.before.rename", "ref.after.rename",
                                   "import.after.sync"}) {
        for (bool crash : {true, false}) {
            runImportFault(point, crash, point == "ref.after.rename");
        }
    }
    runImportFault("pack.after.fsync", true, false);
    // At least one imported object has entered the pack, but its revision
    // must remain unreachable until publication has completed.
    for (bool crash : {true, false}) runImportFault("import.after.object", crash, false);
    for (bool crash : {true, false}) {
        testPartialBundleOutput(crash, false);
        testPartialBundleOutput(crash, true);
    }
    testBackupAndSidecars();
    testInterruptedCompactionHandle(false);
    testInterruptedCompactionHandle(true);
    verifyLocking();
    testTornPackTails();
    testCorruptionBeforeValidRecord("mid-pack-adjacent", 0);
    testCorruptionBeforeValidRecord("mid-pack-distant", 1024U * 1024U + 3U);
    testDamagedPublishedHeadIsNotTruncated();
    testStaleGenerationCleanup();
    testExportFailureBeforePublish();
    testAtomicNamedBundlePublication();
}
} // namespace

int main() {
    try {
        runBackend();
        std::cout << "Recovery, torn pack tails, stale generations, injected ENOSPC/crashes and locking: passed on pack storage\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native recovery test failed: " << error.what() << '\n';
        return 1;
    }
}
