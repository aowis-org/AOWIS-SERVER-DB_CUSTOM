# Engineering evidence and architectural decisions

This document preserves **measured results and reusable engineering conclusions**.
It is not a development diary. The current implementation is described in
[architecture](architecture.md); reproducible measurement commands belong in
[benchmark-plan](benchmark-plan.md), and unfinished qualification in
[roadmap](roadmap.md).

## Storage backend choice

The database uses **one native, append-only, content-addressed pack** per
repository, with immutable revision objects and persistent Merkle model and
reverse-reference trees. An `objects.idx` sidecar is derived from the pack and
can be rebuilt. Branch publication occurs after synchronizing the objects
that a new immutable revision references. The public C++ API, versioning,
merging and replication are not SQL adapters.

Earlier investigations considered SQL-backed storage (SQLite/PostgreSQL) and
an experimental native block layout. SQL's transaction mechanisms alone do
not prove that a whole AOWIS multi-entity operation was implemented as one
atomic SQL transaction; conversely, the native engine's one-ref publication
provides atomic visibility for one branch update without undoing physical
appends. The native engine still needs independently tested recovery, index
integrity, backups and replica semantics. Those properties, rather than the
label of a storage backend, are the useful comparison criteria.

The older experimental block format is intentionally **not supported** by
current repository open. Migrate pre-release data with a compatible old
binary via a portable bundle, then import it into a fresh pack repository.
The pack is the sole production-intended physical implementation here.

**Benchmark evidence boundary:** No adequately controlled numerical
pack-versus-block or SQLite/PostgreSQL-versus-native performance comparison
is preserved in this repository snapshot. No speedup, durability advantage,
or resource figure for those historical alternatives should be quoted as a
verified result. The reproducible numbers below compare *two lookup structures
on the same native pack and format*, not two different storage engines.

## Physical object lookup: measured index improvement

Release measurements from the developer's Linux machine on October 23–24,
2026, with deterministic workloads, shared fixture parameters and an
uncontrolled/warm OS page cache. Physical object count includes historical
revision and Merkle objects, not just currently visible entities.

| Physical objects | Old map: extra RSS | Compact lookup: extra RSS | Old reopen | Compact reopen |
|---:|---:|---:|---:|---:|
| 101,387 | 8.35 MiB | 2.73 MiB | 49.5 ms | 38.9 ms |
| 1,001,959 | 72.8 MiB | 16.7 MiB | 690 ms | 290 ms |
| 10,001,723 | 703.5 MiB | 128.8 MiB | 8.61 s | 3.66 s |

At 10 million objects, **two concurrently open repositories** reached about
1.48 GB total process RSS with the original `unordered_map` lookup, versus
about **278 MB** with the compact implementation. A 1,000-hit read probe on
the 10M fixture took ~47 ms for the old index and ~31.8 ms for the compact
index; cache conditions were not independently controlled. Write latency
remained of similar order, but the test is not a cold-I/O or production SLA.

**Implementation:** 8-byte open-addressed fingerprint/ordinal slots; a
candidate must be verified against the full 32-byte SHA-256 in the checksummed
sidecar before using its physical location. A 4,096-entry bounded hot cache
reduces repeated sidecar reads. Pack bytes, sidecar encoding, revision IDs
and bundle format are **unchanged**. The accelerator presently has a
`2^32-1` sidecar-entry limit per generation.

**Benchmark lesson:** A first comparison incorrectly used an older executable
from `build-linux-tests-release` after rebuilding `build-linux`. The benchmark
driver now rebuilds the **exact selected Release target** and reports its
storage-implementation identity. Always record the executable/build tree and
do not interpret an unchanged result as evidence until binary freshness is
established.

## Incremental replication and Merkle pruning

A measured one-record incremental update on a 2,000-record model originally
re-read **9,442 local objects at 50 commits** (~3 MB) and **202,398 objects
at 800 commits** (~75 MB) in an earlier ASan profile. The approach visited
historical data that the recipient already had. This showed that a small
transfer can still be expensive if validation or delta generation traverses
the complete local ancestry.

