#include "aowis/native/project_service.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using aowis::native::ProjectService;
using aowis::native::ProjectError;
using aowis::native::ProjectErrorKind;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryRoot {
    std::filesystem::path directory;
    TemporaryRoot() {
        char pattern[] = "/tmp/aowis-dvmdb-projects-XXXXXX";
        char* directoryName = ::mkdtemp(pattern);
        require(directoryName != nullptr, "mkdtemp failed");
        this->directory = directoryName;
    }
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(this->directory, error); }
};

void expectError(const std::function<void()>& action, ProjectErrorKind kind) {
    bool rejected = false;
    try { action(); }
    catch (const ProjectError& error) { rejected = true; require(error.kind() == kind, "unexpected project error kind"); }
    require(rejected, "operation was not rejected");
}

void exerciseBackend() {
    TemporaryRoot root;
    std::string firstId;
    std::string secondId;
    aowis::native::Hash firstHead{};
    {
        ProjectService service(root.directory);
        require(service.listProjects().empty(), "new catalogue is not empty");
        const aowis::native::ProjectInfo first = service.createProject("Engineering Å");
        const aowis::native::ProjectInfo second = service.createProject("Second project");
        firstId = first.id;
        secondId = second.id;
        require(ProjectService::validProjectId(firstId), "invalid generated UUID");
        require(firstId != secondId, "duplicate project IDs");
        require(first.name == "Engineering Å", "bad metadata");
        require(service.project(firstId).name == first.name, "catalogue read differs");
        require(service.listProjects().size() == 2, "missing published project");
        require(service.listBranches(firstId) == std::vector<std::string>{"main"}, "missing initial main branch");
        firstHead = service.head(firstId, "main").id;
        require(service.history(firstId, "main", 10).size() == 1, "missing initial revision");
        require(service.revision(firstId, firstHead).id == firstHead, "revision lookup failed");
        require(service.history(firstId, "main", 10, firstHead).empty(), "history cursor should be exclusive");
        require(service.head(secondId, "main").id == firstHead,
                "initial object identities should be deterministic");
        const aowis::native::Revision fork = service.createBranch(firstId, "design", "main", firstHead);
        require(fork.id == firstHead, "pinned fork changed revision");
        service.createBranch(firstId, "independent");
        require(service.listBranches(firstId).size() == 3, "missing new branch");
        require(service.listBranches(secondId).size() == 1, "projects are not isolated");
        // The model HTTP adapter's scoped native callback shares the exact
        // project handle and serializes access without exposing its lifetime.
        aowis::native::Key modelKey{};
        modelKey[0] = 0x91;
        aowis::native::Hash modelRevision{};
        service.withRepository(firstId, [&](aowis::native::Repository& repository) {
            aowis::native::ModelRecord record;
            record.kind = 31;
            record.payload = {1, 3, 5};
            modelRevision = repository.commitModel("main", {{modelKey, record}}, firstHead).id;
            require(repository.getModel("main", modelKey).has_value(),
                    "scoped repository callback did not persist model record");
        });
        require(modelRevision != firstHead && service.head(firstId, "main").id == modelRevision,
                "scoped repository callback lost the model commit");
        require(service.head(secondId, "main").id == firstHead,
                "scoped model commit leaked across project boundaries");
        expectError([&] {
            service.withRepository("../../wrong", [](aowis::native::Repository&) {});
        }, ProjectErrorKind::InvalidInput);
        bool callbackFailure = false;
        try {
            service.withRepository(firstId, [](aowis::native::Repository&) {
                throw std::runtime_error("intentional callback failure");
            });
        } catch (const std::runtime_error&) { callbackFailure = true; }
        require(callbackFailure && service.head(firstId, "main").id == modelRevision,
                "failing scoped callback left catalogue locked or corrupted");
        expectError([&]{ service.createBranch(firstId, "design"); }, ProjectErrorKind::Conflict);
        expectError([&]{ service.project("../../etc/passwd"); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.project("00000000-0000-4000-8000-000000000000"); }, ProjectErrorKind::NotFound);
        expectError([&]{ service.head(firstId, "missing"); }, ProjectErrorKind::NotFound);
        expectError([&]{ service.createBranch(firstId, "fork", "missing"); }, ProjectErrorKind::NotFound);
        expectError([&]{ service.revision(firstId, aowis::native::sha256(aowis::native::Bytes{1,2,3,4})); }, ProjectErrorKind::NotFound);
        expectError([&]{ service.createProject(""); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createProject("bad\nname"); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createProject(std::string("\xc0\xaf", 2)); },
                    ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createProject(std::string("\xed\xa0\x80", 3)); },
                    ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createBranch(firstId, "../invalid"); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createBranch(firstId, "fork", std::nullopt, firstHead); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.history(firstId, "main", 1001); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.history(firstId, "main", 0); }, ProjectErrorKind::InvalidInput);
        // No second service instance (and no separate process) can own the same root.
        expectError([&]{ ProjectService another(root.directory); }, ProjectErrorKind::Conflict);
        const pid_t pid = ::fork();
        require(pid >= 0, "fork failed");
        if (pid == 0) {
            try { ProjectService another(root.directory); _exit(11); }
            catch (const ProjectError& error) {
                _exit(error.kind() == ProjectErrorKind::Conflict ? 0 : 12);
            } catch (...) { _exit(13); }
        }
        int status = 0;
        require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "project catalogue process lock not exclusive");

        std::atomic<int> completed{0};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 6; ++worker) {
            workers.emplace_back([&, worker] {
                const aowis::native::ProjectInfo item = service.createProject("thread-" + std::to_string(worker));
                require(!item.id.empty(), "concurrent create failed");
                ++completed;
            });
        }
        for (std::thread& worker : workers) worker.join();
        require(completed == 6 && service.listProjects().size() == 8,
                "concurrent catalogue publication lost projects");
    }
    {
        ProjectService reopened(root.directory);
        require(reopened.listProjects().size() == 8, "restart lost catalogue entries");
        require(reopened.project(firstId).name == "Engineering Å", "restart changed project metadata");
        require(reopened.head(firstId, "design").id == firstHead, "restart lost branch");
        require(reopened.listBranches(secondId).size() == 1, "restart crossed project boundaries");
    }

    // Staging debris from an interrupted project creation is never advertised.
    std::filesystem::create_directory(root.directory / "projects" / ".creating-orphan");
    {
        ProjectService service(root.directory);
        require(service.listProjects().size() == 8, "incomplete staging project was published");
    }
    // A published but damaged manifest is not silently ignored.
    {
        std::ofstream damaged(root.directory / "projects" / firstId / "project.meta", std::ios::trunc);
        damaged << "bad manifest\n";
    }
    bool failed = false;
    try { ProjectService service(root.directory); }
    catch (const std::exception&) { failed = true; }
    require(failed, "corrupt catalogue should fail startup");
}

