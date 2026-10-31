// Review finding F2: every failure is classified by StorageErrorKind at its
// source. Callers (HTTP, CLI) must never inspect message text. Linked against
// the fault-test engine build for deterministic ENOSPC and compaction faults.
#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
using aowis::dvmdb::BundleLimits;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::Repository;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-dvmdb-error-kinds-" + std::string(label) + "-" + std::to_string(::getpid()) +
             "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++counter));
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

Bytes value(std::uint8_t number) {
    return Bytes(256, number);
}

void expectKind(StorageErrorKind expected, const std::string& label,
                const std::function<void()>& action) {
    try {
        action();
    } catch (const StorageError& error) {
        expect(error.kind() == expected, label + ": expected " +
               StorageError::kindName(expected) + ", got " +
               StorageError::kindName(error.kind()) + " (" + error.what() + ")");
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(label + ": untyped exception: " + error.what());
    }
    throw std::runtime_error(label + ": no exception");
}

// Offset of the pack record (AOBJ\1 | SHA-256 | size | payload) of `hash`.
std::uint64_t packRecordOffset(const std::filesystem::path& pack, const Hash& hash) {
    std::ifstream stream(pack, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::string header{'A', 'O', 'B', 'J', '\x01'};
    header.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    const std::size_t position = bytes.find(header);
    expect(position != std::string::npos, "object record not found in pack");
    return static_cast<std::uint64_t>(position);
}

void flipByte(const std::filesystem::path& file, std::uint64_t offset) {
    std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
    stream.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    stream.read(&byte, 1);
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.write(&byte, 1);
    expect(stream.good(), "cannot flip byte in " + file.string());
}

void appendU64(Bytes& bytes, std::uint64_t number) {
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>(number >> (56U - 8U * i)));
}

void testErrnoMapping() {
    expect(aowis::dvmdb::storageErrorKindForErrno(ENOSPC) == StorageErrorKind::ResourceExhausted &&
           aowis::dvmdb::storageErrorKindForErrno(EDQUOT) == StorageErrorKind::ResourceExhausted &&
           aowis::dvmdb::storageErrorKindForErrno(EIO) == StorageErrorKind::Io &&
           aowis::dvmdb::storageErrorKindForErrno(EACCES) == StorageErrorKind::Io,
           "errno classification");
}

void testRequestKinds() {
    TestDirectory directory("requests");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    const RevisionId firstId = repository.commitModel("main", {
        ModelChange{key(1), ModelRecord{1, {}, value(1)}},
        ModelChange{key(2), ModelRecord{1, {key(1)}, value(2)}}});

    // InvalidInput
    expectKind(StorageErrorKind::InvalidInput, "invalid branch name",
               [&] { repository.createBranch("../escape", "main"); });
    expectKind(StorageErrorKind::InvalidInput, "malformed hex digest",
               [&] { (void)aowis::dvmdb::fromHex(std::string(64, 'g')); });
    expectKind(StorageErrorKind::InvalidInput, "duplicate UUID in one commit", [&] {
        repository.commitModel("main", {ModelChange{key(3), ModelRecord{1, {}, value(3)}},
                                        ModelChange{key(3), ModelRecord{1, {}, value(4)}}});
    });
    expectKind(StorageErrorKind::InvalidInput, "reference to a missing model", [&] {
        repository.commitModel("main", {ModelChange{key(4), ModelRecord{1, {key(99)}, value(4)}}});
    });
    expectKind(StorageErrorKind::InvalidInput, "delete of a referenced model",
               [&] { repository.commitModel("main", {ModelChange{key(1), std::nullopt}}); });
    expectKind(StorageErrorKind::InvalidInput, "history cursor from elsewhere",
               [&] { (void)repository.history("main", 10, RevisionId::generate()); });
    expectKind(StorageErrorKind::InvalidInput, "raw commit to a model-indexed branch",
               [&] { repository.commit("main", {Change{key(5), value(5)}}); });

    // NotFound, including UUIDs that identify something other than a revision
    const RevisionId unknown = RevisionId::generate();
    const RevisionId branchUuid(repository.branchId("main")->uuid());
    expectKind(StorageErrorKind::NotFound, "unknown branch", [&] { (void)repository.head("missing"); });
    expectKind(StorageErrorKind::NotFound, "unknown revision",
               [&] { (void)repository.getModelAt(unknown, key(1)); });
    expectKind(StorageErrorKind::NotFound, "branch UUID used as revision",
               [&] { (void)repository.getModelAt(branchUuid, key(1)); });
    expectKind(StorageErrorKind::NotFound, "diff with unknown revision",
               [&] { (void)repository.diff(unknown, firstId); });
    expectKind(StorageErrorKind::NotFound, "incremental export from an unknown base", [&] {
        repository.exportIncrementalBundle("main", unknown, directory.path / "never.bundle");
    });

    // Conflict
    expectKind(StorageErrorKind::Conflict, "existing branch",
               [&] { repository.createBranch("main", "main"); });
    (void)repository.commitModel("main", {ModelChange{key(6), ModelRecord{1, {}, value(6)}}});
    expectKind(StorageErrorKind::Conflict, "stale expected head", [&] {
        repository.commitModel("main", {ModelChange{key(7), ModelRecord{1, {}, value(7)}}},
                               firstId);
    });
    repository.createBranchAt("side", "main", firstId);
    const RevisionId sideOnly = repository.commitModel("side", {
        ModelChange{key(8), ModelRecord{1, {}, value(8)}}});
    expectKind(StorageErrorKind::Conflict, "incremental export from a non-ancestor", [&] {
        repository.exportIncrementalBundle("main", sideOnly, directory.path / "never.bundle");
    });

    // LimitExceeded
    expectKind(StorageErrorKind::LimitExceeded, "bundle object-count limit", [&] {
        repository.exportBundle("main", directory.path / "limited.bundle", BundleLimits{0, 1, 0});
    });
}

