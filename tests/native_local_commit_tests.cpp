#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"

#include <atomic>
#include <filesystem>
#include <thread>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {
using aowis::dvmdb::Key;
using aowis::dvmdb::LocalCommitInfo;
using aowis::dvmdb::LocalCommitState;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::RepositoryRole;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::WorkingCopyInfo;
using aowis::dvmdb::WorkingCopyLease;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expectConflict(Function function) {
    try { function(); }
    catch (const ProjectError& exception) {
        require(exception.kind() == ProjectErrorKind::Conflict, "unexpected project error kind");
        return;
    }
    throw std::runtime_error("expected stale or reused local Commit to conflict");
}

struct TempRoot {
    TempRoot() {
        std::string pattern = (std::filesystem::temp_directory_path() /
            "aowis-local-commit-XXXXXXXX").string();
        std::vector<char> chars(pattern.begin(), pattern.end());
        chars.push_back('\0');
        const char* created = ::mkdtemp(chars.data());
        require(created != nullptr, "cannot create local commit test root");
        this->path = created;
    }
    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

ModelChange changeWithValue(std::uint8_t value) {
    Key key{};
    key[15] = 7;
    ModelRecord record;
    record.kind = 1;
    record.payload = {value};
    return ModelChange{key, record};
}

void testLocalCommitRecovery() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string secondSession = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    const std::string staleOperationId = UuidV7::generate().toString();
    std::string projectId;
    std::string branchId;
    std::string firstDraft;
    std::string firstToken;
    std::string staleDraft;
    std::string staleToken;
    std::string firstRevision;
    {
        ProjectService service(root.path);
        const aowis::dvmdb::ProjectInfo project =
            service.createUnbornProject("private local commits");
        projectId = project.id;
        branchId = service.branchId(projectId, "main")->toString();
        service.configureRepositoryRole(RepositoryRole::Private);
        const WorkingCopyInfo first = service.createWorkingCopyForBranch(
            projectId, branchId, std::nullopt, session);
        const WorkingCopyInfo second = service.createWorkingCopyForBranch(
            projectId, branchId, std::nullopt, secondSession);
        firstDraft = first.id;
        staleDraft = second.id;
        firstToken = service.acquireWorkingCopyLease(firstDraft, session, 120).token;
        staleToken = service.acquireWorkingCopyLease(staleDraft, secondSession, 120).token;
        require(service.saveWorkingCopyChanges(firstDraft, session, firstToken, 0,
                        {changeWithValue(15)}).generation == 1,
                "first candidate was not durably saved");
        require(service.saveWorkingCopyChanges(staleDraft, secondSession, staleToken, 0,
                        {changeWithValue(22)}).generation == 1,
                "concurrent candidate was not durably saved");
        require(service.revisionGraph(projectId).revisions.empty(),
                "draft autosaves unexpectedly committed a revision");
        const LocalCommitInfo committed = service.commitWorkingCopy(
            operationId, firstDraft, session, firstToken, 1, std::nullopt);
        require(committed.state == LocalCommitState::Succeeded &&
                committed.expectedHead == std::nullopt &&
                committed.repositoryId == service.repositoryId() &&
                RevisionId::parse(committed.reservedRevisionId).has_value() &&
                committed.payloadDigest.size() == 64,
                "first Commit did not return durable UUIDv7 outcome");
        firstRevision = committed.reservedRevisionId;
        const std::optional<RevisionId> saved = service.optionalHead(projectId, "main");
        require(saved && saved->toString() == firstRevision,
                "first Commit did not advance only the local HEAD");
        const std::optional<ModelRecord> loaded = service.getModelAtRevision(
            projectId, *saved, changeWithValue(15).key);
        require(loaded && loaded->payload == std::vector<std::uint8_t>{15},
                "first immutable revision did not contain saved draft candidate");
        const aowis::dvmdb::RevisionGraphRevision revision = service.revision(projectId, *saved);
        require(revision.parents.empty(), "first Commit must have zero parents");
        const LocalCommitInfo replay = service.commitWorkingCopy(
            operationId, firstDraft, session, firstToken, 1, std::nullopt);
        require(replay.state == LocalCommitState::Succeeded &&
                replay.reservedRevisionId == firstRevision &&
                service.revisionGraph(projectId).revisions.size() == 1,
                "operation replay duplicated the first Commit");
        expectConflict([&]() {
            (void)service.commitWorkingCopy(operationId, staleDraft, secondSession,
                                               staleToken, 1, std::nullopt);
        });
        const LocalCommitInfo stale = service.commitWorkingCopy(
            staleOperationId, staleDraft, secondSession, staleToken, 1, std::nullopt);
        require(stale.state == LocalCommitState::Rejected &&
                stale.rejection == "conflict" &&
                service.localCommitStatus(staleOperationId).state == LocalCommitState::Rejected,
                "stale HEAD outcome was not durably rejected");
        require(service.workingCopy(staleDraft).dirty &&
                service.workingCopy(staleDraft).generation == 1,
                "stale first-commit conflict destroyed the competing draft");
    }
    {
        // Simulate interruption after committed HEAD publication but before
        // recording the successful operation status. Reconciliation must not
        // replay the commit or allocate another revision UUID.
        const std::filesystem::path path = root.path / "local-commits" / operationId;
        std::ifstream input(path, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
        const std::string succeeded = "state\tsucceeded\n";
        const std::size_t offset = body.find(succeeded);
        require(offset != std::string::npos, "missing durable operation outcome fixture");
        body.replace(offset, succeeded.size(), "state\tpending\n");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << body;
        require(output.good(), "failed to stage interrupted status fixture");
    }
    {
        ProjectService reopened(root.path);
        const LocalCommitInfo recovered = reopened.localCommitStatus(operationId);
        require(recovered.state == LocalCommitState::Succeeded &&
                recovered.reservedRevisionId == firstRevision,
                "restart failed to recover committed operation from branch ancestry");
        const LocalCommitInfo retry = reopened.commitWorkingCopy(
            operationId, firstDraft, session, firstToken, 1, std::nullopt);
        require(retry.state == LocalCommitState::Succeeded &&
                reopened.revisionGraph(projectId).revisions.size() == 1,
                "recovery retry created a duplicate revision");

        const RevisionId parent = *RevisionId::parse(firstRevision);
        const std::string draftId = reopened.createWorkingCopyForBranch(
            projectId, branchId, parent, session).id;
        const WorkingCopyLease lease = reopened.acquireWorkingCopyLease(draftId, session, 120);
        const ModelChange changed = changeWithValue(27);
        reopened.saveWorkingCopyChanges(draftId, session, lease.token, 0, {changed});
        const std::string secondOperationId = UuidV7::generate().toString();
        const LocalCommitInfo result = reopened.commitWorkingCopy(
            secondOperationId, draftId, session, lease.token, 1, parent);
        require(result.state == LocalCommitState::Succeeded &&
                result.reservedRevisionId != firstRevision,
                "subsequent Commit did not allocate a new revision UUID");
        const RevisionId child = *RevisionId::parse(result.reservedRevisionId);
        const aowis::dvmdb::RevisionGraphRevision revision = reopened.revision(projectId, child);
        require(revision.parents.size() == 1 && revision.parents[0].id == parent,
                "second local Commit did not preserve immutable parent");
        require(reopened.getModelAtRevision(projectId, parent, changed.key)->payload ==
                    std::vector<std::uint8_t>{15} &&
                reopened.getModelAtRevision(projectId, child, changed.key)->payload ==
                    std::vector<std::uint8_t>{27},
                "local Commit modified historical model content");
    }
}


void testConcurrentFirstCommitIsSerialized() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProject("concurrent local Commit").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string firstSession = UuidV7::generate().toString();
    const std::string secondSession = UuidV7::generate().toString();
    const std::string firstDraft = service.createWorkingCopy(
        projectId, "main", std::nullopt, firstSession).id;
    const std::string secondDraft = service.createWorkingCopy(
        projectId, "main", std::nullopt, secondSession).id;
    const std::string firstToken = service.acquireWorkingCopyLease(
        firstDraft, firstSession, 120).token;
    const std::string secondToken = service.acquireWorkingCopyLease(
        secondDraft, secondSession, 120).token;
    service.saveWorkingCopyChanges(firstDraft, firstSession, firstToken, 0,
                                     {changeWithValue(31)});
    service.saveWorkingCopyChanges(secondDraft, secondSession, secondToken, 0,
                                     {changeWithValue(32)});
    const std::string firstOperation = UuidV7::generate().toString();
    const std::string secondOperation = UuidV7::generate().toString();
    LocalCommitInfo first;
    LocalCommitInfo second;
    std::exception_ptr firstError;
    std::exception_ptr secondError;
    std::thread left([&]() {
        try { first = service.commitWorkingCopy(firstOperation, firstDraft,
                                                   firstSession, firstToken, 1, std::nullopt); }
        catch (...) { firstError = std::current_exception(); }
    });
    std::thread right([&]() {
        try { second = service.commitWorkingCopy(secondOperation, secondDraft,
                                                    secondSession, secondToken, 1, std::nullopt); }
        catch (...) { secondError = std::current_exception(); }
    });
    left.join();
    right.join();
    if (firstError) std::rethrow_exception(firstError);
    if (secondError) std::rethrow_exception(secondError);
    require((first.state == LocalCommitState::Succeeded &&
             second.state == LocalCommitState::Rejected) ||
            (second.state == LocalCommitState::Succeeded &&
             first.state == LocalCommitState::Rejected),
            "concurrent initial Commit allowed two successful branches");
    require(service.revisionGraph(projectId).revisions.size() == 1 &&
            service.workingCopy(firstDraft).dirty && service.workingCopy(secondDraft).dirty,
            "concurrent first Commit wrote duplicate history or lost a draft");
}

