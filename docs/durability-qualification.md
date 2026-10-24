# VM filesystem durability qualification and sustained HTTP soak

These are **opt-in**, destructive-on-disposable-data qualification drills, not
normal unit tests. They do not modify the AOWIS native pack/bundle formats.
The VM drill runs the production native library, not the fault-injected library.
Never point the tools at real repositories or physical host disks.

## One-command Docker workflow (recommended)

The top-level `qualify_vm_powercut_docker.sh` is a containerized **launcher for QEMU**, not a replacement for QEMU. Docker supplies the guest-compatible build tools, QEMU binaries and cloud-image provisioning. The actual durability operation still boots a full Linux guest and abruptly kills the entire QEMU process. **Only a working
Docker installation is required on the Linux x86_64 host.** It builds the
Debian 12 qualification image (cached across runs), compiles the production
worker **inside the Debian 12 container**, downloads and SHA-512-verifies the
official Debian 12 genericcloud guest image, generates a new disposable
cloud-init/SSH identity, and launches `tools/qualify_vm_powercut.py`. The
**actual** drill repeatedly kills QEMU and reboots its disposable data disk.
It is not the QEMU-free CTest worker smoke test.

Run from the repository root:

```bash
./qualify_vm_powercut_docker.sh --rounds 4
```

For the more thorough run (24 power cuts for each filesystem):

```bash
./qualify_vm_powercut_docker.sh --rounds 24 --filesystems ext4 xfs
```

You can prepare the reusable container separately without starting QEMU:

```bash
./tools/prepare_vm_powercut_docker.sh
```

The script automatically passes `/dev/kvm` if it is usable by your account;
otherwise QEMU uses software emulation, which can be **much slower**. The
host is not required to install QEMU, `mkfs`, a compiler or `cloud-localds`
directly. Docker uses **host networking by default** for both image building and
qualification execution. This avoids Docker bridge DNS failures when Debian
packages or the guest image must be downloaded, and provides network access
for cloud-init inside the QEMU guest. The VM's SSH port forwarding binds only
to 127.0.0.1. Host networking gives the container access to the host network
namespace; use only in a trusted qualification environment. Set
`AOWIS_VM_DOCKER_NETWORK=bridge` to explicitly use Docker bridge networking
if your host supports functional DNS there. Neither mode requires
`--privileged`, host block disks or a writable source checkout.

### Docker DNS troubleshooting

A Docker build error such as `Temporary failure resolving deb.debian.org`
during `apt-get update` means the Docker **build container** cannot resolve
Debian package servers; it is not a storage-engine failure and no QEMU test
has run. The wrapper now uses `docker build --network host` (including with
the legacy Docker builder) rather than relying on bridge DNS. The same network
choice applies to `docker run`. If this still fails, first check that the
host itself resolves `deb.debian.org`, Docker's proxy configuration and
local firewall/network policy. Fix the underlying Docker/host networking
before treating any qualification attempt as an executed test. Do not silently
skip package installation or mistake the QEMU-free CTest worker smoke for a
successful VM drill. The explicit bridge option is primarily useful on hosts
whose Docker bridge DNS is already functional.

The official Debian cloud image and compiled worker are cached in
`~/.cache/aowis-db-powercut/`. Logs and the JSON qualification reports are
saved under `build-vm-powercut/run-*/`. First execution requires internet
access both to build the container/download the guest image and for the guest
to install ext4/XFS utilities using Debian package repositories. Subsequent
runs reuse the verified image and build cache; `--refresh-image` explicitly
updates the cached guest. A first-time run can download hundreds of megabytes.
A full run may require gigabytes of scratch space for VM overlays and images.

