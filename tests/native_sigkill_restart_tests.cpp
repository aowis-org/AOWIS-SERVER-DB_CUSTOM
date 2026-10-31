#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
namespace fs = std::filesystem;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::Repository;

void require(bool valid, const std::string& message) {
    if (!valid) throw std::runtime_error(message);
}

struct TestRoot {
    fs::path path;
    TestRoot() {
        std::string directory = (fs::temp_directory_path() /
            "aowis-sigkill-restart-XXXXXXXX").string();
        std::vector<char> buffer(directory.begin(), directory.end());
        buffer.push_back('\0');
        require(::mkdtemp(buffer.data()) != nullptr, "mkdtemp failed");
        this->path = buffer.data();
    }
    ~TestRoot() {
        std::error_code error;
        fs::remove_all(this->path, error);
    }
};

Key key() {
    Key result{};
    result.back() = 1;
    return result;
}

Bytes counterValue(std::uint64_t value) {
    Bytes result(8U + 32768U, 0x5a);
    for (unsigned byte = 0; byte != 8; ++byte)
        result[byte] = static_cast<std::uint8_t>(value >> (8U * byte));
    return result;
}

std::uint64_t readCounter(ProjectRepository& repository) {
    const std::optional<Bytes> bytes = repository.get("main", key());
    require(bytes.has_value() && bytes->size() == 8U + 32768U,
            "counter payload missing or corrupted");
    std::uint64_t result = 0;
    for (unsigned byte = 0; byte != 8; ++byte)
        result |= static_cast<std::uint64_t>((*bytes)[byte]) << (8U * byte);
    require(*bytes == counterValue(result), "counter payload bytes damaged");
    return result;
}

void sendAck(int descriptor, const std::string& ack) {
    const std::string record = ack + "\n";
    const ssize_t amount = ::write(descriptor, record.data(), record.size());
    if (amount != static_cast<ssize_t>(record.size())) ::_exit(82);
}

[[noreturn]] void workerWrite(const fs::path& repositoryPath, int ackFd) {
    try {
        ProjectRepository repository(repositoryPath);
        std::uint64_t next = readCounter(repository);
        for (;;) {
            ++next;
            (void)repository.commit("main", {Change{key(), counterValue(next)}});
            sendAck(ackFd, std::to_string(next));
        }
    } catch (const std::exception& error) {
        std::cerr << "Writer: " << error.what() << '\n';
        ::_exit(81);
    }
}

