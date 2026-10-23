#include "aowis/native/native_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
using aowis::native::BundleImportStatus;
using aowis::native::BundleImportResult;
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelRecord;
using aowis::native::Repository;
using aowis::native::Revision;

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
Key key(std::uint64_t number) {
    Key result{};
    for (unsigned index = 0; index < 8; ++index) {
        result[15 - index] = static_cast<std::uint8_t>(number >> (index * 8U));
    }
    return result;
}
Bytes bytes(std::string_view value) { return Bytes(value.begin(), value.end()); }
struct TemporaryDirectory {
    explicit TemporaryDirectory(std::string_view label) {
        static std::uint64_t counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-native-sync-" + std::string(label) + "-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++counter));
        std::filesystem::create_directories(this->path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

template <class Callable>
void expectFailure(Callable operation, std::string_view detail) {
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(std::string(detail));
}

void testPortableBundles() {
    TemporaryDirectory temporary("portable");
    const std::filesystem::path sourcePath = temporary.path / "source";
    const std::filesystem::path destinationPath = temporary.path / "destination";
    const std::filesystem::path firstBundle = temporary.path / "first.aowbundle";
    const std::filesystem::path secondBundle = temporary.path / "second.aowbundle";
    const std::filesystem::path modelBundle = temporary.path / "model.aowbundle";
    const std::filesystem::path alternateBundle = temporary.path / "alternate.aowbundle";
    Hash base{};
    Hash mergeTip{};
    Hash modelTip{};
    {
        Repository source(sourcePath);
        source.createBranch("main");
        base = source.commit("main", {{key(1), bytes("original")}}).id;
        source.createBranch("feature", "main");
        source.commit("main", {{key(2), bytes("on-main")}});
        source.commit("feature", {{key(3), bytes("on-feature")}});
        const aowis::native::MergeResult merged = source.mergeBranches("main", "feature");
        expect(merged.status == aowis::native::MergeStatus::Merged &&
               merged.revision.parents.size() == 2,
               "merge fixture did not preserve two-parent ancestry");
        mergeTip = merged.revision.id;
        source.createBranch("models");
        const ModelRecord node{10, {}, bytes("junction")};
        const ModelRecord pipe{11, {key(10)}, bytes("pipe")};
        modelTip = source.commitModel("models", {{key(10), node}, {key(11), pipe}}).id;
        source.exportBranchBundle("main", firstBundle);
        source.exportBranchBundle("models", modelBundle);
        expectFailure([&] { source.exportBranchBundle("main", firstBundle); },
                      "bundle export overwrote existing destination");
    }
    {
        Repository target(destinationPath);
        const BundleImportResult first = target.importBranchBundle(firstBundle, "remote_main");
        expect(first.status == BundleImportStatus::Created && first.head == mergeTip &&
               first.objectsImported > 0,
               "initial import did not create branch");
        expect(target.get("remote_main", key(1)) == bytes("original") &&
               target.get("remote_main", key(2)) == bytes("on-main") &&
               target.get("remote_main", key(3)) == bytes("on-feature") &&
               target.getAtRevision(base, key(1)) == bytes("original") &&
               !target.getAtRevision(base, key(2)),
               "independent repository replication changed historical contents");
        expect(target.revision(mergeTip).parents.size() == 2 &&
               target.revision(mergeTip).id == mergeTip,
               "replication did not preserve two-parent merge history");
        const BundleImportResult modelImport = target.importBranchBundle(modelBundle, "remote_models");
        expect(modelImport.head == modelTip &&
               target.getModel("remote_models", key(11))->payload == bytes("pipe") &&
               target.referencingModel("remote_models", key(10)) == std::vector<Key>{key(11)},
               "model reverse-reference snapshot did not survive replication");
        expect(target.importBranchBundle(firstBundle, "remote_main").status ==
               BundleImportStatus::UpToDate,
               "repeated bundle import is not idempotent");
        target.verifyPack();
    }
    {
        Repository source(sourcePath);
        source.commit("main", {{key(4), bytes("after-first-bundle")}});
        source.exportBranchBundle("main", secondBundle);
        source.createBranchAtRevision("alternate", "main", mergeTip);
        source.commit("alternate", {{key(99), bytes("alternative-history")}});
        source.exportBranchBundle("alternate", alternateBundle);
    }
    {
        Repository target(destinationPath);
        const BundleImportResult updated = target.importBranchBundle(secondBundle, "remote_main");
        expect(updated.status == BundleImportStatus::FastForward &&
               target.get("remote_main", key(4)) == bytes("after-first-bundle"),
               "subsequent bundle did not fast-forward branch");
        const Hash tip = updated.head;
        expect(target.importBranchBundle(firstBundle, "remote_main").status ==
               BundleImportStatus::UpToDate && target.head("remote_main").id == tip,
               "older bundle rewound branch");
        target.createBranch("local", "remote_main");
        const Hash diverged = target.commit("local", {{key(5), bytes("local-only")}}).id;
        const std::uint64_t before = target.packSize();
        expectFailure([&] { target.importBranchBundle(alternateBundle, "local"); },
                      "divergent bundle was accepted");
        expect(target.head("local").id == diverged && target.packSize() == before,
               "divergent bundle changed existing ref or pack");

        // Corruption, truncation, and trailing bytes are rejected without
        // publishing a ref or writing objects.
        const std::filesystem::path tampered = temporary.path / "tampered.aowbundle";
        std::filesystem::copy_file(modelBundle, tampered);
        {
            std::fstream file(tampered, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(48 + 40);
            const char corrupt = static_cast<char>(0x7f);
            file.write(&corrupt, 1);
        }
        const std::filesystem::path truncated = temporary.path / "truncated.aowbundle";
        std::filesystem::copy_file(secondBundle, truncated);
        std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 1);
        const std::filesystem::path trailing = temporary.path / "trailing.aowbundle";
        std::filesystem::copy_file(secondBundle, trailing);
        {
            std::ofstream file(trailing, std::ios::binary | std::ios::app);
            file.put('X');
        }
        // Every listed hash can be individually valid while the bundle as
        // a whole is missing a referenced object. Reject this case too.
        const std::filesystem::path incomplete = temporary.path / "incomplete.aowbundle";
        {
            std::ifstream original(secondBundle, std::ios::binary);
            const Bytes content((std::istreambuf_iterator<char>(original)),
                                std::istreambuf_iterator<char>());
            expect(content.size() >= 88, "source bundle unexpectedly small");
            const std::uint64_t count = [&content]() {
                std::uint64_t number = 0;
                for (unsigned index = 40; index < 48; ++index) {
                    number = (number << 8U) | content[index];
                }
                return number;
            }();
            expect(count > 1, "bundle fixture requires multiple objects");
            std::uint64_t end = 48;
            std::uint64_t last = end;
            for (std::uint64_t index = 0; index < count; ++index) {
                last = end;
                expect(content.size() - static_cast<std::size_t>(end) >= 40,
                       "bundle fixture entry truncated");
                std::uint64_t size = 0;
                for (unsigned shift = 32; shift < 40; ++shift) {
                    size = (size << 8U) | content[static_cast<std::size_t>(end + shift)];
                }
                end += 40 + size;
                expect(end <= content.size(), "bundle fixture size mismatch");
            }
            Bytes missing(content.begin(), content.begin() + static_cast<std::ptrdiff_t>(last));
            std::uint64_t newCount = count - 1;
            for (unsigned index = 0; index < 8; ++index) {
                missing[47 - index] = static_cast<std::uint8_t>(newCount & 0xffU);
                newCount >>= 8U;
            }
            std::ofstream output(incomplete, std::ios::binary);
            output.write(reinterpret_cast<const char*>(missing.data()),
                         static_cast<std::streamsize>(missing.size()));
        }
        for (const std::filesystem::path& bundle : {tampered, truncated, trailing, incomplete}) {
            expectFailure([&] { target.importBranchBundle(bundle, "invalid_branch"); },
                          "malformed bundle was accepted");
            expect(target.packSize() == before && target.head("local").id == diverged,
                   "malformed import changed repository");
            expectFailure([&] { (void)target.head("invalid_branch"); },
                          "malformed bundle published a branch");
        }
        target.compact();
        target.verifyPack();
    }
    {
        Repository reopened(destinationPath);
        expect(reopened.get("remote_main", key(4)) == bytes("after-first-bundle") &&
               reopened.get("local", key(5)) == bytes("local-only") &&
               reopened.revision(modelTip).id == modelTip &&
               reopened.referencingModel("remote_models", key(10)) == std::vector<Key>{key(11)},
               "imported objects lost after compaction and reopening");
    }
}
void testMergeParentIncremental() {
    TemporaryDirectory temporary("merge-parent-delta");
    Repository source(temporary.path / "source");
    Repository destination(temporary.path / "destination");
    source.createBranch("main");
    source.commit("main", {{key(1), bytes("common")}});
    source.createBranch("feature", "main");
    source.commit("main", {{key(2), bytes("main")}});
    const Hash featureHead = source.commit("feature", {{key(3), bytes("feature")}}).id;
    const std::filesystem::path initial = temporary.path / "feature.aowbundle";
    const std::filesystem::path delta = temporary.path / "merged.aowbundle";
    source.exportBranchBundle("feature", initial);
    destination.importBranchBundle(initial, "remote_merge");
    const aowis::native::MergeResult merged = source.mergeBranches("main", "feature");
    expect(merged.status == aowis::native::MergeStatus::Merged &&
               merged.revision.parents.size() == 2 &&
               merged.revision.parents[1] == featureHead,
           "two-parent incremental fixture is invalid");
    source.exportIncrementalBranchBundle("main", featureHead, delta);
    const BundleImportResult imported = destination.importBranchBundle(delta, "remote_merge");
    expect(imported.status == BundleImportStatus::FastForward &&
               destination.head("remote_merge").id == merged.revision.id &&
               destination.get("remote_merge", key(2)) == bytes("main") &&
               destination.get("remote_merge", key(3)) == bytes("feature") &&
               destination.revision(merged.revision.id).parents.size() == 2,
           "incremental bundle did not preserve second-parent ancestry");
}

void testIncrementalModelReferences() {
    TemporaryDirectory temporary("model-delta");
    const std::filesystem::path full = temporary.path / "models-full.aowbundle";
    const std::filesystem::path delta = temporary.path / "models-delta.aowbundle";
    Repository source(temporary.path / "source");
    Repository destination(temporary.path / "destination");
    source.createBranch("models");
    const ModelRecord node{10, {}, bytes("junction")};
    const ModelRecord pipe{11, {key(10)}, bytes("pipe")};
    const Hash base = source.commitModel("models", {{key(10), node}, {key(11), pipe}}).id;
    source.exportBranchBundle("models", full);
    destination.importBranchBundle(full, "remote_models");
    const ModelRecord secondPipe{11, {key(10)}, bytes("new pipe")};
    const Hash latest = source.commitModel("models", {{key(12), secondPipe}}).id;
    source.exportIncrementalBranchBundle("models", base, delta);
    expect(destination.importBranchBundle(delta, "remote_models").status ==
               BundleImportStatus::FastForward &&
           destination.head("remote_models").id == latest &&
           destination.getModel("remote_models", key(12))->payload == bytes("new pipe") &&
           destination.referencingModel("remote_models", key(10)) ==
               (std::vector<Key>{key(11), key(12)}) &&
           destination.referencingModelAtRevision(base, key(10)) ==
               (std::vector<Key>{key(11)}),
           "incremental transfer damaged typed model references/history");
}

void testIncrementalBundles() {
    TemporaryDirectory temporary("incremental");
    const std::filesystem::path sourcePath = temporary.path / "source";
    const std::filesystem::path destinationPath = temporary.path / "destination";
    const std::filesystem::path missingBasePath = temporary.path / "missing-base";
    const std::filesystem::path full = temporary.path / "full.aowbundle";
    const std::filesystem::path completeUpdate = temporary.path / "complete-update.aowbundle";
    const std::filesystem::path incremental = temporary.path / "incremental.aowbundle";
    const std::filesystem::path noChanges = temporary.path / "no-changes.aowbundle";
    const std::filesystem::path damaged = temporary.path / "damaged.aowbundle";
    Hash base{};
    Hash tip{};
    {
        Repository source(sourcePath);
        Repository destination(destinationPath);
        source.createBranch("main");
        std::vector<aowis::native::Change> initial;
        for (std::uint64_t index = 1; index <= 250; ++index) {
            initial.push_back({key(index), Bytes(240, static_cast<std::uint8_t>(index))});
        }
        base = source.commit("main", initial).id;
        source.exportBranchBundle("main", full);
        expect(destination.importBranchBundle(full, "remote_main").status ==
                   BundleImportStatus::Created,
               "full baseline import failed");
        expect(destination.head("remote_main").id == base, "baseline head mismatch");
        destination.createBranch("diverged", "remote_main");
        const Hash divergentHead = destination.commit("diverged", {{key(501), bytes("local")}}).id;

        source.commit("main", {{key(100), bytes("updated")}});
        tip = source.commit("main", {{key(502), bytes("new-object")}}).id;
        source.exportBranchBundle("main", completeUpdate);
        source.exportIncrementalBranchBundle("main", base, incremental);
        expect(std::filesystem::file_size(incremental) <
                   std::filesystem::file_size(completeUpdate) / 3,
               "incremental transfer retained almost the entire baseline");
        expectFailure([&] {
            source.exportIncrementalBranchBundle("main", Hash{}, temporary.path / "zero.aowbundle");
        }, "zero incremental base accepted");
        source.createBranch("unrelated");
        const Hash unrelated = source.commit("unrelated", {{key(777), bytes("other")}}).id;
        expectFailure([&] {
            source.exportIncrementalBranchBundle(
                "main", unrelated, temporary.path / "nonancestor.aowbundle");
        }, "non-ancestor base accepted");

        {
            Repository empty(missingBasePath);
            expectFailure([&] { empty.importBranchBundle(incremental, "remote_main"); },
                          "incremental bundle imported without recipient baseline");
            expect(empty.objectCount() == 0, "missing baseline import wrote content");
        }
        const std::uint64_t beforeDivergence = destination.packSize();
        expectFailure([&] { destination.importBranchBundle(incremental, "diverged"); },
                      "incremental transfer overwrote divergent head");
        expect(destination.head("diverged").id == divergentHead &&
                   destination.packSize() == beforeDivergence,
               "divergent incremental import touched the store");

        std::filesystem::copy_file(incremental, damaged);
        {
            std::fstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(80 + 40);
            file.put(static_cast<char>(0x7f));
        }
        const std::uint64_t beforeCorruption = destination.packSize();
        expectFailure([&] { destination.importBranchBundle(damaged, "remote_main"); },
                      "corrupted incremental object was accepted");
        expect(destination.packSize() == beforeCorruption &&
                   destination.head("remote_main").id == base,
               "corrupt incremental import changed content or branch");

        // A malicious delta could omit an object without tampering with any
        // other object's SHA-256. The existing baseline cannot fill this gap.
        const std::filesystem::path incomplete = temporary.path / "incomplete-delta.aowbundle";
        {
            std::ifstream sourceFile(incremental, std::ios::binary);
            const Bytes content((std::istreambuf_iterator<char>(sourceFile)),
                                std::istreambuf_iterator<char>());
            expect(content.size() > 120 && content[7] == '2',
                   "incremental fixture was unexpectedly empty");
            std::uint64_t count = 0;
            for (unsigned index = 40; index < 48; ++index) {
                count = (count << 8U) | content[index];
            }
            expect(count > 1, "incremental fixture needs multiple objects");
            std::size_t end = 80;
            std::size_t last = end;
            for (std::uint64_t index = 0; index < count; ++index) {
                last = end;
                expect(content.size() - end >= 40, "truncated delta fixture");
                std::uint64_t size = 0;
                for (unsigned pos = 32; pos < 40; ++pos) {
                    size = (size << 8U) | content[end + pos];
                }
                end += 40 + static_cast<std::size_t>(size);
                expect(end <= content.size(), "truncated delta object");
            }
            Bytes shortened(content.begin(), content.begin() +
                            static_cast<std::ptrdiff_t>(last));
            std::uint64_t reduced = count - 1;
            for (unsigned index = 0; index < 8; ++index) {
                shortened[47 - index] = static_cast<std::uint8_t>(reduced & 0xffU);
                reduced >>= 8U;
            }
            std::ofstream output(incomplete, std::ios::binary);
            output.write(reinterpret_cast<const char*>(shortened.data()),
                         static_cast<std::streamsize>(shortened.size()));
        }
        expectFailure([&] { destination.importBranchBundle(incomplete, "remote_main"); },
                      "incomplete incremental object graph was accepted");
        expect(destination.packSize() == beforeCorruption &&
                   destination.head("remote_main").id == base,
               "incomplete incremental graph changed repository");

        const BundleImportResult updated = destination.importBranchBundle(incremental,
                                                                           "remote_main");
        expect(updated.status == BundleImportStatus::FastForward && updated.head == tip &&
                   updated.objectsImported != 0 &&
                   destination.get("remote_main", key(100)) == bytes("updated") &&
                   destination.get("remote_main", key(502)) == bytes("new-object") &&
                   destination.getAtRevision(base, key(100)) == Bytes(240, 100),
               "incremental transfer lost old/new object contents");
        expect(destination.importBranchBundle(incremental, "remote_main").status ==
                   BundleImportStatus::UpToDate,
               "incremental reimport should be idempotent");
        source.exportIncrementalBranchBundle("main", tip, noChanges);
        expect(std::filesystem::file_size(noChanges) == 80,
               "matching branch tip should generate header-only incremental bundle");
        expect(destination.importBranchBundle(noChanges, "remote_main").status ==
                   BundleImportStatus::UpToDate,
               "header-only incremental bundle should be up to date");
        destination.verifyPack();
    }
    {
        Repository reopened(destinationPath);
        expect(reopened.head("remote_main").id == tip &&
                   reopened.get("remote_main", key(502)) == bytes("new-object"),
               "incremental objects did not survive reopening");
        reopened.compact();
        reopened.verifyPack();
    }
}
} // namespace

int main() {
    try {
        testPortableBundles();
        testIncrementalBundles();
        testIncrementalModelReferences();
        testIncrementalModelReferences();
        testMergeParentIncremental();
        std::cout << "AOWIS native offline sync tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native offline sync tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
