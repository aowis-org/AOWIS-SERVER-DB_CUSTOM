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
using aowis::dvmdb::LocalCommitInfoV1;
using aowis::dvmdb::LocalCommitStateV1;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::RepositoryRoleV1;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::WorkingCopyInfoV1;
using aowis::dvmdb::WorkingCopyLeaseV1;

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
            service.createUnbornProjectV2("private local commits");
        projectId = project.id;
        branchId = service.branchId(projectId, "main")->toString();
        service.configureRepositoryRoleV1(RepositoryRoleV1::Private);
        const WorkingCopyInfoV1 first = service.createWorkingCopyForBranchV1(
            projectId, branchId, std::nullopt, session);
        const WorkingCopyInfoV1 second = service.createWorkingCopyForBranchV1(
            projectId, branchId, std::nullopt, secondSession);
        firstDraft = first.id;
        staleDraft = second.id;
        firstToken = service.acquireWorkingCopyLeaseV1(firstDraft, session, 120).token;
        staleToken = service.acquireWorkingCopyLeaseV1(staleDraft, secondSession, 120).token;
        require(service.saveWorkingCopyChangesV1(firstDraft, session, firstToken, 0,
                        {changeWithValue(15)}).generation == 1,
                "first candidate was not durably saved");
        require(service.saveWorkingCopyChangesV1(staleDraft, secondSession, staleToken, 0,
                        {changeWithValue(22)}).generation == 1,
                "concurrent candidate was not durably saved");
        require(service.revisionGraphV2(projectId).revisions.empty(),
                "draft autosaves unexpectedly committed a revision");
        const LocalCommitInfoV1 committed = service.commitWorkingCopyV1(
            operationId, firstDraft, session, firstToken, 1, std::nullopt);
        require(committed.state == LocalCommitStateV1::Succeeded &&
                committed.expectedHead == std::nullopt &&
                committed.repositoryId == service.repositoryId() &&
                RevisionId::parse(committed.reservedRevisionId).has_value() &&
                committed.payloadDigest.size() == 64,
                "first Commit did not return durable UUIDv7 outcome");
        firstRevision = committed.reservedRevisionId;
        const std::optional<RevisionId> saved = service.optionalHeadV2(projectId, "main");
        require(saved && saved->toString() == firstRevision,
                "first Commit did not advance only the local HEAD");
        const std::optional<ModelRecord> loaded = service.getModelAtRevisionV2(
            projectId, *saved, changeWithValue(15).key);
        require(loaded && loaded->payload == std::vector<std::uint8_t>{15},
                "first immutable revision did not contain saved draft candidate");
        const aowis::dvmdb::RevisionGraphRevisionV2 revision = service.revision(projectId, *saved);
        require(revision.parents.empty(), "first Commit must have zero parents");
        const LocalCommitInfoV1 replay = service.commitWorkingCopyV1(
            operationId, firstDraft, session, firstToken, 1, std::nullopt);
        require(replay.state == LocalCommitStateV1::Succeeded &&
                replay.reservedRevisionId == firstRevision &&
                service.revisionGraphV2(projectId).revisions.size() == 1,
                "operation replay duplicated the first Commit");
        expectConflict([&]() {
            (void)service.commitWorkingCopyV1(operationId, staleDraft, secondSession,
                                               staleToken, 1, std::nullopt);
        });
        const LocalCommitInfoV1 stale = service.commitWorkingCopyV1(
            staleOperationId, staleDraft, secondSession, staleToken, 1, std::nullopt);
        require(stale.state == LocalCommitStateV1::Rejected &&
                stale.rejection == "conflict" &&
                service.localCommitStatusV1(staleOperationId).state == LocalCommitStateV1::Rejected,
                "stale HEAD outcome was not durably rejected");
        require(service.workingCopyV1(staleDraft).dirty &&
                service.workingCopyV1(staleDraft).generation == 1,
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
        const LocalCommitInfoV1 recovered = reopened.localCommitStatusV1(operationId);
        require(recovered.state == LocalCommitStateV1::Succeeded &&
                recovered.reservedRevisionId == firstRevision,
                "restart failed to recover committed operation from branch ancestry");
        const LocalCommitInfoV1 retry = reopened.commitWorkingCopyV1(
            operationId, firstDraft, session, firstToken, 1, std::nullopt);
        require(retry.state == LocalCommitStateV1::Succeeded &&
                reopened.revisionGraphV2(projectId).revisions.size() == 1,
                "recovery retry created a duplicate revision");

        const RevisionId parent = *RevisionId::parse(firstRevision);
        const std::string draftId = reopened.createWorkingCopyForBranchV1(
            projectId, branchId, parent, session).id;
        const WorkingCopyLeaseV1 lease = reopened.acquireWorkingCopyLeaseV1(draftId, session, 120);
        const ModelChange changed = changeWithValue(27);
        reopened.saveWorkingCopyChangesV1(draftId, session, lease.token, 0, {changed});
        const std::string secondOperationId = UuidV7::generate().toString();
        const LocalCommitInfoV1 result = reopened.commitWorkingCopyV1(
            secondOperationId, draftId, session, lease.token, 1, parent);
        require(result.state == LocalCommitStateV1::Succeeded &&
                result.reservedRevisionId != firstRevision,
                "subsequent Commit did not allocate a new revision UUID");
        const RevisionId child = *RevisionId::parse(result.reservedRevisionId);
        const aowis::dvmdb::RevisionGraphRevisionV2 revision = reopened.revision(projectId, child);
        require(revision.parents.size() == 1 && revision.parents[0].id == parent,
                "second local Commit did not preserve immutable parent");
        require(reopened.getModelAtRevisionV2(projectId, parent, changed.key)->payload ==
                    std::vector<std::uint8_t>{15} &&
                reopened.getModelAtRevisionV2(projectId, child, changed.key)->payload ==
                    std::vector<std::uint8_t>{27},
                "local Commit modified historical model content");
    }
}


