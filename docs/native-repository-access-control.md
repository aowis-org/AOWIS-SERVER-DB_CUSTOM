# Deferred native access-policy notes

**Current code, not the planned REST security design.** The repository includes `AccessPolicyStore` and the `aowis-dvmdb-access` utility. This is an optional standalone policy implementation, not a deployed HTTP security layer. The implemented REST server **does not enforce this policy**. Identity, authentication, authorization and HTTPS will be designed later as an AOWIS-wide system.

## What is implemented today

- Persistent records with UUIDs for users, groups, roles and branch-scoped read/write permissions.
- Users receive roles directly or through groups; disabled users and missing grants are denied by `AccessPolicyStore` when queried with an explicitly supplied account identity.
- Administrative data is separate from versioned model revisions, so reverting a model branch cannot revert access-policy changes.
- Policy state is stored in `access-policy.bin` with an atomic replacement/lock protocol and SHA-256 corruption detection. File ownership and filesystem permissions protect it; the checksum is **not a signature**.
- The library is not attached to a running database service. Its `permits()` API accepts an account string from its caller; it must not be treated as authenticated HTTP identity.

Administrative provisioning is available through `./build-linux/aowis-dvmdb-access`. This is a local tool operating under the repository filesystem owner's permissions; it is not a general AOWIS account-management service.

## Agreed direction

The implemented database REST server currently has **no authentication or access-control enforcement**. The provisional policy model may be reused, redesigned or replaced only once an AOWIS-wide security architecture has been specified. Do not assume its present user UUIDs, role naming or branch grants form a stable external API.

**HTTP without security is intentionally unsafe:** access must be limited by the deployment environment during development, not by claims of application-level protection. See [service design](rest-api-design.md) and [roadmap](roadmap.md).

## Future AOWIS-wide security work

The deferred requirements for private HTTP working repositories, per-user
isolation, verified service identity, token revocation, cross-repository
permissions and auditability are tracked in the dedicated
[security and permissions design backlog](security-permissions-design.md).
Selecting an HTTP URL in the GUI config does not make it private.
