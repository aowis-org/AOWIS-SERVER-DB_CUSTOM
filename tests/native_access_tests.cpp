#include "aowis/native/access_policy.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace aowis::native;

void expect(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template <typename F>
void rejected(F&& function, const char* reason) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    expect(failed, reason);
}

struct TemporaryDirectory {
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "aowis-db-custom-access-XXXXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
        char* result = ::mkdtemp(buffer.data());
        expect(result != nullptr, "cannot allocate test directory");
        this->path = result;
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(this->path, ignored); }
    std::filesystem::path path;
};

int invoke(const std::filesystem::path& executable, const std::vector<std::string>& args) {
    std::vector<std::string> strings{executable.string()};
    strings.insert(strings.end(), args.begin(), args.end());
    std::vector<char*> pointers;
    for (std::string& item : strings) pointers.push_back(item.data());
    pointers.push_back(nullptr);
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        const int nullFd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullFd >= 0) { ::dup2(nullFd, STDOUT_FILENO); ::dup2(nullFd, STDERR_FILENO); ::close(nullFd); }
        ::execv(executable.c_str(), pointers.data());
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    expect(waited == child && WIFEXITED(status), "child process failed");
    return WEXITSTATUS(status);
}

void exercise() {
    TemporaryDirectory temp;
    const struct passwd* account = ::getpwuid(::geteuid());
    expect(account && account->pw_name, "cannot determine local test account");
    const std::string owner = account->pw_name;
    const std::filesystem::path repoPath = temp.path / "repo";
    {
        Repository repository(repoPath);
        repository.createBranch("main");
        repository.createBranch("restricted");
    }
    AccessPolicyStore store(repoPath);
    expect(!store.initialized(), "policy should initially be absent");
    expect(store.permits(owner, AccessOperation::Read, "main"), "uninitialized policy semantics changed");
    const Key userId = newAccessUuid();
    const Key groupId = newAccessUuid();
    const Key roleId = newAccessUuid();
    const Key readId = newAccessUuid();
    const Key writeId = newAccessUuid();
    expect(userId != groupId && roleId != writeId, "UUID generation collided");
    expect(parseAccessUuid(accessUuidString(userId)) == userId, "UUID text does not round-trip");
    rejected([] { (void)parseAccessUuid("not-a-uuid"); }, "invalid UUID accepted");

    AccessPolicy initial;
    initial.users.push_back({userId, owner, false, {groupId}, {}});
    initial.groups.push_back({groupId, "editors", {roleId}});
    initial.roles.push_back({roleId, "reader", {readId}});
    initial.permissions.push_back({readId, AccessOperation::Read, "main"});
    store.initialize(initial);
    expect(store.initialized(), "policy was not persisted");
    rejected([&] { store.initialize(initial); }, "policy was reinitialized");
    expect(store.permits(owner, AccessOperation::Read, "main"), "group read grant lost");
    expect(!store.permits(owner, AccessOperation::Write, "main"), "write implicitly allowed");
    expect(!store.permits(owner, AccessOperation::Read, "restricted"), "scope ignored");
    expect(!store.permits("unknown-account", AccessOperation::Read, "main"),
           "unknown account was allowed");

    // A invalid update must be atomic: the original grants remain intact.
    rejected([&] {
        store.modify([&](AccessPolicy& policy) {
            policy.roles[0].permissions.push_back(newAccessUuid());
        });
    }, "invalid permission link was accepted");
    expect(store.permits(owner, AccessOperation::Read, "main"),
           "invalid policy update replaced the old policy");

    store.modify([&](AccessPolicy& policy) {
        policy.permissions.push_back({writeId, AccessOperation::Write, "main"});
        policy.roles[0].permissions.push_back(writeId);
    });
    expect(store.permits(owner, AccessOperation::Write, "main"), "group write grant lost");
    expect(!store.permits(owner, AccessOperation::Write, "restricted"), "write scope ignored");

    AccessPolicyStore reopened(repoPath);
    expect(reopened.permits(owner, AccessOperation::Write, "main"), "policy was not durable");
    reopened.modify([&](AccessPolicy& policy) { policy.users[0].disabled = true; });
    expect(!store.permits(owner, AccessOperation::Read, "main") &&
           !store.permits(owner, AccessOperation::Write, "main"),
           "disabled account still has permissions");

    // Corrupt authorization data must not be treated as an absent policy.
    {
        std::fstream file(repoPath / "access-policy.bin", std::ios::binary | std::ios::in | std::ios::out);
        expect(file.good(), "cannot open policy corruption fixture");
        file.seekp(10); file.put(static_cast<char>(0xab));
    }
    rejected([&] { (void)store.permits(owner, AccessOperation::Read, "main"); },
             "corrupt authorization policy was ignored");
}

