#include "aowis/native/native_store.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: aowis-native-compact <EXISTING_REPOSITORY_DIRECTORY>\n";
        return 2;
    }
    try {
        const std::filesystem::path directory(argv[1]);
        const bool blocks = std::filesystem::exists(directory / "storage.format");
        if (blocks) throw std::runtime_error("obsolete block storage is not supported");
        if (!blocks && !std::filesystem::exists(directory / "objects.pack") &&
            !std::filesystem::exists(directory / "objects.current")) {
            throw std::runtime_error("repository does not exist");
        }
        aowis::native::Repository repository(directory,
            aowis::native::PhysicalStore::Pack);
        const aowis::native::CompactionStats result = repository.compact();
        std::cout << "objects_before=" << result.objectsBefore << '\n'
                  << "objects_after=" << result.objectsAfter << '\n'
                  << "pack_bytes_before=" << result.packBytesBefore << '\n'
                  << "pack_bytes_after=" << result.packBytesAfter << '\n'
                  << "index_bytes_before=" << result.indexBytesBefore << '\n'
                  << "index_bytes_after=" << result.indexBytesAfter << '\n'
                  << "stale_files_removed=" << result.staleFilesRemoved << '\n';
        repository.verifyPack();
        std::cout << "Compacted repository verified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native repository compaction failed: " << error.what() << '\n';
        return 1;
    }
}
