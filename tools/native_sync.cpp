#include "aowis/native/native_store.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using aowis::native::BundleImportResult;
using aowis::native::BundleImportStatus;
using aowis::native::PhysicalStore;
using aowis::native::Repository;

PhysicalStore parseStorage(std::string_view mode) {
    if (mode == "pack") return PhysicalStore::Stage7Pack;
    if (mode == "blocks") return PhysicalStore::ExperimentalBlocks;
    throw std::runtime_error("storage mode must be pack or blocks");
}

void usage() {
    std::cerr <<
        "Usage: aowis-native-sync head REPOSITORY pack|blocks BRANCH\n"
        "       aowis-native-sync export REPOSITORY pack|blocks BRANCH BUNDLE\n"
        "       aowis-native-sync export-incremental REPOSITORY pack|blocks BRANCH BASE_REVISION BUNDLE\n"
        "       aowis-native-sync import REPOSITORY pack|blocks BUNDLE LOCAL_BRANCH\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 5) { usage(); return 2; }
        const std::string command = argv[1];
        if (!((command == "head" && argc == 5) ||
              (command == "export" && argc == 6) ||
              (command == "export-incremental" && argc == 7) ||
              (command == "import" && argc == 6))) {
            usage();
            return 2;
        }
        Repository repository(argv[2], parseStorage(argv[3]));
        if (command == "head") {
            std::cout << aowis::native::toHex(repository.head(argv[4]).id) << '\n';
        } else if (command == "export-incremental") {
            repository.exportIncrementalBranchBundle(
                argv[4], aowis::native::fromHex(argv[5]), argv[6]);
            std::cout << "exported incremental branch " << argv[4] << " to " << argv[6] << '\n';
        } else if (command == "export") {
            repository.exportBranchBundle(argv[4], argv[5]);
            std::cout << "exported branch " << argv[4] << " to " << argv[5] << '\n';
        } else {
            const BundleImportResult result = repository.importBranchBundle(argv[4], argv[5]);
            const char* outcome = result.status == BundleImportStatus::Created ?
                "created" : result.status == BundleImportStatus::FastForward ?
                "fast-forward" : "already up-to-date";
            std::cout << outcome << " " << argv[5] << " at " <<
                aowis::native::toHex(result.head) << ", imported " <<
                result.objectsImported << " objects\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native synchronization failed: " << error.what() << '\n';
        return 1;
    }
}
