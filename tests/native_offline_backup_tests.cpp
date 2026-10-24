#include "aowis/native/project_service.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelRecord;
using aowis::native::ProjectInfo;
using aowis::native::ProjectService;
using aowis::native::Repository;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TemporaryRoot {
    std::filesystem::path directory;
    TemporaryRoot() {
        char value[] = "/tmp/aowis-backup-restore-XXXXXXXX";
        const char* created = ::mkdtemp(value);
        require(created != nullptr, "mkdtemp failed");
        this->directory = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->directory, ignored);
    }
};

int run(const std::string& python, const std::string& script, const std::string& verify,
        const std::string& command, const std::filesystem::path& source,
        const std::filesystem::path& destination = {}) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        std::vector<std::string> parameters{python, script, command, source.string()};
        if (!destination.empty()) parameters.push_back(destination.string());
        parameters.push_back("--verify-binary");
        parameters.push_back(verify);
        std::vector<char*> args;
        for (std::string& value : parameters) args.push_back(value.data());
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status), "backup process failed abnormally");
    return WEXITSTATUS(status);
}

struct Snapshot {
    std::string id;
    std::string name;
    std::string branch;
    Hash main{};
    Hash alternate{};
};

void compare(const std::filesystem::path& root, const std::vector<Snapshot>& expected) {
    ProjectService service(root);
    const std::vector<ProjectInfo> projects = service.listProjects();
    require(projects.size() == expected.size(), "restored project count mismatch");
    for (const Snapshot& item : expected) {
        const ProjectInfo project = service.project(item.id);
        require(project.name == item.name, "restored project identity mismatch");
        require(service.head(item.id, "main").id == item.main, "restored main head mismatch");
        require(service.head(item.id, item.branch).id == item.alternate,
                "restored secondary branch head mismatch");
        require(service.history(item.id, "main", 30).size() == 2,
                "restored revision history mismatch");
    }
}

void tests(const std::string& python, const std::string& script, const std::string& verify) {
    TemporaryRoot temporary;
    const std::filesystem::path source = temporary.directory / "source";
    const std::filesystem::path backup = temporary.directory / "backup";
    const std::filesystem::path restored = temporary.directory / "restored";
    const std::filesystem::path reserved = temporary.directory / "reserved";
    std::vector<Snapshot> expected;
    {
        ProjectService service(source);
        for (int index = 0; index < 3; ++index) {
            const ProjectInfo info = service.createProject("Hydraulic project " + std::to_string(index));
            const Hash before = service.head(info.id, "main").id;
            const std::string secondary = index == 1 ? "scenario" : "design";
            service.createBranch(info.id, secondary, "main", before);
            Key key{};
            key[0] = static_cast<std::uint8_t>(index + 1);
            ModelRecord value;
            value.kind = static_cast<std::uint16_t>(30 + index);
            value.payload = {1, 2, static_cast<std::uint8_t>(index)};
            Hash after{};
            service.withRepository(info.id, [&](Repository& repository) {
                after = repository.commitModel("main", {{key, value}}, before).id;
            });
            expected.push_back({info.id, info.name, secondary, after, before});
        }
        // The catalogue is open; an offline snapshot must refuse it promptly.
        require(run(python, script, verify, "backup", source, reserved) != 0,
                "backup incorrectly accepted live project service");
        require(!std::filesystem::exists(reserved), "live-root rejection published a backup");
    }
    {
        Repository open(source / "projects" / expected[0].id / "repository");
        require(run(python, script, verify, "backup", source, reserved) != 0,
                "backup incorrectly accepted a direct native-library reader");
        require(!std::filesystem::exists(reserved), "direct handle rejection published backup");
    }
    require(run(python, script, verify, "backup", source, backup) == 0,
            "offline backup creation failed");
    compare(backup, expected);
    require(run(python, script, verify, "verify", backup) == 0,
            "offline backup verification failed");
    require(run(python, script, verify, "restore", backup, restored) == 0,
            "verified restore failed");
    compare(restored, expected);
    require(run(python, script, verify, "backup", source, backup) != 0,
            "existing destination was overwritten");
    compare(backup, expected);
    // A source with damaged committed refs must not yield a published backup.
    const std::filesystem::path ref = source / "projects" / expected[0].id / "repository" /
                                      "refs" / "heads" / "main";
    const std::filesystem::path corrupt = temporary.directory / "corrupt";
    std::filesystem::resize_file(ref, 4);
    require(run(python, script, verify, "backup", source, corrupt) != 0,
            "corrupt branch was accepted");
    require(!std::filesystem::exists(corrupt), "corrupt snapshot was published");
    // Reject a symlink anywhere in the root, including non-repository files.
    const std::filesystem::path link = backup / "unsafe-link";
    std::filesystem::create_symlink("/tmp", link);
    const std::filesystem::path unsafe = temporary.directory / "unsafe";
    require(run(python, script, verify, "restore", backup, unsafe) != 0,
            "symlink in backup was accepted");
    require(!std::filesystem::exists(unsafe), "unsafe restore was published");
    std::filesystem::remove(link);
    // A backup with unreadable physical storage must not be restored merely
    // because its catalogue metadata and branch refs still look correct.
    const std::filesystem::path pack = backup / "projects" / expected[1].id /
                                        "repository" / "objects.pack";
    require(std::filesystem::is_regular_file(pack), "expected physical pack is absent");
    std::filesystem::resize_file(pack, 17);
    const std::filesystem::path broken = temporary.directory / "broken";
    require(run(python, script, verify, "restore", backup, broken) != 0,
            "physically corrupted backup was restored");
    require(!std::filesystem::exists(broken), "corrupt restore was published");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected Python interpreter, script and verifier arguments");
        tests(argv[1], argv[2], argv[3]);
        std::cout << "Offline multi-project backup and restore tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Backup/restore test failed: " << error.what() << '\n';
        return 1;
    }
}