// A hash-valid UUIDv7 bundle (AOWREV21) whose HEAD digest names a value
// object, not a revision record: the importer reports InvalidInput.
void writeForgedBundle(const std::filesystem::path& file, const ProjectId& owner) {
    const Bytes payload{'V', 'x'};
    const Hash hash = aowis::dvmdb::sha256(payload);
    const UuidV7 project = owner.uuid();
    const UuidV7 branch = aowis::dvmdb::BranchId::generate().uuid();
    const UuidV7 revision = RevisionId::generate().uuid();
    Bytes bundle{'A', 'O', 'W', 'R', 'E', 'V', '2', '1'};
    bundle.insert(bundle.end(), project.bytes.begin(), project.bytes.end());
    bundle.insert(bundle.end(), branch.bytes.begin(), branch.bytes.end());
    bundle.insert(bundle.end(), revision.bytes.begin(), revision.bytes.end());
    bundle.insert(bundle.end(), hash.begin(), hash.end());
    appendU64(bundle, 1);
    bundle.insert(bundle.end(), hash.begin(), hash.end());
    appendU64(bundle, payload.size());
    bundle.insert(bundle.end(), payload.begin(), payload.end());
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bundle.data()),
                 static_cast<std::streamsize>(bundle.size()));
}

void testImportKinds() {
    TestDirectory directory("import");
    ProjectRepository source(directory.path / "source");
    source.createBranch("main");
    const RevisionId base = source.commitModel("main", {
        ModelChange{key(1), ModelRecord{1, {}, value(1)}}});
    source.exportBundle("main", directory.path / "base.bundle");
    source.commitModel("main", {ModelChange{key(2), ModelRecord{1, {key(1)}, value(2)}}});
    source.exportIncrementalBundle("main", base, directory.path / "delta.bundle");

    writeForgedBundle(directory.path / "forged.bundle", source.project());
    ProjectRepository fresh(directory.path / "fresh", source.project());
    expectKind(StorageErrorKind::InvalidInput, "structurally invalid bundle",
               [&] { (void)fresh.importBundle(directory.path / "forged.bundle", "main"); });
    expectKind(StorageErrorKind::Conflict, "incremental bundle without local base",
               [&] { (void)fresh.importBundle(directory.path / "delta.bundle", "main"); });

    // Divergent local history is a conflict.
    {
        ProjectRepository diverged(directory.path / "diverged");
        diverged.importBundle(directory.path / "base.bundle", "main");
        diverged.commitModel("main", {ModelChange{key(9), ModelRecord{1, {}, value(9)}}});
        source.exportBundle("main", directory.path / "full.bundle");
        expectKind(StorageErrorKind::Conflict, "divergent import",
                   [&] { (void)diverged.importBundle(directory.path / "full.bundle", "main"); });
    }

    // Damage in the LOCAL repository found while verifying an incremental
    // import stays Corruption; it is not the bundle's fault.
    {
        const std::filesystem::path path = directory.path / "local-damage";
        {
            ProjectRepository target(path);
            target.importBundle(directory.path / "base.bundle", "main");
        }
        // Incremental verification reads only the boundary of the local
        // graph; the base revision is always part of it.
        flipByte(path / "objects.pack",
                 packRecordOffset(path / "objects.pack", source.revision(base).recordHash) + 45U + 1U);
        ProjectRepository target(path);
        expectKind(StorageErrorKind::Corruption, "local damage during incremental import",
                   [&] { (void)target.importBundle(directory.path / "delta.bundle", "main"); });
    }

    // ENOSPC during import (deterministic fault) is ResourceExhausted.
    {
        ProjectRepository target(directory.path / "enospc");
        target.importBundle(directory.path / "base.bundle", "main");
        ::setenv("AOWIS_DVMDB_TEST_FAULT", "import.after.sync", 1);
        ::setenv("AOWIS_DVMDB_TEST_ACTION", "error", 1);
        try {
            expectKind(StorageErrorKind::ResourceExhausted, "ENOSPC during import",
                       [&] { (void)target.importBundle(directory.path / "delta.bundle", "main"); });
        } catch (...) {
            ::unsetenv("AOWIS_DVMDB_TEST_FAULT");
            ::unsetenv("AOWIS_DVMDB_TEST_ACTION");
            throw;
        }
        ::unsetenv("AOWIS_DVMDB_TEST_FAULT");
        ::unsetenv("AOWIS_DVMDB_TEST_ACTION");
    }
}

