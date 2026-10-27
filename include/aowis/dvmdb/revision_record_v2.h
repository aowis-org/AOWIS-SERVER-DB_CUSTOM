#pragma once

#include "aowis/dvmdb/native_store.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace aowis::dvmdb {

// Canonical immutable revision record, *not* a branch or the branch HEAD.
// UUIDs identify logical revisions; record and snapshot SHA-256 hashes verify
// immutable content. This codec does not change the active repository format.
struct RevisionParentV2 {
    RevisionId id;
    Hash expectedRecordHash{};
};

struct RevisionRecordV2 {
    RevisionId id;
    ProjectId project;
    Hash root{};
    Hash referencesRoot{};
    bool modelIndexed = false;
    std::vector<RevisionParentV2> parents;
};

// Exact frozen T/02 encoding: 100 + 48 * parent-count bytes, 0..2 parents.
// Input validation failures are StorageError(InvalidInput); malformed records
// obtained from storage are StorageError(Corruption).
Bytes encodeRevisionRecordV2(const RevisionRecordV2& record);
RevisionRecordV2 decodeRevisionRecordV2(const Bytes& bytes);
Hash hashRevisionRecordV2(const RevisionRecordV2& record);

// Verify an entire reachable DAG from its immutable record bytes. `getRecord`
// must fetch by UUID (not by a user-controlled filename or display name).
// Recomputes the SHA of each record and verifies every ordered parent UUID/SHA
// binding, common project provenance, and acyclicity. Bounds traversal to
// maxRevisions; no record or branch is published by this operation.
using RevisionRecordLookupV2 =
    std::function<std::optional<Bytes>(const RevisionId&)>;
Hash verifyRevisionAncestryV2(const Bytes& tipRecord,
                              const RevisionRecordLookupV2& getRecord,
                              std::size_t maxRevisions = 250000);

} // namespace aowis::dvmdb
