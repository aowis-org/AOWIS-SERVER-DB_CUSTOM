#include "aowis/dvmdb/project_service.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using aowis::dvmdb::Hash;
using aowis::dvmdb::Revision;
using aowis::dvmdb::RevisionGraphPage;
using aowis::dvmdb::RevisionGraphCursor;
using aowis::dvmdb::Repository;
using aowis::dvmdb::ProjectService;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        char filename[] = "/tmp/aowis-dvmdb-revision-graph-XXXXXX";
        char* result = ::mkdtemp(filename);
        check(result != nullptr, "mkdtemp failed");
        this->path = result;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(this->path, error); }
};

aowis::dvmdb::Key key(unsigned number) {
    aowis::dvmdb::Key result{};
    result[0] = static_cast<std::uint8_t>(number);
    return result;
}
Revision commit(Repository& repository, const char* branch, unsigned number) {
    return repository.commit(branch, {{key(number), aowis::dvmdb::Bytes{static_cast<std::uint8_t>(number)}}});
}

void run() {
    TemporaryDirectory folder;
    Repository repository(folder.path / "repository");
    repository.createBranch("main");
    const Hash root = repository.head("main").id;
    const Hash first = commit(repository, "main", 1).id;
    repository.createBranch("feature", "main");
    repository.createBranch("alias", "feature");
    const Hash main = commit(repository, "main", 2).id;
    const Hash feature = commit(repository, "feature", 3).id;
    const aowis::dvmdb::MergeResult merge = repository.mergeBranches("main", "feature");
    check(merge.status == aowis::dvmdb::MergeStatus::Merged, "merge did not create a revision");
    check(merge.revision.parents.size() == 2 && merge.revision.parents[0] == main &&
          merge.revision.parents[1] == feature, "merge parent references incorrect");

    const RevisionGraphPage start = repository.revisionGraph(1);
    check(start.heads.size() == 3 && start.next.has_value() && start.revisions.size() == 1,
          "first page must have pinned branch refs and a cursor");
    check(start.heads[0].name == "alias" && start.heads[0].revision == first &&
          start.heads[1].name == "feature" && start.heads[1].revision == feature &&
          start.heads[2].name == "main" && start.heads[2].revision == merge.revision.id,
          "snapshot branch names or HEADs incorrect");

    // The snapshot must not include this new descendant, even if a branch
    // moves between page requests. A fresh request must include it.
    const Hash subsequent = commit(repository, "main", 4).id;
    std::set<Hash> observed;
    std::optional<RevisionGraphCursor> cursor = start.next;
    for (const Revision& revision : start.revisions) observed.insert(revision.id);
    for (int page = 0; cursor; ++page) {
        check(page < 20, "pagination failed to terminate");
        const RevisionGraphPage next = repository.revisionGraph(1, std::move(cursor));
        check(next.heads.size() == 3 && next.heads[2].revision == merge.revision.id,
              "snapshot changed between pages");
        check(next.revisions.size() == 1, "non-terminal page was empty");
        check(observed.insert(next.revisions.front().id).second, "duplicate revision across pages");
        cursor = next.next;
    }
    check(observed == std::set<Hash>({root, first, main, feature, merge.revision.id}),
          "complete paged graph omitted an ancestor, merge side, or duplicated a revision");
    check(!observed.contains(subsequent), "concurrent descendant leaked into pinned snapshot");

    const RevisionGraphPage fresh = repository.revisionGraph(100);
    check(!fresh.next && fresh.revisions.size() == 6 && fresh.heads[2].revision == subsequent,
          "fresh graph did not include new HEAD");
    check(repository.history("main", 100).size() == 5,
          "first-parent history semantics unexpectedly changed");

    // Historical branch HEADs need not be leaves. The alias remains at the
    // first revision despite descendants on two other branches.
    const RevisionGraphPage again = repository.revisionGraph(2);
    check(again.heads[0].revision == first, "non-leaf branch HEAD moved unexpectedly");
    bool invalid = false;
    try {
        RevisionGraphCursor corrupted = *again.next;
        corrupted.heads[0].name = "../invalid";
        (void)repository.revisionGraph(2, corrupted);
    } catch (const aowis::dvmdb::StorageError& error) {
        invalid = error.kind() == aowis::dvmdb::StorageErrorKind::InvalidInput;
    }
    check(invalid, "malformed cursor was not rejected");

    // The public service exposes the same native graph model.
    TemporaryDirectory projects;
    ProjectService service(projects.path);
    const std::string projectId = service.createProject("Sample").id;
    const RevisionGraphPage catalogue = service.revisionGraph(projectId, 100);
    check(catalogue.heads.size() == 1 && catalogue.revisions.size() == 1 && !catalogue.next,
          "project service graph adapter failed");
}
}

int main() {
    try { run(); std::cout << "revision graph snapshot and DAG pagination passed\n"; return 0; }
    catch (const std::exception& error) {
        std::cerr << "revision graph test failed: " << error.what() << '\n'; return 1;
    }
}