void testConcurrentFirstCommitIsSerialized() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProjectV2("concurrent local Commit").id;
    service.configureRepositoryRoleV1(RepositoryRoleV1::Private);
    const std::string firstSession = UuidV7::generate().toString();
    const std::string secondSession = UuidV7::generate().toString();
    const std::string firstDraft = service.createWorkingCopyV1(
        projectId, "main", std::nullopt, firstSession).id;
    const std::string secondDraft = service.createWorkingCopyV1(
        projectId, "main", std::nullopt, secondSession).id;
    const std::string firstToken = service.acquireWorkingCopyLeaseV1(
        firstDraft, firstSession, 120).token;
    const std::string secondToken = service.acquireWorkingCopyLeaseV1(
        secondDraft, secondSession, 120).token;
    service.saveWorkingCopyChangesV1(firstDraft, firstSession, firstToken, 0,
                                     {changeWithValue(31)});
    service.saveWorkingCopyChangesV1(secondDraft, secondSession, secondToken, 0,
                                     {changeWithValue(32)});
    const std::string firstOperation = UuidV7::generate().toString();
    const std::string secondOperation = UuidV7::generate().toString();
    LocalCommitInfoV1 first;
    LocalCommitInfoV1 second;
    std::exception_ptr firstError;
    std::exception_ptr secondError;
    std::thread left([&]() {
        try { first = service.commitWorkingCopyV1(firstOperation, firstDraft,
                                                   firstSession, firstToken, 1, std::nullopt); }
        catch (...) { firstError = std::current_exception(); }
    });
    std::thread right([&]() {
        try { second = service.commitWorkingCopyV1(secondOperation, secondDraft,
                                                    secondSession, secondToken, 1, std::nullopt); }
        catch (...) { secondError = std::current_exception(); }
    });
    left.join();
    right.join();
    if (firstError) std::rethrow_exception(firstError);
    if (secondError) std::rethrow_exception(secondError);
    require((first.state == LocalCommitStateV1::Succeeded &&
             second.state == LocalCommitStateV1::Rejected) ||
            (second.state == LocalCommitStateV1::Succeeded &&
             first.state == LocalCommitStateV1::Rejected),
            "concurrent initial Commit allowed two successful branches");
    require(service.revisionGraphV2(projectId).revisions.size() == 1 &&
            service.workingCopyV1(firstDraft).dirty && service.workingCopyV1(secondDraft).dirty,
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
        const std::string projectId = service.createUnbornProjectV2("rejected Commit").id;
        service.configureRepositoryRoleV1(RepositoryRoleV1::Private);
        draftId = service.createWorkingCopyV1(projectId, "main", std::nullopt, session).id;
        token = service.acquireWorkingCopyLeaseV1(draftId, session, 120).token;
        ModelChange invalid = changeWithValue(5);
        Key absent{};
        absent[15] = 9;
        invalid.record->references.push_back(absent);
        service.saveWorkingCopyChangesV1(draftId, session, token, 0, {invalid});
        const LocalCommitInfoV1 rejected = service.commitWorkingCopyV1(
            operationId, draftId, session, token, 1, std::nullopt);
        require(rejected.state == LocalCommitStateV1::Rejected &&
                rejected.rejection == "invalid-input" &&
                !service.optionalHeadV2(projectId, "main"),
                "invalid model commit was not rejected without changing HEAD");
        const LocalCommitInfoV1 retry = service.commitWorkingCopyV1(
            operationId, draftId, session, token, 1, std::nullopt);
        require(retry.state == LocalCommitStateV1::Rejected &&
                retry.reservedRevisionId == rejected.reservedRevisionId,
                "rejected operation was retried instead of returning outcome");
    }
    {
        ProjectService reopened(root.path);
        require(reopened.localCommitStatusV1(operationId).state == LocalCommitStateV1::Rejected,
                "rejected operation status disappeared after restart");
        require(reopened.workingCopyV1(draftId).dirty,
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
