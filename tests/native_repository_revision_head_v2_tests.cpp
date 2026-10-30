#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_record_v2.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace aowis::dvmdb;

void check(bool condition, std::string_view explanation) {
    if (!condition) throw std::runtime_error(std::string(explanation));
}
template<class F> void rejects(F&& action, StorageErrorKind kind, std::string_view why) {
    try { action(); }
    catch (const StorageError& failure) {
        if (failure.kind() == kind) return;
        throw std::runtime_error(std::string(why) + ": unexpected storage error");
    }
    throw std::runtime_error(std::string(why) + ": operation was accepted");
}
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-v2-head-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code error; fs::remove_all(this->path, error); }
};
Key key(unsigned last) {
    Key result{};
    result[15] = static_cast<std::uint8_t>(last);
    return result;
}
std::optional<Bytes> value(std::string_view text) {
    return Bytes(text.begin(), text.end());
}
void testUnbornNativeRefFoundation() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    BranchId branch = BranchId::generate();
    {
        Repository repo(temp.path);
        repo.createUnbornBranchV2(project, "main");
        branch = *repo.branchId("main");
        check(!repo.optionalHeadV2(project, "main"), "unborn HEAD is not null");
        check(repo.historyV2(project, "main", 10).empty(),
              "unborn branch must have empty history");
        rejects([&] { (void)repo.historyV2(project, "main", 10, RevisionId::generate()); },
                StorageErrorKind::InvalidInput, "unborn history accepted a cursor");
        rejects([&] { repo.createUnbornBranchV2(project, "second"); },
                StorageErrorKind::Conflict, "second unborn branch accepted");
        rejects([&] { (void)repo.headV2(project, "main"); },
                StorageErrorKind::Unavailable, "legacy nonnullable HEAD accepted unborn");
    }
    {
        Repository reopened(temp.path);
        check(reopened.branchId("main") == branch &&
              !reopened.optionalHeadV2(project, "main"),
              "unborn branch not durable across reopen");
        rejects([&] { (void)reopened.commitV2(project, "main", {}, RevisionId::generate()); },
                StorageErrorKind::Conflict, "unborn commit accepted a non-null expected HEAD");
        const RevisionId first = reopened.commitV2(project, "main", {{key(1), value("first")}});
        check(reopened.optionalHeadV2(project, "main") == first,
              "first raw commit did not publish unborn HEAD");
        const auto bytes = reopened.readRevisionV2(project, first);
        check(bytes && decodeRevisionRecordV2(*bytes).parents.empty(),
              "first raw revision has a parent");
        check(reopened.branchId("main") == branch, "first commit changed branch identity");
        rejects([&] { (void)reopened.commitV2(project, "main", {}, RevisionId::generate()); },
                StorageErrorKind::Conflict, "committed branch accepted stale expected HEAD");
    }
    {
        Repository modelRepo(temp.path / "model");
        modelRepo.createUnbornBranchV2(project, "main");
        const RevisionId first = modelRepo.commitModelV2(project, "main", {});
        const auto bytes = modelRepo.readRevisionV2(project, first);
        check(bytes.has_value(), "first model revision missing");
        const RevisionRecordV2 record = decodeRevisionRecordV2(*bytes);
        check(record.parents.empty() && record.modelIndexed,
              "first model revision must be parentless and model-indexed");
        check(modelRepo.optionalHeadV2(project, "main") == first,
              "first model commit did not publish unborn HEAD");
    }
}
void testIdentityPreservingUnbornCheckout() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const BranchId source = BranchId::generate();
    {
        Repository target(temp.path);
        target.provisionUnbornBranchV2(project, "main", source);
        target.provisionUnbornBranchV2(project, "main", source);
        check(target.branchId("main") == source &&
              !target.optionalHeadV2(project, "main") &&
              target.historyV2(project, "main").empty(),
              "unborn checkout fabricated a revision or lost source branch UUID");
        rejects([&] { target.provisionUnbornBranchV2(project, "different", source); },
                StorageErrorKind::Conflict, "unborn checkout renamed existing source branch");
        rejects([&] { target.provisionUnbornBranchV2(project, "main", BranchId::generate()); },
                StorageErrorKind::Conflict, "unborn checkout overwrote a branch identity");
    }
    {
        Repository reopened(temp.path);
        check(reopened.branchId("main") == source &&
              !reopened.optionalHeadV2(project, "main"),
              "identity-preserving unborn checkout was not durable");
    }
}

