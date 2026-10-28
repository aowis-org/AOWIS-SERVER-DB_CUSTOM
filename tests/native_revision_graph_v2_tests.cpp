#include "aowis/dvmdb/project_service.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace aowis::dvmdb;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Operation>
void invalid(Operation operation, StorageErrorKind expected, const char* message) {
    try { operation(); }
    catch (const StorageError& error) {
        if (error.kind() == expected) return;
        throw std::runtime_error(std::string(message) + ": unexpected error kind");
    }
    throw std::runtime_error(message);
}
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-graph-v2-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code error; fs::remove_all(this->path, error); }
};
Key key(unsigned value) {
    Key result{};
    result[15] = static_cast<std::uint8_t>(value);
    return result;
}

void runUnbornGraph() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    Repository repo(tmp.path / "unborn");
    repo.createUnbornBranchV2(project, "main");
    const RevisionGraphPageV2 graph = repo.revisionGraphV2(project);
    check(graph.heads.size() == 1 && graph.heads[0].name == "main" &&
          !graph.heads[0].revision && graph.heads[0].recordHash == Hash{} &&
          graph.revisions.empty() && !graph.next,
          "unborn graph must contain a null HEAD and no revisions");
    Repository reopened(tmp.path / "unborn");
    const RevisionGraphPageV2 persisted = reopened.revisionGraphV2(project);
    check(persisted.heads.size() == 1 && !persisted.heads[0].revision &&
          persisted.revisions.empty(), "unborn graph did not survive reopen");
    RevisionGraphCursorV2 forged{{graph.heads.front()}, 1};
    invalid([&] { (void)repo.revisionGraphV2(project, 1, forged); },
            StorageErrorKind::InvalidInput, "unborn graph cursor beyond history accepted");
}

void runRepository() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    Repository repo(tmp.path / "repository");
    const RevisionId root = repo.createBranchV2(project, "main");
    const RevisionId first = repo.commitV2(project, "main", {{key(1), Bytes{1}}}, root);
    check(repo.createBranchV2(project, "feature", "main") == first, "fork HEAD should be shared");
    check(repo.createBranchV2(project, "alias", "main") == first, "alias HEAD should be shared");
    const RevisionId main = repo.commitV2(project, "main", {{key(2), Bytes{2}}}, first);
    const RevisionId feature = repo.commitV2(project, "feature", {{key(3), Bytes{3}}}, first);
    const MergeResultV2 merged = repo.mergeBranchesV2(project, "main", "feature", main, feature);
    check(merged.status == MergeStatus::Merged && merged.resultHead.has_value(),
          "expected divergent merge");
    const RevisionId merge = *merged.resultHead;
    const RevisionGraphPageV2 start = repo.revisionGraphV2(project, 1);
    check(start.heads.size() == 3 && start.revisions.size() == 1 && start.next,
          "v2 graph did not return page and snapshot");
    check(start.heads[0].name == "alias" && start.heads[0].revision == first &&
          start.heads[1].name == "feature" && start.heads[1].revision == feature &&
          start.heads[2].name == "main" && start.heads[2].revision == merge,
          "v2 HEAD snapshot incorrect");
    check(start.heads[0].branchId != start.heads[1].branchId &&
          start.heads[1].branchId != start.heads[2].branchId,
          "branch UUIDs were duplicated");
    check(start.heads[0].originBranchId == start.heads[2].branchId &&
          start.heads[1].originBranchId == start.heads[2].branchId &&
          !start.heads[2].originBranchId,
          "creation provenance does not point at main's branch UUID");
    check(start.revisions[0].id == first || start.revisions[0].id == feature ||
          start.revisions[0].id == merge, "unexpected first v2 graph node");
    // After first page, HEAD moves; the old cursor still represents the
    // immutable earlier snapshot, and a new first page sees the descendant.
    const RevisionId newer = repo.commitV2(project, "main", {{key(4), Bytes{4}}}, merge);
    std::set<RevisionId> observed;
    std::optional<RevisionGraphCursorV2> cursor = start.next;
    for (const RevisionGraphRevisionV2& item : start.revisions) observed.insert(item.id);
    for (unsigned count = 0; cursor; ++count) {
        check(count < 15, "pagination failed to terminate");
        const RevisionGraphPageV2 next = repo.revisionGraphV2(project, 1, cursor);
        check(next.heads[2].revision == merge && next.revisions.size() == 1,
              "v2 graph mixed changed HEADs into pinned snapshot");
        const RevisionGraphRevisionV2& item = next.revisions.front();
        check(item.recordHash != Hash{} && observed.insert(item.id).second,
              "v2 page returned invalid hash or duplicate UUID");
        cursor = next.next;
    }
    check(observed == std::set<RevisionId>({root, first, main, feature, merge}),
          "v2 paginated DAG missed a revision or included a later commit");
    const RevisionGraphPageV2 fresh = repo.revisionGraphV2(project, 100);
    check(!fresh.next && fresh.revisions.size() == 6 && fresh.heads[2].revision == newer,
          "fresh v2 graph did not see current HEAD");
    bool foundMerge = false;
    for (const RevisionGraphRevisionV2& item : fresh.revisions) {
        if (item.id == merge) {
            foundMerge = item.parents.size() == 2 && item.parents[0].id == main &&
                         item.parents[1].id == feature && item.recordHash != Hash{};
        }
    }
    check(foundMerge, "two authenticated merge parents missing from v2 graph");
    check(fresh.heads[0].revision == first, "non-leaf alias HEAD changed");
    const std::vector<RevisionGraphRevisionV2> mainHistory = repo.historyV2(project, "main", 100);
    check(mainHistory.size() == 5 && mainHistory[0].id == newer &&
          mainHistory[1].id == merge && mainHistory[2].id == main &&
          mainHistory[3].id == first && mainHistory[4].id == root,
          "v2 history did not follow first parents");
    const std::vector<RevisionGraphRevisionV2> historyPage = repo.historyV2(project, "main", 2, merge);
    check(historyPage.size() == 2 && historyPage[0].id == main &&
          historyPage[1].id == first, "v2 exclusive history cursor incorrect");
    invalid([&] { (void)repo.historyV2(project, "main", 10, feature); },
            StorageErrorKind::InvalidInput, "second-parent history cursor accepted");
    invalid([&] { (void)repo.revisionGraphV2(project, 0); },
            StorageErrorKind::InvalidInput, "zero page size accepted");
    invalid([&] {
        RevisionGraphCursorV2 forged = *start.next;
        forged.heads[0].recordHash[0] ^= 1;
        (void)repo.revisionGraphV2(project, 1, forged);
    }, StorageErrorKind::Corruption, "forged HEAD digest accepted");
    invalid([&] {
        RevisionGraphCursorV2 forged = *start.next;
        forged.heads[0].name = "../traversal";
        (void)repo.revisionGraphV2(project, 1, forged);
    }, StorageErrorKind::InvalidInput, "invalid cursor branch name accepted");
    invalid([&] { (void)repo.revisionGraphV2(ProjectId::generate(), 100); },
            StorageErrorKind::Conflict, "wrong project UUID accepted");
    {
        RevisionGraphCursorV2 bogus = *start.next;
        bogus.skip = 250000;
        invalid([&] { (void)repo.revisionGraphV2(project, 2, bogus); },
                StorageErrorKind::InvalidInput, "cursor beyond history accepted");
    }
    Repository reopened(tmp.path / "repository");
    check(reopened.revisionGraphV2(project, 100).revisions.size() == 6,
          "v2 graph lost revisions after reopening");
}

