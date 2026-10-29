#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {
using aowis::dvmdb::BranchId;
using aowis::dvmdb::Key;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::ProjectInfo;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::RevisionGraphPageV2;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::RevisionGraphRevisionV2;

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template <typename Operation>
void expectError(Operation operation, ProjectErrorKind expected) {
    try { operation(); }
    catch (const ProjectError& error) {
        require(error.kind() == expected, "wrong ProjectService error category");
        return;
    }
    throw std::runtime_error("expected ProjectService failure");
}

struct TemporaryRoot {
    TemporaryRoot() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "aowis-canonical-service-XXXXXXXX").string();
        std::vector<char> data(pattern.begin(), pattern.end());
        data.push_back('\0');
        const char* created = ::mkdtemp(data.data());
        require(created != nullptr, "create temporary root failed");
        this->path = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

ModelChange change(std::uint8_t number) {
    Key key{};
    key[15] = number;
    ModelRecord record;
    record.kind = 1;
    record.payload = {number};
    return ModelChange{key, record};
}

void exerciseRemoteTracking() {
    using aowis::dvmdb::RepositoryRoleV1;
    using aowis::dvmdb::RemoteTrackingRefV1;
    TemporaryRoot localRoot;
    TemporaryRoot sharedRoot;
    TemporaryRoot otherSharedRoot;
    std::string remoteId;
    std::string secondRemoteId;
    std::string projectId;
    std::string branchId;
    RevisionId committed = RevisionId::generate();
    RevisionId sharedTip = RevisionId::generate();
    {
        ProjectService shared(sharedRoot.path);
        remoteId = shared.repositoryId();
        const ProjectInfo project = shared.createUnbornProjectV2("shared unborn project");
        projectId = project.id;
        branchId = shared.branchId(projectId, "main")->toString();
    }
    // Clone the same logical UUIDs into independently owned repository roots.
    // Copying the project here is just test fixture setup, not a Fetch API.
    for (const std::filesystem::path& target : {localRoot.path, otherSharedRoot.path}) {
        std::filesystem::create_directories(target / "projects");
        std::filesystem::copy(sharedRoot.path / "projects" / projectId,
                              target / "projects" / projectId,
                              std::filesystem::copy_options::recursive);
    }
    {
        ProjectService second(otherSharedRoot.path);
        secondRemoteId = second.repositoryId();
        committed = second.commitModelV2(projectId, "main", {change(61)});
    }
    {
        ProjectService local(localRoot.path);
        require(local.repositoryRoleV1() == RepositoryRoleV1::Unconfigured &&
                local.remoteRepositoriesV1().empty() && local.remoteTrackingRefsV1().empty(),
                "legacy root unexpectedly has a role or remote tracking data");
        local.configureRepositoryRoleV1(RepositoryRoleV1::Private);
        local.configureRepositoryRoleV1(RepositoryRoleV1::Private);
        expectError([&]() { local.configureRepositoryRoleV1(RepositoryRoleV1::Shared); },
                    ProjectErrorKind::Conflict);
        expectError([&]() { local.registerRemoteRepositoryV1(local.repositoryId(),
                    "https://example.test/dvmdb/v1"); }, ProjectErrorKind::InvalidInput);
        expectError([&]() { local.registerRemoteRepositoryV1(remoteId,
                    "https://user:pass@example.test/"); }, ProjectErrorKind::InvalidInput);
        local.registerRemoteRepositoryV1(remoteId, "https://shared.example.test/dvmdb/v1");
        local.registerRemoteRepositoryV1(remoteId, "https://shared.example.test/dvmdb/v1");
        expectError([&]() { local.registerRemoteRepositoryV1(secondRemoteId,
                    "https://shared.example.test/dvmdb/v1"); }, ProjectErrorKind::Conflict);
        local.registerRemoteRepositoryV1(secondRemoteId, "file:///mnt/shared-dvmdb");
        expectError([&]() { local.registerRemoteRepositoryV1(remoteId,
                    "https://changed.example.test/dvmdb/v1"); }, ProjectErrorKind::Conflict);
        require(local.remoteRepositoriesV1().size() == 2,
                "remote registry did not preserve distinct repository UUIDs");
        const RemoteTrackingRefV1 unborn{remoteId, projectId, branchId, std::nullopt};
        const RemoteTrackingRefV1 committedRemote{secondRemoteId, projectId, branchId, committed};
        local.recordRemoteTrackingRefV1(unborn, std::nullopt);
        local.recordRemoteTrackingRefV1(committedRemote, std::nullopt);
        require(local.remoteTrackingRefsV1().size() == 2,
                "same logical branch from two remotes collided in tracking namespace");
        expectError([&]() { local.recordRemoteTrackingRefV1(unborn, std::nullopt); },
                    ProjectErrorKind::Conflict);
        ProjectService shared(sharedRoot.path);
        sharedTip = shared.commitModelV2(projectId, "main", {change(62)});
        const RemoteTrackingRefV1 firstCommit{remoteId, projectId, branchId, sharedTip};
        local.recordRemoteTrackingRefV1(firstCommit,
            std::optional<std::optional<RevisionId>>(std::optional<RevisionId>{}));
        expectError([&]() { local.recordRemoteTrackingRefV1(unborn,
                    std::optional<std::optional<RevisionId>>(std::optional<RevisionId>{})); },
                    ProjectErrorKind::Conflict);
        require(local.remoteTrackingRefsV1().size() == 2 &&
                !local.listProjects().empty() &&
                !local.optionalHeadV2(projectId, "main").has_value(),
                "remote tracking unexpectedly moved the local unborn branch HEAD");
        const RevisionId localTip = local.commitModelV2(projectId, "main", {change(63)});
        require(localTip != sharedTip && localTip != committed &&
                shared.optionalHeadV2(projectId, "main") == sharedTip,
                "local commit moved shared repository HEAD");
        ProjectService second(otherSharedRoot.path);
        require(second.optionalHeadV2(projectId, "main") == committed,
                "local commit moved another remote repository HEAD");
    }
    {
        ProjectService reopened(localRoot.path);
        require(reopened.repositoryRoleV1() == RepositoryRoleV1::Private &&
                reopened.remoteRepositoriesV1().size() == 2 &&
                reopened.remoteTrackingRefsV1().size() == 2,
                "remote registry or tracking state changed across restart");
        bool foundFirst = false;
        bool foundSecond = false;
        for (const RemoteTrackingRefV1& entry : reopened.remoteTrackingRefsV1()) {
            require(entry.projectId == projectId && entry.branchId == branchId,
                    "reloaded tracking branch identity changed");
            if (entry.remoteRepositoryId == remoteId && entry.observedHead == sharedTip)
                foundFirst = true;
            if (entry.remoteRepositoryId == secondRemoteId && entry.observedHead == committed)
                foundSecond = true;
        }
        require(foundFirst && foundSecond, "reloaded tracking lost repository qualification");
        reopened.recordRemoteTrackingRefV1({remoteId, projectId, branchId, sharedTip},
            std::optional<std::optional<RevisionId>>(std::optional<RevisionId>(sharedTip)));
    }
    {
        const std::filesystem::path tracking = localRoot.path / "repository.tracking";
        std::ofstream corrupted(tracking, std::ios::binary | std::ios::trunc);
        corrupted << "AOWIS-TRACKING-1\nrole\tprivate\nref\tinvalid\n";
        corrupted.close();
        bool rejected = false;
        try { ProjectService invalid(localRoot.path); }
        catch (const aowis::dvmdb::StorageError&) { rejected = true; }
        require(rejected, "corrupt remote tracking metadata was silently accepted");
    }
}