void testV2Publication() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const ProjectId foreign = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId main = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    BranchId mainBranch = BranchId::generate();
    BranchId featureBranch = BranchId::generate();
    {
        Repository repo(temp.path);
        root = repo.createBranchV2(project, "main");
        check(root == repo.headV2(project, "main"), "new main HEAD not published");
        const std::optional<Bytes> rootBytes = repo.readRevisionV2(project, root);
        check(rootBytes.has_value(), "initial UUIDv7 revision missing from real pack");
        check(decodeRevisionRecordV2(*rootBytes).parents.empty(), "initial revision has parents");
        mainBranch = *repo.branchId("main");
        const fs::path recordPath = temp.path / "refs" / "heads" / mainBranch.toString();
        check(fs::exists(recordPath), "UUIDv7 branch filename is missing");
        std::ifstream branchRecord(recordPath);
        std::string magic, name, uuid, digest;
        std::getline(branchRecord, magic);
        std::getline(branchRecord, name);
        std::getline(branchRecord, uuid);
        std::getline(branchRecord, digest);
        check(magic == "AOWIS-BRANCH-2" && name == "main" && uuid == root.toString() &&
              digest == toHex(sha256(*rootBytes)), "branch HEAD is not UUID and checked SHA");
        check(!fs::exists(temp.path / "refs" / "heads" / "main"), "name-keyed ref created");
        rejects([&] { (void)repo.head("main"); }, StorageErrorKind::Unavailable,
                "SHA API accidentally interpreted UUID HEAD");
        rejects([&] { repo.createBranch("old"); }, StorageErrorKind::Unavailable,
                "mixed SHA and UUID branches accepted");
        rejects([&] { (void)repo.headV2(foreign, "main"); }, StorageErrorKind::Conflict,
                "foreign project accessed branch HEAD");
        check(repo.createBranchV2(project, "feature", "main") == root,
              "fork did not share source revision UUID");
        featureBranch = *repo.branchId("feature");
        check(featureBranch != mainBranch, "two branch references have identical UUID");
        rejects([&] { (void)repo.createBranchV2(project, "../evil", "main"); },
                StorageErrorKind::InvalidInput, "malicious branch name accepted");
        rejects([&] { (void)repo.createBranchV2(project, "orphan"); },
                StorageErrorKind::InvalidInput, "second unsourced root allowed");
        main = repo.commitV2(project, "main", {{key(1), value("one")}}, root);
        feature = repo.commitV2(project, "feature", {{key(2), value("two")}}, root);
        check(main != feature && main != root && feature != root, "revision UUID not unique");
        const RevisionRecordV2 mainRecord = decodeRevisionRecordV2(*repo.readRevisionV2(project, main));
        const RevisionRecordV2 featureRecord = decodeRevisionRecordV2(*repo.readRevisionV2(project, feature));
        check(mainRecord.parents.size() == 1 && mainRecord.parents[0].id == root &&
              featureRecord.parents.size() == 1 && featureRecord.parents[0].id == root,
              "fork parent UUID not preserved");
        check(mainRecord.root != featureRecord.root, "independent branch snapshots collapsed");
        rejects([&] { (void)repo.commitV2(project, "main", {}, root); },
                StorageErrorKind::Conflict, "stale expected HEAD accepted");
        rejects([&] { (void)repo.commitV2(foreign, "main", {}); },
                StorageErrorKind::Conflict, "foreign commit accepted");
        check(repo.headV2(project, "main") == main && repo.headV2(project, "feature") == feature,
              "commit moved wrong HEAD");
        const VerificationReport verified = repo.verifyRepository();
        check(verified.branches == 2 && verified.revisions == 3 &&
              verified.reachableObjects >= 3,
              "v2 repository verification did not follow both branch histories");
        const CompactionStats compacted = repo.compact();
        check(compacted.objectsAfter >= 3,
              "v2 compaction discarded published branch revisions");
        check(repo.headV2(project, "main") == main && repo.headV2(project, "feature") == feature,
              "compaction changed v2 branch HEADs");
        check(repo.verifyRepository().revisions == 3,
              "v2 DAG failed verification after compaction");
        repo.renameBranchV2(project, "feature", "experiment", feature);
        check(!repo.hasBranch("feature") && repo.hasBranch("experiment") &&
              repo.headV2(project, "experiment") == feature &&
              *repo.branchId("experiment") == featureBranch,
              "v2 rename changed branch UUID or HEAD");
        rejects([&] { repo.renameBranchV2(project, "experiment", "invalid", root); },
                StorageErrorKind::Conflict, "stale v2 rename accepted");
    }
    {
        Repository repo(temp.path);
        check(repo.headV2(project, "main") == main && repo.headV2(project, "experiment") == feature,
              "reopen changed UUIDv7 branch HEADs");
        check(*repo.branchId("main") == mainBranch &&
              *repo.branchId("experiment") == featureBranch,
              "reopen changed branch IDs");
        const std::optional<Bytes> bytes = repo.readRevisionV2(project, feature);
        check(bytes.has_value(), "reopen lost packed v2 revision");
        rejects([&] { (void)repo.commit("experiment", {}); }, StorageErrorKind::Unavailable,
                "SHA commit mutated UUID HEAD");
    }
}
void testCorruptV2HeadRejected() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    BranchId branch = BranchId::generate();
    {
        Repository repo(temp.path);
        (void)repo.createBranchV2(project, "main");
        branch = *repo.branchId("main");
    }
    const fs::path path = temp.path / "refs" / "heads" / branch.toString();
    {
        std::ifstream in(path);
        std::string magic, name, uuid, sha;
        std::getline(in, magic); std::getline(in, name);
        std::getline(in, uuid); std::getline(in, sha);
        check(sha.size() == 64, "expected revision checksum missing");
        sha[0] = sha[0] == '0' ? '1' : '0';
        std::ofstream out(path, std::ios::trunc);
        out << magic << '\n' << name << '\n' << uuid << '\n' << sha << '\n';
    }
    {
        Repository repo(temp.path);
        rejects([&] { (void)repo.headV2(project, "main"); },
                StorageErrorKind::Corruption, "altered HEAD SHA was not rejected");
        rejects([&] { (void)repo.commitV2(project, "main", {}); },
                StorageErrorKind::Corruption, "commit accepted altered HEAD SHA");
    }
}

