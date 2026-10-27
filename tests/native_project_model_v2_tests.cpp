#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace aowis::dvmdb;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Callback>
void rejects(Callback callback, ProjectErrorKind kind, const char* message) {
    try { callback(); }
    catch (const ProjectError& error) {
        if (error.kind() == kind) return;
        throw std::runtime_error(std::string(message) + ": incorrect error kind");
    }
    throw std::runtime_error(message);
}

struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-project-model-v2-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code ec; fs::remove_all(this->path, ec); }
};

Key key(std::uint8_t value) {
    Key id{};
    id[15] = value;
    return id;
}

ModelRecord record(std::uint8_t value, std::vector<Key> references = {}) {
    ModelRecord result;
    result.kind = 31;
    result.payload = {value};
    result.references = std::move(references);
    return result;
}

void testV2ModelWorkflow() {
    Temporary temp;
    fs::create_directories(temp.path);
    ProjectId project = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId first = RevisionId::generate();
    RevisionId second = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    const Key a = key(1);
    const Key b = key(2);
    BranchId mainBranch = BranchId::generate();
    BranchId featureBranch = BranchId::generate();
    {
        ProjectService service(temp.path);
        const ProjectInfo info = service.createProjectV2("New UUID model project");
        project = *ProjectId::parse(info.id);
        root = service.headV2(info.id, "main");
        check(service.listBranches(info.id) == std::vector<std::string>{"main"},
              "initial branch missing");
        rejects([&] { (void)service.getModelAtRevisionV2(info.id, root, a); },
                ProjectErrorKind::InvalidInput, "raw initial revision exposed model reads");
        service.withRepository(info.id, [&](Repository& repo) {
            mainBranch = *repo.branchId("main");
            const RevisionRecordV2 initial = decodeRevisionRecordV2(*repo.readRevisionV2(project, root));
            check(initial.parents.empty() && !initial.modelIndexed, "initial revision not raw empty");
        });
        first = service.commitModelV2(info.id, "main", {{a, record(11)}}, root);
        check(first != root && service.headV2(info.id, "main") == first,
              "model commit did not advance UUID HEAD");
        check(service.getModelAtRevisionV2(info.id, first, a)->payload == Bytes{11},
              "model payload was not stored");
        service.withRepository(info.id, [&](Repository& repo) {
            const std::vector<Difference> changes = repo.diffRevisionsV2(project, root, first, 1);
            check(changes.size() == 1 && changes.front().key == a &&
                  !changes.front().before && changes.front().after.has_value(),
                  "UUIDv7 immutable diff did not report model addition");
            const std::vector<Difference> replay = repo.diffRevisionsV2(project, first, first, 0);
            check(replay.empty(), "same-revision UUID diff was not empty");
        });
        rejects([&] { (void)service.getModelAtRevisionV2(info.id, root, a); },
                ProjectErrorKind::InvalidInput, "initial revision was mutated");
        const RevisionId shared = service.createBranchV2(info.id, "feature", "main");
        check(shared == first, "branch creation duplicated revision");
        service.withRepository(info.id, [&](Repository& repo) {
            featureBranch = *repo.branchId("feature");
        });
        check(mainBranch != featureBranch, "branch UUIDs are not unique");
        second = service.commitModelV2(info.id, "main", {{b, record(22, {a, a})}}, first);
        feature = service.commitModelV2(info.id, "feature", {{b, record(33)}}, first);
        check(second != feature && service.headV2(info.id, "main") == second &&
              service.headV2(info.id, "feature") == feature,
              "forked UUID revisions did not advance independently");
        service.withRepository(info.id, [&](Repository& repo) {
            const std::vector<Difference> changes = repo.diffRevisionsV2(project, root, second, 2);
            check(changes.size() == 2 && changes[0].key == a && changes[1].key == b,
                  "UUIDv7 immutable diff missed changed model records");
            bool limited = false;
            try { (void)repo.diffRevisionsV2(project, root, second, 1); }
            catch (const StorageError& error) {
                limited = error.kind() == StorageErrorKind::LimitExceeded;
            }
            check(limited, "UUIDv7 immutable diff ignored its result limit");
        });
        check(service.getModelAtRevisionV2(info.id, second, b)->payload == Bytes{22} &&
              service.getModelAtRevisionV2(info.id, feature, b)->payload == Bytes{33},
              "branch snapshots not isolated");
        rejects([&] { (void)service.commitModelV2(info.id, "main", {}, first); },
                ProjectErrorKind::Conflict, "stale HEAD was accepted");
        rejects([&] { (void)service.commitModelV2(info.id, "main", {{a, std::nullopt}}, second); },
                ProjectErrorKind::InvalidInput, "deletion of referenced model was accepted");
        rejects([&] { (void)service.commitModelV2(info.id, "main", {{key(3), record(44, {key(9)})}}, second); },
                ProjectErrorKind::InvalidInput, "dangling reference was accepted");
        check(service.headV2(info.id, "main") == second,
              "rejected commit advanced HEAD");
        service.withRepository(info.id, [&](Repository& repo) {
            const RevisionRecordV2 current = decodeRevisionRecordV2(*repo.readRevisionV2(project, second));
            check(current.modelIndexed && current.parents.size() == 1 &&
                  current.parents.front().id == first,
                  "model index or UUID parent lost");
            check(current.referencesRoot != Hash{}, "reverse index not persisted");
        });
    }
    {
        ProjectService service(temp.path);
        check(service.listProjects().size() == 1, "reopen lost project catalogue");
        const std::string id = project.toString();
        check(service.headV2(id, "main") == second &&
              service.headV2(id, "feature") == feature,
              "reopen changed branch HEAD identities");
        check(service.getModelAtRevisionV2(id, second, b)->references == std::vector<Key>{a},
              "reopen lost deduplicated model references");
        service.withRepository(id, [&](Repository& repo) {
            check(*repo.branchId("main") == mainBranch &&
                  *repo.branchId("feature") == featureBranch,
                  "reopen changed branch UUIDs");
        });
    }
}
}

int main() {
    try {
        testV2ModelWorkflow();
        std::cout << "UUIDv7 project model commit, fork, references and reopen passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 project model test FAILED: " << error.what() << '\n';
        return 1;
    }
}
