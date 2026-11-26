# Awesome-OS POSIX (`awesome-os/posix`)

Ultra-minimal headless terminal & container OS substrate powered by static musl C11 PID 1 (`B8G_ABI_MAGIC_V3 = 0x33564742`), stateless RAM `v11` CAS netboot-first storage, dynamic NIC discovery, and dual `tty0` + `ttyS0` interactive shells — with **zero GUI**.

## The Awesome-OS 3-Repo OS Triad

Awesome-OS separates minimal bootable compute, graphical desktop appliances, and fleet orchestration into three focused repositories:

| Repository | Role | Target Profile |
| :--- | :--- | :--- |
| **[`awesome-os/posix`](https://github.com/awesome-os/posix)** | Ultra-minimal headless terminal & container OS substrate (`system://local/terminal/awesome-os-posix`) | Bare-metal servers, microVMs, netboot nodes, serial/VGA terminal hosts (Zero GUI) |
| **[`awesome-os/desktop`](https://github.com/awesome-os/desktop)** | Graphical Wayland / `/dev/fb0` desktop foundation (`system://local/appliance/awesome-os-desktop`) | Workstations & appliances running Omarchy, Noctalia, ChonkStep, or Sectium |
| **[`awesome-os/host-manager`](https://github.com/awesome-os/host-manager)** | Fleet control plane & host lifecycle orchestrator | Provisioning, netboot/WoL management, and remote orchestration across POSIX & Desktop nodes |

## Architecture Highlights

- **Static musl PID 1 (`src/init.c`):** Single-file C11 init verifying `B8G_ABI_MAGIC_V3` (`0x33564742`), mounting `/proc`, `/sys`, `/dev`, `/tmp`, and `/run`, and reaping zombies cleanly.
- **Stateless RAM `v11` CAS (Netboot-First):** Mounts `/var/lib/b8g/cas` on `tmpfs` (`v11/files/`, `v11/links/`) by default for immutable RAM/PXE execution, with opt-in persistent `ext4` (`AWESOME_V11` / `B8G_CAS` labels) when booted with `awesome.persist=1`.
- **Dual-Console Logging & Shells (`tty0` + `ttyS0`):** Deduplicates console output via `st_rdev` comparison so VGA/UEFI and serial logs never double-echo, and supervises independent `/bin/sh -l` sessions on both `/dev/tty0` and `/dev/ttyS0`.
- **Dynamic NIC Discovery:** Scans `/sys/class/net`, brings up `lo` and all detected network interfaces, and starts background DHCP (`udhcpc`) automatically.
- **CI Self-Test (`awesome.selftest=1`):** Emits `[AWESOME-OS-POSIX] PID 1 READY (B8G_ABI_MAGIC_V3=0x33564742)` and `[AWESOME-OS-POSIX] SELFTEST PASS`, then cleanly powers off for hermetic QEMU verification.

## Cross-Compilation

Compile `src/init.c` natively without WSL using `zig cc` (or `musl-gcc` on Linux):

```powershell
# x86_64 static musl
zig cc -target x86_64-linux-musl -static -Os -s -Wall -Wextra -Werror src/init.c -o dist/init-x86_64

# aarch64 static musl
zig cc -target aarch64-linux-musl -static -Os -s -Wall -Wextra -Werror src/init.c -o dist/init-aarch64
```

## Release Artifacts

Every push to `main` builds, QEMU-tests (Direct Kernel + Hybrid Limine v8 ISO), and publishes:

- `awesome-os-posix-x86_64.iso` — Hybrid BIOS/UEFI Limine v8 bootable ISO
- `vmlinuz-x86_64` — Linux x86_64 kernel image for netboot/QEMU
- `initramfs-x86_64.cpio.zst` — Zstd-compressed initramfs with static PID 1, BusyBox, and `awesome-os/coreutils`
- `init-x86_64` — Standalone static musl PID 1 binary