void testCorruptPublishedMappingRejected() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    RevisionId head = RevisionId::generate();
    {
        Repository repo(tmp.path);
        head = repo.createBranchV2(project, "main");
        check(repo.verifyRepository().revisions == 1,
              "initial v2 binding was not verified");
    }
    const fs::path mapping = tmp.path / "revision-v2" / "ids" / head.toString();
    {
        std::fstream file(mapping, std::ios::in | std::ios::out | std::ios::binary);
        check(file.good(), "cannot open published v2 mapping");
        file.seekg(36);
        char original = 0;
        file.read(&original, 1);
        check(file.good(), "cannot read published UUID binding");
        const char damaged = static_cast<char>(static_cast<unsigned char>(original) ^ 1U);
        file.seekp(36);
        file.write(&damaged, 1);
        check(file.good(), "cannot corrupt published UUID binding");
    }
    {
        Repository repo(tmp.path);
        // Altering the immutable UUID->SHA mapping must not produce a false
        // success even when physical pack checksums are perfectly valid.
        repo.verifyPack();
        rejects([&] { (void)repo.verifyRepository(); }, StorageErrorKind::Corruption,
                "corrupted resolver entry passed whole-repository verification");
        rejects([&] { (void)repo.compact(); }, StorageErrorKind::Corruption,
                "compaction accepted corrupt UUID binding");
    }
}

void testMixingRejected() {
    Temporary temp;
    Repository repo(temp.path);
    repo.createBranch("main");
    rejects([&] { (void)repo.createBranchV2(ProjectId::generate(), "new"); },
            StorageErrorKind::Unavailable, "existing SHA branch allowed a v2 insertion");
}
} // namespace

int main() {
    try {
        testUnbornNativeRefFoundation();
        testIdentityPreservingUnbornCheckout();
        testV2Publication();
        testMixingRejected();
        testCorruptV2HeadRejected();
        testCorruptPublishedMappingRejected();
        std::cout << "UUIDv7 HEAD publication, fork, raw commit, rename and recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 HEAD test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
