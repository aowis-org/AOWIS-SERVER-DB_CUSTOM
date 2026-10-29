#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"

#include <filesystem>
#include <chrono>
#include <thread>
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
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;
using aowis::dvmdb::ProjectInfo;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::RepositoryRoleV1;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::WorkingCopyInfoV1;

void require(bool accepted, const char *message) {
    if (!accepted) throw std::runtime_error(message);
}

template <typename Action>
void expectError(Action action, ProjectErrorKind kind) {
    try { action(); }
    catch (const ProjectError& error) {
        require(error.kind() == kind, "wrong working-copy error category");
        return;
    }
    throw std::runtime_error("expected working-copy error");
}

struct TemporaryRoot {
    TemporaryRoot() {
        std::string pattern = (std::filesystem::temp_directory_path() /
            "aowis-working-copy-XXXXXXXX").string();
        std::vector<char> letters(pattern.begin(), pattern.end());
        letters.push_back('\0');
        const char *created = ::mkdtemp(letters.data());
        require(created != nullptr, "failed to create temporary DVMDB root");
        this->path = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

ModelChange simpleChange() {
    Key key{};
    key[15] = 1;
    ModelRecord record;
    record.kind = 1;
    record.payload = {12};
    return {key, record};
}

void testWorkingCopyCreationAndRecovery() {
    TemporaryRoot root;
    std::string projectId;
    std::string branchId;
    std::string firstId;
    std::string secondId;
    std::string committedId;
    RevisionId committed = RevisionId::generate();
    const std::string sessionOne = UuidV7::generate().toString();
    const std::string sessionTwo = UuidV7::generate().toString();
    {
        ProjectService service(root.path);
        const ProjectInfo project = service.createUnbornProjectV2("checkout from unborn");
        projectId = project.id;
        branchId = service.branchId(projectId, "main")->toString();
        require(service.listWorkingCopiesV1().empty(), "new root contains drafts");
        expectError([&]() { service.createWorkingCopyV1(projectId, "main", std::nullopt, sessionOne); },
                    ProjectErrorKind::Conflict);
        service.configureRepositoryRoleV1(RepositoryRoleV1::Private);
        const WorkingCopyInfoV1 first = service.createWorkingCopyV1(projectId, "main", std::nullopt, sessionOne);
        const WorkingCopyInfoV1 second = service.createWorkingCopyV1(projectId, "main", std::nullopt, sessionTwo);
        firstId = first.id;
        secondId = second.id;
        require(first.id != second.id && UuidV7::parse(first.id).has_value(),
                "distinct sessions did not receive distinct draft UUIDv7s");
        require(first.projectId == projectId && first.branchId == branchId &&
                first.sessionId == sessionOne && !first.baseRevision.has_value() &&
                !first.dirty && first.generation == 0,
                "unborn draft metadata incorrect");
        require(service.listWorkingCopiesV1().size() == 2 &&
                !service.optionalHeadV2(projectId, "main").has_value() &&
                service.revisionGraphV2(projectId).revisions.empty(),
                "draft creation modified committed ancestry");
        expectError([&]() { service.workingCopyV1(sessionOne); }, ProjectErrorKind::NotFound);
        expectError([&]() { service.workingCopyV1("not-a-uuid"); }, ProjectErrorKind::InvalidInput);
        committed = service.commitModelV2(projectId, "main", {simpleChange()});
        committedId = service.createWorkingCopyV1(projectId, "main", committed, sessionOne).id;
        expectError([&]() { service.createWorkingCopyV1(projectId, "main", std::nullopt, sessionTwo); },
                    ProjectErrorKind::Conflict);
        require(service.optionalHeadV2(projectId, "main") == committed,
                "working-copy creation moved committed HEAD");
    }
    {
        ProjectService reopened(root.path);
        require(reopened.repositoryRoleV1() == RepositoryRoleV1::Private &&
                reopened.listWorkingCopiesV1().size() == 3,
                "working copies not recovered after restart");
        const WorkingCopyInfoV1 first = reopened.workingCopyV1(firstId);
        const WorkingCopyInfoV1 second = reopened.workingCopyV1(secondId);
        const WorkingCopyInfoV1 third = reopened.workingCopyV1(committedId);
        require(first.projectId == projectId && first.branchId == branchId &&
                first.sessionId == sessionOne && !first.baseRevision &&
                second.sessionId == sessionTwo && !second.baseRevision &&
                third.baseRevision == committed && third.generation == 0 &&
                reopened.optionalHeadV2(projectId, "main") == committed,
                "recovered drafts lost pinned base or identity");
        const std::filesystem::path stages = root.path / "working-copies" /
            (".creating-" + UuidV7::generate().toString());
        std::ofstream(stages) << "not-published";
        require(reopened.listWorkingCopiesV1().size() == 3,
                "unpublished draft staging was incorrectly advertised");
    }
    // Corruption must never be silently treated as a missing draft.
    {
        std::ofstream altered(root.path / "working-copies" / firstId,
                              std::ios::binary | std::ios::trunc);
        altered << "invalid draft contents\n";
    }
    {
        ProjectService reopened(root.path);
        bool rejected = false;
        try { (void)reopened.listWorkingCopiesV1(); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "corrupt draft was silently accepted");
    }
}
void testLeasedTransactionalCandidateSaves() {
    TemporaryRoot root;
    const std::string writerA = UuidV7::generate().toString();
    const std::string writerB = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string staleToken;
    Key firstKey{};
    firstKey[15] = 4;
    Key secondKey{};
    secondKey[15] = 8;
    ModelRecord firstRecord;
    firstRecord.kind = 12;
    firstRecord.payload = {5, 6, 7};
    ModelRecord secondRecord;
    secondRecord.kind = 15;
    secondRecord.payload = {8, 9};
    {
        ProjectService service(root.path);
        service.configureRepositoryRoleV1(RepositoryRoleV1::Private);
        projectId = service.createUnbornProjectV2("private editor saves").id;
        draftId = service.createWorkingCopyV1(projectId, "main", std::nullopt, writerA).id;
        expectError([&]() { (void)service.saveWorkingCopyChangesV1(
            draftId, writerA, UuidV7::generate().toString(), 0,
            {ModelChange{firstKey, firstRecord}}); }, ProjectErrorKind::Conflict);
        const aowis::dvmdb::WorkingCopyLeaseV1 lease =
            service.acquireWorkingCopyLeaseV1(draftId, writerA, 120);
        staleToken = lease.token;
        require(lease.generation == 0 && lease.expiresAtMs != 0,
                "initial draft lease invalid");
        expectError([&]() { (void)service.acquireWorkingCopyLeaseV1(draftId, writerB, 120); },
                    ProjectErrorKind::Conflict);
        const WorkingCopyInfoV1 saved = service.saveWorkingCopyChangesV1(
            draftId, writerA, lease.token, lease.generation,
            {ModelChange{firstKey, firstRecord}, ModelChange{secondKey, secondRecord}});
        require(saved.dirty && saved.generation == 1 && saved.candidateChanges.size() == 2 &&
                saved.candidateChanges[0].key == firstKey &&
                saved.candidateChanges[1].record->payload == secondRecord.payload &&
                !service.optionalHeadV2(projectId, "main") &&
                service.revisionGraphV2(projectId).revisions.empty(),
                "draft save changed committed history or lost candidate changes");
        expectError([&]() { (void)service.saveWorkingCopyChangesV1(
            draftId, writerA, lease.token, 0, {}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChangesV1(
            draftId, writerB, lease.token, 1, {}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChangesV1(
            draftId, writerA, lease.token, 1,
            {ModelChange{firstKey, firstRecord}, ModelChange{firstKey, firstRecord}}); },
            ProjectErrorKind::InvalidInput);
        require(service.workingCopyV1(draftId).generation == 1,
                "rejected duplicate-key save changed draft generation");
        const aowis::dvmdb::WorkingCopyLeaseV1 refreshed =
            service.acquireWorkingCopyLeaseV1(draftId, writerA, 120);
        require(refreshed.token != lease.token && refreshed.generation == 1,
                "reacquiring a lease did not rotate its fencing token");
        expectError([&]() { (void)service.saveWorkingCopyChangesV1(
            draftId, writerA, lease.token, 1, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfoV1 changed = service.saveWorkingCopyChangesV1(
            draftId, writerA, refreshed.token, 1,
            {ModelChange{firstKey, std::nullopt}});
        require(changed.generation == 2 && changed.candidateChanges.size() == 2 &&
                !changed.candidateChanges[0].record.has_value() &&
                changed.candidateChanges[1].record->payload == secondRecord.payload,
                "incremental Save failed to merge change overlay");
        service.releaseWorkingCopyLeaseV1(draftId, writerA, refreshed.token);
        expectError([&]() { (void)service.saveWorkingCopyChangesV1(
            draftId, writerA, refreshed.token, 2, {}); }, ProjectErrorKind::Conflict);
    }
    {
        ProjectService reopened(root.path);
        const WorkingCopyInfoV1 recovered = reopened.workingCopyV1(draftId);
        require(recovered.generation == 2 && recovered.candidateChanges.size() == 2 &&
                recovered.dirty && !recovered.leaseToken &&
                !reopened.optionalHeadV2(projectId, "main"),
                "restart lost draft saves or changed committed HEAD");
        const aowis::dvmdb::WorkingCopyLeaseV1 taken =
            reopened.acquireWorkingCopyLeaseV1(draftId, writerB, 120);
        require(taken.generation == 2 && taken.token != staleToken,
                "another session could not acquire a released lease");
        expectError([&]() { (void)reopened.saveWorkingCopyChangesV1(
            draftId, writerA, staleToken, 2, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfoV1 third = reopened.saveWorkingCopyChangesV1(
            draftId, writerB, taken.token, 2, {ModelChange{secondKey, secondRecord}});
        require(third.generation == 3 && third.candidateChanges.size() == 2,
                "new writer lost existing candidate state");
        reopened.releaseWorkingCopyLeaseV1(draftId, writerB, taken.token);
        const aowis::dvmdb::WorkingCopyLeaseV1 shortLease =
            reopened.acquireWorkingCopyLeaseV1(draftId, writerA, 1);
        expectError([&]() { (void)reopened.acquireWorkingCopyLeaseV1(draftId, writerB, 120); },
                    ProjectErrorKind::Conflict);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const aowis::dvmdb::WorkingCopyLeaseV1 takeover =
            reopened.acquireWorkingCopyLeaseV1(draftId, writerB, 120);
        require(takeover.generation == 3 && takeover.token != shortLease.token,
                "expired lease takeover did not preserve generation/fence");
        expectError([&]() { (void)reopened.saveWorkingCopyChangesV1(
            draftId, writerA, shortLease.token, 3, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfoV1 final = reopened.saveWorkingCopyChangesV1(
            draftId, writerB, takeover.token, 3, {});
        require(final.generation == 4 && final.candidateChanges.size() == 2,
                "successful empty save lost stored candidate changes");
    }
}

}

int main() {
    try {
        testWorkingCopyCreationAndRecovery();
        testLeasedTransactionalCandidateSaves();
        std::cout << "VC-04 persistent working-copy bootstrap passed\n";
    } catch (const std::exception& error) {
        std::cerr << "VC-04 working-copy bootstrap failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