void exerciseCli(const std::filesystem::path& accessCli) {
    TemporaryDirectory temp;
    const std::filesystem::path directory = temp.path / "repository";
    {
        Repository repository(directory);
        repository.createBranch("main");
    }
    const struct passwd* account = ::getpwuid(::geteuid());
    expect(account && account->pw_name, "cannot identify bootstrap account");
    const std::string owner = account->pw_name;
    expect(invoke(accessCli, {"bootstrap", directory.string(), owner}) == 0,
           "administrative bootstrap failed");
    expect(invoke(accessCli, {"bootstrap", directory.string(), owner}) != 0,
           "access policy was bootstrapped twice");
    AccessPolicyStore store(directory);
    expect(store.permits(owner, AccessOperation::Read, "main") &&
           store.permits(owner, AccessOperation::Write, "main"),
           "bootstrap did not grant administrator read/write");
    expect(invoke(accessCli, {"add-user", directory.string(), "new-user"}) == 0,
           "cannot add access user");
    expect(invoke(accessCli, {"add-group", directory.string(), "reviewers"}) == 0,
           "cannot add access group");
    expect(invoke(accessCli, {"add-role", directory.string(), "read-role"}) == 0,
           "cannot add access role");
    expect(invoke(accessCli, {"add-permission", directory.string(), "read", "main"}) == 0,
           "cannot add access permission");
    AccessPolicy policy = store.read();
    const Key userId = policy.users.back().id;
    const Key groupId = policy.groups.back().id;
    const Key roleId = policy.roles.back().id;
    const Key permissionId = policy.permissions.back().id;
    expect(invoke(accessCli, {"bind-user-group", directory.string(),
                              accessUuidString(userId), accessUuidString(groupId)}) == 0,
           "cannot add user to group");
    expect(invoke(accessCli, {"bind-group-role", directory.string(),
                              accessUuidString(groupId), accessUuidString(roleId)}) == 0,
           "cannot bind group role");
    expect(invoke(accessCli, {"bind-role-permission", directory.string(),
                              accessUuidString(roleId), accessUuidString(permissionId)}) == 0,
           "cannot bind role permission");
    expect(store.permits("new-user", AccessOperation::Read, "main") &&
           !store.permits("new-user", AccessOperation::Write, "main"),
           "CLI failed to provision read-only group role");
    expect(invoke(accessCli, {"unbind-role-permission", directory.string(),
                              accessUuidString(roleId), accessUuidString(permissionId)}) == 0,
           "cannot revoke permission from role");
    expect(!store.permits("new-user", AccessOperation::Read, "main"),
           "permission revocation did not take effect");
    expect(invoke(accessCli, {"bind-role-permission", directory.string(),
                              accessUuidString(roleId), accessUuidString(permissionId)}) == 0,
           "cannot restore permission to role");
    expect(invoke(accessCli, {"disable-user", directory.string(), accessUuidString(userId)}) == 0,
           "cannot disable user");
    expect(!store.permits("new-user", AccessOperation::Read, "main"),
           "disabled CLI user retained read permission");
    expect(invoke(accessCli, {"enable-user", directory.string(), accessUuidString(userId)}) == 0,
           "cannot enable user");
    expect(store.permits("new-user", AccessOperation::Read, "main"),
           "enabled CLI user did not recover assigned roles");
}

} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "pass the access CLI path");
        exercise();
        exerciseCli(std::filesystem::absolute(argv[1]));
        std::cout << "Native access-policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native access-policy tests failed: " << error.what() << '\n';
        return 1;
    }
}