void runBranchOriginsAndNames() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const std::string unicodeLabel = "Feature Branch – Süd";
    BranchId mainId = BranchId::generate();
    BranchId featureId = BranchId::generate();
    RevisionId featureRevision = RevisionId::generate();
    {
        Repository repository(temp.path / "repository");
        const RevisionId root = repository.createBranchV2(project, "main");
        mainId = *repository.branchId("main");
        check(repository.createBranchV2(project, unicodeLabel, "main") == root,
              "branch names with spaces and UTF-8 were rejected");
        featureId = *repository.branchId(unicodeLabel);
        check(repository.createBranchV2(project, "Feature Follow Up", unicodeLabel) == root,
              "second-level branch creation failed");
        (void)repository.commitV2(project, "main", {{key(10), Bytes{10}}}, root);
        const RevisionGraphPageV2 initial = repository.revisionGraphV2(project, 1);
        check(initial.heads.size() == 3 && initial.next.has_value(),
              "branch ancestry fixture did not produce a pinned cursor");
        check(initial.heads[0].originBranchId == mainId &&
              initial.heads[1].originBranchId == featureId &&
              !initial.heads[2].originBranchId,
              "branch creation origins were not persisted in the first graph page");
        check(repository.revisionGraphV2(project, 1, initial.next).heads[1].originBranchId == featureId,
              "pinned pagination lost branch origin");
        repository.renameBranchV2(project, unicodeLabel, "Renamed Feature Branch", root);
        featureRevision = repository.commitV2(project, "Renamed Feature Branch",
                                               {{key(11), Bytes{11}}}, root);
        const RevisionGraphPageV2 changed = repository.revisionGraphV2(project, 100);
        check(changed.heads[1].name == "Renamed Feature Branch" &&
              changed.heads[1].branchId == featureId &&
              changed.heads[1].originBranchId == mainId &&
              changed.heads[0].originBranchId == featureId,
              "rename or commit destroyed immutable branch creation provenance");
        invalid([&] { repository.createBranchV2(project, " bad", "main"); },
                StorageErrorKind::InvalidInput, "leading spaces accepted");
        invalid([&] { repository.createBranchV2(project, "bad\nname", "main"); },
                StorageErrorKind::InvalidInput, "embedded newline accepted");
        invalid([&] { repository.createBranchV2(project, "bad/name", "main"); },
                StorageErrorKind::InvalidInput, "unsafe route separators accepted");
    }
    {
        Repository reopened(temp.path / "repository");
        const RevisionGraphPageV2 persisted = reopened.revisionGraphV2(project, 100);
        check(persisted.heads.size() == 3 && persisted.heads[0].originBranchId == featureId
              && persisted.heads[1].originBranchId == mainId
              && persisted.heads[1].revision == featureRevision,
              "reopen lost branch provenance or renamed UTF-8 branch");
    }
}

