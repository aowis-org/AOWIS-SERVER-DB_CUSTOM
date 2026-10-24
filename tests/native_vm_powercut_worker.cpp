#include "aowis/native/native_store.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Key;
using aowis::native::Repository;

Key counterKey() {
    Key result{};
    result.back() = 0x71;
    return result;
}

Bytes counterData(std::uint64_t sequence) {
    Bytes result(131072U);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::uint8_t>((sequence * 127U + i * 53U + (i >> 7U)) & 0xffU);
    }
    for (unsigned i = 0; i < 8; ++i)
        result[i] = static_cast<std::uint8_t>(sequence >> (8U * i));
    return result;
}

std::uint64_t readCounter(Repository& repository) {
    const std::optional<Bytes> value = repository.get("main", counterKey());
    if (!value.has_value() || value->size() != 131072U)
        throw std::runtime_error("missing or incorrectly sized counter");
    std::uint64_t sequence = 0;
    for (unsigned i = 0; i < 8; ++i)
        sequence |= static_cast<std::uint64_t>((*value)[i]) << (8U * i);
    if (*value != counterData(sequence))
        throw std::runtime_error("counter checksum/payload mismatch");
    return sequence;
}

void check(Repository& repository) {
    repository.verifyRepository();
    const std::uint64_t sequence = readCounter(repository);
    std::cout << "STATE " << sequence << " "
              << aowis::native::toHex(repository.head("main").id) << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: aowis-native-vm-powercut-worker init|writer|verify REPOSITORY\n";
        return 2;
    }
    try {
        const std::string_view mode(argv[1]);
        if (mode != "init" && mode != "writer" && mode != "verify")
            throw std::runtime_error("unknown mode");
        const std::filesystem::path repositoryPath(argv[2]);
        // 'verify' must never create a missing repository and thereby disguise
        // lost storage or an accidentally unmounted data disk.
        if (mode == "verify" &&
            !std::filesystem::exists(repositoryPath / "objects.pack") &&
            !std::filesystem::exists(repositoryPath / "objects.current"))
            throw std::runtime_error("repository missing; is the test disk mounted?");
        Repository repository(repositoryPath);
        if (mode == "init") {
            repository.createBranch("main");
            (void)repository.commit("main", {Change{counterKey(), counterData(0)}});
            check(repository);
        } else if (mode == "verify") {
            check(repository);
        } else if (mode == "writer") {
            std::uint64_t next = readCounter(repository);
            // Host acknowledges each commit before allowing another. This
            // bounds crash-publication ambiguity to at most one revision.
            for (;;) {
                ++next;
                const aowis::native::Revision revision = repository.commit(
                    "main", {Change{counterKey(), counterData(next)}});
                std::cout << "ACK " << next << " " << aowis::native::toHex(revision.id)
                          << std::endl;
                std::string proceed;
                if (!std::getline(std::cin, proceed) || proceed != "GO") break;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VM power-cut worker failed: " << error.what() << '\n';
        return 1;
    }
}
