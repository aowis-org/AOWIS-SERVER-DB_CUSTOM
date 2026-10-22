#include "aowis/native/native_store.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::MergeStatus;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::Repository;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Key key(std::uint32_t value) {
    Key uuid{};
    for (unsigned position = 0; position < 4; ++position)
        uuid[12U + position] = static_cast<std::uint8_t>(value >> (position * 8U));
    return uuid;
}

struct TestDirectory {
    TestDirectory() {
        std::filesystem::path pattern = std::filesystem::temp_directory_path() /
            "aowis-pack-operations-XXXXXXXX";
        std::string name = pattern.string();
        char* result = ::mkdtemp(name.data());
        check(result != nullptr, "cannot create unique test directory");
        this->path = result;
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

void exercise() {
    TestDirectory root;
    const std::filesystem::path source = root.path / "source";
    const std::filesystem::path destination = root.path / "destination";
    const std::filesystem::path initialBundle = root.path / "baseline.aowbundle";
    const std::filesystem::path deltaBundle = root.path / "update.aowbundle";
    constexpr std::uint32_t records = 400;
    constexpr int updates = 240;
    Hash initial{};
    {
        Repository repository(source);
        repository.createBranch("main");
        std::vector<ModelChange> changes;
        changes.reserve(records);
        for (std::uint32_t number = 1; number <= records; ++number) {
            ModelRecord record;
            record.kind = 1;
            record.payload = Bytes{static_cast<std::uint8_t>(number & 0xffU)};
            if (number > 1) record.references.push_back(key(number - 1));
            changes.emplace_back(ModelChange{key(number), std::move(record)});
        }
        initial = repository.commitModel("main", std::move(changes)).id;
        repository.exportBranchBundle("main", initialBundle);
    }
    {
        Repository recipient(destination);
        check(recipient.importBranchBundle(initialBundle, "main").head == initial,
              "initial replica differs from original");
        recipient.verifyRepository();
    }

    std::atomic<int> readersReady{0};
    std::atomic<int> readsDuringWrites{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::thread writer([&] {
        try {
            Repository repository(source);
            while (readersReady.load() != 3 && !failed.load())
                std::this_thread::yield();
            if (failed.load()) { finished.store(true); return; }
            for (int i = 0; i < updates; ++i) {
                const Bytes payload{static_cast<std::uint8_t>((i + 1) & 0xff)};
                repository.commitModel("main", {
                    ModelChange{key(1), ModelRecord{1, {}, payload}}
                });
            }
        } catch (...) { failed.store(true); }
        finished.store(true);
    });
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 3; ++reader) {
        readers.emplace_back([&] {
            try {
                Repository repository(source);
                readersReady.fetch_add(1);
                std::size_t visits = 0;
                while (!finished.load() || visits < 250) {
                    const Hash snapshot = repository.head("main").id;
                    const std::optional<ModelRecord> first =
                        repository.getModelAtRevision(snapshot, key(1));
                    const std::optional<ModelRecord> last =
                        repository.getModelAtRevision(snapshot, key(records));
                    check(first.has_value() && last.has_value(),
                          "concurrent historical snapshot is incomplete");
                    check(last->references == std::vector<Key>{key(records - 1)},
                          "concurrent read changed immutable references");
                    check(repository.getModelAtRevision(initial, key(1))->payload == Bytes{1},
                          "historical snapshot changed during writes");
                    if (!finished.load()) readsDuringWrites.fetch_add(1);
                    ++visits;
                    if (failed.load()) break;
                }
            } catch (...) { failed.store(true); }
        });
    }
    writer.join();
    for (std::thread& reader : readers) reader.join();
    check(!failed.load() && readsDuringWrites.load() > 0,
          "concurrent reader/writer stress did not overlap successfully");

    Hash finalHead{};
    {
        Repository repository(source);
        bool staleRejected = false;
        try {
            repository.commitModel("main", {ModelChange{key(1), ModelRecord{1, {}, Bytes{9}}}},
                                   initial);
        } catch (const std::exception&) { staleRejected = true; }
        check(staleRejected, "stale optimistic write was accepted");
        repository.createBranch("feature", "main");
        repository.commitModel("feature", {
            ModelChange{key(records + 1), ModelRecord{1, {key(records)}, Bytes{'f'}}}
        });
        repository.commitModel("main", {
            ModelChange{key(records + 2), ModelRecord{1, {key(records)}, Bytes{'m'}}}
        });
        check(repository.mergeBranches("main", "feature").status == MergeStatus::Merged,
              "disjoint merge failed after sustained writes");
        check(repository.referencingModel("main", key(records)).size() == 2,
              "merge lost incoming references");
        finalHead = repository.head("main").id;
        check(repository.verifyRepository().revisions >= static_cast<std::uint64_t>(updates),
              "history truncated during sustained writes");
        repository.exportIncrementalBranchBundle("main", initial, deltaBundle);
    }
    {
        Repository recipient(destination);
        check(recipient.importBranchBundle(deltaBundle, "main").head == finalHead,
              "incremental sync lost revision identity");
        check(recipient.head("main").id == finalHead &&
              recipient.referencingModel("main", key(records)).size() == 2,
              "incremental sync lost merged references");
        recipient.verifyRepository();
    }
    {
        Repository stillOpen(source);
        Repository maintenance(source);
        bool compactRejected = false;
        try { (void)maintenance.compact(); }
        catch (const std::exception&) { compactRejected = true; }
        check(compactRejected, "compaction accepted another open repository handle");
    }
    {
        Repository maintenance(source);
        const aowis::native::CompactionStats stats = maintenance.compact();
        check(stats.objectsAfter <= stats.objectsBefore, "compaction increased object count");
    }
    {
        Repository reopened(source);
        check(reopened.head("main").id == finalHead,
              "compaction changed branch head");
        reopened.verifyRepository();
        reopened.commitModel("main", {
            ModelChange{key(records + 3), ModelRecord{1, {key(records)}, Bytes{'n'}}}
        });
        reopened.verifyRepository();
    }

    // Never silently interpret an obsolete experimental repository as an
    // empty pack repository, especially when its branch refs are present.
    const std::filesystem::path legacy = root.path / "obsolete";
    std::filesystem::create_directories(legacy);
    {
        std::ofstream marker(legacy / "storage.format");
        marker << "AOWIS-BLOCK-EXPERIMENT-1\n";
    }
    bool refused = false;
    try { Repository rejected(legacy); }
    catch (const std::exception&) { refused = true; }
    check(refused && !std::filesystem::exists(legacy / "objects.pack"),
          "obsolete block repository was silently converted");
}
}

int main() {
    try {
        exercise();
        std::cout << "Pack concurrency, merge, replication, compaction and recovery: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Pack operational stress failed: " << error.what() << '\n';
        return 1;
    }
}
