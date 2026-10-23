#include "aowis/native/native_store.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Repository;

void expect(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct TemporaryDirectory {
    TemporaryDirectory() {
        std::string name = (std::filesystem::temp_directory_path() /
                            "aowis-native-sync-cli-XXXXXXXX").string();
        std::vector<char> buffer(name.begin(), name.end());
        buffer.push_back('\0');
        const char* created = ::mkdtemp(buffer.data());
        expect(created != nullptr, "cannot create test directory");
        this->path = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

int run(const std::filesystem::path& cli, const std::vector<std::string>& arguments) {
    std::vector<std::string> values{cli.string()};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> raw;
    for (std::string& value : values) raw.push_back(value.data());
    raw.push_back(nullptr);
    const pid_t child = ::fork();
    expect(child >= 0, "cannot fork synchronization CLI");
    if (child == 0) {
        ::execv(cli.c_str(), raw.data());
        ::_exit(127);
    }
    int status = 0;
    pid_t waited = -1;
    do { waited = ::waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);
    expect(waited == child && WIFEXITED(status), "sync CLI exited abnormally");
    return WEXITSTATUS(status);
}

void exercise(const std::filesystem::path& cli) {
    TemporaryDirectory temp;
    const std::filesystem::path sourcePath = temp.path / "source";
    const std::filesystem::path destinationPath = temp.path / "destination";
    const std::filesystem::path bundle = temp.path / "full.aowbundle";
    const std::filesystem::path delta = temp.path / "incremental.aowbundle";
    aowis::native::Hash base{};
    aowis::native::Hash tip{};
    aowis::native::Key key{};
    key[0] = 73;
    {
        Repository source(sourcePath);
        source.createBranch("main");
        base = source.commit("main", {{key, Bytes{4, 5, 6}}}).id;
    }
    expect(run(cli, {"export", sourcePath.string(), "main", bundle.string()}) == 0,
           "offline full export failed");
    expect(run(cli, {"import", destinationPath.string(),
                     bundle.string(), "incoming"}) == 0,
           "offline full import failed");
    {
        Repository destination(destinationPath);
        expect(destination.head("incoming").id == base, "full CLI sync lost revision ID");
    }
    {
        Repository source(sourcePath);
        tip = source.commit("main", {{key, Bytes{9, 8, 7}}}).id;
    }
    expect(run(cli, {"export-incremental", sourcePath.string(), "main",
                     aowis::native::toHex(base), delta.string()}) == 0,
           "offline incremental export failed");
    expect(run(cli, {"import", destinationPath.string(),
                     delta.string(), "incoming"}) == 0,
           "offline incremental import failed");
    expect(run(cli, {"import", destinationPath.string(),
                     delta.string(), "incoming"}) == 0,
           "offline incremental reimport should be idempotent");
    {
        Repository destination(destinationPath);
        expect(destination.head("incoming").id == tip, "incremental CLI sync lost revision ID");
        expect(destination.get("incoming", key) == Bytes({9, 8, 7}),
               "incremental CLI sync lost payload");
    }
    // The former storage-mode argument ("pack") is no longer accepted.
    expect(run(cli, {"head", destinationPath.string(), "pack", "incoming"}) == 2,
           "obsolete storage-mode argument accepted");
    expect(run(cli, {"head", destinationPath.string(), "incoming"}) == 0, "head command failed");
    expect(run(cli, {"fetch", destinationPath.string(),
                     "example.org", "repo", "main", "incoming"}) == 2,
           "removed remote fetch command must not be accepted");
    expect(run(cli, {"remote-head", destinationPath.string(), "incoming"}) == 2,
           "removed remote helper command must not be accepted");
}

} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "usage: native_sync_cli_tests SYNC_CLI");
        const std::filesystem::path cli = std::filesystem::absolute(argv[1]);
        exercise(cli);
        std::cout << "Offline synchronization CLI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Offline synchronization CLI tests failed: " << error.what() << '\n';
        return 1;
    }
}
