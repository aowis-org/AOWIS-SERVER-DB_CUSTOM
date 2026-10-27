#include "aowis/dvmdb/revision_resolver_v2.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace aowis::dvmdb;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Fn>
void expectError(Fn&& fn, StorageErrorKind kind, const char* message) {
    try { fn(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        std::cerr << message << ": actual error kind " << StorageError::kindName(error.kind()) << '\n';
        throw;
    }
    throw std::runtime_error(message);
}

struct Sandbox final {
    fs::path path;
    Sandbox() {
        this->path = fs::temp_directory_path() / ("aowis-v2-resolver-" + UuidV7::generate().toString());
        fs::create_directories(this->path);
    }
    ~Sandbox() { std::error_code ec; fs::remove_all(this->path, ec); }
};

struct ObjectPack final {
    std::map<Hash, Bytes> objects;
    void add(const Bytes& bytes) { this->objects.emplace(sha256(bytes), bytes); }
    std::optional<Bytes> read(const Hash& hash) const {
        const std::map<Hash, Bytes>::const_iterator i = this->objects.find(hash);
        if (i == this->objects.end()) return std::nullopt;
        return i->second;
    }
    RevisionObjectReaderV2 reader() const {
        return [this](const Hash& hash) { return read(hash); };
    }
};

RevisionRecordV2 makeRecord(ProjectId project, RevisionId revision) {
    RevisionRecordV2 record{revision, project};
    record.root.fill(0x12);
    return record;
}

void testPersistedGraphAndProjectBinding() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    const RevisionId rootId = RevisionId::generate();
    const RevisionId childId = RevisionId::generate();
    const RevisionId mergeId = RevisionId::generate();
    const RevisionId siblingId = RevisionId::generate();
    ObjectPack pack;
    const RevisionRecordV2 root = makeRecord(project, rootId);
    const Bytes rootBytes = encodeRevisionRecordV2(root);
    pack.add(rootBytes);
    RevisionRecordV2 child = makeRecord(project, childId);
    child.parents.push_back({rootId, sha256(rootBytes)});
    const Bytes childBytes = encodeRevisionRecordV2(child);
    pack.add(childBytes);
    RevisionRecordV2 sibling = makeRecord(project, siblingId);
    sibling.parents.push_back({rootId, sha256(rootBytes)});
    const Bytes siblingBytes = encodeRevisionRecordV2(sibling);
    pack.add(siblingBytes);
    RevisionRecordV2 merge = makeRecord(project, mergeId);
    merge.parents.push_back({childId, sha256(childBytes)});
    merge.parents.push_back({siblingId, sha256(siblingBytes)});
    const Bytes mergeBytes = encodeRevisionRecordV2(merge);
    pack.add(mergeBytes);

    {
        RevisionResolverV2 index(sandbox.path, project);
        check(!index.resolve(rootId, pack.reader()).has_value(), "unknown revision resolved");
        check(index.publish(rootBytes, pack.reader()) == sha256(rootBytes), "root digest incorrect");
        check(index.publish(rootBytes, pack.reader()) == sha256(rootBytes), "idempotent root publication failed");
        check(index.publish(childBytes, pack.reader()) == sha256(childBytes), "child digest incorrect");
        check(index.publish(siblingBytes, pack.reader()) == sha256(siblingBytes), "sibling digest incorrect");
        check(index.publish(mergeBytes, pack.reader()) == sha256(mergeBytes), "merge digest incorrect");
        const std::optional<ResolvedRevisionV2> resolved = index.resolve(mergeId, pack.reader());
        check(resolved && resolved->record.parents.size() == 2 &&
              resolved->recordHash == sha256(mergeBytes), "merge ancestry not resolved");
        expectError([&] { (void)index.resolve(mergeId, pack.reader(), 2); },
                    StorageErrorKind::LimitExceeded, "ancestry limit not enforced");
        RevisionRecordV2 malicious = root;
        malicious.root[0] = 0x15;
        const Bytes otherBytes = encodeRevisionRecordV2(malicious);
        pack.add(otherBytes);
        expectError([&] { (void)index.publish(otherBytes, pack.reader()); },
                    StorageErrorKind::Conflict, "revision UUID was rebound");
        RevisionRecordV2 foreign = makeRecord(ProjectId::generate(), RevisionId::generate());
        const Bytes foreignBytes = encodeRevisionRecordV2(foreign);
        pack.add(foreignBytes);
        expectError([&] { (void)index.publish(foreignBytes, pack.reader()); },
                    StorageErrorKind::InvalidInput, "foreign project revision published");
    }
    {
        RevisionResolverV2 reopened(sandbox.path, project);
        check(reopened.resolve(mergeId, pack.reader()).has_value(), "bindings lost on restart");
        check(fs::exists(sandbox.path / "revision-v2" / "ids" / mergeId.toString()),
              "UUIDv7 file not created");
        check(!fs::exists(sandbox.path / "revision-v2" / "ids" / "main"),
              "user-supplied branch name used as filename");
    }
    expectError([&] { RevisionResolverV2 wrong(sandbox.path, ProjectId::generate()); },
                StorageErrorKind::Conflict, "repository project identity was not pinned");
    {
        const fs::path manifest = sandbox.path / "revision-v2" / "project";
        const fs::path missing = sandbox.path / "revision-v2" / "project.missing";
        fs::rename(manifest, missing);
        expectError([&] { RevisionResolverV2 wrong(sandbox.path, ProjectId::generate()); },
                    StorageErrorKind::Corruption, "missing project manifest was silently reinitialized");
        check(!fs::exists(manifest), "damaged project manifest was recreated");
        fs::rename(missing, manifest);
    }

    // Verified resolution must fail closed when immutable content is missing or
    // even just one SHA-addressed parent has been altered.
    pack.objects.erase(sha256(siblingBytes));
    RevisionResolverV2 reopened(sandbox.path, project);
    expectError([&] { (void)reopened.resolve(mergeId, pack.reader()); },
                StorageErrorKind::Corruption, "missing merge ancestor accepted");
    pack.objects[sha256(siblingBytes)] = rootBytes;
    expectError([&] { (void)reopened.resolve(mergeId, pack.reader()); },
                StorageErrorKind::Corruption, "altered merge ancestor accepted");
}

