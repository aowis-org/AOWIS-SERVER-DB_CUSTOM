#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <filesystem>
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

void exercise() {
    TemporaryRoot root;
    std::string projectName;
    RevisionId initial = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    RevisionId featureTip = RevisionId::generate();
    {
        ProjectService service(root.path);
        const ProjectInfo project = service.createProject("canonical native UUIDv7");
        projectName = project.id;
        const ProjectInfo other = service.createProject("another project");
        const ProjectInfo unborn = service.createUnbornProjectV2("unborn project");
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
        require(reopened.head(projectName, "main") == merged &&
                reopened.head(projectName, "develop") == featureTip &&
                reopened.head(projectName, "historical") == initial,
                "canonical UUIDv7 identities changed after reopening");
        require(reopened.revision(projectName, merged).parents.size() == 2,
                "reopened native merger lost ancestry");
    }
}
} // namespace

int main() {
    try {
        exercise();
        std::cout << "Canonical UUIDv7 ProjectService tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Canonical UUIDv7 ProjectService tests failed: " << error.what() << '\n';
        return 1;
    }
}
