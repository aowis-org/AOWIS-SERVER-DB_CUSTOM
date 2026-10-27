#pragma once

#include "aowis/dvmdb/native_store.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aowis::dvmdb::test {
inline std::filesystem::path branchFile(Repository& repository,
                                         const std::filesystem::path& root,
                                         std::string_view name) {
    const std::optional<BranchId> id = repository.branchId(name);
    if (!id) throw std::runtime_error("test branch missing UUIDv7");
    return root / "refs" / "heads" / id->toString();
}

inline void overwriteBranch(Repository& repository, const std::filesystem::path& root,
                             std::string_view name, std::string_view newHead) {
    const std::filesystem::path path = branchFile(repository, root, name);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("test branch record cannot open");
    output << "AOWIS-BRANCH-1\n" << name << '\n' << newHead << '\n';
    if (!output) throw std::runtime_error("test branch record cannot write");
}
} // namespace aowis::dvmdb::test
