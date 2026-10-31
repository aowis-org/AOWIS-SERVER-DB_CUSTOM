#include "aowis/dvmdb/revision_record.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

namespace aowis::dvmdb {
namespace {

void check(bool valid, StorageErrorKind kind, const char* message) {
    if (!valid) throw StorageError(kind, message);
}

UuidV7 readUuid(const Bytes& bytes, std::size_t offset) {
    std::array<std::uint8_t, 16> value{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), value.size(), value.begin());
    check((value[6] & 0xf0U) == 0x70U && (value[8] & 0xc0U) == 0x80U,
          StorageErrorKind::Corruption, "non-UUIDv7 revision identifier");
    return UuidV7{value};
}

void writeUuid(Bytes& out, std::size_t offset, const UuidV7& value) {
    std::copy(value.bytes.begin(), value.bytes.end(),
              out.begin() + static_cast<std::ptrdiff_t>(offset));
}

void writeHash(Bytes& out, std::size_t offset, const Hash& value) {
    std::copy(value.begin(), value.end(),
              out.begin() + static_cast<std::ptrdiff_t>(offset));
}

Hash readHash(const Bytes& bytes, std::size_t offset) {
    Hash value{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), value.size(), value.begin());
    return value;
}

bool isUuidV7(const UuidV7& value) {
    return (value.bytes[6] & 0xf0U) == 0x70U &&
           (value.bytes[8] & 0xc0U) == 0x80U;
}

void validateRecord(const RevisionRecord& record, StorageErrorKind kind) {
    check(isUuidV7(record.id.uuid()) && isUuidV7(record.project.uuid()), kind,
          "invalid revision or project UUIDv7");
    check(record.parents.size() <= 2, kind, "revision has too many parents");
    check(record.modelIndexed || record.referencesRoot == Hash{}, kind,
          "raw revision cannot contain model references");
    std::set<UuidV7> seen;
    for (const RevisionParent& parent : record.parents) {
        check(isUuidV7(parent.id.uuid()), kind, "invalid parent UUIDv7");
        check(parent.id != record.id, kind, "revision is its own parent");
        check(seen.insert(parent.id.uuid()).second, kind, "duplicate revision parent");
        check(parent.expectedRecordHash != Hash{}, kind, "zero parent revision digest");
    }
}

struct Frame {
    RevisionRecord record;
    Hash digest;
    std::size_t nextParent = 0;
};

} // namespace

Bytes encodeRevisionRecord(const RevisionRecord& record) {
    validateRecord(record, StorageErrorKind::InvalidInput);
    // The canonical format has a fixed size. Filling its offsets directly
    // avoids both reallocations and any intermediate noncanonical shape.
    Bytes bytes(100 + 48 * record.parents.size(), 0);
    bytes[0] = static_cast<std::uint8_t>('T');
    bytes[1] = 2;
    writeUuid(bytes, 2, record.id.uuid());
    writeUuid(bytes, 18, record.project.uuid());
    writeHash(bytes, 34, record.root);
    writeHash(bytes, 66, record.referencesRoot);
    bytes[98] = record.modelIndexed ? 1 : 0;
    bytes[99] = static_cast<std::uint8_t>(record.parents.size());
    for (std::size_t i = 0; i < record.parents.size(); ++i) {
        const std::size_t offset = 100 + 48 * i;
        writeUuid(bytes, offset, record.parents[i].id.uuid());
        writeHash(bytes, offset + 16, record.parents[i].expectedRecordHash);
    }
    return bytes;
}

RevisionRecord decodeRevisionRecord(const Bytes& bytes) {
    check(bytes.size() >= 100, StorageErrorKind::Corruption, "truncated revision record");
    check(bytes[0] == static_cast<std::uint8_t>('T') && bytes[1] == 2,
          StorageErrorKind::Corruption, "unsupported revision record header");
    const std::size_t parentCount = bytes[99];
    check(parentCount <= 2 && bytes.size() == 100 + 48 * parentCount,
          StorageErrorKind::Corruption, "invalid revision length or parent count");
    check(bytes[98] <= 1, StorageErrorKind::Corruption, "invalid indexing flag");

    RevisionRecord record{
        RevisionId(readUuid(bytes, 2)),
        ProjectId(readUuid(bytes, 18)),
        readHash(bytes, 34),
        readHash(bytes, 66),
        bytes[98] == 1,
        {}
    };
    record.parents.reserve(parentCount);
    for (std::size_t i = 0; i < parentCount; ++i) {
        const std::size_t offset = 100 + 48 * i;
        record.parents.push_back({RevisionId(readUuid(bytes, offset)), readHash(bytes, offset + 16)});
    }
    validateRecord(record, StorageErrorKind::Corruption);
    return record;
}

Hash hashRevisionRecord(const RevisionRecord& record) {
    return sha256(encodeRevisionRecord(record));
}

Hash verifyRevisionAncestry(const Bytes& tipRecord,
                              const RevisionRecordLookup& getRecord,
                              std::size_t maxRevisions,
                              const TrustedRevision& trusted) {
    check(static_cast<bool>(getRecord), StorageErrorKind::InvalidInput,
          "missing revision resolver");
    check(maxRevisions > 0, StorageErrorKind::InvalidInput,
          "zero revision traversal budget");
    const RevisionRecord tip = decodeRevisionRecord(tipRecord);
    const Hash tipHash = sha256(tipRecord);
    std::vector<Frame> stack;
    stack.push_back({tip, tipHash, 0});
    std::set<UuidV7> active{tip.id.uuid()};
    std::map<UuidV7, Hash> verified;
    std::size_t traversed = 1;

    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.nextParent == frame.record.parents.size()) {
            verified.emplace(frame.record.id.uuid(), frame.digest);
            active.erase(frame.record.id.uuid());
            stack.pop_back();
            continue;
        }
        const RevisionParent parent = frame.record.parents[frame.nextParent++];
        check(!active.contains(parent.id.uuid()), StorageErrorKind::Corruption,
              "cycle in revision ancestry");
        const std::map<UuidV7, Hash>::const_iterator known = verified.find(parent.id.uuid());
        if (known != verified.end()) {
            check(known->second == parent.expectedRecordHash,
                  StorageErrorKind::Corruption, "conflicting revision identity binding");
            continue;
        }
        if (trusted && trusted(parent)) {
            verified.emplace(parent.id.uuid(), parent.expectedRecordHash);
            continue;
        }
        check(traversed < maxRevisions, StorageErrorKind::LimitExceeded,
              "revision ancestry budget exceeded");
        const std::optional<Bytes> encoded = getRecord(parent.id);
        check(encoded.has_value(), StorageErrorKind::Corruption, "missing parent revision");
        const Hash parentHash = sha256(*encoded);
        check(parentHash == parent.expectedRecordHash, StorageErrorKind::Corruption,
              "parent revision SHA-256 mismatch");
        const RevisionRecord parentRecord = decodeRevisionRecord(*encoded);
        check(parentRecord.id == parent.id, StorageErrorKind::Corruption,
              "resolver returned wrong revision UUID");
        check(parentRecord.project == tip.project, StorageErrorKind::Corruption,
              "cross-project revision parent");
        active.insert(parent.id.uuid());
        stack.push_back({parentRecord, parentHash, 0});
        ++traversed;
    }
    return tipHash;
}

} // namespace aowis::dvmdb
