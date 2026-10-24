#include "aowis/native/project_service.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ProjectInfo;
using aowis::native::ProjectService;
using aowis::native::Repository;
using aowis::native::Revision;

constexpr std::string_view oldMain = "7bf2c352bb7308517a3785101b1634b206a954c013549070eb4760ea52f452d1";
constexpr std::string_view oldFeature = "4f0f347555c80634f2578dae167d80b841759511a3b891ab0045a479a8f15bb2";
constexpr std::string_view oldBase = "b8d2bafee89295d4bb183f90b7aad1e2e4a8a76b39229827f79c71405935f468";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryRoot {
    std::filesystem::path directory;
    TemporaryRoot() {
        std::string pattern = (std::filesystem::temp_directory_path() / "aowis-upgrade-restore-XXXXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* created = ::mkdtemp(buffer.data());
        require(created != nullptr, "could not create temporary upgrade directory");
        this->directory = created;
    }
    ~TemporaryRoot() {
        std::error_code error;
        std::filesystem::remove_all(this->directory, error);
    }
};

Key key(std::uint8_t first) {
    Key value{};
    value[0] = first;
    return value;
}

Bytes bytes(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

int offline(const std::string& python, const std::string& script,
            const std::string& verifier, const std::string& command,
            const std::filesystem::path& source, const std::filesystem::path& target = {}) {
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        std::vector<std::string> arguments{python, script, command, source.string()};
        if (!target.empty()) arguments.push_back(target.string());
        arguments.push_back("--verify-binary");
        arguments.push_back(verifier);
        std::vector<char*> pointers;
        for (std::string& argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        ::execvp(pointers.front(), pointers.data());
        ::_exit(127);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
            "offline tool terminated unexpectedly");
    return WEXITSTATUS(status);
}

struct ProjectSnapshot {
    ProjectInfo info;
    std::map<std::string, std::vector<Hash>> histories;
    std::map<std::string, std::vector<std::pair<Key, Bytes>>> records;
};

std::vector<ProjectSnapshot> capture(const std::filesystem::path& root,
                                     const std::vector<ProjectInfo>& projects) {
    std::vector<ProjectSnapshot> result;
    ProjectService service(root);
    require(service.listProjects().size() == projects.size(), "project catalogue count changed");
    for (const ProjectInfo& info : projects) {
        ProjectSnapshot snapshot;
        snapshot.info = info;
        require(service.project(info.id).name == info.name, "project metadata changed");
        service.withRepository(info.id, [&](Repository& repository) {
            repository.verifyRepository();
            const std::vector<std::string> branches = repository.listBranches();
            require(branches.size() == 2, "branch set changed during upgrade");
            for (const std::string& branch : branches) {
                std::vector<Hash> revisions;
                for (const Revision& revision : repository.history(branch, 100)) {
                    revisions.push_back(revision.id);
                }
                require(!revisions.empty(), "empty upgraded branch history");
                snapshot.histories.emplace(branch, revisions);
                snapshot.records.emplace(branch, repository.scan(branch));
            }
        });
        result.push_back(std::move(snapshot));
    }
    return result;
}

void compare(const std::filesystem::path& root, const std::vector<ProjectSnapshot>& expected) {
    ProjectService service(root);
    const std::vector<ProjectInfo> projects = service.listProjects();
    require(projects.size() == expected.size(), "restored project catalogue count mismatch");
    for (const ProjectSnapshot& snapshot : expected) {
        require(service.project(snapshot.info.id).name == snapshot.info.name,
                "restored project ID or display name changed");
        service.withRepository(snapshot.info.id, [&](Repository& repository) {
            repository.verifyRepository();
            const std::vector<std::string> branches = repository.listBranches();
            require(branches.size() == snapshot.histories.size(), "restored branch count changed");
            for (const std::pair<const std::string, std::vector<Hash>>& item : snapshot.histories) {
                require(std::find(branches.begin(), branches.end(), item.first) != branches.end(),
                        "restored branch is missing");
                std::vector<Hash> history;
                for (const Revision& revision : repository.history(item.first, 100)) {
                    history.push_back(revision.id);
                }
                require(history == item.second, "restored revision ancestry differs");
                require(repository.head(item.first).id == item.second.front(),
                        "restored branch head differs");
                require(repository.scan(item.first) == snapshot.records.at(item.first),
                        "restored branch model data differs");
            }
        });
    }
}

void checkHistorical(const std::filesystem::path& root, const std::vector<ProjectInfo>& info) {
    ProjectService service(root);
    for (int index = 0; index < 2; ++index) {
        service.withRepository(info[index].id, [&](Repository& repository) {
            require(aowis::native::toHex(repository.head("main").id) == oldMain,
                    "frozen old-pack main revision changed");
            require(aowis::native::toHex(repository.head("feature").id) == oldFeature,
                    "frozen old-pack divergent branch changed");
            require(repository.get("main", key(0x10)) == bytes("alpha-v2"),
                    "old-pack latest value incorrect");
            require(repository.get("feature", key(0x20)) == bytes("beta-feature"),
                    "old-pack secondary branch value incorrect");
            require(repository.get("main", key(0x20)) == std::nullopt,
                    "old-pack deletion was lost");
            require(repository.getAtRevision(aowis::native::fromHex(oldBase), key(0x20)) == bytes("beta-v1"),
                    "old-pack historic value cannot be read");
        });
    }
}

void exercise(const std::string& python, const std::string& script,
              const std::string& verifier, const std::filesystem::path& fixture) {
    TemporaryRoot temporary;
    const std::filesystem::path source = temporary.directory / "source";
    const std::filesystem::path backup = temporary.directory / "backup";
    const std::filesystem::path restored = temporary.directory / "restored";
    const std::filesystem::path upgradedBackup = temporary.directory / "upgraded-backup";
    const std::filesystem::path upgradedRestored = temporary.directory / "upgraded-restored";
    const std::filesystem::path frozenRepository = fixture / "repository";
    require(std::filesystem::is_regular_file(frozenRepository / "objects.pack"),
            "historical pack fixture is missing");

    std::vector<ProjectInfo> info;
    {
        ProjectService service(source);
        for (int number = 0; number < 4; ++number) {
            info.push_back(service.createProject("Upgrade/restore project " + std::to_string(number)));
        }
        // Two current-format projects: independent branch histories and a large
        // nontrivial payload ensure the test checks more than catalogue files.
        for (int index = 2; index < 4; ++index) {
            service.withRepository(info[index].id, [&](Repository& repository) {
                repository.createBranch("design", "main");
                std::vector<aowis::native::Change> edits;
                const int count = index == 3 ? 160 : 16;
                for (int number = 0; number < count; ++number) {
                    Key recordKey{};
                    recordKey[0] = static_cast<std::uint8_t>(index);
                    recordKey[1] = static_cast<std::uint8_t>(number);
                    const Bytes value(static_cast<std::size_t>(index == 3 ? 8192 : 128),
                                      static_cast<std::uint8_t>((number * 17 + index) % 251));
                    edits.push_back({recordKey, value});
                }
                repository.commit("main", edits);
                repository.commit("design", {{key(static_cast<std::uint8_t>(index + 0x40)),
                                                 bytes("divergent hydraulic design")}});
                repository.verifyRepository();
            });
        }
    }

    // Stage two byte-for-byte historic pre-release packs under stable project
    // identities. ProjectService is stopped before modifying offline fixtures.
    for (int index = 0; index < 2; ++index) {
        const std::filesystem::path path = source / "projects" / info[index].id / "repository";
        std::filesystem::remove_all(path);
        std::filesystem::copy(frozenRepository, path, std::filesystem::copy_options::recursive);
        // The frozen standalone pack omits the runtime lifetime-lock file.
        // A real service root retains that file across clean shutdowns.
        const int lock = ::open((path / "maintenance.lock").c_str(),
                                O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        require(lock >= 0, "could not restore historical repository lifetime lock");
        require(::close(lock) == 0, "could not close historical lifetime lock");
        require(!std::filesystem::exists(path / "objects.idx"),
                "frozen pack unexpectedly contains a derived index");
    }

    require(offline(python, script, verifier, "backup", source, backup) == 0,
            "historical multi-project offline backup failed");
    for (int index = 0; index < 2; ++index) {
        require(!std::filesystem::exists(source / "projects" / info[index].id /
                                          "repository" / "objects.idx"),
                "backup must not rebuild indexes inside the source root");
        require(std::filesystem::exists(backup / "projects" / info[index].id /
                                        "repository" / "objects.idx"),
                "backup failed to rebuild a historic derived index");
    }

    checkHistorical(backup, info);
    const std::vector<ProjectSnapshot> before = capture(backup, info);
    require(before.size() == 4, "snapshot did not capture all four projects");
    require(before[3].records.at("main").size() == 160,
            "large current-format project unexpectedly lost records");
    require(offline(python, script, verifier, "restore", backup, restored) == 0,
            "offline restore from historical multi-project backup failed");
    compare(restored, before);
    checkHistorical(restored, info);

    // First write under the upgraded executable must be durable. An independent
    // source/backup must not observe changes made to the restored copy.
    {
        ProjectService service(restored);
        service.withRepository(info[0].id, [&](Repository& repository) {
            repository.commit("main", {{key(0x60), bytes("post-upgrade commit")}});
            repository.verifyRepository();
            require(repository.getAtRevision(aowis::native::fromHex(oldBase), key(0x20)) == bytes("beta-v1"),
                    "post-upgrade commit broke historic reads");
        });
    }
    const std::vector<ProjectSnapshot> after = capture(restored, info);
    require(after[0].histories.at("main").size() == before[0].histories.at("main").size() + 1,
            "post-upgrade revision was not appended");
    require(after[0].records.at("main").size() == before[0].records.at("main").size() + 1,
            "post-upgrade model write was not retained");
    compare(backup, before);

    require(offline(python, script, verifier, "backup", restored, upgradedBackup) == 0,
            "upgraded multi-project backup failed");
    require(offline(python, script, verifier, "restore", upgradedBackup, upgradedRestored) == 0,
            "second restore of upgraded repositories failed");
    compare(upgradedRestored, after);
    compare(source, before);
    require(offline(python, script, verifier, "verify", upgradedRestored) == 0,
            "final independent offline root verification failed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 5, "expected Python, backup script, native verifier and frozen fixture");
        exercise(argv[1], argv[2], argv[3], std::filesystem::absolute(argv[4]));
        std::cout << "Historical four-project deployment upgrade/restore passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Multi-project upgrade/restore FAILED: " << error.what() << '\n';
        return 1;
    }
}
