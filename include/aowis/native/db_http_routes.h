#pragma once

#include "aowis/native/project_service.h"

class QHttpServer;

namespace aowis::native::http {
// Registers project, revision and hydraulic REST routes; does not own the repository
// catalogue, listen socket or process lifecycle.
void registerRoutes(QHttpServer& server, ProjectService& service,
                    PhysicalStore defaultStorage);
} // namespace aowis::native::http