The current design uses trusted, already-verified local objects as import
boundaries and aligns old/new compressed Merkle tries to enumerate relevant
changes. In a focused 1-record delta regression, only **106 local object
reads at both 50 and 400 commits** were observed (previously roughly 9,400
and 100,000 respectively), with a **17-object delta bundle**. This is a
specific fixture/result, not a claim of constant-time synchronization in
all histories. Exported deltas may contain duplicate-existing content when
older revisions restore a previously deleted subtree, but imports do not
store identical content twice. More complex revision DAGs and large deltas
still need representative profiling.

Full-history export uses disk-backed deduplication and external sorting to
avoid an in-memory set of all reachable hashes. HTTP transfer staging and
internal native scratch have separate budgets; see [operations](operations.md).

## Durability and correctness evidence

- **Atomic branch visibility:** immutable objects are synchronized before
  branch-reference publication. An unacknowledged operation can nevertheless
  have published; on retry, reread the branch head first.
- **Pack corruption policy:** unpublished torn tails can be saved as evidence
  and truncated if safe; damage to published objects or in the middle of a
  pack fails closed. A lost/damaged *derived* sidecar is rebuildable.
- **Semantic safety:** model reference integrity, hydraulic domain validation
  and recursive criss-cross merge bases are separate responsibilities. Native
  generic merge correctness does not replace hydraulic-network validation.
- **Physical ENOSPC:** a disposable 32 MiB Linux tmpfs test forces real
  `ENOSPC` during commit, export and incremental import, then verifies recovery
  and retry. It is not an ext4/XFS power-loss test.
- **Process termination:** production-library SIGKILL/reopen tests cover
  acknowledged commit recovery, potential unacknowledged publication and
  complete named-bundle visibility. The filesystem/kernel itself remains live.
- **Backups:** an offline whole-service backup utility copies a locked
  multi-project root, independently verifies its repositories and publishes
  a new backup directory atomically. Online backup is not implemented.

**QEMU guest-cache-loss evidence (October 24, 2026):** A Debian 12 Linux
cloud guest running under QEMU/KVM with disposable virtio data disks completed
**four abrupt whole-VM kills on ext4 and four on XFS**. After each cut, the
same disk was remounted and the production native repository was fully
verified in a new process. Every acknowledged branch revision survived. In
three of four rounds on each filesystem, one additional complete but
unacknowledged revision was recovered; this is permitted because publication
can precede the client's acknowledgement. Both filesystems finished at
sequence **7**, and each independently reported `result: passed`. The operator's
JSON report was preserved outside the ephemeral VM and contains per-round
revision hashes. The run used guest-cache loss (`qemu-system-x86_64` killed by
`SIGKILL`, virtual disks with `cache=none`), **not power loss to the host SSD or
controller**. Results are specific to the observed guest, filesystem and write
schedule; a four-round run is not exhaustive crash-interleaving coverage.

Merely passing the QEMU-worker CTest does **not** execute the VM drill. The
optional offline evidence checker verifies the report's consistency and can
record hashes of supplied CTest and HTTP-soak logs; it does not execute any
workload or authenticate the operator's evidence. See [durability
qualification](durability-qualification.md).

## Service concurrency: observed load

The server uses per-project operation coordination and a bounded worker pool,
with independent projects rather than a global operation mutex. A developer
Linux Release test on October 24, 2026 completed a five-minute soak over
**three projects**, with **7,516 successful imports and 7,516 exports**, zero
admission-limit 503 responses, and **12 ms worst health request latency**
reported by the harness. The most recent operator-run suite passed **44/44 tests in Debug and Release**.
These figures describe the tested workload, not guaranteed throughput/latency
under arbitrary contention. Longer soaks and realistic hardware/slow-client
workloads remain necessary.

## Historical format regression baseline

