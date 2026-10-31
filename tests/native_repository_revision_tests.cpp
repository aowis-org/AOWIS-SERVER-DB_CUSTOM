#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_record.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace aowis::dvmdb;
namespace fs = std::filesystem;

void check(bool ok, const char* context) {
    if (!ok) throw std::runtime_error(context);
}

template<class Callable>
void expected(Callable&& operation, StorageErrorKind kind, const char* context) {
    try { operation(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        std::cerr << context << ": unexpected error kind " << StorageError::kindName(error.kind()) << '\n';
        throw;
    }
    throw std::runtime_error(context);
}

struct Sandbox {
    fs::path path;
    Sandbox() {
        this->path = fs::temp_directory_path() / ("aowis-repo-v2-" + UuidV7::generate().toString());
        fs::create_directories(this->path);
    }
    ~Sandbox() { std::error_code ec; fs::remove_all(this->path, ec); }
};

void testLivePackStorage() {
    Sandbox tmp;
    const ProjectId project = ProjectId::generate();
    const ProjectId other = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId main = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    {
        Repository repo(tmp.path);
        check(!repo.readRevision(other, RevisionId::generate()),
              "empty read unexpectedly resolved a revision");
        check(!fs::exists(tmp.path / "revision-v2"),
              "empty read pinned a caller-chosen project UUID");
        root = repo.writeRevision(project, Hash{}, Hash{}, false);
        main = repo.writeRevision(project, Hash{}, Hash{}, false, {root});
        feature = repo.writeRevision(project, Hash{}, Hash{}, false, {root});
        merged = repo.writeRevision(project, Hash{}, Hash{}, false, {main, feature});
        check(root != main && root != feature && main != feature && feature != merged,
              "equal snapshots were given identical revision identities");
        const std::optional<Bytes> bytes = repo.readRevision(project, merged);
        check(bytes.has_value(), "could not retrieve merge from real content pack");
        const RevisionRecord record = decodeRevisionRecord(*bytes);
        check(record.id == merged && record.project == project && record.parents.size() == 2,
              "wrong project or revision identity for packed merge");
        check(record.parents[0].id == main && record.parents[1].id == feature,
              "merge parent ordering was lost");
        const std::optional<Bytes> first = repo.readRevision(project, main);
        check(first.has_value() && record.parents[0].expectedRecordHash == sha256(*first),
              "merge parent SHA-256 not bound to stored bytes");
        expected([&] { (void)repo.readRevision(other, merged); }, StorageErrorKind::Conflict,
                 "foreign project lookup was not rejected");
        expected([&] { (void)repo.writeRevision(project, Hash{}, Hash{}, false,
                                                    {RevisionId::generate()}); },
                 StorageErrorKind::NotFound, "unknown revision parent accepted");
        expected([&] { (void)repo.writeRevision(project, Hash{}, sha256(Bytes{'x'}), false); },
                 StorageErrorKind::InvalidInput, "raw record with reference index accepted");
        expected([&] { (void)repo.writeRevision(project, sha256(Bytes{'x'}), Hash{}, false); },
                 StorageErrorKind::Corruption, "unpacked content root accepted");
        // Published UUID->SHA bindings are durable even before a branch
        // points to them; the whole repository verifier and compactor must
        // preserve the complete ancestry, including merge second parents.
        repo.verifyPack();
        const VerificationReport report = repo.verifyRepository();
        check(report.revisions == 4 && report.branches == 0 &&
              report.reachableObjects == 4,
              "whole-repository verification missed unanchored UUIDv7 revisions");
        const CompactionStats compacted = repo.compact();
        check(compacted.objectsAfter == 4,
              "compaction did not retain every published revision");
        const std::optional<Bytes> retained = repo.readRevision(project, merged);
        check(retained && decodeRevisionRecord(*retained).parents.size() == 2,
              "merge revision disappeared during compaction");
        check(repo.verifyRepository().revisions == 4,
              "post-compaction repository verification missed ancestry");
    }
    {
        Repository repo(tmp.path);
        const std::optional<Bytes> saved = repo.readRevision(project, merged);
        check(saved.has_value(), "revision not recoverable after repository reopen");
        check(decodeRevisionRecord(*saved).id == merged, "reopened revision changed identity");
        check(!repo.readRevision(project, RevisionId::generate()),
              "unknown revision unexpectedly resolved");
        expected([&] { (void)repo.writeRevision(other, Hash{}, Hash{}, false); },
                 StorageErrorKind::Conflict, "repository accepted second project UUID");
    }
    check(fs::exists(tmp.path / "revision-v2" / "ids" / merged.toString()),
          "expected UUID-only identity filename missing");
}

} // namespace

int main() {
    try {
        testLivePackStorage();
        std::cout << "Packed UUIDv7 revision storage and recovery passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Packed revision test failed:" << error.what() << '\n';
        return 1;
    }
}