// A growing branch catalogue must not make a single-branch request depend on
// enumerating every other ref. Use real, immutable ref files to build the
// directory without spending thousands of fsyncs creating test-only branches.
void exerciseLargeBranchCatalogue() {
    TemporaryRoot root;
    ProjectService service(root.directory);
    const aowis::native::ProjectInfo project = service.createProject("Branch scaling");
    const aowis::native::Hash mainHead = service.head(project.id, "main").id;
    const std::filesystem::path heads = root.directory / "projects" / project.id /
                                        "repository" / "refs" / "heads";
    constexpr unsigned branchCount = 4096;
    for (unsigned number = 0; number < branchCount; ++number) {
        const std::string branch = "bulk_" + std::to_string(number);
        std::filesystem::copy_file(heads / "main", heads / branch);
    }
    require(service.listBranches(project.id).size() == branchCount + 1U,
            "large branch catalogue is missing entries");
    for (const unsigned number : {0U, 1024U, 2048U, 4095U}) {
        const std::string branch = "bulk_" + std::to_string(number);
        require(service.head(project.id, branch).id == mainHead,
                "targeted branch read changed revision");
        require(service.history(project.id, branch, 1).at(0).id == mainHead,
                "targeted branch history changed revision");
    }
    service.createBranch(project.id, "new_from_bulk", "bulk_2048");
    require(service.head(project.id, "new_from_bulk").id == mainHead,
            "fork from large branch catalogue lost head");
    expectError([&] { service.head(project.id, "absent"); }, ProjectErrorKind::NotFound);
    expectError([&] { service.history(project.id, "absent", 1); }, ProjectErrorKind::NotFound);
    expectError([&] { service.createBranch(project.id, "missing_parent", "absent"); },
                ProjectErrorKind::NotFound);
    expectError([&] { service.createBranch(project.id, "bulk_2048"); },
                ProjectErrorKind::Conflict);

    // Directory enumeration ignored unsafe branch refs before the targeted
    // lookup change. Preserve that boundary and do not follow symlinks.
    std::filesystem::create_symlink("main", heads / "symlink_ref");
    std::filesystem::create_directory(heads / "directory_ref");
    require(service.listBranches(project.id).size() == branchCount + 2U,
            "unsafe branch refs were included in enumeration");
    for (const char* branch : {"symlink_ref", "directory_ref"}) {
        expectError([&] { service.head(project.id, branch); }, ProjectErrorKind::NotFound);
        expectError([&] { service.history(project.id, branch, 1); }, ProjectErrorKind::NotFound);
        expectError([&] { service.createBranch(project.id, "unsafe_source", branch); },
                    ProjectErrorKind::NotFound);
    }
}

void exerciseUnsafeCatalogue() {
    TemporaryRoot root;
    const std::filesystem::path directory = root.directory / "projects";
    std::filesystem::create_directory(directory);
    std::filesystem::create_symlink("/tmp", directory / "00000000-0000-4000-8000-000000000000");
    bool rejected = false;
    try { ProjectService service(root.directory); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "symlinked project must fail startup");
}
} // namespace

int main() {
    try {
        exerciseBackend();
        exerciseUnsafeCatalogue();
        exerciseLargeBranchCatalogue();
        std::cout << "Project catalogue and lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project service test failed: " << error.what() << '\n';
        return 1;
    }
}
