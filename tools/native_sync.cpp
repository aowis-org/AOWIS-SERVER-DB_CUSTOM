#include "aowis/dvmdb/native_store.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using aowis::dvmdb::BundleImportResult;
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::Repository;

void usage() {
    std::cerr <<
        "Usage: aowis-dvmdb-sync head REPOSITORY BRANCH\n"
        "       aowis-dvmdb-sync export REPOSITORY BRANCH BUNDLE\n"
        "       aowis-dvmdb-sync export-incremental REPOSITORY BRANCH BASE_REVISION BUNDLE\n"
        "       aowis-dvmdb-sync import REPOSITORY BUNDLE LOCAL_BRANCH\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4) { usage(); return 2; }
        const std::string command = argv[1];
        if (!((command == "head" && argc == 4) ||
              (command == "export" && argc == 5) ||
              (command == "export-incremental" && argc == 6) ||
              (command == "import" && argc == 5))) {
            usage();
            return 2;
        }
        Repository repository(argv[2]);
        if (command == "head") {
            std::cout << aowis::dvmdb::toHex(repository.head(argv[3]).id) << '\n';
        } else if (command == "export-incremental") {
            repository.exportIncrementalBranchBundle(
                argv[3], aowis::dvmdb::fromHex(argv[4]), argv[5]);
            std::cout << "exported incremental branch " << argv[3] << " to " << argv[5] << '\n';
        } else if (command == "export") {
            repository.exportBranchBundle(argv[3], argv[4]);
            std::cout << "exported branch " << argv[3] << " to " << argv[4] << '\n';
        } else {
            const BundleImportResult result = repository.importBranchBundle(argv[3], argv[4]);
            const char* outcome = result.status == BundleImportStatus::Created ?
                "created" : result.status == BundleImportStatus::FastForward ?
                "fast-forward" : "already up-to-date";
            std::cout << outcome << " " << argv[4] << " at " <<
                aowis::dvmdb::toHex(result.head) << ", imported " <<
                result.objectsImported << " objects\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native synchronization failed: " << error.what() << '\n';
        return 1;
    }
}
