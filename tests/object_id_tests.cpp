#include "aowis/dvmdb/object_id.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::UuidV7Generator;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::BranchId;
using aowis::dvmdb::RevisionId;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exerciseFormat() {
    static_assert(!std::is_same_v<ProjectId, BranchId>);
    static_assert(!std::is_same_v<BranchId, RevisionId>);
    static_assert(!std::is_convertible_v<ProjectId, BranchId>);
    const UuidV7 expected = *UuidV7::parse("018f84c0-0000-7abc-8def-0123456789ab");
    check(expected.toString() == "018f84c0-0000-7abc-8def-0123456789ab", "format mismatch");
    check(expected.unixMilliseconds() == 0x018f84c00000ULL, "incorrect timestamp byte order");
    check(!UuidV7::parse("018f84c0-0000-4abc-8def-0123456789ab"), "accepts v4");
    check(!UuidV7::parse("018f84c0-0000-7abc-cdef-0123456789ab"), "accepts invalid variant");
    check(!UuidV7::parse("018F84C0-0000-7abc-8def-0123456789ab"), "accepts noncanonical case");
    check(!UuidV7::parse("018f84c000007abc8def0123456789ab"), "accepts no hyphens");
    check(!UuidV7::parse("018f84c0-0000-7abc-8def-0123456789ag"), "accepts malformed hex");
    check(!UuidV7::parse(""), "accepts empty ID");
    check(ProjectId::parse(expected.toString()).has_value(), "typed parse failed");
}

void exerciseClockRollback() {
    std::uint64_t clock = 0x018f84c00000ULL;
    UuidV7Generator generator([&clock] { return clock; });
    const UuidV7 first = generator.generate();
    check(first.unixMilliseconds() == clock, "incorrect injected timestamp");
    std::set<std::string> all{first.toString()};
    UuidV7 previous = first;
    for (int index = 0; index < 20000; ++index) {
        if (index == 10000) clock -= 2500;
        const UuidV7 next = generator.generate();
        check(next.unixMilliseconds() == first.unixMilliseconds(), "clock rolled back in UUID");
        check(previous < next, "non-monotonic same-millisecond UUIDs");
        check(all.insert(next.toString()).second, "duplicate UUID from one generator");
        check(UuidV7::parse(next.toString()) == next, "UUID roundtrip failed");
        previous = next;
    }
    clock = 0x018f84c00000ULL + 1;
    const UuidV7 advanced = generator.generate();
    check(advanced.unixMilliseconds() == clock && previous < advanced, "clock advancement failed");
    bool badClockRejected = false;
    try {
        UuidV7Generator invalid([] { return 1ULL << 48; });
        invalid.generate();
    } catch (const std::runtime_error&) { badClockRejected = true; }
    check(badClockRejected, "accepted overflowing millisecond clock");
}

void exerciseForkedGeneration() {
    // Exercise a *pre-seeded* generator, where an unguarded fork would copy
    // the exact same logical millisecond and incrementing random sequence.
    const std::uint64_t millisecond = 0x018f84c00000ULL;
    UuidV7Generator generator([millisecond] { return millisecond; });
    generator.generate();
    int pipes[2]{};
    check(::pipe(pipes) == 0, "pipe failed");
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        ::close(pipes[0]);
        try {
            const std::string id = generator.generate().toString();
            const ssize_t written = ::write(pipes[1], id.data(), id.size());
            ::close(pipes[1]);
            _exit(written == static_cast<ssize_t>(id.size()) ? 0 : 1);
        } catch (...) {
            _exit(2);
        }
    }
    ::close(pipes[1]);
    const std::string parent = generator.generate().toString();
    char buffer[37]{};
    std::size_t consumed = 0;
    while (consumed < 36) {
        const ssize_t readBytes = ::read(pipes[0], buffer + consumed, 36 - consumed);
        if (readBytes <= 0) break;
        consumed += static_cast<std::size_t>(readBytes);
    }
    ::close(pipes[0]);
    int status = 0;
    check(::waitpid(child, &status, 0) == child, "waitpid failed");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && consumed == 36,
          "child generation failed");
    const std::string forked(buffer, consumed);
    check(UuidV7::parse(forked).has_value(), "child UUID invalid");
    check(forked != parent, "fork duplicated in-process UUIDv7 random state");
}

void exerciseConcurrentGeneration() {
    constexpr int Workers = 8;
    constexpr int Each = 2000;
    std::vector<std::future<std::vector<std::string>>> jobs;
    for (int worker = 0; worker < Workers; ++worker) {
        jobs.push_back(std::async(std::launch::async, [] {
            std::vector<std::string> values;
            values.reserve(Each);
            for (int i = 0; i < Each; ++i) {
                const ProjectId id = ProjectId::generate();
                values.push_back(id.toString());
            }
            return values;
        }));
    }
    std::set<std::string> unique;
    for (std::future<std::vector<std::string>>& job : jobs) {
        const std::vector<std::string> ids = job.get();
        for (const std::string& id : ids) {
            check(ProjectId::parse(id).has_value(), "invalid generated project ID");
            check(unique.insert(id).second, "duplicate ID in concurrent generation");
        }
    }
    check(unique.size() == Workers * Each, "missing IDs");
}
} // namespace

int main() {
    try {
        exerciseFormat();
        exerciseClockRollback();
        exerciseConcurrentGeneration();
        exerciseForkedGeneration();
        std::cout << "UUIDv7 identities: RFC bits, parsing, clock rollback, types and concurrency passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 identities: " << error.what() << '\n';
        return 1;
    }
}
