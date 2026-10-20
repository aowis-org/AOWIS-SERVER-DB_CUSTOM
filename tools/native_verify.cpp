#include "aowis/native/native_store.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: aowis-native-verify <EXISTING_REPOSITORY_DIRECTORY>\n";
        return 2;
    }
    try {
        const std::filesystem::path directory(argv[1]);
        const bool blocks = std::filesystem::exists(directory / "storage.format");
        if (!blocks && !std::filesystem::exists(directory / "objects.pack") &&
            !std::filesystem::exists(directory / "objects.current")) {
            throw std::runtime_error("repository has no objects.pack");
        }
        aowis::native::Repository repository(directory,
            blocks ? aowis::native::PhysicalStore::ExperimentalBlocks
                   : aowis::native::PhysicalStore::Stage7Pack);
        repository.verifyPack();
        std::cout << "Pack verified: " << repository.objectCount() << " objects, "
                  << repository.packSize() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native repository verification failed: " << error.what() << '\n';
        return 1;
    }
}