void testStoredDataKinds() {
    TestDirectory directory("stored");
    const std::filesystem::path path = directory.path / "repo";
    RevisionId first = RevisionId::generate();
    RevisionId head = RevisionId::generate();
    Hash firstRecord{};
    {
        ProjectRepository repository(path);
        repository.createBranch("main");
        first = repository.commit("main", {Change{key(1), value(1)}});
        head = repository.commit("main", {Change{key(2), value(2)}});
        firstRecord = repository.revision(first).recordHash;
    }
    flipByte(path / "objects.pack", packRecordOffset(path / "objects.pack", firstRecord) + 45U + 1U);
    ProjectRepository repository(path);
    expect(repository.revision(head).parents.front() == first, "unexpected history fixture");
    expectKind(StorageErrorKind::Corruption, "checksum mismatch of a stored revision",
               [&] { (void)repository.revision(first); });
    expectKind(StorageErrorKind::Corruption, "full verification of damaged history",
               [&] { (void)repository.verifyRepository(); });
}

void testUnavailableAfterInterruptedCompaction() {
    TestDirectory directory("compaction");
    ProjectRepository repository(directory.path / "repo");
    repository.createBranch("main");
    repository.commit("main", {Change{key(1), value(1)}});
    ::setenv("AOWIS_DVMDB_TEST_FAULT", "compact.pack.after.pointer", 1);
    ::setenv("AOWIS_DVMDB_TEST_ACTION", "error", 1);
    bool failed = false;
    try { (void)repository.compact(); }
    catch (const std::exception&) { failed = true; }
    ::unsetenv("AOWIS_DVMDB_TEST_FAULT");
    ::unsetenv("AOWIS_DVMDB_TEST_ACTION");
    expect(failed, "compaction fault not reached");
    expectKind(StorageErrorKind::Unavailable, "handle after interrupted compaction",
               [&] { repository.commit("main", {Change{key(2), value(2)}}); });
}

} // namespace

int main() {
    try {
        testErrnoMapping();
        testRequestKinds();
        testImportKinds();
        testStoredDataKinds();
        testUnavailableAfterInterruptedCompaction();
        std::cout << "Storage error classification tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Storage error classification test failed: " << error.what() << '\n';
        return 1;
    }
}
