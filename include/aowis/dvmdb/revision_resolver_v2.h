#pragma once

#include "aowis/dvmdb/revision_record_v2.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace aowis::dvmdb {

// The immutable-object reader MUST read previously fsynced, SHA-addressed
// bytes from the owning repository's object pack. Returning nullopt means the
// referenced content is not durably available. The resolver never fabricates
// content and never treats an unverified UUID as a content address.
using RevisionObjectReaderV2 = std::function<std::optional<Bytes>(const Hash&)>;

struct ResolvedRevisionV2 {
    RevisionRecordV2 record;
    Hash recordHash{};
};

// Phase 2b persistence primitive. The project is pinned in the repository's
// revision-v2 namespace; one UUIDv7 filename is an immutable UUID->SHA binding.
// Used by Repository's native v2 pack/branch-head path; the ordinary
// ProjectService, REST and SHA-facing APIs are not yet converted.
// Linux/POSIX implementation; flock serializes readers/writers from separate
// instances/processes. Do not call the same resolver recursively from an
// object-reader callback.
class RevisionResolverV2 final {
public:
    RevisionResolverV2(const std::filesystem::path& repository, ProjectId project);
    ~RevisionResolverV2();
    RevisionResolverV2(const RevisionResolverV2&) = delete;
    RevisionResolverV2& operator=(const RevisionResolverV2&) = delete;

    const ProjectId& project() const noexcept;

    // Requires the immutable record to have already been written and fsynced
    // to the object pack; checks the round-trip bytes and all parent bindings
    // before atomically publishing the new UUID mapping. Identical retries
    // are idempotent. Rebinding a UUID is never allowed.
    Hash publish(const Bytes& canonicalRecord, const RevisionObjectReaderV2& readObject);

    // Lookup verifies the binding, the SHA-addressed stored bytes and the
    // *entire* reachable ancestry (bounded by maxRevisions). Missing IDs
    // return nullopt; dangling/corrupt references raise Corruption instead.
    // Authenticated direct lookup for linear-work full-DAG readers. Verifies
    // project/record SHA and every parent UUID->expected SHA binding, but does
    // not recursively load parents. A graph reader must visit ALL parent IDs.
    std::optional<ResolvedRevisionV2> resolveDirect(
        const RevisionId& id, const RevisionObjectReaderV2& readObject) const;

    std::optional<ResolvedRevisionV2> resolve(
        const RevisionId& id, const RevisionObjectReaderV2& readObject,
        std::size_t maxRevisions = 250000) const;

private:
    ProjectId project_;
    int dirFd_ = -1;
    int identitiesFd_ = -1;
    int lockFd_ = -1;

    std::optional<Hash> readBindingLocked(const RevisionId& id) const;
    void verifyStoredLocked(const RevisionRecordV2& record,
                            const Hash& digest,
                            const RevisionObjectReaderV2& readObject,
                            std::size_t maxRevisions) const;
};

} // namespace aowis::dvmdb