An October 24, 2026 **frozen native pack** is retained with the actual
published branch references, original revision SHA-256 identities, a complete
bundle at an early revision, an incremental bundle and two full branch
bundles. Unlike a producer/consumer test against the same binary, the
regression uses immutable bytes emitted by an earlier build; it requires
later binaries to rebuild the derived index, recover historical values,
import historical transfers, preserve revision identities and reject a
corrupted bundle. This protects a concrete pre-release compatibility
baseline, not every legacy storage implementation or a public long-term
format guarantee. See [format compatibility](format-compatibility.md).

The frozen-format regression passes as part of the **44/44 Debug and Release**
operator-run suite. The QEMU evidence checker independently validates the
structure of the operator's eight ext4/XFS cut reports.

## Service-root restore and format upgrade qualification

The regression harness also has a four-project offline upgrade/restore drill:
it installs two frozen earlier-build pack repositories under current catalogue
identities, leaves their derived indexes absent, and mixes them with two
independently written repositories (one with over 1 MiB of user data). It
performs verified offline backup, independent restore, checks project IDs,
branch histories and payloads, writes into a restored historical pack, then
performs a second verified backup and restore. The original pre-upgrade root
is not rewritten by this test. This guards real migration workflows against
cross-project omissions; it is **not** evidence that an older executable can
read new writes or that all historic experimental formats are supported.

## What these results do not prove

The work does not yet establish durability after physical power loss, formal
linearizability, production latency SLOs, large-scale cold-cache throughput,
all-node corruption repair, supported long-term on-disk format compatibility,
or any authentication/security boundary for the HTTP service. The complete
list of qualification gates is in the [roadmap](roadmap.md).

## Measured five-minute Linux HTTP resource envelope

On October 25, 2026, the operator ran the **same Release server and workload
binaries twice** on an x86_64, eight-logical-CPU Linux host (kernel
`7.2.6-arch2-1`). Each run exercised six clients and three projects for
300 seconds. The exact binary SHA-256 values and complete run reports are
retained in the operator's `build-qualification` directories; report values
were supplied to this qualification record:

| Observation | Run A | Run B |
|---|---:|---:|
| Successful bundle imports | 7,505 | 7,459 |
| Successful bundle exports | 7,505 | 7,459 |
| Combined transfers/s | 50.03 | 49.73 |
| Admission 503 | 0 | 0 |
| Worst reported health latency | 4 ms | 3 ms |
| Server initial RSS | 27,500 KiB | 27,688 KiB |
| Server final RSS | 28,188 KiB | 28,384 KiB |
| Peak sampled server RSS | 28,752 KiB | 28,736 KiB |
| Peak server file descriptors | 37 | 37 |
| Peak server threads | 10 | 10 |
| Server CPU time (user + system) | 914.96 s | 909.89 s |

The two runs' combined transfer rates differ by about **0.6%**. First-to-last
server RSS changed by just **688 KiB** and **696 KiB**, respectively; peak
sampled RSS was approximately **28.1 MiB** in both. CPU consumption is about
**3.0 logical cores on average** during each 300-second run, not 915 seconds
of elapsed time and not a host-wide utilization measurement. The sampling
interval was 500 ms; the server was observed for approximately 299.8 seconds.
The unchanged descriptor peak, small short-run RSS change, zero admission
503s and reproducible transfer rate are positive **short-duration** evidence.

These two reports **do not establish absence of memory leaks** over hours,
quantify retained revision-history growth, capture host contention, or set a
production memory/CPU/latency guarantee. Five-minute CPU load, page cache,
disk and background workloads were not independently controlled. A separate
read-only report analyzer (`tools/analyze_http_resource_runs.py`) checks source
reports against their per-process CSV and saved log, and compares central
10–25% versus 75–90% server-RSS medians. It reports changes descriptively,
without inventing a leakage threshold. The first one-hour run and further
representative datasets are still outstanding.

## Current test baseline

The operator reported **44/44 CTest tests passing in both Debug and Release**
following the Linux resource-monitor race fix. This includes the four-project
historical-format upgrade/restore drill and the offline resource-monitor
regression. The eight-cut QEMU/KVM ext4/XFS drill was run separately from CTest.
