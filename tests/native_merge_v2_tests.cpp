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
void rejects(Callback callback, StorageErrorKind kind, const char* message) {
    try { callback(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        throw std::runtime_error(std::string(message) + ": unexpected storage error");
    }
    throw std::runtime_error(message);
}
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-v2-merge-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code ignored; fs::remove_all(this->path, ignored); }
};
Key key(std::uint8_t value) {
    Key result{};
    result[15] = value;
    return result;
}
ModelRecord model(std::uint8_t value, std::vector<Key> references = {}) {
    ModelRecord result;
    result.kind = 31;
    result.payload = {value};
    result.references = std::move(references);
    return result;
}
void testRawAndHistoricalFork() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId first = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    BranchId mainBranch = BranchId::generate();
    BranchId featureBranch = BranchId::generate();
    {
        Repository repository(temp.path);
        root = repository.createBranchV2(project, "main");
        mainBranch = *repository.branchId("main");
        first = repository.commitV2(project, "main", {{key(1), Bytes{11}}}, root);
        check(repository.createBranchAtRevisionV2(project, "feature", "main", root) == root,
              "historical fork changed source revision");
        featureBranch = *repository.branchId("feature");
        check(mainBranch != featureBranch, "fork reused a branch ID");
        rejects([&] { (void)repository.createBranchAtRevisionV2(
                       project, "invalid", "feature", first); },
                StorageErrorKind::InvalidInput, "unreachable fork accepted");
        check(!repository.hasBranch("invalid"), "rejected fork still published");
        feature = repository.commitV2(project, "feature", {{key(2), Bytes{22}}}, root);
        check(repository.revisionReachableFromV2(project, feature, root),
              "v2 ancestry cannot find root");
        check(!repository.revisionReachableFromV2(project, feature, first),
              "unrelated history incorrectly reachable");
        rejects([&] { (void)repository.mergeBranchesV2(project, "main", "feature", root, feature); },
                StorageErrorKind::Conflict, "stale target merge accepted");
        const MergeResultV2 result = repository.mergeBranchesV2(project, "main", "feature", first, feature);
        check(result.status == MergeStatus::Merged && result.resultHead &&
              repository.headV2(project, "main") == *result.resultHead &&
              repository.headV2(project, "feature") == feature,
              "divergent merge did not publish target only");
        merged = *result.resultHead;
        const RevisionRecordV2 record = decodeRevisionRecordV2(*repository.readRevisionV2(project, merged));
        check(record.parents.size() == 2 && record.parents[0].id == first &&
              record.parents[1].id == feature, "two parent UUID ancestry lost");
        check(repository.revisionReachableFromV2(project, merged, feature),
              "second parent ancestry not traversed");
        check(repository.mergeBranchesV2(project, "main", "feature").status == MergeStatus::UpToDate,
              "remerging incorporated history changed HEAD");
        check(repository.createBranchAtRevisionV2(project, "historic", "main", feature) == feature,
              "historical fork from second merge parent failed");
        const RevisionId historical = repository.headV2(project, "historic");
        check(historical == feature, "historic branch head incorrect");
    }
    {
        Repository repository(temp.path);
        check(repository.headV2(project, "main") == merged &&
              repository.headV2(project, "feature") == feature &&
              *repository.branchId("main") == mainBranch &&
              *repository.branchId("feature") == featureBranch,
              "reopened merge lost HEAD or identity");
    }
}
void testFastForwardAndConflicts() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    Repository repo(temp.path);
    const RevisionId root = repo.createBranchV2(project, "main");
    check(repo.createBranchV2(project, "feature", "main") == root, "shared head not created");
    const RevisionId f1 = repo.commitV2(project, "feature", {{key(1), Bytes{1}}}, root);
    const MergeResultV2 fast = repo.mergeBranchesV2(project, "main", "feature", root, f1);
    check(fast.status == MergeStatus::FastForward && fast.resultHead == f1 &&
          repo.headV2(project, "main") == f1, "fast-forward did not advance target");
    check(repo.mergeBranchesV2(project, "feature", "main").status == MergeStatus::UpToDate,
          "source-containing-target should be up to date");
    const RevisionId main2 = repo.commitV2(project, "main", {{key(2), Bytes{5}}}, f1);
    const RevisionId feature2 = repo.commitV2(project, "feature", {{key(2), Bytes{6}}}, f1);
    const MergeResultV2 conflict = repo.mergeBranchesV2(project, "main", "feature", main2, feature2);
    check(conflict.status == MergeStatus::Conflicts && conflict.conflicts.size() == 1 &&
          repo.headV2(project, "main") == main2 &&
          repo.headV2(project, "feature") == feature2,
          "concurrent edits were not rejected atomically");
}
void testModelMergeAndInvalidReferences() {
    Temporary temp;
    ProjectService service(temp.path);
    const ProjectInfo project = service.createProjectV2("model-merge");
    const RevisionId root = service.headV2(project.id, "main");
    const RevisionId base = service.commitModelV2(project.id, "main", {{key(1), model(1)}}, root);
    check(service.createBranchV2(project.id, "feature", "main") == base,
          "model fork did not share base");
    const RevisionId main = service.commitModelV2(project.id, "main", {{key(2), model(2, {key(1)})}}, base);
    const RevisionId feature = service.commitModelV2(project.id, "feature", {{key(3), model(3)}}, base);
    const MergeResultV2 merge = service.mergeBranchesV2(project.id, "main", "feature", main, feature);
    check(merge.status == MergeStatus::Merged && merge.resultHead.has_value(),
          "model merge did not publish a two-parent revision");
    check(service.headV2(project.id, "feature") == feature, "model merge rewound source");
    check(service.getModelAtRevisionV2(project.id, *merge.resultHead, key(1)).has_value() &&
          service.getModelAtRevisionV2(project.id, *merge.resultHead, key(2)).has_value() &&
          service.getModelAtRevisionV2(project.id, *merge.resultHead, key(3)).has_value(),
          "model merge lost disjoint records");
    const RevisionId current = *merge.resultHead;
    service.withRepository(project.id, [&](Repository& repo) {
        const VerificationReport report = repo.verifyRepository();
        check(report.branches == 2 && report.revisions == 5 &&
              report.modelSnapshots >= 3,
              "v2 verifier missed model-indexed merge ancestry");
        const CompactionStats stats = repo.compact();
        check(stats.objectsAfter <= stats.objectsBefore,
              "v2 compaction grew the immutable pack object count");
        check(repo.verifyRepository().revisions == 5,
              "compacted v2 model merge failed integrity verification");
    });
    check(service.getModelAtRevisionV2(project.id, current, key(2)).has_value(),
          "compaction lost model-indexed merge content");
    check(service.createBranchAtRevisionV2(project.id, "deleter", "main", base) == base,
          "service historical branch creation failed");
    const RevisionId removed = service.commitModelV2(project.id, "deleter", {{key(1), std::nullopt}}, base);
    const MergeResultV2 conflict = service.mergeBranchesV2(project.id, "main", "deleter", current, removed);
    check(conflict.status == MergeStatus::Conflicts && !conflict.conflicts.empty() &&
          service.headV2(project.id, "main") == current &&
          service.headV2(project.id, "deleter") == removed,
          "merge with dangling reference did not preserve HEADs");
}
void testTypedConflictResolutionAndValidation() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    Repository repository(temp.path);
    const RevisionId root = repository.createBranchV2(project, "main");
    const RevisionId base = repository.commitModelV2(project, "main", {{key(1), model(1)}}, root);
    check(repository.createBranchV2(project, "feature", "main") == base,
          "typed v2 test failed to share its base revision");
    const RevisionId target = repository.commitModelV2(project, "main", {{key(1), model(2)}}, base);
    const RevisionId source = repository.commitModelV2(project, "feature", {{key(1), model(3)}}, base);
    const std::uint64_t bytes = repository.packSize();
    const MergeResultV2 rejected = repository.mergeBranchesV2(project, "main", "feature", target, source);
    check(rejected.status == MergeStatus::Conflicts && rejected.conflicts.size() == 1 &&
          repository.packSize() == bytes, "v2 conflict wrote objects before resolution");

    MergeOptions choices;
    choices.resolve = [](const Key& changed, const ModelRecord& original,
                         const ModelRecord& left, const ModelRecord& right) {
        check(changed == key(1) && original.payload == Bytes{1} &&
              left.payload == Bytes{2} && right.payload == Bytes{3},
              "v2 resolver observed wrong immutable revisions");
        return MergeResolution{model(4), {}};
    };
    choices.checkSelections = []() -> std::optional<std::string> { return std::nullopt; };
    choices.validate = [](const MergeCandidate& candidate) -> std::optional<std::string> {
        const std::optional<ModelRecord> merged = candidate.get(key(1));
        if (!merged || merged->payload != Bytes{4}) return "bad resolved candidate";
        std::size_t counted = 0;
        candidate.forEach([&counted](const Key&, const ModelRecord&) { ++counted; });
        return counted == 1 ? std::nullopt : std::optional<std::string>("bad merged traversal");
    };
    const MergeResultV2 accepted = repository.mergeBranchesV2(
        project, "main", "feature", target, source, choices);
    check(accepted.status == MergeStatus::Merged && accepted.resultHead &&
          *accepted.resultHead != target && repository.headV2(project, "feature") == source,
          "typed v2 conflict resolution failed");
    const std::optional<ModelRecord> merged = repository.getModelAtRevisionV2(
        project, *accepted.resultHead, key(1));
    check(merged && merged->payload == Bytes{4}, "typed v2 merge lost selected field payload");
    const RevisionRecordV2 revision = decodeRevisionRecordV2(
        *repository.readRevisionV2(project, *accepted.resultHead));
    check(revision.parents.size() == 2 && revision.parents[0].id == target &&
          revision.parents[1].id == source, "typed v2 merge lost its two UUIDv7 parents");

    check(repository.createBranchAtRevisionV2(project, "reject-left", "main", base) == base,
          "cannot create test branch from common ancestor");
    check(repository.createBranchAtRevisionV2(project, "reject-right", "main", base) == base,
          "cannot create second test branch from common ancestor");
    const RevisionId left = repository.commitModelV2(project, "reject-left", {{key(1), model(5)}}, base);
    const RevisionId right = repository.commitModelV2(project, "reject-right", {{key(1), model(6)}}, base);
    MergeOptions rejectedCandidate;
    rejectedCandidate.resolve = [](const Key&, const ModelRecord&, const ModelRecord&,
                                   const ModelRecord&) { return MergeResolution{model(7), {}}; };
    rejectedCandidate.validate = [](const MergeCandidate&) -> std::optional<std::string> {
        return "domain validation rejected this hydraulic candidate";
    };
    const std::uint64_t sizeBefore = repository.packSize();
    const MergeResultV2 invalid = repository.mergeBranchesV2(
        project, "reject-left", "reject-right", left, right, rejectedCandidate);
    check(invalid.status == MergeStatus::Conflicts && !invalid.conflicts.empty() &&
          repository.headV2(project, "reject-left") == left &&
          repository.headV2(project, "reject-right") == right &&
          repository.packSize() == sizeBefore,
          "invalid typed v2 candidate modified immutable storage or branch tips");

    MergeOptions unused;
    unused.checkSelections = []() -> std::optional<std::string> { return "unused selection"; };
    rejects([&] { (void)repository.mergeBranchesV2(
        project, "reject-left", "reject-right", left, right, unused); },
        StorageErrorKind::InvalidInput, "unused explicit v2 choice was accepted");
    rejects([&] { (void)repository.mergeBranchesV2(
        project, "reject-left", "reject-right", base, right, choices); },
        StorageErrorKind::Conflict, "stale v2 selection was accepted");

    check(repository.createBranchAtRevisionV2(project, "ff-target", "main", base) == base,
          "cannot create fast-forward target");
    check(repository.createBranchAtRevisionV2(project, "ff-source", "main", base) == base,
          "cannot create fast-forward source");
    const RevisionId ff = repository.commitModelV2(project, "ff-source", {{key(1), model(8)}}, base);
    const std::uint64_t ffBytes = repository.packSize();
    MergeOptions invalidFastForward;
    invalidFastForward.validate = [](const MergeCandidate& candidate) -> std::optional<std::string> {
        const std::optional<ModelRecord> current = candidate.get(key(1));
        return current && current->payload == Bytes{8} ? std::optional<std::string>("bad hydraulic tip")
                                                     : std::nullopt;
    };
    const MergeResultV2 rejectedFF = repository.mergeBranchesV2(
        project, "ff-target", "ff-source", base, ff, invalidFastForward);
    check(rejectedFF.status == MergeStatus::Conflicts &&
          repository.headV2(project, "ff-target") == base &&
          repository.packSize() == ffBytes,
          "v2 fast forward bypassed semantic validation or wrote pack objects");
    const MergeResultV2 acceptedFF = repository.mergeBranchesV2(project, "ff-target", "ff-source", base, ff);
    check(acceptedFF.status == MergeStatus::FastForward &&
          repository.headV2(project, "ff-target") == ff,
          "safe v2 fast-forward did not reuse the existing UUIDv7 revision");
    // Two branches independently add a record with the *same* UUID. A typed
    // concurrent-addition resolver must be able to resolve it and reconcile
    // the candidate while retaining correct reverse references.
    check(repository.createBranchAtRevisionV2(project, "add-left", "main", base) == base &&
          repository.createBranchAtRevisionV2(project, "add-right", "main", base) == base,
          "cannot create concurrent-addition branches");
    const RevisionId addLeft = repository.commitModelV2(
        project, "add-left", {{key(4), model(12)}}, base);
    const RevisionId addRight = repository.commitModelV2(
        project, "add-right", {{key(4), model(13)}}, base);
    bool resolvedAddition = false;
    bool reconciledCandidate = false;
    MergeOptions addition;
    addition.resolveConcurrentAddition = [&resolvedAddition](
        const Key& record, const ModelRecord& left, const ModelRecord& right) {
        check(record == key(4) && left.payload == Bytes{12} && right.payload == Bytes{13},
              "v2 concurrent addition resolver saw the wrong records");
        resolvedAddition = true;
        return MergeResolution{model(14), {}};
    };
    addition.reconcile = [&reconciledCandidate](
        const MergeCandidate& candidate,
        const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
        const std::function<std::optional<ModelRecord>(const Key&)>& targetSnapshot,
        const std::function<std::optional<ModelRecord>(const Key&)>& sourceSnapshot)
        -> std::optional<std::string> {
        const std::optional<ModelRecord> choice = candidate.get(key(4));
        const std::optional<ModelRecord> originalLeft = targetSnapshot(key(4));
        const std::optional<ModelRecord> originalRight = sourceSnapshot(key(4));
        if (!choice || choice->payload != Bytes{14} ||
            !originalLeft || originalLeft->payload != Bytes{12} ||
            !originalRight || originalRight->payload != Bytes{13})
            return "invalid reconciled concurrent addition";
        set(key(5), model(15, {key(4)}));
        reconciledCandidate = true;
        return std::nullopt;
    };
    addition.validate = [](const MergeCandidate& candidate) -> std::optional<std::string> {
        const std::optional<ModelRecord> dependent = candidate.get(key(5));
        return dependent && dependent->references == std::vector<Key>{key(4)}
               ? std::nullopt : std::optional<std::string>("candidate misses reference repair");
    };
    const MergeResultV2 added = repository.mergeBranchesV2(
        project, "add-left", "add-right", addLeft, addRight, addition);
    check(added.status == MergeStatus::Merged && added.resultHead &&
          resolvedAddition && reconciledCandidate &&
          repository.getModelAtRevisionV2(project, *added.resultHead, key(4))->payload == Bytes{14} &&
          repository.referencingModelAtRevisionV2(project, *added.resultHead, key(4)) ==
              std::vector<Key>{key(5)},
          "v2 concurrent addition and candidate reconciliation were not published atomically");

    // A whole-object choice can accept either side of a genuine divergent
    // record. It must not be offered for an unchanged model UUID.
    check(repository.createBranchAtRevisionV2(project, "object-left", "main", base) == base &&
          repository.createBranchAtRevisionV2(project, "object-right", "main", base) == base,
          "cannot create object-choice branches");
    const RevisionId objectLeft = repository.commitModelV2(
        project, "object-left", {{key(1), model(21)}}, base);
    const RevisionId objectRight = repository.commitModelV2(
        project, "object-right", {{key(1), model(22)}}, base);
    MergeOptions objectChoice;
    objectChoice.chooseConflict = [](const MergeConflict& conflict)
        -> std::optional<MergeChoiceSide> {
        return conflict.key == key(1) ? std::optional<MergeChoiceSide>(MergeChoiceSide::Source)
                                      : std::nullopt;
    };
    const MergeResultV2 selected = repository.mergeBranchesV2(
        project, "object-left", "object-right", objectLeft, objectRight, objectChoice);
    check(selected.status == MergeStatus::Merged && selected.resultHead &&
          repository.getModelAtRevisionV2(project, *selected.resultHead, key(1))->payload == Bytes{22},
          "v2 whole-object source choice was ignored");

    check(repository.verifyRepository().revisions >= 8,
          "typed v2 merge graph not verifiable");
}
}
int main() {
    try {
        testRawAndHistoricalFork();
        testFastForwardAndConflicts();
        testModelMergeAndInvalidReferences();
        testTypedConflictResolutionAndValidation();
        std::cout << "UUIDv7 merge ancestry, fast-forward, conflict and model validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 merge regression FAILED: " << error.what() << '\n';
        return 1;
    }
}