void testMissingStoreAndInvalidParents() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    RevisionResolverV2 index(sandbox.path, project);
    ObjectPack pack;
    const RevisionId rootId = RevisionId::generate();
    const RevisionRecordV2 root = makeRecord(project, rootId);
    const Bytes rootBytes = encodeRevisionRecordV2(root);
    expectError([&] { (void)index.publish(rootBytes, pack.reader()); },
                StorageErrorKind::Corruption, "unpersisted revision published");
    check(!index.resolve(rootId, pack.reader()).has_value(), "published unpersisted revision");
    expectError([&] { (void)index.resolve(rootId, {}, 1); },
                StorageErrorKind::InvalidInput, "missing reader was not rejected");
    expectError([&] { (void)index.resolve(rootId, pack.reader(), 0); },
                StorageErrorKind::InvalidInput, "zero verification budget was not rejected");
    pack.add(rootBytes);
    RevisionRecordV2 child = makeRecord(project, RevisionId::generate());
    child.parents.push_back({rootId, sha256(rootBytes)});
    const Bytes childBytes = encodeRevisionRecordV2(child);
    pack.add(childBytes);
    expectError([&] { (void)index.publish(childBytes, pack.reader()); },
                StorageErrorKind::Corruption, "unmapped parent accepted");
    (void)index.publish(rootBytes, pack.reader());
    (void)index.publish(childBytes, pack.reader());
    RevisionRecordV2 wrongParent = makeRecord(project, RevisionId::generate());
    Hash bogus{};
    bogus[0] = 1;
    wrongParent.parents.push_back({rootId, bogus});
    const Bytes wrongBytes = encodeRevisionRecordV2(wrongParent);
    pack.add(wrongBytes);
    expectError([&] { (void)index.publish(wrongBytes, pack.reader()); },
                StorageErrorKind::Corruption, "incorrect parent SHA accepted");
}

void testCorruptAndUnsafeFiles() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    const RevisionId revisionId = RevisionId::generate();
    ObjectPack pack;
    const Bytes bytes = encodeRevisionRecordV2(makeRecord(project, revisionId));
    pack.add(bytes);
    RevisionResolverV2 index(sandbox.path, project);
    const fs::path ids = sandbox.path / "revision-v2" / "ids";
    const fs::path binding = ids / revisionId.toString();
    {
        std::ofstream invalid(binding);
        invalid << "bad";
    }
    expectError([&] { (void)index.resolve(revisionId, pack.reader()); },
                StorageErrorKind::Corruption, "truncated mapping accepted");
    fs::remove(binding);
    fs::create_symlink("nonexistent", binding);
    expectError([&] { (void)index.resolve(revisionId, pack.reader()); },
                StorageErrorKind::Corruption, "symlink mapping accepted");
    fs::remove(binding);
    {
        std::ofstream staged(ids / (".stage-" + RevisionId::generate().toString()));
        staged << "partial";
    }
    check(!index.resolve(revisionId, pack.reader()), "uncommitted staging entry became visible");
    (void)index.publish(bytes, pack.reader());
    check(index.resolve(revisionId, pack.reader()).has_value(), "private staging residue prevented publish");
    {
        std::fstream altered(binding, std::ios::binary | std::ios::in | std::ios::out);
        altered.seekp(20);
        altered.put('x');
    }
    expectError([&] { (void)index.resolve(revisionId, pack.reader()); },
                StorageErrorKind::Corruption, "altered UUID/project binding accepted");
}

void testConcurrentPublish() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    const RevisionId revisionId = RevisionId::generate();
    ObjectPack pack;
    const Bytes bytes = encodeRevisionRecordV2(makeRecord(project, revisionId));
    pack.add(bytes);
    RevisionResolverV2 first(sandbox.path, project);
    RevisionResolverV2 second(sandbox.path, project);
    std::vector<std::exception_ptr> failures(2);
    std::thread a([&] { try { (void)first.publish(bytes, pack.reader()); }
                       catch (...) { failures[0] = std::current_exception(); } });
    std::thread b([&] { try { (void)second.publish(bytes, pack.reader()); }
                       catch (...) { failures[1] = std::current_exception(); } });
    a.join();
    b.join();
    for (const std::exception_ptr& e : failures) if (e) std::rethrow_exception(e);
    check(first.resolve(revisionId, pack.reader()).has_value(), "parallel publication failed");
}

} // namespace

int main() {
    try {
        testPersistedGraphAndProjectBinding();
        testMissingStoreAndInvalidParents();
        testCorruptAndUnsafeFiles();
        testConcurrentPublish();
        std::cout << "UUIDv7 revision resolver v2 persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 revision resolver v2 FAILED: " << error.what() << '\n';
        return 1;
    }
}
