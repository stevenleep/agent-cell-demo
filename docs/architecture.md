# Minimal Agent Cell: phase-1 design

## Product boundary

The only useful way to be materially lighter than a mature microVM is to support less. `agent-cell` is an immutable compute appliance, not a small cloud VM. The agent may run commands from a prebuilt guest image, send bounded input, receive bounded output, and be killed on a deadline. Dynamic disks, arbitrary kernels, TAP networking, hotplug, snapshots, SSH, package installation, and a REST server are outside the demo.

Firecracker publicly targets at most 5 MiB VMM overhead and at most 125 ms to guest init for its reference 1-vCPU/128-MiB configuration. The demo targets **under 2 MiB private VMM RSS** (guest RAM excluded), **under 30 ms cold start**, and **under 3 ms warm reset** on a pinned Linux bare-metal benchmark host. These are hypotheses until measured; CI must not claim them before the benchmark passes.

## Architecture

```text
Agent SDK ── Unix SOCK_SEQPACKET ── supervisor
                                        │ clone3 + cgroup + seccomp
                                        ▼
                                  one tiny VMM process
                                  ┌─────────────────┐
                                  │ KVM VM + 1 vCPU │
                                  │ guest RAM       │
                                  │ 64 KiB mailbox  │◄── doorbell (ioeventfd/irqfd)
                                  └────────┬────────┘
                                           │
                                  Linux + builtin initramfs
                                  /init → exec tool → reply → halt
```

The supervisor never enters the guest fast path. It authenticates a local caller, validates a bounded binary request, allocates a cgroup, starts a cell, enforces the wall-clock deadline with `pidfd`, and destroys the cgroup. One cell handles one job.

## Minimal VMM boot sequence

1. Open `/dev/kvm`, verify API version and required capabilities.
2. `KVM_CREATE_VM`; map one anonymous, `MADV_DONTDUMP` guest-memory region and register one memory slot.
3. Copy a measured, fixed kernel containing its initramfs. On arm64, place a tiny DTB describing CPU, RAM, PSCI, architected timer, and the mailbox MMIO range. On x86_64, use direct bzImage boot params.
4. Create one vCPU, initialize registers, and install two eventfds for the mailbox doorbells.
5. Enter `KVM_RUN`. Handle only shutdown, system event, MMIO mailbox access, and fatal exits. Any unexpected exit kills the cell.

Start with arm64 because the virtual platform has less legacy setup. Do not use a full virtio transport for the demo: the custom mailbox is smaller. Before production, fuzz this ABI and decide whether the maintenance/security cost is preferable to standardized virtio-vsock.

## Mailbox ABI

The first 24 bytes are the explicitly encoded `cell_mailbox_header` in `include/cell.h`, followed by a length-delimited binary payload. The wire format never relies on native struct layout or host endianness. The shared page has explicit ownership states: `EMPTY → HOST_READY → GUEST_BUSY → GUEST_DONE → EMPTY`. Every transition uses release/acquire ordering. Maximum argv is 64 KiB and maximum stdin/output is 1 MiB in the control protocol; data larger than the mailbox is chunked with monotonically increasing sequence numbers. Invalid magic, version, length, state, or sequence terminates the VM.

For the first executable guest, encode payload fields manually as little-endian length-prefixed bytes. Avoid JSON, protobuf, an HTTP parser, and guest libc on the trusted boot path.

The boot MVP uses x86 debug port `0xE9` or arm64 completion MMIO at `0x10000000`
for its fixed readiness marker, followed by reset/PSCI completion. This avoids
carrying a general device model before the mailbox exists. These are temporary
bootstrap ABIs and disappear when the mailbox request/reply state machine is
implemented.

## Guest image

Build a monolithic Linux kernel with KVM paravirtualization, devtmpfs, procfs, tmpfs, seccomp, namespaces, and only the drivers required for the CPU/timer/console/mailbox. Embed a static `/init` and the chosen agent tool bundle in initramfs. `/init` mounts proc/dev/tmp, reads one request, applies rlimits and guest seccomp, forks/execs the tool, captures bounded output, reports status, zeroes the writable workspace, and powers off.

For a generic coding agent, read-only tools dominate image size. A later phase should expose a shared read-only squashfs or DAX image, but adding that device is intentionally deferred until measurements prove initramfs duplication is the limiting cost.

## Isolation layers

KVM is the tenant boundary, but the VMM is still hostile-input-facing. Run each VMM as a dedicated unprivileged uid in new user, pid, mount, network, and IPC namespaces; drop every capability; expose only `/dev/kvm`, the kernel image fd, sealed memfds, eventfds, and its control socket; apply a syscall allowlist; enforce `memory.max`, `pids.max`, and `cpu.max`; use `PR_SET_NO_NEW_PRIVS`; close inherited fds; and kill via pidfd plus cgroup cleanup.

No guest network is the safest demo default. If an Agent needs internet, put an authenticated, policy-enforcing HTTP fetch service on the host side of the mailbox. This avoids a guest NIC and gives an auditable egress allowlist, response-size limit, and credential boundary.

## Milestones

1. **Protocol:** validation, mailbox layout, lifecycle IDs, tests, and benchmark harness.
2. **KVM smoke (implemented and verified on aarch64; implemented on x86_64):** create a VM and vCPU, collect guest output from an I/O/MMIO exit, and tear down every mapping and fd.
3. **Boot (implemented and verified on aarch64; implemented on x86_64):** direct-boot Linux reaches embedded `/init` without firmware or disk.
4. **Execute MVP (implemented and verified on aarch64):** static init executes one embedded workload, checks its status, emits a completion marker, and resets within a deadline.
5. **Jail:** namespaces, cgroup v2, seccomp, uid separation, pidfd teardown, and negative security tests.
6. **Measure:** compare identical kernels/jobs against Firecracker; publish raw data and `/proc` accounting definitions.
7. **Warm pool:** keep prebooted cells at the mailbox wait point; discard rather than reuse after a tenant job.

## Go/no-go checks

- If arbitrary OCI images, general TCP, or writable persistent disks become requirements, use Firecracker instead of growing this VMM.
- If same-kernel isolation is acceptable for the trust model, a namespace/seccomp/cgroup sandbox will be dramatically smaller than any VM; make that a separate product tier.
- If custom-device fuzzing and kernel maintenance cannot be funded, use Firecracker with a reduced guest image. A smaller codebase is not automatically a safer sandbox.
