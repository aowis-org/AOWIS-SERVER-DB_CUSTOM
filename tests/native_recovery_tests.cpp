#include "aowis/native/native_store.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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
using aowis::native::PhysicalStore;
using aowis::native::Repository;

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

void baseline(const std::filesystem::path& path, PhysicalStore backend, bool model) {
    Repository repository(path, backend);
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

void recover(const std::filesystem::path& path, PhysicalStore backend,
             const Hash& original, bool afterPublish, bool model, bool compaction) {
    Repository repository(path, backend);
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
void runFault(std::string_view point, bool crash, PhysicalStore backend,
              bool model, bool compaction, bool afterPublish) {
    TestDirectory directory(point);
    baseline(directory.path / "repo", backend, model);
    Hash original{};
    {
        Repository repository(directory.path / "repo", backend);
        original = repository.head("main").id;
    }
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        const std::string selected(point);
        ::setenv("AOWIS_NATIVE_TEST_FAULT", selected.c_str(), 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", crash ? "crash" : "error", 1);
        try {
            Repository repository(directory.path / "repo", backend);
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
        recover(directory.path / "repo", backend, original, afterPublish, model, compaction);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(point) + ": " + error.what());
    }
}

void runImportFault(std::string_view point, bool crash, PhysicalStore backend,
                    bool afterPublish) {
    TestDirectory directory("import");
    const std::filesystem::path from = directory.path / "from";
    const std::filesystem::path to = directory.path / "to";
    const std::filesystem::path initial = directory.path / "initial.bundle";
    const std::filesystem::path incremental = directory.path / "incremental.bundle";
    Hash initialHead{};
    {
        Repository origin(from, backend);
        origin.createBranch("main");
        origin.commitModel("main", {ModelChange{key(1), ModelRecord{1, {}, value(11)}}});
        initialHead = origin.head("main").id;
        origin.exportBranchBundle("main", initial);
        origin.commitModel("main", {
            ModelChange{key(2), ModelRecord{1, {key(1)}, value(22)}}});
        origin.exportIncrementalBranchBundle("main", initialHead, incremental);
    }
    {
        Repository destination(to, backend);
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
            Repository destination(to, backend);
            (void)destination.importBranchBundle(incremental, "main");
        } catch (const std::exception&) { ::_exit(crash ? 95 : 94); }
        ::_exit(96);
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child, "import waitpid failed");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == (crash ? 93 : 94),
           "import did not reach fault point: " + std::string(point));
    Repository recovered(to, backend);
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

void testBackupAndSidecars(PhysicalStore backend) {
    TestDirectory directory("backup");
    const std::filesystem::path origin = directory.path / "origin";
    const std::filesystem::path backup = directory.path / "backup";
    baseline(origin, backend, true);
    // Backups are made with all repository handles closed. Copying while the
    // engine is live is NOT promised to yield a consistent filesystem snapshot.
    std::filesystem::copy(origin, backup, std::filesystem::copy_options::recursive);
    {
        Repository restored(backup, backend);
        restored.verifyRepository();
    }
    // Index sidecars are reconstructible. Simulate a crash that leaves only an
    // incomplete index suffix; object packs and commit markers remain intact.
    const std::filesystem::path index = origin / "objects.idx";
    const std::uintmax_t oldSize = std::filesystem::file_size(index);
    expect(oldSize > 16, "fixture lacks an index");
    std::filesystem::resize_file(index, oldSize - 7);
    {
        Repository restored(origin, backend);
        restored.verifyRepository();
        restored.commitModel("main", {
            ModelChange{key(3), ModelRecord{1, {}, value(33)}}});
        restored.verifyRepository();
    }
}

void verifyLocking(PhysicalStore backend) {
    TestDirectory directory("locking");
    baseline(directory.path / "repo", backend, false);
    int ready[2]{};
    int finish[2]{};
    expect(::pipe(ready) == 0 && ::pipe(finish) == 0, "pipe failed");
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        try {
            Repository repository(directory.path / "repo", backend);
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
        Repository repository(directory.path / "repo", backend);
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
    Repository repository(directory.path / "repo", backend);
    repository.verifyRepository();
}

void testInterruptedCompactionHandle(PhysicalStore backend, bool afterSwitch) {
    TestDirectory directory("compaction-handle");
    const std::filesystem::path path = directory.path / "repo";
    baseline(path, backend, false);
    {
        Repository repository(path, backend);
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
    Repository reopened(path, backend);
    reopened.verifyRepository();
    reopened.commit("main", {Change{key(6), value(66)}});
    reopened.verifyRepository();
}

void runBackend(PhysicalStore backend) {
    const std::vector<std::string_view> packPre{
        "pack.before.header", "pack.after.header", "pack.partial.body",
        "pack.after.body",
        "pack.before.fsync", "pack.after.fsync"
    };
    const std::vector<std::string_view>& checkpoints = packPre;
    for (std::string_view point : checkpoints) {
        for (bool crash : {true, false}) runFault(point, crash, backend, false, false, false);
    }
    for (std::string_view point : {
        "ref.partial.temp", "ref.before.fsync", "ref.after.fsync", "ref.before.rename",
        "ref.after.rename", "ref.after.dirsync"}) {
        for (bool crash : {true, false}) {
            const bool after = point == "ref.after.rename" || point == "ref.after.dirsync";
            runFault(point, crash, backend, false, false, after);
        }
    }
    // Repeat representative checkpoints with model-indexed revisions.
    for (std::string_view point : {"ref.before.rename", "ref.after.rename"}) {
        runFault(point, true, backend, true, false, point == "ref.after.rename");
    }
    for (bool crash : {true, false}) {
        runFault("compact.pack.before.pointer",
                 crash, backend, false, true, false);
        runFault("compact.pack.after.pointer",
                 crash, backend, false, true, false);
    }
    for (std::string_view point : {"ref.before.rename", "ref.after.rename",
                                   "import.after.sync"}) {
        for (bool crash : {true, false}) {
            runImportFault(point, crash, backend, point == "ref.after.rename");
        }
    }
    runImportFault("pack.after.fsync",
                   true, backend, false);
    testBackupAndSidecars(backend);
    testInterruptedCompactionHandle(backend, false);
    testInterruptedCompactionHandle(backend, true);
    verifyLocking(backend);
}
} // namespace

int main() {
    try {
        runBackend(PhysicalStore::Pack);
        std::cout << "Recovery, injected ENOSPC/crashes and locking: passed on pack storage\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native recovery test failed: " << error.what() << '\n';
        return 1;
    }
}