void exercise() {
    TemporaryRoot root;
    std::string projectName;
    std::string savedRepositoryId;
    std::string pendingUnbornProject;
    std::optional<BranchId> pendingUnbornBranch;
    RevisionId initial = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    RevisionId featureTip = RevisionId::generate();
    {
        ProjectService service(root.path);
        savedRepositoryId = service.repositoryId();
        require(aowis::dvmdb::UuidV7::parse(savedRepositoryId).has_value(),
                "new repository root has no UUIDv7 identity");
        require(service.repositoryId() == savedRepositoryId,
                "repository UUID changed during the service lifetime");
        const ProjectInfo project = service.createProject("canonical native UUIDv7");
        projectName = project.id;
        const ProjectInfo other = service.createProject("another project");
        const ProjectInfo unborn = service.createUnbornProjectV2("unborn project");
        const ProjectInfo pending = service.createUnbornProjectV2("persistently unborn project");
        pendingUnbornProject = pending.id;
        pendingUnbornBranch = service.branchId(pending.id, "main");
        require(pendingUnbornBranch.has_value(), "new unborn main has no branch identity");
        require(!service.optionalHeadV2(pending.id, "main").has_value(),
                "new unborn project has a committed HEAD");
        require(!service.optionalHeadV2(unborn.id, "main").has_value(),
                "unborn project unexpectedly has committed HEAD");
        require(service.historyV2(unborn.id, "main").empty(),
                "unborn project has a nonempty revision history");
        require(service.revisionGraphV2(unborn.id).revisions.empty(),
                "unborn project has graph revisions");
        const RevisionId firstUnborn = service.commitModelV2(unborn.id, "main", {change(9)});
        require(service.optionalHeadV2(unborn.id, "main") == firstUnborn,
                "first unborn commit did not publish the HEAD");
        require(service.revision(unborn.id, firstUnborn).parents.empty(),
                "first unborn commit must have no parents");

        require(ProjectId::parse(project.id).has_value(), "canonical project is not UUIDv7");
        initial = service.head(project.id, "main");
        require(initial != service.head(other.id, "main"),
                "independent empty projects must have different revision identities");
        const RevisionGraphRevisionV2 first = service.revision(project.id, initial);
        require(first.id == initial && first.parents.empty(), "canonical initial revision is invalid");
        require(service.history(project.id, "main").size() == 1,
                "canonical native history cannot read initial revision");

        require(service.createBranch(project.id, "feature", "main") == initial,
                "fork must share HEAD revision identity");
        const std::optional<BranchId> branch = service.branchId(project.id, "feature");
        require(branch.has_value(), "canonical branch has no UUIDv7 identity");
        const RevisionId mainTip = service.commitModel(project.id, "main", {change(1)}, initial);
        featureTip = service.commitModel(project.id, "feature", {change(2)}, initial);
        require(mainTip != initial && mainTip != featureTip,
                "distinct commits must create distinct revisions");
        const aowis::dvmdb::MergeResultV2 result = service.mergeBranches(
            project.id, "main", "feature", mainTip, featureTip);
        require(result.status == MergeStatus::Merged && result.resultHead.has_value(),
                "disjoint native merge must produce a new revision");
        merged = *result.resultHead;
        require(service.head(project.id, "main") == merged &&
                service.head(project.id, "feature") == featureTip,
                "native merge moved the wrong HEAD");
        const RevisionGraphRevisionV2 record = service.revision(project.id, merged);
        require(record.parents.size() == 2 && record.parents[0].id == mainTip &&
                record.parents[1].id == featureTip, "merge lost ordered parent UUIDs");
        Key firstKey{};
        firstKey[15] = 1;
        Key secondKey{};
        secondKey[15] = 2;
        require(service.getModelAtRevision(project.id, merged, firstKey).has_value() &&
                service.getModelAtRevision(project.id, merged, secondKey).has_value(),
                "merged model lost one of its parents' objects");
        require(!service.getModelAtRevision(project.id, featureTip, firstKey).has_value(),
                "historical feature snapshot was overwritten");

        const RevisionGraphPageV2 firstPage = service.revisionGraph(project.id, 1);
        require(firstPage.revisions.size() == 1 && firstPage.next.has_value() &&
                firstPage.heads.size() == 2, "canonical DAG pagination is incomplete");
        require(service.createBranch(project.id, "historical", "main", initial) == initial,
                "historical fork must share its selected ancestor revision");
        const RevisionGraphPageV2 pinned = service.revisionGraph(project.id, 100, firstPage.next);
        require(pinned.heads.size() == 2 && pinned.revisions.size() == 3,
                "pinned DAG page changed when a branch was created");
        service.renameBranch(project.id, "feature", "develop", featureTip);
        require(service.branchId(project.id, "develop") == branch &&
                service.head(project.id, "develop") == featureTip,
                "rename changed branch identity or HEAD");
        expectError([&]() { service.renameBranch(project.id, "develop", "wrong", initial); },
                    ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.commitModel(project.id, "main", {change(3)}, initial); },
                    ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.createBranch(project.id, "invalid", std::nullopt, initial); },
                    ProjectErrorKind::InvalidInput);
        expectError([&]() { (void)service.revision(project.id, service.head(other.id, "main")); },
                    ProjectErrorKind::NotFound);
        expectError([&]() { (void)service.history(project.id, "main", 0); },
                    ProjectErrorKind::InvalidInput);
    }
    {
        ProjectService reopened(root.path);
        require(reopened.repositoryId() == savedRepositoryId,
                "repository UUID changed after restart");
        require(reopened.branchId(pendingUnbornProject, "main") == pendingUnbornBranch,
                "unborn main branch identity changed after service restart");
        require(!reopened.optionalHeadV2(pendingUnbornProject, "main").has_value(),
                "unborn HEAD changed after service restart");
        require(reopened.historyV2(pendingUnbornProject, "main").empty(),
                "reopened unborn project contains fabricated history");
        const RevisionGraphPageV2 unbornGraph = reopened.revisionGraphV2(pendingUnbornProject);
        require(unbornGraph.revisions.empty() && unbornGraph.heads.size() == 1 &&
                unbornGraph.heads.front().branchId == *pendingUnbornBranch &&
                !unbornGraph.heads.front().revision.has_value(),
                "reopened unborn graph lost its null HEAD or branch identity");
        require(reopened.head(projectName, "main") == merged &&
                reopened.head(projectName, "develop") == featureTip &&
                reopened.head(projectName, "historical") == initial,
                "canonical UUIDv7 identities changed after reopening");
        require(reopened.revision(projectName, merged).parents.size() == 2,
                "reopened native merger lost ancestry");
    }
    // Independent DVMDB roots have different identities even when the
    // project names coincide. Identity is never derived from a GUI label.
    {
        TemporaryRoot otherRoot;
        ProjectService other(otherRoot.path);
        require(other.repositoryId() != savedRepositoryId,
                "independent repository roots share the same UUID");
    }
    // Corrupted identity must not be silently regenerated on restart.
    {
        const std::filesystem::path identity = root.path / "repository.id";
        std::ofstream corrupted(identity, std::ios::binary | std::ios::trunc);
        corrupted << "AOWIS-REPOSITORY-1\nnot-a-uuid\n";
        corrupted.close();
        bool rejected = false;
        try { ProjectService invalid(root.path); }
        catch (const aowis::dvmdb::StorageError&) { rejected = true; }
        require(rejected, "corrupted repository identity was silently regenerated");
    }
}
} // namespace

int main() {
    try {
        exercise();
        exerciseRemoteTracking();
        std::cout << "Canonical UUIDv7 ProjectService tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Canonical UUIDv7 ProjectService tests failed: " << error.what() << '\n';
        return 1;
    }
}