The Docker container runs with your UID/GID. Its image build also creates a
matching account entry when that numeric UID is absent from Debian's
`/etc/passwd`; this is required by OpenSSH's `ssh-keygen`. Merely passing
`docker run --user UID:GID` is insufficient because it does not create the
account. The runner checks this immediately, before downloading an image or
compiling the worker. No production repositories or real host block devices
are mounted. VM data disks are **new sparse files**.
The test refuses unexpected in-guest disk layouts before formatting. On
failure, isolated evidence (which may include an ephemeral SSH private key)
is retained under `build-vm-powercut/` for inspection; protect or delete it
after diagnosis. On success, private keys and cloud-init artifacts are removed
and the JSON result is retained. To override locations, set `AOWIS_VM_CACHE`
or `AOWIS_VM_RESULTS` in the shell environment.

A successful Docker-launched QEMU run qualifies *guest cache loss on disposable ext4/XFS
virtual disks*. It does **not** prove physical SSD/controller power-loss
semantics. The manual runner below remains available for custom distributions
and guest images.

## 1. Manual guest-cache-loss drill (QEMU, ext4 and XFS)

`tools/qualify_vm_powercut.py` boots an x86_64 Linux guest from a **new qcow2
overlay** of a pre-existing boot image and attaches a **new sparse raw data
disk** at `/dev/vdb`. Nothing is formatted or mounted on the host. The test
checks that `/dev/vda` is the guest boot disk, `/dev/vdb` is a separate disk,
and the AOWIS test directory really resolves to its mount before writing.

Each round uses the following protocol:

1. Boot Linux, mount the disposable ext4/XFS data disk at `/mnt/aowis-powercut`.
   Stage the guest-compatible worker executable on **every** boot. Its installation
   is on the guest OS overlay, not the data disk being tested; unsynchronized OS
   writes can disappear when QEMU is killed. Re-staging the test executable is
   separate from recovery of the AOWIS repository and must not repair or
   reinitialize that repository.
2. On first boot, initialize the native repository. On later boots, reopen it
   in a new process and run full `Repository::verifyRepository()`.
3. Start an independent write worker. After every synchronous commit it emits
   an `ACK <sequence> <revision-sha256>`, then waits for the host's `GO`.
4. The host confirms the ACK, sends `GO` to permit **one** more potentially
   in-flight commit, then kills **QEMU itself** with `SIGKILL` (no guest sync,
   unmount or orderly shutdown).
5. Reboot the same guest overlay and data disk. Linux replays the filesystem
   journal on mounting. Verify the entire native repository and check the
   recovered counter is either the last acknowledged sequence or exactly one
   greater (an unacknowledged publication). Continue from the recovered head.
6. Repeat with a **separate freshly created disk** for the other filesystem.

### Requirements

- Linux x86_64 host with `qemu-system-x86_64`, `qemu-img`, OpenSSH client and
  optionally accessible `/dev/kvm` for acceleration. KVM is strongly preferred.
- A **pre-existing, user-supplied** bootable x86_64 Linux qcow2/raw image with
  SSH server, util-linux (`lsblk`, `mount`, `findmnt`), `mkfs.ext4`, `mkfs.xfs`,
  and the required C++ runtime. The image must boot from virtio `/dev/vda`.
- SSH access via a dedicated test key, as `root` or an account with passwordless
  `sudo`; SSH forwards to loopback only. Test binaries must be ABI-compatible
  with the guest distro. No production storage or host block-device privileges
  are needed.
- Enough free space for one OS overlay and a sparse data disk for each test
  filesystem. Each run uses a **randomized private directory**.

A cloud-init-compatible Fedora (or other matching-distro) cloud image is one
possible guest base. If you have `cloud-localds`, you can create a seed disk for
SSH test-user provisioning. For example, after creating a **new throwaway**
SSH key:

```bash
ssh-keygen -t ed25519 -N '' -f "$HOME/.ssh/aowis-vm-test"
cat > /tmp/aowis-vm-user-data <<EOF
#cloud-config
users:
  - name: aowis-test
    groups: [wheel]
    shell: /bin/bash
    sudo: 'ALL=(ALL) NOPASSWD:ALL'
    ssh_authorized_keys:
      - $(cat "$HOME/.ssh/aowis-vm-test.pub")
packages: [e2fsprogs, xfsprogs, util-linux]
EOF
cloud-localds /tmp/aowis-vm-seed.img /tmp/aowis-vm-user-data
```

