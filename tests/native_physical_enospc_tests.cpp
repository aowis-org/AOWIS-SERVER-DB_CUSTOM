#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <linux/magic.h>
#include <stdexcept>
#include <string>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
namespace fs = std::filesystem;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::Repository;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Key key(std::uint8_t last) {
    Key result{};
    result.back() = last;
    return result;
}

std::uint64_t available(const fs::path& directory) {
    struct statvfs status{};
    check(::statvfs(directory.c_str(), &status) == 0, "statvfs failed");
    return static_cast<std::uint64_t>(status.f_bavail) * status.f_frsize;
}

// This file reserves REAL tmpfs pages. ftruncate alone would create a sparse
// extent and would not exercise the kernel's ENOSPC path.
void exhaust(const fs::path& root) {
    const fs::path filler = root / "space-reservation";
    check(!fs::exists(filler), "old filler still present");
    const int fd = ::open(filler.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    check(fd >= 0, "cannot create filler file");
    const std::uint64_t freeBytes = available(root);
    check(freeBytes > 6U * 1024U * 1024U, "insufficient room to create the exhaustion fixture");
    // Leave space for directory entries and recovery metadata; much less than
    // the 4 MiB object/bundle used by each rejected operation.
    const off_t allocate = static_cast<off_t>(freeBytes - 512U * 1024U);
    const int status = ::posix_fallocate(fd, 0, allocate);
    const int closed = ::close(fd);
    check(status == 0 && closed == 0, "cannot reserve tmpfs space: " + std::to_string(status));
    check(available(root) < 2U * 1024U * 1024U,
          "filesystem not nearly full; physical ENOSPC test would be invalid");

    // Independently demonstrate that the KERNEL reports ENOSPC on this mount.
    const fs::path probe = root / "kernel-enospc-probe";
    const int probeFd = ::open(probe.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    check(probeFd >= 0, "cannot open ENOSPC probe");
    const Bytes probeBytes(3U * 1024U * 1024U, 0x39);
    const ssize_t written = ::write(probeFd, probeBytes.data(), probeBytes.size());
    const int writeErrno = errno;
    (void)::close(probeFd);
    fs::remove(probe);
    // Linux tmpfs may return a short write on the first write, then ENOSPC.
    check((written < 0 && writeErrno == ENOSPC) ||
          (written >= 0 && static_cast<std::size_t>(written) < probeBytes.size()),
          "kernel failed to enforce the small mounted filesystem limit");
}

void release(const fs::path& root) {
    check(fs::remove(root / "space-reservation"), "failed to release filesystem space");
    check(available(root) > 6U * 1024U * 1024U, "filesystem space not reclaimed");
}

template<class Operation>
void expectsDiskFull(const char* label, Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const StorageError& error) {
        check(error.kind() == StorageErrorKind::ResourceExhausted,
              std::string(label) + " returned wrong error kind: " + error.what());
        rejected = true;
    }
    check(rejected, std::string(label) + " unexpectedly succeeded on a full filesystem");
}

void run(const fs::path& root) {
    const fs::path originPath = root / "origin";
    const fs::path destinationPath = root / "destination";
    const fs::path writePath = root / "write";
    const fs::path initialBundle = root / "initial.bundle";
    const fs::path deltaBundle = root / "delta.bundle";
    const fs::path failedBundle = root / "failed.bundle";
    const fs::path completedBundle = root / "completed.bundle";
    const Bytes payload(4U * 1024U * 1024U, 0x6b);
    RevisionId original = RevisionId::generate();
    RevisionId originalWrite = RevisionId::generate();
    RevisionId latest = RevisionId::generate();
    {
        ProjectRepository source(originPath);
        source.createBranch("main");
        (void)source.commit("main", {Change{key(1), Bytes(1024, 0x11)}});
        original = source.head("main");
        source.exportBundle("main", initialBundle);
        (void)source.commit("main", {Change{key(2), payload}});
        latest = source.head("main");
        source.exportIncrementalBundle("main", original, deltaBundle);
    }
    {
        ProjectRepository destination(destinationPath);
        check(destination.importBundle(initialBundle, "main").head == original,
              "initial bundle did not publish expected base");
        destination.verifyRepository();
    }
    {
        ProjectRepository writable(writePath);
        writable.createBranch("main");
        (void)writable.commit("main", {Change{key(1), Bytes(512, 0x12)}});
        originalWrite = writable.head("main");
    }
    check(fs::file_size(deltaBundle) > 4U * 1024U * 1024U,
          "incremental bundle too small for physical ENOSPC case");

    exhaust(root);
    expectsDiskFull("commit", [&] {
        ProjectRepository writable(writePath);
        (void)writable.commit("main", {Change{key(3), payload}});
    });
    release(root);
    {
        ProjectRepository writable(writePath);
        check(writable.head("main") == originalWrite, "disk-full commit published a branch");
        check(!writable.get("main", key(3)).has_value(), "uncommitted value was exposed");
        writable.verifyRepository();
        (void)writable.commit("main", {Change{key(3), payload}});
        check(writable.get("main", key(3)).value() == payload, "retry after ENOSPC failed");
        writable.verifyRepository();
    }
    std::cout << "Physical ENOSPC commit recovery: passed\n";

    exhaust(root);
    expectsDiskFull("named export", [&] {
        ProjectRepository origin(originPath);
        origin.exportBundle("main", failedBundle);
    });
    check(!fs::exists(failedBundle), "failed export exposed a partial bundle filename");
    release(root);
    {
        ProjectRepository origin(originPath);
        check(origin.head("main") == latest, "failed export changed source head");
        origin.verifyRepository();
        origin.exportBundle("main", completedBundle);
    }
    check(fs::file_size(completedBundle) > 4U * 1024U * 1024U,
          "retry did not export complete bundle");
    std::cout << "Physical ENOSPC atomic named export: passed\n";

    exhaust(root);
    expectsDiskFull("incremental import", [&] {
        ProjectRepository destination(destinationPath);
        (void)destination.importBundle(deltaBundle, "main");
    });
    release(root);
    {
        ProjectRepository destination(destinationPath);
        check(destination.head("main") == original,
              "disk-full import advanced destination branch");
        check(!destination.get("main", key(2)).has_value(),
              "disk-full import exposed uncommitted data");
        destination.verifyRepository();
        check(destination.importBundle(deltaBundle, "main").head == latest,
              "incremental import retry published wrong revision");
        check(destination.get("main", key(2)).value() == payload,
              "incremental import retry lost payload");
        destination.verifyRepository();
    }
    std::cout << "Physical ENOSPC incremental import recovery: passed\n";
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected isolated mounted filesystem root argument");
        const fs::path root(argv[1]);
        check(fs::exists(root) && fs::is_directory(root), "invalid test mount");
        struct statfs mounted{};
        struct stat rootInfo{};
        struct stat parentInfo{};
        check(::statfs(root.c_str(), &mounted) == 0 && mounted.f_type == TMPFS_MAGIC,
              "refusing to run on any filesystem other than tmpfs");
        const std::uint64_t totalBytes =
            static_cast<std::uint64_t>(mounted.f_blocks) * mounted.f_bsize;
        check(totalBytes >= 24U * 1024U * 1024U &&
              totalBytes <= 40U * 1024U * 1024U,
              "refusing to run outside a private 32 MiB tmpfs");
        check(::stat(root.c_str(), &rootInfo) == 0 &&
              ::stat(root.parent_path().c_str(), &parentInfo) == 0 &&
              rootInfo.st_dev != parentInfo.st_dev,
              "refusing to run unless root is a separate mounted filesystem");
        run(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Physical ENOSPC regression FAILED: " << error.what() << '\n';
        return 1;
    }
}
