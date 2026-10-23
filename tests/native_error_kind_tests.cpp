// Review finding F2: every failure is classified by StorageErrorKind at its
// source. Callers (HTTP, CLI) must never inspect message text. Linked against
// the fault-test engine build for deterministic ENOSPC and compaction faults.
#include "aowis/native/native_store.h"

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using aowis::native::BundleLimits;
using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelChange;
using aowis::native::ModelRecord;
using aowis::native::Repository;
using aowis::native::Revision;
using aowis::native::StorageError;
using aowis::native::StorageErrorKind;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-native-error-kinds-" + std::string(label) + "-" + std::to_string(::getpid()) +
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
    expect(aowis::native::storageErrorKindForErrno(ENOSPC) == StorageErrorKind::ResourceExhausted &&
           aowis::native::storageErrorKindForErrno(EDQUOT) == StorageErrorKind::ResourceExhausted &&
           aowis::native::storageErrorKindForErrno(EIO) == StorageErrorKind::Io &&
           aowis::native::storageErrorKindForErrno(EACCES) == StorageErrorKind::Io,
           "errno classification");
}

void testRequestKinds() {
    TestDirectory directory("requests");
    Repository repository(directory.path / "repo");
    repository.createBranch("main");
    const Revision first = repository.commitModel("main", {
        ModelChange{key(1), ModelRecord{1, {}, value(1)}},
        ModelChange{key(2), ModelRecord{1, {key(1)}, value(2)}}});

    // InvalidInput
    expectKind(StorageErrorKind::InvalidInput, "invalid branch name",
               [&] { repository.createBranch("../escape"); });
    expectKind(StorageErrorKind::InvalidInput, "malformed revision hex",
               [&] { (void)aowis::native::fromHex(std::string(64, 'g')); });
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
               [&] { (void)repository.history("main", 10, aowis::native::sha256(value(9))); });
    expectKind(StorageErrorKind::InvalidInput, "raw commit to a model-indexed branch",
               [&] { repository.commit("main", {Change{key(5), value(5)}}); });

    // NotFound, including identities of objects that are not revisions (F10b)
    const Hash unknown = aowis::native::sha256(Bytes{1, 2, 3});
    expectKind(StorageErrorKind::NotFound, "unknown branch", [&] { (void)repository.head("missing"); });
    expectKind(StorageErrorKind::NotFound, "unknown revision", [&] { (void)repository.revision(unknown); });
    expectKind(StorageErrorKind::NotFound, "tree hash used as revision",
               [&] { (void)repository.revision(first.root); });
    expectKind(StorageErrorKind::NotFound, "reference-index tree hash used as revision", [&] {
        (void)repository.getModelAtRevision(first.referencesRoot, key(1));
    });
    expectKind(StorageErrorKind::NotFound, "diff with unknown revision",
               [&] { (void)repository.diffRevisions(unknown, first.id); });

    // Conflict
    expectKind(StorageErrorKind::Conflict, "existing branch", [&] { repository.createBranch("main"); });
    expectKind(StorageErrorKind::Conflict, "stale expected head", [&] {
        repository.commitModel("main", {ModelChange{key(6), ModelRecord{1, {}, value(6)}}},
                               first.parents.front());
    });
    expectKind(StorageErrorKind::Conflict, "incremental export from a non-ancestor", [&] {
        repository.exportIncrementalBranchBundle("main", unknown, directory.path / "never.bundle");
    });

    // LimitExceeded
    expectKind(StorageErrorKind::LimitExceeded, "bundle object-count limit", [&] {
        repository.exportBranchBundle("main", directory.path / "limited.bundle", BundleLimits{0, 1, 0});
    });
}

