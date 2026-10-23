#pragma once

#include "aowis/native/project_service.h"

#include <cstddef>

namespace httplib {
class Server;
}

namespace aowis::native::http {

// Request bounds applied before a body byte is read. Bundle uploads may be up
// to BundleLimits::maxBytes and stream into a project scratch file; JSON
// bodies have smaller per-route limits (see registerRoutes()).
constexpr std::size_t maxJsonSmallBytes = 16U * 1024U;          // projects, branches
constexpr std::size_t maxJsonMergeBytes = 256U * 1024U;         // merges
constexpr std::size_t maxJsonModelBytes = 8U * 1024U * 1024U;   // network saves, entity edits

// Registers project, revision, synchronization and hydraulic REST routes and
// configures the server's request limits (payload ceiling, per-route JSON
// limits, timeouts). Does not own the catalogue, the listen socket or the
// process lifecycle. Handlers run on the server's worker threads.
void registerRoutes(httplib::Server& server, ProjectService& service,
                    BundleLimits transferLimits = BundleLimits{32ULL * 1024ULL * 1024ULL,
                                                               100000ULL, 8ULL * 1024ULL * 1024ULL});
} // namespace aowis::native::http
