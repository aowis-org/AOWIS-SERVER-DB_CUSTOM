#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <future>
#include <thread>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
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
// R4: a session that took over an expired lease is the writer: it can save
// AND commit, while the creating session's stale token can do neither.
void testLeaseTakeoverCanCommit() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProject("lease takeover").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string creator = UuidV7::generate().toString();
    const std::string successor = UuidV7::generate().toString();
    const std::string draftId = service.createWorkingCopy(
        projectId, "main", std::nullopt, creator).id;
    const std::string staleToken = service.acquireWorkingCopyLease(draftId, creator, 1).token;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const std::string token = service.acquireWorkingCopyLease(draftId, successor, 120).token;
    expectConflict([&]() {
        (void)service.saveWorkingCopyChanges(draftId, creator, staleToken, 0, {changeWithValue(3)});
    });
    require(service.saveWorkingCopyChanges(draftId, successor, token, 0,
                                           {changeWithValue(4)}).generation == 1,
            "lease successor could not save");
    expectConflict([&]() {
        (void)service.commitWorkingCopy(UuidV7::generate().toString(), draftId, creator,
                                        staleToken, 1, std::nullopt);
    });
    const LocalCommitInfo committed = service.commitWorkingCopy(
        UuidV7::generate().toString(), draftId, successor, token, 1, std::nullopt);
    require(committed.state == LocalCommitState::Succeeded &&
            service.optionalHead(projectId, "main") ==
                RevisionId::parse(committed.reservedRevisionId),
            "lease successor could save but not Commit the recovered draft");
}

// Commits a one-record candidate from a fresh draft at the current HEAD.
LocalCommitInfo commitFreshDraft(ProjectService& service, const std::string& projectId,
                                 const std::string& operationId, const ModelChange& change) {
    const std::string session = UuidV7::generate().toString();
    const std::optional<RevisionId> head = service.optionalHead(projectId, "main");
    const std::string draftId = service.createWorkingCopy(projectId, "main", head, session).id;
    const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    service.saveWorkingCopyChanges(draftId, session, token, 0, {change});
    return service.commitWorkingCopy(operationId, draftId, session, token, 1, head);
}

// R5: a Commit that failed with an I/O error before HEAD publication stays
// pending only while HEAD is still at its base. Once another Commit moves
// HEAD, the outcome is decided: status and retries converge to Rejected.
void testFailedCommitIsRejectedOnceHeadMoves() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string token;
    std::optional<RevisionId> base;
    {
        ProjectService service(root.path);
        projectId = service.createUnbornProject("decidable pending Commit").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        // A large first revision makes the pack far bigger than any draft
        // or ledger record, so a file-size limit only hits the pack append.
        ModelChange large = changeWithValue(1);
        large.key[15] = 1;
        large.record->payload.assign(256 * 1024, 0x5a);
        require(commitFreshDraft(service, projectId, UuidV7::generate().toString(), large).state ==
                    LocalCommitState::Succeeded, "large base Commit failed");
        base = service.optionalHead(projectId, "main");
        draftId = service.createWorkingCopy(projectId, "main", base, session).id;
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        ModelChange small = changeWithValue(2);
        small.record->payload.assign(64 * 1024, 0x33); // unique, not deduplicated
        service.saveWorkingCopyChanges(draftId, session, token, 0, {small});

        const std::filesystem::path pack =
            root.path / "projects" / projectId / "repository" / "objects.pack";
        struct rlimit previous{};
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "getrlimit failed");
        void (*const oldHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit limited = previous;
        limited.rlim_cur = static_cast<rlim_t>(std::filesystem::file_size(pack) + 1024);
        require(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "setrlimit failed");
        bool failed = false;
        try {
            (void)service.commitWorkingCopy(operationId, draftId, session, token, 1, base);
        } catch (const std::exception&) {
            failed = true;
        }
        require(::setrlimit(RLIMIT_FSIZE, &previous) == 0, "restoring RLIMIT_FSIZE failed");
        std::signal(SIGXFSZ, oldHandler);
        require(failed, "file-size limit did not make the pack write fail");
        require(service.optionalHead(projectId, "main") == base,
                "failed Commit moved HEAD");
        require(service.localCommitStatus(operationId).state == LocalCommitState::Pending,
                "an undecided Commit (HEAD still at its base) must stay pending");

        // Another Commit moves HEAD; the failed operation is now decidable.
        const LocalCommitInfo other = commitFreshDraft(
            service, projectId, UuidV7::generate().toString(), changeWithValue(9));
        require(other.state == LocalCommitState::Succeeded, "competing Commit failed");
        const LocalCommitInfo status = service.localCommitStatus(operationId);
        require(status.state == LocalCommitState::Rejected && status.rejection == "conflict",
                "status of an undecidable-no-longer Commit did not converge to Rejected");
        const LocalCommitInfo retry = service.commitWorkingCopy(
            operationId, draftId, session, token, 1, base);
        require(retry.state == LocalCommitState::Rejected &&
                retry.reservedRevisionId == status.reservedRevisionId,
                "retry of a decided Commit did not return its rejected outcome");
    }
    ProjectService reopened(root.path);
    require(reopened.localCommitStatus(operationId).state == LocalCommitState::Rejected,
            "rejected outcome was not durable");
    require(reopened.workingCopy(draftId).dirty, "rejected Commit lost the saved draft");
}

