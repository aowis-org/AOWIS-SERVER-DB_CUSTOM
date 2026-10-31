#include "model_edit_event_fixture.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"

#include <atomic>
#include <filesystem>
#include <functional>
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
using aowis::dvmdb::RepositoryRole;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::WorkingCopyInfo;

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
        const ProjectInfo project = service.createUnbornProject("checkout from unborn");
        projectId = project.id;
        branchId = service.branchId(projectId, "main")->toString();
        require(service.listWorkingCopies().empty(), "new root contains drafts");
        expectError([&]() { service.createWorkingCopy(projectId, "main", std::nullopt, sessionOne); },
                    ProjectErrorKind::Conflict);
        service.configureRepositoryRole(RepositoryRole::Private);
        const WorkingCopyInfo first = service.createWorkingCopy(projectId, "main", std::nullopt, sessionOne);
        const WorkingCopyInfo second = service.createWorkingCopy(projectId, "main", std::nullopt, sessionTwo);
        firstId = first.id;
        secondId = second.id;
        require(first.id != second.id && UuidV7::parse(first.id).has_value(),
                "distinct sessions did not receive distinct draft UUIDv7s");
        require(first.projectId == projectId && first.branchId == branchId &&
                first.sessionId == sessionOne && !first.baseRevision.has_value() &&
                !first.dirty && first.generation == 0,
                "unborn draft metadata incorrect");
        require(service.listWorkingCopies().size() == 2 &&
                !service.optionalHead(projectId, "main").has_value() &&
                service.revisionGraph(projectId).revisions.empty(),
                "draft creation modified committed ancestry");
        expectError([&]() { service.workingCopy(sessionOne); }, ProjectErrorKind::NotFound);
        expectError([&]() { service.workingCopy("not-a-uuid"); }, ProjectErrorKind::InvalidInput);
        committed = service.commitModel(projectId, "main", {simpleChange()});
        committedId = service.createWorkingCopy(projectId, "main", committed, sessionOne).id;
        expectError([&]() { service.createWorkingCopy(projectId, "main", std::nullopt, sessionTwo); },
                    ProjectErrorKind::Conflict);
        require(service.optionalHead(projectId, "main") == committed,
                "working-copy creation moved committed HEAD");
    }
    {
        ProjectService reopened(root.path);
        require(reopened.repositoryRole() == RepositoryRole::Private &&
                reopened.listWorkingCopies().size() == 3,
                "working copies not recovered after restart");
        const WorkingCopyInfo first = reopened.workingCopy(firstId);
        const WorkingCopyInfo second = reopened.workingCopy(secondId);
        const WorkingCopyInfo third = reopened.workingCopy(committedId);
        require(first.projectId == projectId && first.branchId == branchId &&
                first.sessionId == sessionOne && !first.baseRevision &&
                second.sessionId == sessionTwo && !second.baseRevision &&
                third.baseRevision == committed && third.generation == 0 &&
                reopened.optionalHead(projectId, "main") == committed,
                "recovered drafts lost pinned base or identity");
        const std::filesystem::path stages = root.path / "working-copies" /
            (".creating-" + UuidV7::generate().toString());
        std::ofstream(stages) << "not-published";
        require(reopened.listWorkingCopies().size() == 3,
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
        try { (void)reopened.listWorkingCopies(); }
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
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("private editor saves").id;
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, writerA).id;
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, UuidV7::generate().toString(), 0,
            {ModelChange{firstKey, firstRecord}}); }, ProjectErrorKind::Conflict);
        const aowis::dvmdb::WorkingCopyLease lease =
            service.acquireWorkingCopyLease(draftId, writerA, 120);
        staleToken = lease.token;
        require(lease.generation == 0 && lease.expiresAtMs != 0,
                "initial draft lease invalid");
        expectError([&]() { (void)service.acquireWorkingCopyLease(draftId, writerB, 120); },
                    ProjectErrorKind::Conflict);
        const WorkingCopyInfo saved = service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, lease.generation,
            {ModelChange{firstKey, firstRecord}, ModelChange{secondKey, secondRecord}});
        require(saved.dirty && saved.generation == 1 && saved.candidateChanges.size() == 2 &&
                saved.candidateChanges[0].key == firstKey &&
                saved.candidateChanges[1].record->payload == secondRecord.payload &&
                !service.optionalHead(projectId, "main") &&
                service.revisionGraph(projectId).revisions.empty(),
                "draft save changed committed history or lost candidate changes");
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, 0, {}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerB, lease.token, 1, {}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, 1,
            {ModelChange{firstKey, firstRecord}, ModelChange{firstKey, firstRecord}}); },
            ProjectErrorKind::InvalidInput);
        require(service.workingCopy(draftId).generation == 1,
                "rejected duplicate-key save changed draft generation");
        const aowis::dvmdb::WorkingCopyLease refreshed =
            service.acquireWorkingCopyLease(draftId, writerA, 120);
        require(refreshed.token != lease.token && refreshed.generation == 1,
                "reacquiring a lease did not rotate its fencing token");
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, 1, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfo changed = service.saveWorkingCopyChanges(
            draftId, writerA, refreshed.token, 1,
            {ModelChange{firstKey, std::nullopt}});
        require(changed.generation == 2 && changed.candidateChanges.size() == 2 &&
                !changed.candidateChanges[0].record.has_value() &&
                changed.candidateChanges[1].record->payload == secondRecord.payload,
                "incremental Save failed to merge change overlay");
        service.releaseWorkingCopyLease(draftId, writerA, refreshed.token);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, refreshed.token, 2, {}); }, ProjectErrorKind::Conflict);
    }
    {
        ProjectService reopened(root.path);
        const WorkingCopyInfo recovered = reopened.workingCopy(draftId);
        require(recovered.generation == 2 && recovered.candidateChanges.size() == 2 &&
                recovered.dirty && !recovered.leaseToken &&
                !reopened.optionalHead(projectId, "main"),
                "restart lost draft saves or changed committed HEAD");
        const aowis::dvmdb::WorkingCopyLease taken =
            reopened.acquireWorkingCopyLease(draftId, writerB, 120);
        require(taken.generation == 2 && taken.token != staleToken,
                "another session could not acquire a released lease");
        expectError([&]() { (void)reopened.saveWorkingCopyChanges(
            draftId, writerA, staleToken, 2, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfo third = reopened.saveWorkingCopyChanges(
            draftId, writerB, taken.token, 2, {ModelChange{secondKey, secondRecord}});
        require(third.generation == 3 && third.candidateChanges.size() == 2,
                "new writer lost existing candidate state");
        reopened.releaseWorkingCopyLease(draftId, writerB, taken.token);
        const aowis::dvmdb::WorkingCopyLease shortLease =
            reopened.acquireWorkingCopyLease(draftId, writerA, 1);
        expectError([&]() { (void)reopened.acquireWorkingCopyLease(draftId, writerB, 120); },
                    ProjectErrorKind::Conflict);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const aowis::dvmdb::WorkingCopyLease takeover =
            reopened.acquireWorkingCopyLease(draftId, writerB, 120);
        require(takeover.generation == 3 && takeover.token != shortLease.token,
                "expired lease takeover did not preserve generation/fence");
        expectError([&]() { (void)reopened.saveWorkingCopyChanges(
            draftId, writerA, shortLease.token, 3, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfo final = reopened.saveWorkingCopyChanges(
            draftId, writerB, takeover.token, 3, {});
        require(final.generation == 4 && final.candidateChanges.size() == 2,
                "successful empty save lost stored candidate changes");
    }
}

void testAtomicJournalAndIdempotentReplay() {
    TemporaryRoot root;
    const std::string session = UuidV7::generate().toString();
    const UuidV7 eventId = UuidV7::generate();
    const UuidV7 entityId = UuidV7::generate();
    const aowis::dvmdb::Bytes event = aowis::dvmdb::test::editEvent(eventId, entityId);
    const aowis::dvmdb::Bytes differentEvent =
        aowis::dvmdb::test::editEvent(eventId, entityId, 1, 3);
    Key key = entityId.bytes;
    ModelRecord record;
    record.kind = 5;
    record.payload = {8, 9, 10};
    const std::vector<ModelChange> changes{{key, record}};
    std::string projectId;
    std::string draftId;
    std::string token;
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("atomic edit journal").id;
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        const WorkingCopyInfo saved = service.saveWorkingCopyTransactions(
            draftId, session, token, 0, changes, {event});
        require(saved.generation == 1 && saved.journal.size() == 1 &&
                saved.journal[0].id == eventId.toString() &&
                saved.journal[0].encoded == event &&
                saved.candidateChanges[0].record->payload == record.payload &&
                !service.optionalHead(projectId, "main").has_value(),
                "atomic event save lost journal/model or advanced HEAD");
        const WorkingCopyInfo retry = service.saveWorkingCopyTransactions(
            draftId, session, token, 0, changes, {event});
        require(retry.generation == 1 && retry.journal.size() == 1,
                "lost-acknowledgement retry duplicated a journal event");
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 0, changes, {differentEvent}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 1, changes, {event}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, session, token, 1, changes); }, ProjectErrorKind::Conflict);
        aowis::dvmdb::Bytes corrupt = aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId);
        corrupt.back() ^= 0x80;
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 1, changes, {corrupt}); }, ProjectErrorKind::InvalidInput);
        require(service.workingCopy(draftId).generation == 1,
                "rejected journal save mutated generation");
    }
    {
        ProjectService reopened(root.path);
        const WorkingCopyInfo persisted = reopened.workingCopy(draftId);
        require(persisted.generation == 1 && persisted.journal.size() == 1 &&
                persisted.journal[0].encoded == event &&
                persisted.candidateChanges.size() == 1,
                "restart lost journal or materialized candidate");
        const std::string nextToken = reopened.acquireWorkingCopyLease(draftId, session, 120).token;
        const aowis::dvmdb::Bytes next =
            aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId, 2, 3);
        const WorkingCopyInfo appended = reopened.saveWorkingCopyTransactions(
            draftId, session, nextToken, 1, changes, {next});
        require(appended.generation == 2 && appended.journal.size() == 2 &&
                appended.journal[0].encoded == event && appended.journal[1].encoded == next,
                "append did not preserve transaction order");
    }
    // Damage the persisted event's hex bytes, not just the record marker.
    const std::filesystem::path path = root.path / "working-copies" / draftId;
    std::ifstream input(path, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(input)), {});
    const std::size_t pos = body.find("event\t");
    require(pos != std::string::npos, "missing stored journal for corruption test");
    const std::size_t payload = body.find('\t', pos + 6);
    require(payload != std::string::npos && payload + 10 < body.size(), "bad journal test fixture");
    body[payload + 10] = body[payload + 10] == '0' ? '1' : '0';
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << body;
    output.close();
    ProjectService reopened(root.path);
    bool rejected = false;
    try { (void)reopened.workingCopy(draftId); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "corrupt on-disk event was accepted");
}

}

