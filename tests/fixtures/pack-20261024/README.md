# Frozen native-pack and bundle compatibility specimen (2026-10-24)

These are real, immutable bytes emitted by the pre-release native pack
implementation. They must **not** be regenerated with the executable under
test: that would hide accidental format incompatibilities.

Original creation used the AOWIS `Repository` API (now at `include/aowis/dvmdb/native_store.h`):

1. `createBranch("main")`, commit keys `10` and `20` (first byte of 16-byte
   UUID keys, remaining bytes zero), values `alpha-v1` and `beta-v1`.
2. Export `base.aowbundle` and create `feature` from `main`.
3. On `feature`, set key `20` to `beta-feature` and key `40` to `feature-new`;
   export `feature.aowbundle`.
4. On `main`, set key `10` to `alpha-v2` and key `30` to `gamma-v1`.
5. On `main`, delete key `20` and set key `30` to `gamma-v2`; export
   `delta.aowbundle` using the first commit as the recipient base, then
   export `tip.aowbundle`.
6. Verify the complete repository. Retain `objects.pack` and the two
   published branch references. Discard the *derived* index, lifetime/operation
   locks and temporary files. No filesystem-path metadata is authoritative
   in this fixture.

Revision identities (SHA-256):

- Base: `b8d2bafee89295d4bb183f90b7aad1e2e4a8a76b39229827f79c71405935f468`
- Feature: `4f0f347555c80634f2578dae167d80b841759511a3b891ab0045a479a8f15bb2`
- Middle main: `b13743ce2d342a1c4e8c4490ba4ac60cbcbf1e7d20436a561fac67983e217e9a`
- Final main: `7bf2c352bb7308517a3785101b1634b206a954c013549070eb4760ea52f452d1`

`tests/native_format_compatibility_tests.cpp` records the SHA-256 digest of
**each** file, verifies the above identities, and asserts the on-disk and
bundle semantics. Do not modify these bytes in place. If the product supports
a new format, add a new dated independent fixture and preserve the old one
unless there is an explicit decision to withdraw support.

For public compatibility policy and limitations see
`docs/format-compatibility.md`.
