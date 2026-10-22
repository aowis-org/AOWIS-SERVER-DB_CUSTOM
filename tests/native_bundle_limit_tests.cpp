#include "aowis/native/native_store.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
using aowis::native::BundleImportStatus;
using aowis::native::BundleLimits;
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::PhysicalStore;
using aowis::native::Repository;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <class Callable>
void rejectsLimit(Callable operation) {
    try {
        operation();
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()).find("bundle transfer limit:") != std::string_view::npos,
              "wrong rejection reason for transfer limit");
        return;
    }
    throw std::runtime_error("transfer violating configured limit was accepted");
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char path[] = "/tmp/aowis-db-bundle-limits-XXXXXX";
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

void exercise(PhysicalStore senderStore, PhysicalStore receiverStore) {
    TemporaryRoot temporary;
    Repository source(temporary.path() / "sender", senderStore);
    Repository destination(temporary.path() / "receiver", receiverStore);
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
    const aowis::native::BundleImportResult imported =
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
        exercise(PhysicalStore::Pack, PhysicalStore::Pack);
        std::cout << "AOWIS native bundle limit tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native bundle limit tests failed: " << error.what() << '\n';
        return 1;
    }
}