// R9: leases granted by this process are enforced against a monotonic
// deadline, so a wall-clock step neither extends nor cuts them short. After
// a restart only the persisted wall-clock expiry is known and is used.
void testLeasesIgnoreWallClockSteps() {
    TemporaryRoot root;
    std::atomic<std::uint64_t> wall{1'800'000'000'000ULL};
    std::atomic<std::int64_t> steadyOffsetMs{0};
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    const aowis::dvmdb::LeaseClock clock{
        [&wall]() { return wall.load(); },
        [&origin, &steadyOffsetMs]() {
            return origin + std::chrono::milliseconds(steadyOffsetMs.load());
        }};
    const std::string holder = UuidV7::generate().toString();
    const std::string other = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    {
        ProjectService service(root.path, 4, clock);
        projectId = service.createUnbornProject("lease clock").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, holder).id;

        // Wall clock jumps an hour ahead right after the lease was granted:
        // the 30 s lease is still held, takeover is refused, saves work.
        const std::string token = service.acquireWorkingCopyLease(draftId, holder, 30).token;
        wall += 3'600'000ULL;
        expectError([&]() { (void)service.acquireWorkingCopyLease(draftId, other, 30); },
                    ProjectErrorKind::Conflict);
        require(service.saveWorkingCopyChanges(draftId, holder, token, 0, {simpleChange()})
                    .generation == 1, "a forward wall-clock step cut a live lease short");

        // Wall clock jumps two hours back: the lease still ends after its
        // 30 monotonic seconds, then another session may take over.
        wall -= 7'200'000ULL;
        steadyOffsetMs += 31'000;
        require(!service.workingCopyLeaseLive(service.workingCopy(draftId)),
                "a backward wall-clock step extended an expired lease");
        expectError([&]() {
            (void)service.saveWorkingCopyChanges(draftId, holder, token, 1, {simpleChange()});
        }, ProjectErrorKind::Conflict);
        const std::string taken = service.acquireWorkingCopyLease(draftId, other, 30).token;
        require(service.saveWorkingCopyChanges(draftId, other, taken, 1, {simpleChange()})
                    .generation == 2, "lease takeover after monotonic expiry failed");
    }
    {
        // A restarted service only knows the persisted wall-clock expiry,
        // which was written as (wall at grant + 30 s).
        ProjectService reopened(root.path, 4, clock);
        require(reopened.workingCopyLeaseLive(reopened.workingCopy(draftId)),
                "persisted wall-clock expiry was not honoured after restart");
        wall += 31'000ULL;
        require(!reopened.workingCopyLeaseLive(reopened.workingCopy(draftId)),
                "an expired persisted lease was still live after restart");
    }
}