// Holds the repository's file lock from a second descriptor, so every
// repository operation of that project blocks inside the engine.
class RepositoryLockHold {
public:
    explicit RepositoryLockHold(const std::filesystem::path& repository)
        : fd_(::open((repository / "repo.lock").c_str(), O_RDWR | O_CLOEXEC)) {
        require(this->fd_ >= 0 && ::flock(this->fd_, LOCK_EX) == 0, "cannot hold repository lock");
    }
    ~RepositoryLockHold() { this->release(); }
    RepositoryLockHold(const RepositoryLockHold&) = delete;
    RepositoryLockHold& operator=(const RepositoryLockHold&) = delete;
    void release() {
        if (this->fd_ >= 0) {
            (void)::flock(this->fd_, LOCK_UN);
            (void)::close(this->fd_);
            this->fd_ = -1;
        }
    }
private:
    int fd_ = -1;
};

// Runs `action` on a thread and reports whether it finished within `limit`.
bool finishesWithin(const std::function<void()>& action, std::chrono::milliseconds limit,
                    std::future<void>& pending) {
    pending = std::async(std::launch::async, action);
    return pending.wait_for(limit) == std::future_status::ready;
}

// R3: a local Commit (and a status lookup) blocked inside the repository of
// project A must not stall the catalogue, other projects, other drafts of
// A, or draft reads. Only the committing draft itself waits.
void testCommitDoesNotHoldCatalogue() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string a = service.createUnbornProject("blocked project").id;
    const std::string b = service.createUnbornProject("independent project").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    require(commitFreshDraft(service, a, UuidV7::generate().toString(), changeWithValue(1)).state ==
                LocalCommitState::Succeeded, "fixture Commit failed");
    const std::optional<RevisionId> head = service.optionalHead(a, "main");
    const std::string session = UuidV7::generate().toString();
    const std::string committing = service.createWorkingCopy(a, "main", head, session).id;
    const std::string token = service.acquireWorkingCopyLease(committing, session, 120).token;
    service.saveWorkingCopyChanges(committing, session, token, 0, {changeWithValue(2)});
    const std::string sibling = service.createWorkingCopy(a, "main", head, session).id;
    const std::string siblingToken = service.acquireWorkingCopyLease(sibling, session, 120).token;
    const std::string other = service.createWorkingCopy(b, "main", std::nullopt, session).id;
    const std::string otherToken = service.acquireWorkingCopyLease(other, session, 120).token;
    const std::string operationId = UuidV7::generate().toString();

    RepositoryLockHold hold(root.path / "projects" / a / "repository");
    std::future<void> commit = std::async(std::launch::async, [&]() {
        require(service.commitWorkingCopy(operationId, committing, session, token, 1, head).state ==
                    LocalCommitState::Succeeded, "blocked Commit did not succeed after release");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(commit.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
            "repository lock fixture did not block the Commit");
    std::future<void> independent;
    const bool responsive = finishesWithin([&]() {
        (void)service.project(b);
        (void)service.listProjects();
        (void)service.listWorkingCopies();
        (void)service.workingCopy(committing);
        require(service.saveWorkingCopyChanges(sibling, session, siblingToken, 0,
                                               {changeWithValue(3)}).generation == 1,
                "sibling draft save failed");
        require(service.saveWorkingCopyChanges(other, session, otherToken, 0,
                                               {changeWithValue(4)}).generation == 1,
                "other project's draft save failed");
    }, std::chrono::seconds(5), independent);
    std::future<void> sameDraft;
    const bool sameDraftWaited = !finishesWithin([&]() {
        (void)service.acquireWorkingCopyLease(committing, session, 120);
    }, std::chrono::milliseconds(200), sameDraft);
    hold.release();
    commit.get();
    independent.get();
    sameDraft.get();
    require(responsive, "a Commit blocked in one repository stalled the catalogue or other drafts");
    require(sameDraftWaited, "the committing draft was not protected against concurrent writers");

    // The status lookup takes the same path; it must not block either.
    RepositoryLockHold statusHold(root.path / "projects" / a / "repository");
    std::future<void> status = std::async(std::launch::async, [&]() {
        require(service.localCommitStatus(operationId).state == LocalCommitState::Succeeded,
                "status after release is wrong");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::future<void> catalogue;
    const bool available = finishesWithin([&]() {
        (void)service.project(b);
        (void)service.listWorkingCopies();
    }, std::chrono::seconds(5), catalogue);
    statusHold.release();
    status.get();
    catalogue.get();
    require(available, "a blocked status lookup stalled the catalogue");
}
}

int main() {
    try {
        testLocalCommitRecovery();
        testConcurrentFirstCommitIsSerialized();
        testRejectedOperationIsDurable();
        testLeaseTakeoverCanCommit();
        testFailedCommitIsRejectedOnceHeadMoves();
        testCommitDoesNotHoldCatalogue();
        std::cout << "VC-05 durable local Commit tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "VC-05 durable local Commit tests failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