[[noreturn]] void workerExport(const fs::path& repositoryPath, const fs::path& output,
                               unsigned round, int ackFd) {
    try {
        ProjectRepository repository(repositoryPath);
        for (std::uint64_t sequence = 0;; ++sequence) {
            const fs::path bundle = output /
                ("round-" + std::to_string(round) + "-" + std::to_string(sequence) + ".bundle");
            repository.exportBundle("main", bundle);
            sendAck(ackFd, bundle.filename().string());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (const std::exception& error) {
        std::cerr << "Exporter: " << error.what() << '\n';
        ::_exit(81);
    }
}

// The worker is exec'd rather than merely forked, so repository locks,
// descriptors, and in-process indices never leak from the parent.
std::vector<std::string> interrupt(const std::string& executable,
                                    std::string_view mode, const fs::path& repository,
                                    const fs::path& output, unsigned round, unsigned delayMs) {
    int pipeFd[2]{};
    require(::pipe(pipeFd) == 0, "pipe failed");
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        ::close(pipeFd[0]);
        const std::string fd = std::to_string(pipeFd[1]);
        const std::string roundText = std::to_string(round);
        ::execl(executable.c_str(), executable.c_str(), "--worker", std::string(mode).c_str(),
                repository.c_str(), output.c_str(), roundText.c_str(), fd.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(83);
    }
    ::close(pipeFd[1]);
    // Arm the kill timer only after the worker has acknowledged its first
    // operation, so slow startup (exec, sanitizers, repository open) cannot
    // consume the whole delay. The kill then lands at a varying point of a
    // later operation.
    std::string received;
    std::array<char, 4096> chunk{};
    const std::chrono::steady_clock::time_point startupDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (received.find('\n') == std::string::npos) {
        pollfd ready{pipeFd[0], POLLIN, 0};
        const int polled = ::poll(&ready, 1, 100);
        if (polled < 0 && errno == EINTR) continue;
        require(polled >= 0, "acknowledgement pipe poll failed");
        require(std::chrono::steady_clock::now() < startupDeadline,
                "worker did not acknowledge its first operation");
        if (polled == 0) continue;
        const ssize_t amount = ::read(pipeFd[0], chunk.data(), chunk.size());
        if (amount < 0 && errno == EINTR) continue;
        require(amount > 0, "worker exited before its first acknowledgement");
        received.append(chunk.data(), static_cast<std::size_t>(amount));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    require(::kill(child, SIGKILL) == 0, "SIGKILL failed");
    int status = 0;
    require(::waitpid(child, &status, 0) == child, "waitpid failed");
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "child was not killed by SIGKILL");
    while (true) {
        const ssize_t amount = ::read(pipeFd[0], chunk.data(), chunk.size());
        if (amount == 0) break;
        if (amount < 0 && errno == EINTR) continue;
        require(amount > 0, "acknowledgement pipe read failed");
        received.append(chunk.data(), static_cast<std::size_t>(amount));
    }
    ::close(pipeFd[0]);
    std::vector<std::string> acknowledgements;
    std::size_t start = 0;
    while (start < received.size()) {
        const std::size_t end = received.find('\n', start);
        if (end == std::string::npos) break; // Killed during acknowledgement.
        acknowledgements.push_back(received.substr(start, end - start));
        start = end + 1U;
    }
    return acknowledgements;
}

void writeKillDrill(const std::string& executable, const fs::path& root) {
    const fs::path repositoryPath = root / "write-repository";
    {
        ProjectRepository repository(repositoryPath);
        repository.createBranch("main");
        (void)repository.commit("main", {Change{key(), counterValue(0)}});
    }
    std::uint64_t acknowledged = 0;
    unsigned acknowledgedCommits = 0;
    unsigned interruptedCommits = 0;
    for (unsigned round = 0; round != 24U; ++round) {
        const unsigned delay = 2U + ((round * 13U) % 21U);
        const std::vector<std::string> acks =
            interrupt(executable, "write", repositoryPath, root, round, delay);
        for (const std::string& ack : acks) {
            const std::uint64_t sequence = std::stoull(ack);
            require(sequence == acknowledged + 1, "missing or duplicate commit acknowledgement");
            acknowledged = sequence;
            ++acknowledgedCommits;
        }
        // A completed publication might not yet have been acknowledged.
        // Such a revision is valid, but cannot advance by more than one.
        ProjectRepository recovered(repositoryPath);
        const std::uint64_t durable = readCounter(recovered);
        require(durable >= acknowledged, "acknowledged commit disappeared after SIGKILL");
        require(durable <= acknowledged + 1, "unexpected unacknowledged revision sequence");
        if (durable > acknowledged) {
            ++interruptedCommits;
            acknowledged = durable;
        }
        recovered.verifyRepository();
    }
    require(acknowledgedCommits > 0, "writer never completed a commit before interruption");
    {
        ProjectRepository repository(repositoryPath);
        const std::uint64_t before = readCounter(repository);
        (void)repository.commit("main", {Change{key(), counterValue(before + 1)}});
        require(readCounter(repository) == before + 1, "write after repeated SIGKILL failed");
        repository.verifyRepository();
    }
    std::cout << "Production writer SIGKILL/reopen: 24 cycles, " << acknowledgedCommits
              << " acknowledgements, " << interruptedCommits << " unacknowledged publications\n";
}

void exportKillDrill(const std::string& executable, const fs::path& root) {
    const fs::path repositoryPath = root / "export-repository";
    const fs::path output = root / "exports";
    fs::create_directories(output);
    RevisionId head = RevisionId::generate();
    {
        ProjectRepository repository(repositoryPath);
        repository.createBranch("main");
        (void)repository.commit("main", {Change{key(), counterValue(7)}});
        head = repository.head("main");
    }
    unsigned acknowledgedExports = 0;
    for (unsigned round = 0; round != 8U; ++round) {
        const std::vector<std::string> acks = interrupt(
            executable, "export", repositoryPath, output, round, 2U + ((round * 11U) % 20U));
        for (const std::string& filename : acks) {
            require(fs::is_regular_file(output / filename),
                    "acknowledged bundle missing after SIGKILL");
            ++acknowledgedExports;
        }
        // A final filename can appear immediately before an acknowledgement;
        // it must still be complete and importable. Private staging names are
        // not published bundles and are deliberately ignored here.
        unsigned verified = 0;
        for (const fs::directory_entry& entry : fs::directory_iterator(output)) {
            if (entry.path().extension() != ".bundle") continue;
            const fs::path imported = root / "verify-import";
            {
                ProjectRepository destination(imported);
                const aowis::dvmdb::RevisionBundleImport result =
                    destination.importBundle(entry.path(), "main");
                require(result.head == head, "completed export has wrong head");
                destination.verifyRepository();
            }
            // Release the destination lifetime lock before removing its files.
            fs::remove_all(imported);
            ++verified;
        }
        require(verified >= acknowledgedExports, "not all acknowledged bundles verified");
        ProjectRepository source(repositoryPath);
        require(source.head("main") == head, "export changed source head");
        source.verifyRepository();
    }
    require(acknowledgedExports > 0, "no bundle exported before interruption");
    std::cout << "Production named exports SIGKILL/reopen: 8 cycles, "
              << acknowledgedExports << " acknowledged exports\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 7 && std::string_view(argv[1]) == "--worker") {
            const int descriptor = std::stoi(argv[6]);
            const unsigned round = static_cast<unsigned>(std::stoul(argv[5]));
            if (std::string_view(argv[2]) == "write") workerWrite(argv[3], descriptor);
            if (std::string_view(argv[2]) == "export") workerExport(argv[3], argv[4], round, descriptor);
            return 84;
        }
        require(argc == 1, "unexpected command line arguments");
        TestRoot root;
        const std::string executable = fs::canonical(argv[0]).string();
        writeKillDrill(executable, root.path);
        exportKillDrill(executable, root.path);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SIGKILL/restart qualification FAILED: " << error.what() << '\n';
        return 1;
    }
}
