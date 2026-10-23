# AOWIS database REST API — implemented foundation and next capabilities

**Status:** Project/catalogue, branch, head, history, immutable revisions, revision diffs, canonical hydraulic network reads/writes, indexed entity reads/edits and semantic merges are implemented in the `aowis-server-db` executable (when the model codec is enabled). The machine-readable [OpenAPI contract](db-http-api.yaml) describes these routes. Hydraulic preview-only merging, network enumeration and streaming HTTP bundles remain planned; bounded buffered full/incremental bundles are implemented.

## Goals and deployment

- One `aowis-server-db` process serves **multiple independent AOWIS projects**. A project has a stable UUID, descriptive metadata and one native repository with its own branch/revision history.
- Expose **RESTful HTTP over TCP**, with a configurable listen address and port. Use the vendored, pinned **cpp-httplib** (decision 2026-10-23: QHttpServer cannot bound requests before Qt 6.11 and buffers whole bodies) and the existing Qt/C++ model types internally. Do not develop another HTTP parser.
- **HTTP only, no TLS/HTTPS in this implementation.** Authentication, authorization, sessions, keys and identities are intentionally **not implemented or enforced** in the initial REST service. These will be defined by a future AOWIS-wide security system, not separately for database storage.
- The native database and hydraulic APIs remain usable directly as libraries by AOWIS standalone and in-process services. REST is an adapter, not a second storage implementation.
- Keep a clear service boundary between request parsing, project lookup, native storage operations, domain validation and response encoding.

**Operational consequence:** the initial REST server is unauthenticated and unencrypted. Any client able to reach its configured listener can call its exposed operations. Deploy only on deliberately trusted/development networks and avoid sensitive content. Choosing a configurable public bind address is a feature, not a security guarantee.

## Implemented and planned routes

The table distinguishes implemented routes from the remaining proposed network-enumeration endpoint; the machine-readable OpenAPI specification documents actual operations. The `/api/v1` prefix versions the HTTP interface, **not the model serialization format**.

| Operation | Proposed method and route |
|---|---|
| Service health and readiness | `GET /api/v1/health`, `GET /api/v1/ready` |
| List projects | `GET /api/v1/projects` |
| Create project | `POST /api/v1/projects` |
| Read project details | `GET /api/v1/projects/{projectId}` |
| List project branches | `GET /api/v1/projects/{projectId}/branches` |
| Create/fork branch | `POST /api/v1/projects/{projectId}/branches` |
| Read branch head | `GET /api/v1/projects/{projectId}/branches/{branch}/head` |
| Browse branch history | `GET /api/v1/projects/{projectId}/branches/{branch}/history` |
| Inspect immutable revision | `GET /api/v1/projects/{projectId}/revisions/{revisionHash}` |
| Compare revisions | `GET /api/v1/projects/{projectId}/diff?from={hash}&to={hash}` (implemented) |
| List hydraulic networks | `GET /api/v1/projects/{projectId}/branches/{branch}/networks` (planned) |
| Read network | `GET /api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}` (implemented) |
| List a network's entities | `GET /api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/entities?type=9` (implemented, ordinal type) |
| Read one entity | `GET /api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/entities/{entityId}` (implemented) |
| Edit entities atomically | `POST /api/v1/projects/{projectId}/branches/{branch}/networks/{networkId}/edits` (implemented) |
| Perform merge | `POST /api/v1/projects/{projectId}/merges` (implemented; conflicts returned without publication) |
| Export branch bundle | `GET /api/v1/projects/{projectId}/branches/{branch}/bundle` |
| Import branch bundle | `POST /api/v1/projects/{projectId}/branches/{branch}/bundle` |

An implemented incremental export supplies a known base revision as a `base` query parameter; the native exporter verifies it is an ancestor of the requested branch head. We should avoid shipping full bundles where a correct delta is possible. The project ID is a UUID from the server catalogue, **never a client-provided filesystem path**.

## Response and request principles

- **JSON** for project metadata, branch/head/history, small model edits, merge options, diagnostics and error envelopes. **Bounded buffered binary bundles** for synchronization; true streaming remains future work. The current HTTP model API uses canonical ModelCodec bytes encoded as `payloadBase64` in JSON; this is a deliberately simple initial format, not streaming, and is limited to 8 MiB per mutation request.
- Use appropriate HTTP status codes (`400` malformed input, `404` unknown resource, `409` revision conflict or divergence, `413` size limit, `422` semantic validation failure where appropriate, `5xx` internal/server errors). Provide stable machine-readable error codes and useful human-readable descriptions.
- Pagination and limits for large collections/history. Use immutable revision IDs for reproducible historical reads, with explicit semantics when a branch moves.
- Each **mutating** request identifies the expected branch-head revision (for example via an `If-Match`/ETag contract or an explicit expected revision field). Validate it under the same repository lock as publication. Return a conflict on stale writes; do not silently retry against a different model.
- One HTTP edit request maps to **one atomic native model commit**, not multiple independent partial commits. Domain validation stays in the model/storage layer.
- Merges report conflict information and support explicit field/record choices using the existing native hydraulic merge APIs. Candidate merged networks must undergo semantic validation before publishing divergent merges.
- Synchronization import must verify object hashes, complete Merkle/revision graphs and branch fast-forward semantics before publishing. Never overwrite a divergent branch automatically. Fetch to a separate tracking branch and merge explicitly when required.
- Return canonical model/entity identifiers consistently; do not confuse `NetworkHydraulic`, project UUIDs, revision hashes or serializer format versions.

## Repository and server lifecycle

- Persist a server-side **project catalogue** mapping UUIDs to administrator-controlled repository directories. The project catalogue uses durable UUID-named directories with atomic project publication and tested restart recovery.
- Decide and document repository-handle lifetime, concurrent request handling, graceful shutdown and safe offline maintenance. Never promise online compaction before it is implemented.
- Use configuration for bind address, port, project data root and logging; align with other AOWIS servers' conventions rather than invent per-request filesystem routing.
- Maintain the [OpenAPI specification](db-http-api.yaml) alongside implemented endpoints and test them over real local TCP on both physical backends.
- Keep a storage-agnostic native C++ façade so embedding the database in the AOWIS standalone build does not require TCP/HTTP.

## Deferred security

The initial REST service intentionally has **no authentication, authorization enforcement, TLS or HTTPS**. This is an explicit development-stage constraint, **not** an assertion that plain HTTP is secure. The separately persisted provisional `access-policy.bin` format is not the future REST identity system. Do not transport or trust user UUIDs, credential headers or proxy-auth claims as if a secure boundary already existed.

An AOWIS-wide security design will later specify identity/session lifecycle, trust across the GUI and all AOWIS services, project and branch permissions, service-to-service access, TLS/reverse proxies, and audit requirements. Only then should the REST service enforce those decisions.

## Implementation sequence

See [roadmap](roadmap.md). The configurable TCP/HTTP service, durable multi-project catalogue, canonical hydraulic editing and semantic merges have been implemented. Preview-only merges, streaming and further resource hardening follow. At every step, current and planned behavior must remain explicitly distinguished in documentation.