void testRejectedOperationIsDurable() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    std::string draftId;
    std::string token;
    {
        ProjectService service(root.path);
        const std::string projectId = service.createUnbornProject("rejected Commit").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        ModelChange invalid = changeWithValue(5);
        Key absent{};
        absent[15] = 9;
        invalid.record->references.push_back(absent);
        service.saveWorkingCopyChanges(draftId, session, token, 0, {invalid});
        const LocalCommitInfo rejected = service.commitWorkingCopy(
            operationId, draftId, session, token, 1, std::nullopt);
        require(rejected.state == LocalCommitState::Rejected &&
                rejected.rejection == "invalid-input" &&
                !service.optionalHead(projectId, "main"),
                "invalid model commit was not rejected without changing HEAD");
        const LocalCommitInfo retry = service.commitWorkingCopy(
            operationId, draftId, session, token, 1, std::nullopt);
        require(retry.state == LocalCommitState::Rejected &&
                retry.reservedRevisionId == rejected.reservedRevisionId,
                "rejected operation was retried instead of returning outcome");
    }
    {
        ProjectService reopened(root.path);
        require(reopened.localCommitStatus(operationId).state == LocalCommitState::Rejected,
                "rejected operation status disappeared after restart");
        require(reopened.workingCopy(draftId).dirty,
                "rejected Commit unexpectedly deleted saved draft");
    }
}
}

int main() {
    try {
        testLocalCommitRecovery();
        testConcurrentFirstCommitIsSerialized();
        testRejectedOperationIsDurable();
        std::cout << "VC-05 durable local Commit tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "VC-05 durable local Commit tests failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
