#include "aowis/dvmdb/revision_record_v2.h"

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
using aowis::dvmdb::RevisionParentV2;
using aowis::dvmdb::RevisionRecordV2;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;
using aowis::dvmdb::decodeRevisionRecordV2;
using aowis::dvmdb::encodeRevisionRecordV2;
using aowis::dvmdb::hashRevisionRecordV2;
using aowis::dvmdb::sha256;
using aowis::dvmdb::toHex;
using aowis::dvmdb::verifyRevisionAncestryV2;

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

RevisionRecordV2 makeRecord(std::string_view id, std::string_view project,
                            bool indexed = false) {
    RevisionRecordV2 record{revisionId(id), projectId(project)};
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
    RevisionRecordV2 root = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                       "018f84c0-0000-7000-8000-000000000002", true);
    const Bytes rootBytes = encodeRevisionRecordV2(root);
    check(rootBytes.size() == 100 && rootBytes[0] == 'T' && rootBytes[1] == 2,
          "golden root record format incorrect");
    check(toHex(sha256(rootBytes)) ==
          "8b1a70f56b56a30edf970a7c7bd0e5f3b89825249e1d070378cfbc2b75c61d7c",
          "root does not match frozen SHA-256 vector");
    const RevisionRecordV2 decoded = decodeRevisionRecordV2(rootBytes);
    check(encodeRevisionRecordV2(decoded) == rootBytes && decoded.project == root.project,
          "root decode did not preserve canonical bytes");

    RevisionRecordV2 child = makeRecord("018f84c0-0000-7000-8000-000000000003",
                                        "018f84c0-0000-7000-8000-000000000002", true);
    child.parents.push_back({root.id, sha256(rootBytes)});
    check(toHex(hashRevisionRecordV2(child)) ==
          "fddb8eb27f2f6193b1dcefe185b47b604e96f6814620ff916ab623273689dc6a",
          "child does not match frozen SHA-256 vector");
    const Bytes childBytes = encodeRevisionRecordV2(child);
    check(childBytes.size() == 148 && encodeRevisionRecordV2(decodeRevisionRecordV2(childBytes)) == childBytes,
          "child was not canonically decoded");

    const std::map<std::string, Bytes> records{{root.id.toString(), rootBytes}};
    const auto lookup = [&](const RevisionId& id) -> std::optional<Bytes> {
        const auto iter = records.find(id.toString());
        if (iter == records.end()) return std::nullopt;
        return iter->second;
    };
    check(verifyRevisionAncestryV2(childBytes, lookup) == sha256(childBytes),
          "verified two-record DAG produced wrong tip digest");
    expectFailure([&]{ (void)verifyRevisionAncestryV2(childBytes, lookup, 1); },
                  StorageErrorKind::LimitExceeded, "ancestry traversal budget was ignored");
}

void testMalformedBytes() {
    RevisionRecordV2 record = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                         "018f84c0-0000-7000-8000-000000000002", true);
    const Bytes canonical = encodeRevisionRecordV2(record);
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        const Bytes shortRecord(canonical.begin(), canonical.begin() + static_cast<std::ptrdiff_t>(i));
        expectFailure([&]{ (void)decodeRevisionRecordV2(shortRecord); },
                      StorageErrorKind::Corruption, "truncated v2 record was accepted");
    }
    const auto mutate = [&](std::size_t offset, std::uint8_t newValue) {
        Bytes corrupted = canonical;
        corrupted[offset] = newValue;
        expectFailure([&]{ (void)decodeRevisionRecordV2(corrupted); },
                      StorageErrorKind::Corruption, "invalid v2 field was accepted");
    };
    mutate(0, 'S'); mutate(1, 3); mutate(8, 0x40); mutate(24, 0x40);
    mutate(10, 0x00); mutate(26, 0x00); mutate(98, 2); mutate(99, 3);
    Bytes extra = canonical;
    extra.push_back(0);
    expectFailure([&]{ (void)decodeRevisionRecordV2(extra); },
                  StorageErrorKind::Corruption, "extra v2 bytes were accepted");

    RevisionRecordV2 invalid = record;
    invalid.modelIndexed = false;
    expectFailure([&]{ (void)encodeRevisionRecordV2(invalid); },
                  StorageErrorKind::InvalidInput, "raw v2 record had nonzero reverse root");
    invalid = record;
    aowis::dvmdb::UuidV7 malformed = invalid.id.uuid();
    malformed.bytes[6] = 0x40;
    invalid.id = RevisionId(malformed);
    expectFailure([&]{ (void)encodeRevisionRecordV2(invalid); },
                  StorageErrorKind::InvalidInput, "invalid generated revision UUID accepted");
    invalid = record;
    invalid.parents.push_back({record.id, sha256(canonical)});
    expectFailure([&]{ (void)encodeRevisionRecordV2(invalid); },
                  StorageErrorKind::InvalidInput, "self-parent accepted");
    invalid = record;
    invalid.parents.push_back({revisionId("018f84c0-0000-7000-8000-000000000003"), sha256(canonical)});
    invalid.parents.push_back(invalid.parents.front());
    expectFailure([&]{ (void)encodeRevisionRecordV2(invalid); },
                  StorageErrorKind::InvalidInput, "duplicate parent accepted");
    invalid.parents.emplace_back(RevisionParentV2{revisionId("018f84c0-0000-7000-8000-000000000004"), sha256(canonical)});
    expectFailure([&]{ (void)encodeRevisionRecordV2(invalid); },
                  StorageErrorKind::InvalidInput, "three parents accepted");
}

