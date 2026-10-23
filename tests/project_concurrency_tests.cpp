// Review finding F4: projects never wait for each other. The catalogue mutex
// guards only the project map; each project serializes its own scoped
// operations and limits how many may queue (ProjectErrorKind::Busy).
#include "aowis/native/project_service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::ProjectError;
using aowis::native::ProjectErrorKind;
using aowis::native::ProjectService;
using aowis::native::Repository;
using aowis::native::StorageError;
using aowis::native::StorageErrorKind;
using Clock = std::chrono::steady_clock;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-project-concurrency-" + std::string(label) + "-" + std::to_string(::getpid()) +
             "-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter));
        std::filesystem::create_directories(this->path);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

Key key(std::uint8_t last) {
    Key result{};
    result.back() = last;
    return result;
}

// Holds one scoped operation of a project open until release().
class Holder {
public:
    Holder(ProjectService& service, const std::string& projectId) {
        this->thread_ = std::thread([this, &service, projectId] {
            try {
                service.withRepository(projectId, [this](Repository&) {
                    std::unique_lock<std::mutex> lock(this->mutex_);
                    this->entered_ = true;
                    this->changed_.notify_all();
                    this->changed_.wait(lock, [this] { return this->released_; });
                });
            } catch (...) {
                std::lock_guard<std::mutex> lock(this->mutex_);
                this->failure_ = std::current_exception();
                this->entered_ = true;
                this->changed_.notify_all();
            }
        });
        std::unique_lock<std::mutex> lock(this->mutex_);
        expect(this->changed_.wait_for(lock, std::chrono::seconds(10), [this] { return this->entered_; }),
               "holder did not enter its operation");
        if (this->failure_) std::rethrow_exception(this->failure_);
    }
    ~Holder() { this->release(); }
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    void release() {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            this->released_ = true;
        }
        this->changed_.notify_all();
        if (this->thread_.joinable()) this->thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    std::exception_ptr failure_;
    std::thread thread_;
};

// Joins a helper thread on scope exit, so a failed check reports its
// message instead of terminating with a joinable thread.
struct Joiner {
    explicit Joiner(std::thread& target) : thread(target) {}
    ~Joiner() {
        if (this->thread.joinable()) this->thread.join();
    }
    Joiner(const Joiner&) = delete;
    Joiner& operator=(const Joiner&) = delete;
    std::thread& thread;
};

// Runs `action` on another thread; fails (instead of hanging) if it does not
// finish in time, e.g. because it waits for a lock it must not need.
void completesWithin(std::chrono::milliseconds limit, const std::string& label,
                     const std::function<void()>& action) {
    std::packaged_task<void()> task(action);
    std::future<void> done = task.get_future();
    std::thread runner(std::move(task));
    if (done.wait_for(limit) != std::future_status::ready) {
        runner.detach(); // the holder's release unblocks it before exit
        throw std::runtime_error(label + ": blocked by a held operation");
    }
    runner.join();
    done.get();
}

