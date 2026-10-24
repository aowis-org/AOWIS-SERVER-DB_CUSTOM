# VM filesystem durability qualification and sustained HTTP soak

These are **opt-in**, destructive-on-disposable-data qualification drills, not
normal unit tests. They do not modify the AOWIS native pack/bundle formats.
The VM drill runs the production native library, not the fault-injected library.
Never point the tools at real repositories or physical host disks.

## 1. Guest-cache-loss power-cut drill (QEMU, ext4 and XFS)

`tools/qualify_vm_powercut.py` boots an x86_64 Linux guest from a **new qcow2
overlay** of a pre-existing boot image and attaches a **new sparse raw data
disk** at `/dev/vdb`. Nothing is formatted or mounted on the host. The test
checks that `/dev/vda` is the guest boot disk, `/dev/vdb` is a separate disk,
and the AOWIS test directory really resolves to its mount before writing.

Each round uses the following protocol:

1. Boot Linux, mount the disposable ext4/XFS data disk at `/mnt/aowis-powercut`.
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