void runImmutableForkProvenance() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const std::string sourceName = "Source – Süd";
    const std::string forkName = "Historical Fork";
    BranchId sourceId = BranchId::generate();
    BranchId forkId = BranchId::generate();
    RevisionId original = RevisionId::generate();
    RevisionId later = RevisionId::generate();
    {
        Repository repository(temp.path / "repository");
        original = repository.createBranchV2(project, sourceName);
        sourceId = *repository.branchId(sourceName);
        later = repository.commitV2(project, sourceName, {{key(20), Bytes{20}}}, original);
        const std::size_t before = repository.revisionGraphV2(project).revisions.size();
        check(repository.createBranchAtRevisionV2(project, forkName, sourceName, original) == original,
              "historical fork did not select the requested immutable revision");
        forkId = *repository.branchId(forkName);
        const RevisionGraphPageV2 created = repository.revisionGraphV2(project);
        check(created.revisions.size() == before,
              "creating a historical branch introduced a synthetic revision");
        check(repository.headV2(project, sourceName) == later &&
              repository.headV2(project, forkName) == original,
              "historical fork moved the source HEAD or forked from the wrong revision");
        bool found = false;
        for (const RevisionGraphHeadV2& head : created.heads) {
            if (head.branchId != forkId) continue;
            check(head.originBranchId == sourceId && head.revision == original,
                  "created branch lost its source UUID or selected historical HEAD");
            found = true;
        }
        check(found, "historical fork is missing from revision graph");
        repository.renameBranchV2(project, sourceName, "Renamed Source", later);
        (void)repository.commitV2(project, "Renamed Source", {{key(21), Bytes{21}}}, later);
        (void)repository.commitV2(project, forkName, {{key(22), Bytes{22}}}, original);
    }
    {
        Repository reopened(temp.path / "repository");
        const RevisionGraphPageV2 persisted = reopened.revisionGraphV2(project);
        bool found = false;
        for (const RevisionGraphHeadV2& head : persisted.heads) {
            if (head.branchId != forkId) continue;
            check(head.name == forkName && head.originBranchId == sourceId &&
                  head.revision.has_value() && head.revision != original,
                  "restart lost source identity or updated fork HEAD");
            found = true;
        }
        check(found && reopened.branchId("Renamed Source") == sourceId,
              "restart lost branch identity or original source after rename");
    }
}

void runService() {
    Temporary tmp;
    ProjectService service(tmp.path);
    const ProjectInfo project = service.createProjectV2("first");
    const RevisionId root = service.headV2(project.id, "main");
    check(service.createBranchV2(project.id, "second", "main") == root,
          "service did not create shared HEAD");
    const RevisionGraphPageV2 graph = service.revisionGraphV2(project.id, 2);
    check(graph.heads.size() == 2 && graph.revisions.size() == 1 && !graph.next &&
          graph.heads[0].revision == root && graph.heads[1].revision == root,
          "ProjectService v2 graph adapter not correct");
    check(service.historyV2(project.id, "second").size() == 1,
          "ProjectService v2 first-parent reader missing");
    const ProjectInfo other = service.createProjectV2("second");
    const RevisionId otherRoot = service.headV2(other.id, "main");
    check(root != otherRoot, "two empty projects received same UUIDv7 revision");
    check(service.revisionGraphV2(other.id).revisions.front().id == otherRoot,
          "project graph scoped to incorrect project");
}
}

int main() {
    try {
        runUnbornGraph();
        runRepository();
        runBranchOriginsAndNames();
        runImmutableForkProvenance();
        runService();
        std::cout << "UUIDv7 first-parent history and pinned all-parent graph passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 graph test FAILED: " << error.what() << '\n';
        return 1;
    }
}
