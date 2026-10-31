#pragma once

#include "aowis/dvmdb/revision_record.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace aowis::dvmdb {

// The immutable-object reader MUST read previously fsynced, SHA-addressed
// bytes from the owning repository's object pack. Returning nullopt means the
// referenced content is not durably available. The resolver never fabricates
// content and never treats an unverified UUID as a content address.
using RevisionObjectReader = std::function<std::optional<Bytes>(const Hash&)>;

struct ResolvedRevision {
    RevisionRecord record;
    Hash recordHash{};
};

// The project is pinned in the repository's revision-v2 namespace; one UUIDv7
// filename is an immutable UUID->SHA binding. Linux/POSIX implementation;
// flock serializes readers/writers from separate instances/processes. Do not
// call the same resolver recursively from an object-reader callback.
class RevisionResolver final {
public:
    RevisionResolver(const std::filesystem::path& repository, ProjectId project);
    ~RevisionResolver();
    RevisionResolver(const RevisionResolver&) = delete;
    RevisionResolver& operator=(const RevisionResolver&) = delete;

    const ProjectId& project() const noexcept;

    // Requires the immutable record to have already been written and fsynced
    // to the object pack, and every parent to be published already with the
    // digest this record names. Identical retries are idempotent. Rebinding a
    // UUID is never allowed.
    Hash publish(const Bytes& canonicalRecord, const RevisionObjectReader& readObject);

    // Authenticated O(1) lookup: verifies the binding, the SHA-addressed record,
    // its project and every parent UUID->SHA binding, without loading parents.
    // The DAG is valid by induction over publish(); full ancestry and content
    // verification is Repository::verifyRepository() and bundle import.
    // Missing IDs return nullopt; damaged bindings or records raise Corruption.
    std::optional<ResolvedRevision> resolve(
        const RevisionId& id, const RevisionObjectReader& readObject) const;

    // True when a UUID binding is published (no record is read).
    bool contains(const RevisionId& id) const;

private:
    ProjectId project_;
    int dirFd_ = -1;
    int identitiesFd_ = -1;
    int lockFd_ = -1;

    std::optional<Hash> readBindingLocked(const RevisionId& id) const;
    void verifyRecordLocked(const RevisionRecord& record, const Hash& digest,
                            const RevisionObjectReader& readObject) const;
};

} // namespace aowis::dvmdb