// R11: crash remnants of the catalogue's no-replace publication are removed
// when the service opens; nothing published is touched, and a retry of an
// interrupted caller-chosen project creation succeeds instead of staying Busy.
void testCatalogueRemnantsAreRemovedOnOpen() {
    TemporaryRoot root;
    std::string draftId;
    {
        ProjectService service(root.path);
        const std::string projectId = service.createUnbornProject("remnants").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt,
                                            UuidV7::generate().toString()).id;
    }
    const std::string chosen = aowis::dvmdb::ProjectId::generate().toString();
    std::filesystem::create_directories(root.path / "local-commits");
    std::filesystem::create_directories(root.path / "projects" / (".creating-" + chosen) / "repository");
    const std::vector<std::filesystem::path> remnants{
        root.path / (".repository-id-creating-" + UuidV7::generate().toString()),
        root.path / "working-copies" / (".creating-" + UuidV7::generate().toString()),
        root.path / "working-copies" / (".updating-" + UuidV7::generate().toString()),
        root.path / "local-commits" / (".creating-" + UuidV7::generate().toString()),
        root.path / "local-commits" / (".updating-" + UuidV7::generate().toString()),
        root.path / "projects" / (".creating-" + chosen) / "project.meta"};
    for (const std::filesystem::path& path : remnants) {
        std::ofstream output(path, std::ios::binary);
        output << "interrupted";
        require(output.good(), "cannot create remnant fixture");
    }
    ProjectService reopened(root.path);
    for (const std::filesystem::path& path : remnants)
        require(!std::filesystem::exists(path), "a crash remnant survived opening the catalogue");
    require(!std::filesystem::exists(root.path / "projects" / (".creating-" + chosen)),
            "an unpublished project directory survived opening the catalogue");
    require(reopened.listWorkingCopies().size() == 1 &&
            reopened.workingCopy(draftId).id == draftId &&
            reopened.listProjects().size() == 1,
            "remnant cleanup touched published drafts or projects");
    require(reopened.createUnbornProject("retried", chosen).id == chosen,
            "retry of an interrupted caller-chosen project creation did not succeed");
}

int main() {
    try {
        testWorkingCopyCreationAndRecovery();
        testLeasedTransactionalCandidateSaves();
        testAtomicJournalAndIdempotentReplay();
        testLeasesIgnoreWallClockSteps();
        testCatalogueRemnantsAreRemovedOnOpen();
        std::cout << "VC-04 persistent working-copy bootstrap passed\n";
    } catch (const std::exception& error) {
        std::cerr << "VC-04 working-copy bootstrap failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
