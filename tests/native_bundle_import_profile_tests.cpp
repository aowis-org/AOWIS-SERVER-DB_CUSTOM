#include "aowis/native/native_store.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aowis::native::BundleImportProfile;
using aowis::native::BundleImportResult;
using aowis::native::BundleImportStatus;
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::PhysicalStore;
using aowis::native::Repository;

void check(bool condition, std::string_view reason) {
    if (!condition) throw std::runtime_error(std::string(reason));
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        const std::filesystem::path pattern =
            std::filesystem::temp_directory_path() / "aowis-native-import-profile-XXXXXX";
        std::string writable = pattern.string();
        writable.push_back('\0');
        char* const created = ::mkdtemp(writable.data());
        check(created != nullptr, "create temporary test root");
        this->path_ = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path_, ignored);
    }
    const std::filesystem::path& path() const { return this->path_; }
private:
    std::filesystem::path path_;
};

Key id(unsigned value) {
    Key result{};
    result[0] = static_cast<std::uint8_t>(value);
    result[15] = static_cast<std::uint8_t>(value ^ 0xa5U);
    return result;
}

void checkProfile(const BundleImportProfile& profile, bool expectsLocalReads) {
    check(profile.totalMs > 0 && profile.scanMs > 0 && profile.graphTraversalMs > 0 &&
          profile.modelReferenceMs >= 0 && profile.reachabilityMs >= 0 &&
          profile.branchCheckMs >= 0 && profile.objectWriteMs >= 0 &&
          profile.publicationMs >= 0, "missing import phase timing");
    const double subtotal = profile.scanMs + profile.graphTraversalMs +
        profile.modelReferenceMs + profile.reachabilityMs + profile.branchCheckMs +
        profile.objectWriteMs + profile.publicationMs;
    check(subtotal <= profile.totalMs + 1.0, "profile phases must not overlap");
    check(profile.bundleObjectReads > 0 && profile.bundleObjectBytes > 0,
          "transferred bundle reads not counted");
    if (expectsLocalReads) {
        check(profile.localObjectReads > 0 && profile.localObjectBytes > 0,
              "incremental import local reads not counted");
    } else {
        check(profile.localObjectReads == 0 && profile.localObjectBytes == 0,
              "fresh import should not read locally stored ancestors");
    }
}

void testPair(PhysicalStore sourceBackend, PhysicalStore destinationBackend,
              const std::filesystem::path& root) {
    Repository source(root / "source", sourceBackend);
    source.createBranch("main");
    ModelRecord one;
    one.kind = 1;
    one.payload = Bytes{1, 2, 3};
    source.commitModel("main", {ModelChange{id(1), one}});
    const Hash baseHead = source.head("main").id;
    source.exportBranchBundle("main", root / "base.aowbundle");

    ModelRecord two;
    two.kind = 1;
    two.payload = Bytes{4, 5, 6};
    two.references = {id(1)};
    source.commitModel("main", {ModelChange{id(2), two}});
    const Hash finalHead = source.head("main").id;
    source.exportBranchBundle("main", root / "full.aowbundle");
    source.exportIncrementalBranchBundle("main", baseHead, root / "delta.aowbundle");

    Repository fresh(root / "full", destinationBackend);
    BundleImportProfile fullProfile{};
    const BundleImportResult full = fresh.importBranchBundle(root / "full.aowbundle",
                                                              "main", {}, &fullProfile);
    check(full.status == BundleImportStatus::Created && full.head == finalHead,
          "full import has incorrect result");
    checkProfile(fullProfile, false);

    BundleImportProfile repeatedProfile{};
    const BundleImportResult repeated = fresh.importBranchBundle(root / "full.aowbundle",
                                                                  "main", {}, &repeatedProfile);
    check(repeated.status == BundleImportStatus::UpToDate && repeated.objectsImported == 0,
          "repeated import should be up to date");
    check(repeatedProfile.totalMs > 0 && repeatedProfile.objectWriteMs == 0 &&
          repeatedProfile.publicationMs == 0,
          "up-to-date import profile counted nonexistent publication");

    Repository incremental(root / "incremental", destinationBackend);
    incremental.importBranchBundle(root / "base.aowbundle", "main");
    BundleImportProfile deltaProfile{};
    const BundleImportResult delta = incremental.importBranchBundle(root / "delta.aowbundle",
                                                                     "main", {}, &deltaProfile);
    check(delta.status == BundleImportStatus::FastForward && delta.head == finalHead,
          "incremental import did not fast-forward");
    checkProfile(deltaProfile, true);
    check(incremental.verifyRepository().revisions == fresh.verifyRepository().revisions,
          "profiled imports did not preserve revision graph");

    // Profiling is observational only; checksum rejection still leaves the
    // published branch head unchanged.
    std::filesystem::copy_file(root / "delta.aowbundle", root / "corrupt.aowbundle");
    const std::uint64_t bytes = std::filesystem::file_size(root / "corrupt.aowbundle");
    std::fstream corrupt(root / "corrupt.aowbundle", std::ios::binary |
                         std::ios::in | std::ios::out);
    check(corrupt.good() && bytes > 0, "open forged transfer");
    corrupt.seekg(static_cast<std::streamoff>(bytes - 1));
    char last = 0;
    corrupt.get(last);
    check(corrupt.good(), "read corruption byte");
    corrupt.seekp(static_cast<std::streamoff>(bytes - 1));
    corrupt.put(static_cast<char>(last ^ 1));
    corrupt.close();
    bool rejected = false;
    try {
        BundleImportProfile rejectedProfile{};
        (void)incremental.importBranchBundle(root / "corrupt.aowbundle", "main", {},
                                             &rejectedProfile);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected && incremental.head("main").id == finalHead,
          "profiling weakened rejection of corrupted transfers");
}

} // namespace

int main() {
    try {
        for (unsigned source = 0; source < 2; ++source) {
            for (unsigned destination = 0; destination < 2; ++destination) {
                TemporaryRoot root;
                testPair(source == 0 ? PhysicalStore::Pack :
                         PhysicalStore::Pack,
                         destination == 0 ? PhysicalStore::Pack :
                         PhysicalStore::Pack, root.path());
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Bundle import profile test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