void testDivergenceAndMerge() {
    const std::string project = "018f84c0-0000-7000-8000-000000000002";
    RevisionRecordV2 root = makeRecord("018f84c0-0000-7000-8000-000000000001", project);
    RevisionRecordV2 left = makeRecord("018f84c0-0000-7000-8000-000000000003", project);
    RevisionRecordV2 right = makeRecord("018f84c0-0000-7000-8000-000000000004", project);
    left.parents.push_back({root.id, hashRevisionRecordV2(root)});
    right.parents.push_back({root.id, hashRevisionRecordV2(root)});
    RevisionRecordV2 merge = makeRecord("018f84c0-0000-7000-8000-000000000005", project);
    merge.parents.push_back({left.id, hashRevisionRecordV2(left)});
    merge.parents.push_back({right.id, hashRevisionRecordV2(right)});
    const Bytes merged = encodeRevisionRecordV2(merge);
    std::map<std::string, Bytes> records{{root.id.toString(), encodeRevisionRecordV2(root)},
                                         {left.id.toString(), encodeRevisionRecordV2(left)},
                                         {right.id.toString(), encodeRevisionRecordV2(right)}};
    std::size_t fetches = 0;
    const auto lookup = [&](const RevisionId& id) -> std::optional<Bytes> {
        ++fetches;
        const auto it = records.find(id.toString());
        if (it == records.end()) return std::nullopt;
        return it->second;
    };
    check(verifyRevisionAncestryV2(merged, lookup) == sha256(merged), "merge verification failed");
    check(fetches == 3, "shared ancestors were not deduplicated");

    records.erase(root.id.toString());
    expectFailure([&]{ (void)verifyRevisionAncestryV2(merged, lookup); },
                  StorageErrorKind::Corruption, "missing parent was accepted");
    records[root.id.toString()] = encodeRevisionRecordV2(root);
    records[left.id.toString()][34] ^= 1;
    expectFailure([&]{ (void)verifyRevisionAncestryV2(merged, lookup); },
                  StorageErrorKind::Corruption, "altered parent snapshot was accepted");
    records[left.id.toString()] = encodeRevisionRecordV2(left);
    RevisionRecordV2 wrongProject = root;
    wrongProject.project = projectId("018f84c0-0000-7000-8000-000000000099");
    left.parents[0].expectedRecordHash = hashRevisionRecordV2(wrongProject);
    records[left.id.toString()] = encodeRevisionRecordV2(left);
    records[root.id.toString()] = encodeRevisionRecordV2(wrongProject);
    merge.parents[0].expectedRecordHash = hashRevisionRecordV2(left);
    expectFailure([&]{ (void)verifyRevisionAncestryV2(encodeRevisionRecordV2(merge), lookup); },
                  StorageErrorKind::Corruption, "cross-project parent accepted");
    // A reused UUID with distinct SHA bindings on the two merge arms is invalid.
    right.parents[0].expectedRecordHash.fill(0x44);
    records[right.id.toString()] = encodeRevisionRecordV2(right);
    merge.parents[1].expectedRecordHash = hashRevisionRecordV2(right);
    expectFailure([&]{ (void)verifyRevisionAncestryV2(encodeRevisionRecordV2(merge), lookup); },
                  StorageErrorKind::Corruption, "conflicting repeated parent UUID accepted");
}

void testIdentitySeparateFromSnapshot() {
    const std::string project = "018f84c0-0000-7000-8000-000000000002";
    const RevisionRecordV2 one = makeRecord("018f84c0-0000-7000-8000-000000000001", project);
    const RevisionRecordV2 two = makeRecord("018f84c0-0000-7000-8000-000000000003", project);
    check(one.root == two.root && hashRevisionRecordV2(one) != hashRevisionRecordV2(two),
          "two revisions with the same snapshot must have distinct record hashes");
    RevisionRecordV2 foreign = one;
    foreign.project = projectId("018f84c0-0000-7000-8000-000000000099");
    check(hashRevisionRecordV2(one) != hashRevisionRecordV2(foreign),
          "project identity was not included in the revision digest");
}
} // namespace

int main() {
    try {
        testGoldenVectors();
        testMalformedBytes();
        testDivergenceAndMerge();
        testIdentitySeparateFromSnapshot();
        std::cout << "UUIDv7 revision record v2 golden vectors and integrity tests passed\n";
    } catch (const std::exception& exception) {
        std::cerr << "UUIDv7 revision record v2 FAILED: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