Use a cloud image containing a working SSH server and cloud-init, with guest
package repositories available if the necessary formatting tools are not
already installed. Do not reuse a real SSH identity or production VM disk.
The seed is attached **read-only** as the guest's third virtio disk, and
`cloud-init status --wait` is checked before starting the first filesystem
operation. A guest may require several minutes on the first boot to provision.

Build the production worker (the test build includes the same production native
store; unlike the injected-fault tests it does not use special hooks):

```bash
cmake --build build-linux-tests-release --target aowis-native-vm-powercut-worker --parallel
```

Run the ext4/XFS drill, using an existing downloaded, bootable **test** guest
image and your guest test credentials:

```bash
work_root="$(mktemp -d /tmp/aowis-vm-qualification.XXXXXXXX)"
python3 tools/qualify_vm_powercut.py \
  --guest-image "$HOME/vm-images/fedora-test-base.qcow2" \
  --cloud-init-seed /tmp/aowis-vm-seed.img \
  --ssh-key "$HOME/.ssh/aowis-vm-test" \
  --guest-user aowis-test \
  --worker ./build-linux-tests-release/aowis-native-vm-powercut-worker \
  --filesystems ext4 xfs --rounds 24 --kvm \
  --work-root "$work_root"
```

Without cloud-init, omit `--cloud-init-seed` and use a **pre-provisioned** SSH
account and filesystem tools. Omit `--kvm` when acceleration is unavailable
(TCG is much slower). `--boot-timeout` can be raised for slow guest startups.

On success, the randomized overlay and raw data disks are deleted; the host
prints a per-round summary. On failure, the work directory is preserved,
including `serial.log`, SSH worker stderr, and the **disposable** raw image.
Keep these as evidence and inspect before cleanup. A failed drill must not be
reported as a pass. This drill is not in default CTest, because provisioning
QEMU, a guest and suitable test keys is explicitly an operator action.

**Critical evidence limitation:** killing the guest VM drops guest RAM and
its kernel page cache, and `cache=none` avoids ordinary host page-cache writes
for the virtual disks. It does **not** power off the host storage controller or
prove behavior under torn sector writes, lying flush implementations, device
cache loss, or a physical server's sudden power removal. Real hardware and
hypervisor/device-cache qualification remain separate deployment requirements.
Successful verification establishes observed consistency only for the
specified guest, filesystem, virtio device and tested power-cut timings.

## 2. Sustained multi-project HTTP soak

The existing `aowis-native-http-transfer-stress-tests` binary has an **opt-in**
mode that holds **one HTTP server process** for the requested duration. Six
concurrent workers continuously import immutable multi-MiB native bundles into
new branches across three projects, export them, byte-compare bundle contents,
read the branch heads and continuously probe `/api/v1/health`. Legitimate
admission-budget 503 responses are retried and counted. When time expires,
the server is stopped and **all** project repositories are reopened, fully
verified and checked against the completed import count. Failure preserves the
fixture directory and server log for investigation.

```bash
./compile_linux_tests.sh --both
./build-linux-tests-release/aowis-native-http-transfer-stress-tests \
  --soak-seconds 3600 ./build-linux-tests-release/aowis-server-db
```

A short developer smoke can use `--soak-seconds 30`. This sustained mode is
not registered in normal CTest: test duration is controlled explicitly. Its
reported import/export counts, staging-busy responses and worst health time
are **measurements**, not promised production SLOs. Run on actual target
hardware with external RSS/CPU/I/O monitoring and representative network
conditions before defining production capacity targets.

It is not a full replication conflict/fuzz test, does not emulate a failing
physical device, and does not replace a real power-loss drill. Additional
production qualification should cover repeated cold-cache runs, larger
replica divergence/merging, degraded devices and more varied model workloads.
