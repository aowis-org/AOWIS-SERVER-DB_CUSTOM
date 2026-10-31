#include "aowis/dvmdb/revision_record.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Hash;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::RevisionParent;
using aowis::dvmdb::RevisionRecord;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;
using aowis::dvmdb::decodeRevisionRecord;
using aowis::dvmdb::encodeRevisionRecord;
using aowis::dvmdb::hashRevisionRecord;
using aowis::dvmdb::sha256;
using aowis::dvmdb::toHex;
using aowis::dvmdb::verifyRevisionAncestry;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RevisionId revisionId(std::string_view text) {
    const std::optional<RevisionId> parsed = RevisionId::parse(text);
    check(parsed.has_value(), "invalid fixture revision UUID");
    return *parsed;
}
ProjectId projectId(std::string_view text) {
    const std::optional<ProjectId> parsed = ProjectId::parse(text);
    check(parsed.has_value(), "invalid fixture project UUID");
    return *parsed;
}

RevisionRecord makeRecord(std::string_view id, std::string_view project,
                            bool indexed = false) {
    RevisionRecord record{revisionId(id), projectId(project)};
    record.root.fill(0x22);
    record.modelIndexed = indexed;
    if (indexed) record.referencesRoot.fill(0x33);
    return record;
}

void expectFailure(const std::function<void()>& call, StorageErrorKind kind, const char* caseName) {
    try { call(); }
    catch (const StorageError& error) {
        check(error.kind() == kind, caseName);
        return;
    }
    throw std::runtime_error(caseName);
}

void testGoldenVectors() {
    RevisionRecord root = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                       "018f84c0-0000-7000-8000-000000000002", true);
    const Bytes rootBytes = encodeRevisionRecord(root);
    check(rootBytes.size() == 100 && rootBytes[0] == 'T' && rootBytes[1] == 2,
          "golden root record format incorrect");
    check(toHex(sha256(rootBytes)) ==
          "8b1a70f56b56a30edf970a7c7bd0e5f3b89825249e1d070378cfbc2b75c61d7c",
          "root does not match frozen SHA-256 vector");
    const RevisionRecord decoded = decodeRevisionRecord(rootBytes);
    check(encodeRevisionRecord(decoded) == rootBytes && decoded.project == root.project,
          "root decode did not preserve canonical bytes");

    RevisionRecord child = makeRecord("018f84c0-0000-7000-8000-000000000003",
                                        "018f84c0-0000-7000-8000-000000000002", true);
    child.parents.push_back({root.id, sha256(rootBytes)});
    check(toHex(hashRevisionRecord(child)) ==
          "fddb8eb27f2f6193b1dcefe185b47b604e96f6814620ff916ab623273689dc6a",
          "child does not match frozen SHA-256 vector");
    const Bytes childBytes = encodeRevisionRecord(child);
    check(childBytes.size() == 148 && encodeRevisionRecord(decodeRevisionRecord(childBytes)) == childBytes,
          "child was not canonically decoded");

    const std::map<std::string, Bytes> records{{root.id.toString(), rootBytes}};
    const auto lookup = [&](const RevisionId& id) -> std::optional<Bytes> {
        const auto iter = records.find(id.toString());
        if (iter == records.end()) return std::nullopt;
        return iter->second;
    };
    check(verifyRevisionAncestry(childBytes, lookup) == sha256(childBytes),
          "verified two-record DAG produced wrong tip digest");
    expectFailure([&]{ (void)verifyRevisionAncestry(childBytes, lookup, 1); },
                  StorageErrorKind::LimitExceeded, "ancestry traversal budget was ignored");
}

void testMalformedBytes() {
    RevisionRecord record = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                         "018f84c0-0000-7000-8000-000000000002", true);
    const Bytes canonical = encodeRevisionRecord(record);
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        const Bytes shortRecord(canonical.begin(), canonical.begin() + static_cast<std::ptrdiff_t>(i));
        expectFailure([&]{ (void)decodeRevisionRecord(shortRecord); },
                      StorageErrorKind::Corruption, "truncated record was accepted");
    }
    const auto mutate = [&](std::size_t offset, std::uint8_t newValue) {
        Bytes corrupted = canonical;
        corrupted[offset] = newValue;
        expectFailure([&]{ (void)decodeRevisionRecord(corrupted); },
                      StorageErrorKind::Corruption, "invalid field was accepted");
    };
    mutate(0, 'S'); mutate(1, 3); mutate(8, 0x40); mutate(24, 0x40);
    mutate(10, 0x00); mutate(26, 0x00); mutate(98, 2); mutate(99, 3);
    Bytes extra = canonical;
    extra.push_back(0);
    expectFailure([&]{ (void)decodeRevisionRecord(extra); },
                  StorageErrorKind::Corruption, "extra bytes were accepted");

    RevisionRecord invalid = record;
    invalid.modelIndexed = false;
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "raw record had nonzero reverse root");
    invalid = record;
    aowis::dvmdb::UuidV7 malformed = invalid.id.uuid();
    malformed.bytes[6] = 0x40;
    invalid.id = RevisionId(malformed);
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "invalid generated revision UUID accepted");
    invalid = record;
    invalid.parents.push_back({record.id, sha256(canonical)});
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "self-parent accepted");
    invalid = record;
    invalid.parents.push_back({revisionId("018f84c0-0000-7000-8000-000000000003"), sha256(canonical)});
    invalid.parents.push_back(invalid.parents.front());
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "duplicate parent accepted");
    invalid.parents.emplace_back(RevisionParent{revisionId("018f84c0-0000-7000-8000-000000000004"), sha256(canonical)});
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "three parents accepted");
}

