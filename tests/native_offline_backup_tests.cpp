#include "aowis/dvmdb/project_service.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectInfo;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::Repository;

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
    std::optional<RevisionId> main;
    std::optional<RevisionId> alternate;
    std::string mainBranchId;
    std::string alternateBranchId;
};

void compare(const std::filesystem::path& root, const std::vector<Snapshot>& expected) {
    ProjectService service(root);
    const std::vector<ProjectInfo> projects = service.listProjects();
    require(projects.size() == expected.size(), "restored project count mismatch");
    for (const Snapshot& item : expected) {
        const ProjectInfo project = service.project(item.id);
        require(project.name == item.name, "restored project identity mismatch");
        require(service.head(item.id, "main") == item.main, "restored main head mismatch");
        require(service.head(item.id, item.branch) == item.alternate,
                "restored secondary branch head mismatch");
        require(service.history(item.id, "main", 30).size() == 2,
                "restored revision history mismatch");
        const std::optional<aowis::dvmdb::BranchId> mainId = service.branchId(item.id, "main");
        const std::optional<aowis::dvmdb::BranchId> alternateId =
            service.branchId(item.id, item.branch);
        require(mainId.has_value() && mainId->toString() == item.mainBranchId,
                "backup/restore changed the main branch UUIDv7");
        require(alternateId.has_value() && alternateId->toString() == item.alternateBranchId,
                "backup/restore changed the secondary branch UUIDv7");
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
            const RevisionId before = service.head(info.id, "main");
            const std::string secondary = index == 1 ? "scenario" : "design";
            service.createBranch(info.id, secondary, "main", before);
            Key key{};
            key[0] = static_cast<std::uint8_t>(index + 1);
            ModelRecord value;
            value.kind = static_cast<std::uint16_t>(30 + index);
            value.payload = {1, 2, static_cast<std::uint8_t>(index)};
            std::optional<RevisionId> after;
            service.withRepository(info.id, [&](Repository& repository) {
                const ProjectId project = *ProjectId::parse(info.id);
                after = repository.commitModelV2(project, "main", {{key, value}}, before);
            });
            expected.push_back({info.id, info.name, secondary, *after, before,
                                service.branchId(info.id, "main")->toString(),
                                service.branchId(info.id, secondary)->toString()});
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
    // Corrupt/duplicated identity metadata must fail the same full verifier
    // used by offline backup/restore, even though every revision is valid.
    const std::filesystem::path refs = backup / "projects" / expected[0].id /
                                       "repository" / "refs" / "heads";
    const std::filesystem::path sourceId = refs / expected[0].mainBranchId;
    const std::filesystem::path destinationId = refs / expected[0].alternateBranchId;
    // A copied record now duplicates the *name*, while its filename remains
    // the stable original UUID. Full verification must reject this corruption.
    std::filesystem::copy_file(sourceId, destinationId,
                               std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path badIdentityRestore = temporary.directory / "bad-identity";
    require(run(python, script, verify, "restore", backup, badIdentityRestore) != 0,
            "duplicate branch name survived restore verification");
    require(!std::filesystem::exists(badIdentityRestore),
            "invalid branch identity restore was published");
    // Restore the backup's authoritative identity bytes before later checks.
    std::filesystem::copy_file(source / "projects" / expected[0].id /
                               "repository" / "refs" / "heads" / expected[0].alternateBranchId,
                               destinationId, std::filesystem::copy_options::overwrite_existing);
    require(run(python, script, verify, "backup", source, backup) != 0,
            "existing destination was overwritten");
    compare(backup, expected);
    // Pre-release SHA-head records are not eligible for canonical backups.
    const std::filesystem::path shaRef = source / "projects" / expected[0].id /
                                          "repository" / "refs" / "heads" / expected[0].mainBranchId;
    const std::string originalRef = [&] {
        std::ifstream input(shaRef, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }();
    {
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << "AOWIS-BRANCH-1\nmain\n" << std::string(64, 'a') << "\n";
    }
    const std::filesystem::path obsolete = temporary.directory / "obsolete";
    require(run(python, script, verify, "backup", source, obsolete) != 0,
            "SHA-revision branch was accepted into UUIDv7-only backup");
    require(!std::filesystem::exists(obsolete), "obsolete backup was published");
    {
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << originalRef;
    }
    // A syntactically valid UUIDv7 HEAD with a forged record digest must
    // fail full immutable-record verification before destination publication.
    {
        std::string forged = originalRef;
        require(forged.size() > 66, "unexpected short BRANCH-2 record");
        const std::size_t lastLine = forged.rfind('\n', forged.size() - 2);
        require(lastLine != std::string::npos && forged.size() - lastLine == 66,
                "unexpected BRANCH-2 digest layout");
        forged[lastLine + 1] = forged[lastLine + 1] == 'a' ? 'b' : 'a';
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << forged;
    }
    const std::filesystem::path forgedDestination = temporary.directory / "forged-checksum";
    require(run(python, script, verify, "backup", source, forgedDestination) != 0,
            "forged UUIDv7 revision checksum was accepted");
    require(!std::filesystem::exists(forgedDestination), "forged backup was published");
    {
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << originalRef;
    }
    const std::filesystem::path hidden = shaRef.parent_path() / ".unexpected";
    {
        std::ofstream output(hidden, std::ios::binary);
        output << "not a branch";
    }
    const std::filesystem::path hiddenDestination = temporary.directory / "hidden-entry";
    require(run(python, script, verify, "backup", source, hiddenDestination) != 0,
            "unknown hidden branch entry was silently ignored");
    require(!std::filesystem::exists(hiddenDestination), "invalid hidden-entry backup was published");
    std::filesystem::remove(hidden);
    // A source with damaged committed refs must not yield a published backup.
    const std::filesystem::path ref = source / "projects" / expected[0].id / "repository" /
                                      "refs" / "heads" / expected[0].mainBranchId;
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
