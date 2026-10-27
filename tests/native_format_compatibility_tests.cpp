#include "aowis/dvmdb/native_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::Repository;

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct TemporaryDirectory {
    TemporaryDirectory() {
        char name[] = "/tmp/aowis-format-current-XXXXXXXX";
        expect(::mkdtemp(name) != nullptr, "could not create test directory");
        this->path = name;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

Key key(std::uint8_t value) {
    Key result{};
    result[0] = value;
    return result;
}

void rejectOldRepository(const std::filesystem::path& fixture,
                         const std::filesystem::path& destination) {
    expect(std::filesystem::is_regular_file(fixture / "repository" / "objects.pack"),
           "frozen pre-release repository missing");
    std::filesystem::copy(fixture / "repository", destination,
                          std::filesystem::copy_options::recursive);
    bool refused = false;
    try { Repository obsolete(destination); }
    catch (const std::exception&) { refused = true; }
    expect(refused, "obsolete name-keyed branch refs opened without explicit rejection");
}

void rejectOldBundle(const std::filesystem::path& fixture,
                     const std::filesystem::path& destination) {
    Repository receiver(destination);
    const std::uint64_t before = receiver.objectCount();
    bool refused = false;
    try { (void)receiver.importBranchBundle(fixture / "base.aowbundle", "old"); }
    catch (const std::exception&) { refused = true; }
    expect(refused && receiver.listBranches().empty() && receiver.objectCount() == before,
           "legacy SHA-only transfer was accepted or modified the repository");
}

void checkBundle(const std::filesystem::path& path, char version) {
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "bundle missing");
    char marker[8]{};
    input.read(marker, sizeof(marker));
    expect(input.good() && std::string_view(marker, 7) == "AOWSYNC" && marker[7] == version,
           "export did not use required identity-bearing wire format");
}

void testCurrentFormat(const std::filesystem::path& root) {
    const std::filesystem::path sourcePath = root / "source";
    const std::filesystem::path destinationPath = root / "destination";
    const std::filesystem::path mainBundle = root / "main.bundle";
    const std::filesystem::path featureBundle = root / "feature.bundle";
    const std::filesystem::path deltaBundle = root / "delta.bundle";
    Hash base{};
    Hash mainHead{};
    Hash featureHead{};
    aowis::dvmdb::BranchId mainId = aowis::dvmdb::BranchId::generate();
    aowis::dvmdb::BranchId featureId = aowis::dvmdb::BranchId::generate();
    {
        Repository source(sourcePath);
        source.createBranch("main");
        base = source.commit("main", {{key(0x10), Bytes{'a'}}}).id;
        source.createBranch("feature", "main");
        mainId = *source.branchId("main");
        featureId = *source.branchId("feature");
        expect(mainId != featureId, "new branches reused UUIDv7 identity");
        featureHead = source.commit("feature", {{key(0x20), Bytes{'f'}}}).id;
        mainHead = source.commit("main", {{key(0x30), Bytes{'m'}}}).id;
        source.exportBranchBundle("main", mainBundle);
        source.exportBranchBundle("feature", featureBundle);
        source.verifyRepository();
    }
    checkBundle(mainBundle, '3');
    checkBundle(featureBundle, '3');
    {
        Repository destination(destinationPath);
        expect(destination.importBranchBundle(mainBundle, "main").status == BundleImportStatus::Created,
               "initial identity-bearing transfer failed");
        expect(destination.importBranchBundle(featureBundle, "feature").status == BundleImportStatus::Created,
               "divergent identity-bearing transfer failed");
        expect(destination.branchId("main") == mainId && destination.branchId("feature") == featureId,
               "transfer changed branch UUIDv7 identity");
        expect(destination.head("main").id == mainHead && destination.head("feature").id == featureHead,
               "transfer changed immutable revision heads");
        expect(destination.revision(mainHead).parents.front() == base &&
               destination.revision(featureHead).parents.front() == base,
               "transfer changed shared revision ancestry");
        destination.verifyRepository();
        expect(destination.importBranchBundle(mainBundle, "main").status == BundleImportStatus::UpToDate,
               "identical current-format bundle was not idempotent");
        const std::uint64_t before = destination.objectCount();
        bool aliasRejected = false;
        try { (void)destination.importBranchBundle(mainBundle, "alias"); }
        catch (const std::exception&) { aliasRejected = true; }
        expect(aliasRejected && destination.objectCount() == before &&
               destination.listBranches().size() == 2,
               "one UUIDv7 was incorrectly published as two branches");
    }
    {
        Repository source(sourcePath);
        (void)source.commit("main", {{key(0x40), Bytes{'n'}}});
        source.exportIncrementalBranchBundle("main", mainHead, deltaBundle);
    }
    checkBundle(deltaBundle, '4');
    {
        Repository destination(destinationPath);
        expect(destination.importBranchBundle(deltaBundle, "main").status == BundleImportStatus::FastForward &&
               destination.branchId("main") == mainId &&
               destination.get("main", key(0x40)) == Bytes{'n'},
               "identity-bearing incremental transfer lost branch identity or contents");
        destination.verifyRepository();
    }
    {
        Repository reopened(destinationPath);
        expect(reopened.branchId("main") == mainId && reopened.branchId("feature") == featureId,
               "current-format branch identities did not survive restart");
        reopened.verifyRepository();
    }
}
}

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "expected frozen fixture directory for explicit rejection test");
        TemporaryDirectory temporary;
        rejectOldRepository(argv[1], temporary.path / "old-repository");
        rejectOldBundle(argv[1], temporary.path / "old-transfer-receiver");
        testCurrentFormat(temporary.path);
        std::cout << "Current UUIDv7 branch format and explicit obsolete-format rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Pre-release format test FAILED: " << error.what() << '\n';
        return 1;
    }
}
