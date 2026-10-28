# AOWIS / DVMDB security and permission design backlog

**Dedicated deferred design document, not a claim of implemented security.**
This is the VC-12 track in the [implementation roadmap](roadmap.md), and
multi-user/public writable deployment cannot be approved before its gates.
This records findings and requirements to revisit as one AOWIS-wide security
and permission architecture after the version-control lifecycle and GUI
integration are stable. Refer to [repository access-control findings](native-repository-access-control.md),
[private working repositories](private-working-repositories.md), and
[REST design](rest-api-design.md).

## Immediate facts and deployment cautions

- A configured `workingRepository` or repository UUID is **not proof of user
  ownership**. A URL in a GUI config does not restrict who can connect.
- A private DVMDB over HTTP is only usable for single-user editing today
  if access is isolated by deployment controls (local/private network,
  network policy, operating-system permissions, private endpoint or trusted
  reverse proxy). Do not expose an unauthenticated editing service publicly.
- The existing native `AccessPolicyStore` is not an enforced HTTP
  authentication/authorization layer. Do not represent its policy records
  as effective protection at the REST boundary.
- UUIDv7 and SHA-256 provide identity and integrity, **not** authorization,
  confidentiality, source authentication or non-repudiation.
- Credentials/secrets must not be stored in plaintext shared application
  configuration, debug logs or URLs. Define a secure credential provider.

## Security design questions to resolve in a dedicated phase

1. **Identity:** users, service accounts, devices, sessions and recovery;
   stable account identity versus per-device login; federation if needed.
2. **Authentication:** HTTP and native entry points, token expiry,
   verification, revocation, logout and trusted service-to-service calls;
   TLS and credential rotation.
3. **Authorization:** scope ownership for user working repositories,
   projects, branch refs, unpublished drafts, committed snapshots,
   synchronization, merge, deletion, history export and administration.
4. **Draft isolation:** distinct sessions for one user, private copy sharing
   by explicit grant, background autosave authorization, takeover and
   concurrent-draft locking/version checks.
5. **Remote origin trust:** bind tracking refs to authenticated remote
   identity; prevent wrong-project import, poisoned history, cross-tenant
   repo spoofing, unsafe redirect and credential forwarding.
6. **Audit:** successful and rejected editing, commit, publish, merge,
   branch operations, access policy changes, credential events and recovery;
   tamper-evident append-only audit policies and retention.
7. **Integrity and durability:** transport quotas, replay resistance,
   idempotency key ownership, body limits, path traversal, rate limits,
   backup encryption, revocation and secure deletion/retention of drafts.
8. **Native and REST parity:** one policy decision model, explicit
   service/system identity, correct admin bypass controls, no client-supplied
   `ownerId` taken as proof of identity.
9. **Roles and UX:** project/branch read/write/publish rights, private
   repository registration, sharing and denial messaging without exposing
   another user's draft contents.
10. **Threat modeling:** malicious clients, stolen tokens, compromised
    working repositories, untrusted replication bundles, replayed pushes,
    unsafe log contents and denial-of-service via oversized histories.

## Required security qualification before multi-user exposure

- Authentication on every writable HTTP operation and server-side isolation
  for personal working repositories and working copies.
- Authorization and audit checks for fetch, push, history reads, graph
  traversal, bundle import/export, create/delete, draft access and merges.
- Tested tenant-crossing denial, session revocation, token reuse/replay,
  concurrent writers, wrong-repo UUID targeting and private-data leakage.
- Secret management, TLS expectations, deployment guide, backups and
  a release gate explicitly disallowing unauthenticated multi-user access.

**Schedule:** complete the core lifecycle first, but treat unauthenticated
remote editing as development-only and do not ship it for shared/public
exposure before these requirements are enforced.

## Interim constraints for the approved private-repository design

- One intended user has a separate DVMDB working repository, but multiple
  devices/sessions may reach it. Until VC-12 identity enforcement, these
  must be isolated operationally and **not exposed as unauthenticated
  multi-user services**.
- Draft writer leases, fencing tokens and version checks prevent accidental
  concurrent overwrites; they **are not authorization**. Future policy must
  bind lease issuance, takeover and recovery to authenticated ownership.
- UUIDv7 operation IDs and idempotency payload digests prevent duplicate
  logical operations; they **do not authenticate** the caller. Status-lookup
  visibility must be access-controlled before multi-user deployment.
- Shared branch UUID preservation does not grant access to its ref: the
  repository-qualified target and authenticated project/branch rights must
  be checked before Fetch, provisioning or Publish.
- Explicit three-way merge needs audit provenance for source parents, human
  conflict resolutions, model validation and publication authorization.

Exact roles, credential storage, authentication, revocation, tenancy, quotas,
and security-test gates remain part of dedicated VC-12 work, not prerequisites
for isolated developer test deployments.
