#pragma once

#include "aowis/dvmdb/native_store.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aowis::dvmdb {

// Authorization is deliberately independent of project/branch revision history:
// checking out an older model revision must never roll back its access rules.
// This optional policy is not enforced by the planned initial HTTP service.
enum class AccessOperation : std::uint8_t { Read = 1, Write = 2 };

struct AccessPermission {
    Key id{};
    AccessOperation operation = AccessOperation::Read;
    // Exactly one branch name, or "*" for all branches. No pattern matching.
    std::string branch;
};

struct AccessRole {
    Key id{};
    std::string name;
    std::vector<Key> permissions;
};

struct AccessGroup {
    Key id{};
    std::string name;
    std::vector<Key> roles;
};

struct AccessUser {
    Key id{};
    // Policy-local account name (not an HTTP or authenticated identity).
    std::string account;
    bool disabled = false;
    std::vector<Key> groups;
    std::vector<Key> roles;
};

struct AccessPolicy {
    std::vector<AccessUser> users;
    std::vector<AccessGroup> groups;
    std::vector<AccessRole> roles;
    std::vector<AccessPermission> permissions;
};

// Access policy is a separate, atomically replaced, 0600 file. The local
// repository owner is its administrator; arbitrary filesystem write access to
// the repository remains fully privileged. A missing policy preserves the
// permissive pre-initialization behavior; a PRESENT policy denies by default.
class AccessPolicyStore final {
public:
    explicit AccessPolicyStore(std::filesystem::path repositoryDirectory);
    bool initialized() const;
    AccessPolicy read() const;
    void initialize(const AccessPolicy& initial);
    // Filesystem-owner-only administration. The callback runs under an
    // exclusive POSIX lock, and the whole new policy is validated before save.
    void modify(const std::function<void(AccessPolicy&)>& change);
    bool permits(std::string_view accountName, AccessOperation operation,
                 std::string_view branch) const;
    // Strict service authorization: missing or corrupt policies never fall
    // back to permissive pre-initialization behavior.
    bool permitsRequired(std::string_view accountName, AccessOperation operation,
                         std::string_view branch) const;

private:
    std::filesystem::path directory_;
};

void validateAccessPolicy(const AccessPolicy& policy);
Key newAccessUuid();
std::string accessUuidString(const Key& id);
Key parseAccessUuid(std::string_view value);

} // namespace aowis::dvmdb
