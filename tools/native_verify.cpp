#include "aowis/dvmdb/native_store.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    bool physicalOnly = false;
    std::filesystem::path directory;
    if (argc == 2) {
        directory = argv[1];
    } else if (argc == 3 && std::string_view(argv[1]) == "--physical") {
        physicalOnly = true;
        directory = argv[2];
    } else if (argc == 3 && std::string_view(argv[1]) == "--full") {
        directory = argv[2];
    } else {
        std::cerr << "Usage: aowis-dvmdb-verify [--physical | --full] "
                     "<EXISTING_REPOSITORY_DIRECTORY>\n";
        return 2;
    }
    try {
        if (!std::filesystem::exists(directory / "objects.pack") &&
            !std::filesystem::exists(directory / "objects.current")) {
            throw std::runtime_error("repository has no objects.pack");
        }
        aowis::dvmdb::Repository repository(directory);
        const std::uint64_t recovered = repository.recoveredPackTailBytes();
        if (recovered != 0) {
            std::cout << "Recovered: discarded " << recovered
                      << " bytes of an unpublished torn pack tail on open; a copy is kept in "
                      << (directory / "recovered-pack-tails").string() << "\n";
        }
        if (physicalOnly) {
            repository.verifyPack();
            std::cout << "Physical integrity verified: " << repository.objectCount()
                      << " objects, " << repository.packSize() << " bytes\n"
                      << "Logical repository integrity NOT checked (--physical).\n";
        } else {
            const aowis::dvmdb::VerificationReport result = repository.verifyRepository();
            std::cout << "Full native storage integrity verified: "
                      << result.physicalObjects << " physical objects, "
                      << result.reachableObjects << " reachable objects, "
                      << result.branches << " branches, " << result.revisions
                      << " revisions, " << result.treeNodes << " verified trie nodes, "
                      << result.modelSnapshots << " distinct model snapshots\n"
                      << "Checked: physical storage, revision ancestry, canonical Merkle "
                         "tries, model references and reverse indexes.\n"
                      << "Not checked: application-specific hydraulic semantics.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native repository verification failed: " << error.what() << '\n';
        return 1;
    }
}