void testDivergenceAndMerge() {
    const std::string project = "018f84c0-0000-7000-8000-000000000002";
    RevisionRecord root = makeRecord("018f84c0-0000-7000-8000-000000000001", project);
    RevisionRecord left = makeRecord("018f84c0-0000-7000-8000-000000000003", project);
    RevisionRecord right = makeRecord("018f84c0-0000-7000-8000-000000000004", project);
    left.parents.push_back({root.id, hashRevisionRecord(root)});
    right.parents.push_back({root.id, hashRevisionRecord(root)});
    RevisionRecord merge = makeRecord("018f84c0-0000-7000-8000-000000000005", project);
    merge.parents.push_back({left.id, hashRevisionRecord(left)});
    merge.parents.push_back({right.id, hashRevisionRecord(right)});
    const Bytes merged = encodeRevisionRecord(merge);
    std::map<std::string, Bytes> records{{root.id.toString(), encodeRevisionRecord(root)},
                                         {left.id.toString(), encodeRevisionRecord(left)},
                                         {right.id.toString(), encodeRevisionRecord(right)}};
    std::size_t fetches = 0;
    const auto lookup = [&](const RevisionId& id) -> std::optional<Bytes> {
        ++fetches;
        const auto it = records.find(id.toString());
        if (it == records.end()) return std::nullopt;
        return it->second;
    };
    check(verifyRevisionAncestry(merged, lookup) == sha256(merged), "merge verification failed");
    check(fetches == 3, "shared ancestors were not deduplicated");

    records.erase(root.id.toString());
    expectFailure([&]{ (void)verifyRevisionAncestry(merged, lookup); },
                  StorageErrorKind::Corruption, "missing parent was accepted");
    records[root.id.toString()] = encodeRevisionRecord(root);
    records[left.id.toString()][34] ^= 1;
    expectFailure([&]{ (void)verifyRevisionAncestry(merged, lookup); },
                  StorageErrorKind::Corruption, "altered parent snapshot was accepted");
    records[left.id.toString()] = encodeRevisionRecord(left);
    RevisionRecord wrongProject = root;
    wrongProject.project = projectId("018f84c0-0000-7000-8000-000000000099");
    left.parents[0].expectedRecordHash = hashRevisionRecord(wrongProject);
    records[left.id.toString()] = encodeRevisionRecord(left);
    records[root.id.toString()] = encodeRevisionRecord(wrongProject);
    merge.parents[0].expectedRecordHash = hashRevisionRecord(left);
    expectFailure([&]{ (void)verifyRevisionAncestry(encodeRevisionRecord(merge), lookup); },
                  StorageErrorKind::Corruption, "cross-project parent accepted");
    // A reused UUID with distinct SHA bindings on the two merge arms is invalid.
    right.parents[0].expectedRecordHash.fill(0x44);
    records[right.id.toString()] = encodeRevisionRecord(right);
    merge.parents[1].expectedRecordHash = hashRevisionRecord(right);
    expectFailure([&]{ (void)verifyRevisionAncestry(encodeRevisionRecord(merge), lookup); },
                  StorageErrorKind::Corruption, "conflicting repeated parent UUID accepted");
}

void testIdentitySeparateFromSnapshot() {
    const std::string project = "018f84c0-0000-7000-8000-000000000002";
    const RevisionRecord one = makeRecord("018f84c0-0000-7000-8000-000000000001", project);
    const RevisionRecord two = makeRecord("018f84c0-0000-7000-8000-000000000003", project);
    check(one.root == two.root && hashRevisionRecord(one) != hashRevisionRecord(two),
          "two revisions with the same snapshot must have distinct record hashes");
    RevisionRecord foreign = one;
    foreign.project = projectId("018f84c0-0000-7000-8000-000000000099");
    check(hashRevisionRecord(one) != hashRevisionRecord(foreign),
          "project identity was not included in the revision digest");
}
} // namespace

int main() {
    try {
        testGoldenVectors();
        testMalformedBytes();
        testDivergenceAndMerge();
        testIdentitySeparateFromSnapshot();
        std::cout << "UUIDv7 revision record golden vectors and integrity tests passed\n";
    } catch (const std::exception& exception) {
        std::cerr << "UUIDv7 revision record FAILED:" << exception.what() << '\n';
        return 1;
    }
    return 0;
}
