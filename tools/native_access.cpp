#include "aowis/dvmdb/access_policy.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace aowis::dvmdb;

void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

void usage() {
    std::cerr <<
        "Usage: aowis-dvmdb-access bootstrap REPOSITORY UNIX_ACCOUNT\n"
        "       aowis-dvmdb-access add-user REPOSITORY UNIX_ACCOUNT\n"
        "       aowis-dvmdb-access add-group REPOSITORY NAME\n"
        "       aowis-dvmdb-access add-role REPOSITORY NAME\n"
        "       aowis-dvmdb-access add-permission REPOSITORY read|write BRANCH|*\n"
        "       aowis-dvmdb-access bind-user-role REPOSITORY USER_UUID ROLE_UUID\n"
        "       aowis-dvmdb-access bind-user-group REPOSITORY USER_UUID GROUP_UUID\n"
        "       aowis-dvmdb-access bind-group-role REPOSITORY GROUP_UUID ROLE_UUID\n"
        "       aowis-dvmdb-access bind-role-permission REPOSITORY ROLE_UUID PERMISSION_UUID\n"
        "       aowis-dvmdb-access unbind-user-role REPOSITORY USER_UUID ROLE_UUID\n"
        "       aowis-dvmdb-access unbind-user-group REPOSITORY USER_UUID GROUP_UUID\n"
        "       aowis-dvmdb-access unbind-group-role REPOSITORY GROUP_UUID ROLE_UUID\n"
        "       aowis-dvmdb-access unbind-role-permission REPOSITORY ROLE_UUID PERMISSION_UUID\n"
        "       aowis-dvmdb-access enable-user|disable-user REPOSITORY USER_UUID\n"
        "       aowis-dvmdb-access list REPOSITORY\n";
}

template <typename T>
T& byId(std::vector<T>& records, const Key& id) {
    const auto found = std::find_if(records.begin(), records.end(),
                                    [&id](const T& value) { return value.id == id; });
    require(found != records.end(), "unknown access entity UUID");
    return *found;
}

void appendUnique(std::vector<Key>& values, const Key& id) {
    require(std::find(values.begin(), values.end(), id) == values.end(),
            "access assignment already exists");
    values.push_back(id);
}

void removeAssigned(std::vector<Key>& values, const Key& id) {
    const auto found = std::find(values.begin(), values.end(), id);
    require(found != values.end(), "access assignment does not exist");
    values.erase(found);
}

void list(const AccessPolicy& policy) {
    for (const AccessUser& user : policy.users)
        std::cout << "user " << accessUuidString(user.id) << " " << user.account <<
            (user.disabled ? " disabled" : " enabled") << '\n';
    for (const AccessGroup& group : policy.groups)
        std::cout << "group " << accessUuidString(group.id) << " " << group.name << '\n';
    for (const AccessRole& role : policy.roles)
        std::cout << "role " << accessUuidString(role.id) << " " << role.name << '\n';
    for (const AccessPermission& permission : policy.permissions)
        std::cout << "permission " << accessUuidString(permission.id) << " " <<
            (permission.operation == AccessOperation::Read ? "read" : "write") << " " <<
            permission.branch << '\n';
    for (const AccessUser& user : policy.users) {
        for (const Key& role : user.roles)
            std::cout << "user-role " << accessUuidString(user.id) << " " << accessUuidString(role) << '\n';
        for (const Key& group : user.groups)
            std::cout << "user-group " << accessUuidString(user.id) << " " << accessUuidString(group) << '\n';
    }
    for (const AccessGroup& group : policy.groups)
        for (const Key& role : group.roles)
            std::cout << "group-role " << accessUuidString(group.id) << " " << accessUuidString(role) << '\n';
    for (const AccessRole& role : policy.roles)
        for (const Key& permission : role.permissions)
            std::cout << "role-permission " << accessUuidString(role.id) << " " << accessUuidString(permission) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) { usage(); return 2; }
        const std::string command = argv[1];
        AccessPolicyStore store(argv[2]);
        if (command == "bootstrap" && argc == 4) {
            AccessPolicy policy;
            const Key user = newAccessUuid();
            const Key role = newAccessUuid();
            const Key read = newAccessUuid();
            const Key write = newAccessUuid();
            policy.permissions = {{read, AccessOperation::Read, "*"},
                                  {write, AccessOperation::Write, "*"}};
            policy.roles = {{role, "repository-admin", {read, write}}};
            policy.users = {{user, argv[3], false, {}, {role}}};
            store.initialize(policy);
            std::cout << "administrator " << accessUuidString(user) << '\n';
        } else if (command == "list" && argc == 3) {
            list(store.read());
        } else if ((command == "add-user" || command == "add-group" || command == "add-role") && argc == 4) {
            const Key id = newAccessUuid();
            store.modify([&](AccessPolicy& policy) {
                if (command == "add-user") policy.users.push_back({id, argv[3], false, {}, {}});
                else if (command == "add-group") policy.groups.push_back({id, argv[3], {}});
                else policy.roles.push_back({id, argv[3], {}});
            });
            std::cout << accessUuidString(id) << '\n';
        } else if (command == "add-permission" && argc == 5) {
            const std::string operation = argv[3];
            require(operation == "read" || operation == "write", "expected read or write permission");
            const Key id = newAccessUuid();
            store.modify([&](AccessPolicy& policy) {
                policy.permissions.push_back({id, operation == "read" ? AccessOperation::Read :
                                              AccessOperation::Write, argv[4]});
            });
            std::cout << accessUuidString(id) << '\n';
        } else if ((command == "disable-user" || command == "enable-user") && argc == 4) {
            const Key user = parseAccessUuid(argv[3]);
            store.modify([&](AccessPolicy& policy) {
                byId(policy.users, user).disabled = command == "disable-user";
            });
        } else if ((command == "bind-user-role" || command == "bind-user-group" ||
                    command == "bind-group-role" || command == "bind-role-permission" ||
                    command == "unbind-user-role" || command == "unbind-user-group" ||
                    command == "unbind-group-role" || command == "unbind-role-permission") && argc == 5) {
            const Key parent = parseAccessUuid(argv[3]);
            const Key child = parseAccessUuid(argv[4]);
            store.modify([&](AccessPolicy& policy) {
                const bool add = command.starts_with("bind-");
                std::vector<Key>* assigned = nullptr;
                if (command.ends_with("user-role")) assigned = &byId(policy.users, parent).roles;
                else if (command.ends_with("user-group")) assigned = &byId(policy.users, parent).groups;
                else if (command.ends_with("group-role")) assigned = &byId(policy.groups, parent).roles;
                else assigned = &byId(policy.roles, parent).permissions;
                if (add) appendUnique(*assigned, child);
                else removeAssigned(*assigned, child);
            });
        } else { usage(); return 2; }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native access management failed: " << error.what() << '\n';
        return 1;
    }
}