void waitForPending(ProjectService& service, const std::string& id, std::size_t count) {
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    while (service.pendingOperations(id) != count) {
        expect(Clock::now() < deadline, "pending operation count not reached");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void expectProjectError(ProjectErrorKind kind, const std::string& label, const std::function<void()>& action) {
    try {
        action();
    } catch (const ProjectError& error) {
        expect(error.kind() == kind, label + ": wrong project error kind (" + error.what() + ")");
        return;
    }
    throw std::runtime_error(label + ": no ProjectError");
}

void testProjectsIndependent() {
    TestDirectory directory("independent");
    ProjectService service(directory.path, 4);
    const std::string a = service.createProject("A").id;
    const std::string b = service.createProject("B").id;
    Holder held(service, a);
    completesWithin(std::chrono::milliseconds(2000), "other project and catalogue", [&] {
        const Clock::time_point start = Clock::now();
        expect(service.listProjects().size() == 2, "catalogue listing failed");
        expect(service.project(a).name == "A", "project lookup failed");
        (void)service.createProject("C");
        service.withRepository(b, [](Repository& repository) {
            repository.commit("main", {Change{key(1), Bytes{1}}});
        });
        expect(service.createBranch(b, "feature", std::string_view("main")).id == service.head(b, "main").id,
               "branch of the other project failed");
        // Single reads of the held project take no operation lock.
        expect(service.listBranches(a).size() == 1 && service.history(a, "main", 10).size() == 1,
               "reads of the held project failed");
        (void)service.revision(a, service.head(a, "main").id);
        (void)service.createScratchFile(a);
        expect(Clock::now() - start < std::chrono::milliseconds(1000), "independent operations were slow");
    });
}

void testSameProjectSerialized() {
    TestDirectory directory("serialized");
    ProjectService service(directory.path, 4);
    const std::string a = service.createProject("A").id;
    std::atomic<bool> entered{false};
    bool enteredWhileHeld = false;
    std::thread second;
    const Joiner joinSecond(second);
    {
        Holder held(service, a);
        second = std::thread([&] {
            service.withRepository(a, [&](Repository&) { entered = true; });
        });
        waitForPending(service, a, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        enteredWhileHeld = entered.load();
    }
    second.join();
    expect(!enteredWhileHeld, "second operation of one project ran concurrently");
    expect(entered.load() && service.pendingOperations(a) == 0, "second operation did not run after release");
}

void testQueueLimit() {
    TestDirectory directory("limit");
    expectProjectError(ProjectErrorKind::InvalidInput, "zero queue limit",
                       [&] { ProjectService invalid(directory.path / "zero", 0); });
    ProjectService service(directory.path / "root", 2);
    expect(service.maxPendingOperations() == 2, "limit not stored");
    const std::string a = service.createProject("A").id;
    const std::string b = service.createProject("B").id;
    std::thread waiting;
    const Joiner joinWaiting(waiting);
    {
        Holder held(service, a);
        waiting = std::thread([&] { service.withRepository(a, [](Repository&) {}); });
        waitForPending(service, a, 2);
        // Rejected at once; without the limit these would wait for the hold.
        completesWithin(std::chrono::milliseconds(2000), "full queue", [&] {
            expectProjectError(ProjectErrorKind::Busy, "third operation",
                               [&] { service.withRepository(a, [](Repository&) {}); });
            expectProjectError(ProjectErrorKind::Busy, "branch creation", [&] { service.createBranch(a, "x"); });
            expectProjectError(ProjectErrorKind::Busy, "reservation", [&] { (void)service.reserveOperation(a); });
        });
        expect(service.pendingOperations(a) == 2, "rejected requests changed the queue");
        // The other project's queue is separate; reads are never queued.
        ProjectService::Operation other = service.reserveOperation(b);
        expect(service.pendingOperations(b) == 1, "reservation not counted");
        (void)service.head(a, "main");
    }
    waiting.join();
    expect(service.pendingOperations(a) == 0 && service.pendingOperations(b) == 0,
           "queue places not released");
    {
        ProjectService::Operation reserved = service.reserveOperation(a);
        ProjectService::Operation moved(std::move(reserved));
        expect(service.pendingOperations(a) == 1, "moved reservation counted twice");
        expectProjectError(ProjectErrorKind::InvalidInput, "moved-from reservation",
                           [&] { service.withRepository(reserved, [](Repository&) {}); });
        unsigned runs = 0;
        service.withRepository(moved, [&](Repository&) { ++runs; });
        service.withRepository(moved, [&](Repository&) { ++runs; }); // reusable
        expect(runs == 2 && service.pendingOperations(a) == 1, "reserved operation did not run");
    }
    expect(service.pendingOperations(a) == 0, "reservation not released on destruction");
    expectProjectError(ProjectErrorKind::NotFound, "unknown project",
                       [&] { (void)service.reserveOperation("00000000-0000-4000-8000-000000000000"); });
    expectProjectError(ProjectErrorKind::InvalidInput, "invalid project",
                       [&] { (void)service.pendingOperations("../x"); });
}

// Threads racing on one expected head: exactly one commit wins.
void testExpectedHeadRace() {
    TestDirectory directory("race");
    ProjectService service(directory.path, 16);
    const std::string a = service.createProject("A").id;
    service.withRepository(a, [](Repository& repository) {
        repository.commitModel("main", {ModelChange{key(1), ModelRecord{1, {}, Bytes{1}}}});
    });
    const Hash expected = service.head(a, "main").id;
    std::atomic<unsigned> succeeded{0};
    std::atomic<unsigned> conflicts{0};
    std::atomic<unsigned> other{0};
    std::vector<std::thread> threads;
    for (std::uint8_t i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            try {
                service.withRepository(a, [&](Repository& repository) {
                    repository.commitModel("main", {ModelChange{key(static_cast<std::uint8_t>(10 + i)),
                                                                ModelRecord{1, {}, Bytes{i}}}}, expected);
                });
                ++succeeded;
            } catch (const StorageError& error) {
                if (error.kind() == StorageErrorKind::Conflict) ++conflicts; else ++other;
            } catch (...) {
                ++other;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    expect(succeeded == 1 && conflicts == 7 && other == 0, "expected-head race not exactly one winner");
    // Branch creation with one name: one 201-equivalent, the rest Conflict.
    threads.clear();
    succeeded = 0;
    conflicts = 0;
    for (unsigned i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            try {
                (void)service.createBranch(a, "same");
                ++succeeded;
            } catch (const ProjectError& error) {
                if (error.kind() == ProjectErrorKind::Conflict) ++conflicts; else ++other;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    expect(succeeded == 1 && conflicts == 7 && other == 0, "branch creation race not exactly one winner");
}

// Many threads opening and using unopened projects at once, plus parallel
// imports into two projects, then a full verification of each.
void testConcurrentOpenAndImport() {
    TestDirectory directory("open");
    std::string a;
    std::string b;
    {
        ProjectService service(directory.path / "root");
        a = service.createProject("A").id;
        b = service.createProject("B").id;
    }
    Repository source(directory.path / "source");
    source.createBranch("main");
    std::vector<Change> changes;
    for (unsigned i = 0; i < 3000; ++i) {
        Key item{};
        item[0] = static_cast<std::uint8_t>(i >> 8U);
        item[1] = static_cast<std::uint8_t>(i);
        item[15] = 1;
        changes.push_back(Change{item, Bytes(64, static_cast<std::uint8_t>(i))});
    }
    const Hash head = source.commit("main", changes).id;
    source.exportBranchBundle("main", directory.path / "bundle");

    ProjectService service(directory.path / "root", 16);
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 16; ++i) {
        threads.emplace_back([&, i] {
            try {
                const std::string& id = (i % 2U) == 0U ? a : b;
                if (i < 2) {
                    service.withRepository(id, [&](Repository& repository) {
                        expect(repository.importBranchBundle(directory.path / "bundle", "imported").head == head,
                               "parallel import failed");
                    });
                } else {
                    (void)service.head(id, "main");
                    (void)service.listBranches(id);
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    expect(failures == 0, "concurrent open or import failed");
    for (const std::string& id : {a, b}) {
        expect(service.head(id, "imported").id == head, "imported branch missing");
        service.withRepository(id, [](Repository& repository) { (void)repository.verifyRepository(); });
    }
}

} // namespace

int main() {
    try {
        testProjectsIndependent();
        testSameProjectSerialized();
        testQueueLimit();
        testExpectedHeadRace();
        testConcurrentOpenAndImport();
        std::cout << "Project service concurrency tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project service concurrency test failed: " << error.what() << '\n';
        return 1;
    }
}