void testImportKinds() {
    TestDirectory directory("import");
    Repository source(directory.path / "source");
    source.createBranch("main");
    const Hash base = source.commitModel("main", {
        ModelChange{key(1), ModelRecord{1, {}, value(1)}}}).id;
    source.exportBranchBundle("main", directory.path / "base.bundle");
    source.commitModel("main", {ModelChange{key(2), ModelRecord{1, {key(1)}, value(2)}}});
    source.exportIncrementalBranchBundle("main", base, directory.path / "delta.bundle");

    // A hash-valid bundle whose tip is a value object, not a revision: the
    // structural verifier reports Corruption, the importer InvalidInput.
    {
        const Bytes payload{'V', 'x'};
        const Hash hash = aowis::native::sha256(payload);
        Bytes bundle{'A', 'O', 'W', 'S', 'Y', 'N', 'C', '1'};
        bundle.insert(bundle.end(), hash.begin(), hash.end());
        appendU64(bundle, 1);
        bundle.insert(bundle.end(), hash.begin(), hash.end());
        appendU64(bundle, payload.size());
        bundle.insert(bundle.end(), payload.begin(), payload.end());
        std::ofstream output(directory.path / "forged.bundle", std::ios::binary);
        output.write(reinterpret_cast<const char*>(bundle.data()),
                     static_cast<std::streamsize>(bundle.size()));
    }
    Repository fresh(directory.path / "fresh");
    expectKind(StorageErrorKind::InvalidInput, "structurally invalid bundle",
               [&] { (void)fresh.importBranchBundle(directory.path / "forged.bundle", "main"); });
    expectKind(StorageErrorKind::Conflict, "incremental bundle without local base",
               [&] { (void)fresh.importBranchBundle(directory.path / "delta.bundle", "main"); });

    // Divergent local history is a conflict.
    {
        Repository diverged(directory.path / "diverged");
        diverged.importBranchBundle(directory.path / "base.bundle", "main");
        diverged.commitModel("main", {ModelChange{key(9), ModelRecord{1, {}, value(9)}}});
        source.exportBranchBundle("main", directory.path / "full.bundle");
        expectKind(StorageErrorKind::Conflict, "divergent import",
                   [&] { (void)diverged.importBranchBundle(directory.path / "full.bundle", "main"); });
    }

    // Damage in the LOCAL repository found while verifying an incremental
    // import stays Corruption; it is not the bundle's fault.
    {
        const std::filesystem::path path = directory.path / "local-damage";
        {
            Repository target(path);
            target.importBranchBundle(directory.path / "base.bundle", "main");
        }
        flipByte(path / "objects.pack", 45U + 1U); // body of the first imported object
        Repository target(path);
        expectKind(StorageErrorKind::Corruption, "local damage during incremental import",
                   [&] { (void)target.importBranchBundle(directory.path / "delta.bundle", "main"); });
    }

    // ENOSPC during import (deterministic fault) is ResourceExhausted.
    {
        Repository target(directory.path / "enospc");
        target.importBranchBundle(directory.path / "base.bundle", "main");
        ::setenv("AOWIS_NATIVE_TEST_FAULT", "import.after.sync", 1);
        ::setenv("AOWIS_NATIVE_TEST_ACTION", "error", 1);
        try {
            expectKind(StorageErrorKind::ResourceExhausted, "ENOSPC during import",
                       [&] { (void)target.importBranchBundle(directory.path / "delta.bundle", "main"); });
        } catch (...) {
            ::unsetenv("AOWIS_NATIVE_TEST_FAULT");
            ::unsetenv("AOWIS_NATIVE_TEST_ACTION");
            throw;
        }
        ::unsetenv("AOWIS_NATIVE_TEST_FAULT");
        ::unsetenv("AOWIS_NATIVE_TEST_ACTION");
    }
}

void testStoredDataKinds() {
    TestDirectory directory("stored");
    const std::filesystem::path path = directory.path / "repo";
    Hash head{};
    {
        Repository repository(path);
        repository.createBranch("main");
        head = repository.commit("main", {Change{key(1), value(1)}, Change{key(2), value(2)}}).id;
    }
    // The first record is the empty initial revision; its successors follow.
    flipByte(path / "objects.pack", 45U + 1U);
    Repository repository(path);
    const Hash initial = repository.revision(head).parents.front();
    expectKind(StorageErrorKind::Corruption, "checksum mismatch of a stored revision",
               [&] { (void)repository.revision(initial); });
    expectKind(StorageErrorKind::Corruption, "full verification of damaged history",
               [&] { (void)repository.verifyRepository(); });
}

void testUnavailableAfterInterruptedCompaction() {
    TestDirectory directory("compaction");
    Repository repository(directory.path / "repo");
    repository.createBranch("main");
    repository.commit("main", {Change{key(1), value(1)}});
    ::setenv("AOWIS_NATIVE_TEST_FAULT", "compact.pack.after.pointer", 1);
    ::setenv("AOWIS_NATIVE_TEST_ACTION", "error", 1);
    bool failed = false;
    try { (void)repository.compact(); }
    catch (const std::exception&) { failed = true; }
    ::unsetenv("AOWIS_NATIVE_TEST_FAULT");
    ::unsetenv("AOWIS_NATIVE_TEST_ACTION");
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
