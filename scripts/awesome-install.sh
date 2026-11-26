#!/bin/sh
# Awesome-OS 1-Click GPT A/B Disk Installer & O(1) Slot Switcher
# Layout:
#   1. ESP         256 MiB  FAT32  (Limine UEFI + A/B kernels & limine.conf)
#   2. SLOT_A      512 MiB  ext4   (Verified OS Slot A)
#   3. SLOT_B      512 MiB  ext4   (Verified OS Slot B)
#   4. AWESOME_V11 100%     ext4   (Persistent v11 CAS: /var/lib/b8g/cas)
set -eu

PROG="awesome-install.sh"
MODE=""
TARGET_DEV=""
CONFIRM_YES=0
SWITCH_SLOT=""
ESP_MOUNT="/boot/efi"
SLOT_A_RELEASE="awesome-os/posix@v11-slot-a"
SLOT_B_RELEASE="awesome-os/posix@v11-slot-b"
KERNEL_SRC="/boot/vmlinuz"
INITRAMFS_SRC="/boot/initramfs.cpio.zst"
LIMINE_EFI_SRC="/usr/share/limine/BOOTX64.EFI"

usage() {
    cat <<EOF
Usage:
  $PROG --dry-run <device>
  $PROG --target <device> --yes [--kernel <path>] [--initramfs <path>] [--limine-efi <path>]
  $PROG --switch-slot <A|B> [--esp-mount /boot/efi]

Options:
  --dry-run <device>         Print exact parted, mkfs, and A/B limine.conf plan without modifying disk
  --target <device>          Target block device (e.g. /dev/sda, /dev/nvme0n1, /dev/vda)
  --yes                      Mandatory safety gate confirming destructive partitioning of --target
  --switch-slot <A|B>        Atomically rewrite default_entry in limine.conf for O(1) A/B switch/rollback
  --esp-mount <dir>          ESP mountpoint for --switch-slot (default: /boot/efi)
  --slot-a-release <label>   Slot A release label for limine.conf
  --slot-b-release <label>   Slot B release label for limine.conf
  --kernel <path>            Source kernel path (default: /boot/vmlinuz)
  --initramfs <path>         Source initramfs path (default: /boot/initramfs.cpio.zst)
  --limine-efi <path>        Source Limine UEFI binary (default: /usr/share/limine/BOOTX64.EFI)
  -h, --help                 Show this help message
EOF
}

die() {
    printf '%s: error: %s\n' "$PROG" "$1" >&2
    exit 1
}

validate_device() {
    dev="$1"
    case "$dev" in
        /dev/*) ;;
        *) die "device must be under /dev/ (got: '$dev')" ;;
    esac
    case "$dev" in
        *[!a-zA-Z0-9/_.-]*) die "device contains unsafe characters: '$dev'" ;;
    esac
}

part_path() {
    dev="$1"
    num="$2"
    case "$dev" in
        *[0-9]) printf '%sp%s' "$dev" "$num" ;;
        *)      printf '%s%s' "$dev" "$num" ;;
    esac
}

render_limine_conf() {
    active="$1"
    case "$active" in
        A) default_entry=1 ;;
        B) default_entry=2 ;;
        *) die "invalid slot '$active' (expected A or B)" ;;
    esac

    cat <<EOF
timeout: 2
serial: yes
default_entry: $default_entry

/Awesome-OS Slot A ($SLOT_A_RELEASE)
    protocol: linux
    kernel_path: boot():/boot/slot-a/vmlinuz
    cmdline: console=ttyS0,115200 console=tty0 quiet loglevel=3 awesome.slot=A awesome.persist=1 root=LABEL=SLOT_A
    module_path: boot():/boot/slot-a/initramfs.cpio.zst

/Awesome-OS Slot B ($SLOT_B_RELEASE)
    protocol: linux
    kernel_path: boot():/boot/slot-b/vmlinuz
    cmdline: console=ttyS0,115200 console=tty0 quiet loglevel=3 awesome.slot=B awesome.persist=1 root=LABEL=SLOT_B
    module_path: boot():/boot/slot-b/initramfs.cpio.zst
EOF
}

print_plan() {
    dev="$1"
    p1="$(part_path "$dev" 1)"
    p2="$(part_path "$dev" 2)"
    p3="$(part_path "$dev" 3)"
    p4="$(part_path "$dev" 4)"

    cat <<EOF
# Awesome-OS 1-Click GPT A/B Disk Plan (DRY-RUN — no changes made)
# Target device: $dev
# Partitions:
#   $p1  ESP          256MiB  FAT32  (1MiB..257MiB, flags: esp)
#   $p2  SLOT_A       512MiB  ext4   (257MiB..769MiB)
#   $p3  SLOT_B       512MiB  ext4   (769MiB..1281MiB)
#   $p4  AWESOME_V11  remain  ext4   (1281MiB..100%)

# 1. GPT Partitioning (parted)
parted -s "$dev" mklabel gpt
parted -s "$dev" mkpart ESP fat32 1MiB 257MiB
parted -s "$dev" set 1 esp on
parted -s "$dev" mkpart SLOT_A ext4 257MiB 769MiB
parted -s "$dev" mkpart SLOT_B ext4 769MiB 1281MiB
parted -s "$dev" mkpart AWESOME_V11 ext4 1281MiB 100%

# 2. Filesystem Creation (mkfs)
mkfs.fat -F 32 -n ESP "$p1"
mkfs.ext4 -F -L SLOT_A "$p2"
mkfs.ext4 -F -L SLOT_B "$p3"
mkfs.ext4 -F -L AWESOME_V11 "$p4"

# 3. Generated A/B limine.conf (Active: Slot A -> default_entry: 1)
EOF
    render_limine_conf "A"
}

do_install() {
    dev="$1"
    validate_device "$dev"
    if [ "$CONFIRM_YES" -ne 1 ]; then
        die "refusing to wipe '$dev' without explicit --yes safety flag"
    fi
    if [ ! -b "$dev" ]; then
        die "target '$dev' is not a block device"
    fi

    for tool in parted mkfs.fat mkfs.ext4 mount umount; do
        command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
    done

    p1="$(part_path "$dev" 1)"
    p2="$(part_path "$dev" 2)"
    p3="$(part_path "$dev" 3)"
    p4="$(part_path "$dev" 4)"

    printf '[awesome-install] Partitioning %s (GPT: ESP 256M + SLOT_A 512M + SLOT_B 512M + AWESOME_V11)...\n' "$dev"
    parted -s "$dev" mklabel gpt
    parted -s "$dev" mkpart ESP fat32 1MiB 257MiB
    parted -s "$dev" set 1 esp on
    parted -s "$dev" mkpart SLOT_A ext4 257MiB 769MiB
    parted -s "$dev" mkpart SLOT_B ext4 769MiB 1281MiB
    parted -s "$dev" mkpart AWESOME_V11 ext4 1281MiB 100%

    if command -v partprobe >/dev/null 2>&1; then
        partprobe "$dev" || true
    fi

    printf '[awesome-install] Formatting filesystems...\n'
    mkfs.fat -F 32 -n ESP "$p1"
    mkfs.ext4 -F -L SLOT_A "$p2"
    mkfs.ext4 -F -L SLOT_B "$p3"
    mkfs.ext4 -F -L AWESOME_V11 "$p4"

    work_dir="$(mktemp -d /tmp/awesome-install.XXXXXX)"
    cleanup() {
        umount "$work_dir/esp" 2>/dev/null || true
        umount "$work_dir/slot-a" 2>/dev/null || true
        umount "$work_dir/slot-b" 2>/dev/null || true
        umount "$work_dir/cas" 2>/dev/null || true
        rm -rf "$work_dir"
    }
    trap cleanup EXIT INT TERM

    mkdir -p "$work_dir/esp" "$work_dir/slot-a" "$work_dir/slot-b" "$work_dir/cas"
    mount "$p1" "$work_dir/esp"
    mount "$p2" "$work_dir/slot-a"
    mount "$p3" "$work_dir/slot-b"
    mount "$p4" "$work_dir/cas"

    printf '[awesome-install] Installing Limine UEFI + Slot A kernel/initramfs...\n'
    mkdir -p "$work_dir/esp/EFI/BOOT" "$work_dir/esp/boot/limine" "$work_dir/esp/boot/slot-a" "$work_dir/esp/boot/slot-b"
    if [ -f "$LIMINE_EFI_SRC" ]; then
        cp "$LIMINE_EFI_SRC" "$work_dir/esp/EFI/BOOT/BOOTX64.EFI"
    fi
    if [ -f "$KERNEL_SRC" ]; then
        cp "$KERNEL_SRC" "$work_dir/esp/boot/slot-a/vmlinuz"
        mkdir -p "$work_dir/slot-a/boot"
        cp "$KERNEL_SRC" "$work_dir/slot-a/boot/vmlinuz"
    fi
    if [ -f "$INITRAMFS_SRC" ]; then
        cp "$INITRAMFS_SRC" "$work_dir/esp/boot/slot-a/initramfs.cpio.zst"
        mkdir -p "$work_dir/slot-a/boot"
        cp "$INITRAMFS_SRC" "$work_dir/slot-a/boot/initramfs.cpio.zst"
    fi

    # Write A/B limine.conf atomically
    render_limine_conf "A" > "$work_dir/esp/boot/limine/limine.conf.tmp"
    mv "$work_dir/esp/boot/limine/limine.conf.tmp" "$work_dir/esp/boot/limine/limine.conf"
    cp "$work_dir/esp/boot/limine/limine.conf" "$work_dir/esp/limine.conf"

    # Initialize Slot A, Slot B, and AWESOME_V11 persistent CAS tree
    mkdir -p "$work_dir/slot-a/etc" "$work_dir/slot-b/boot" "$work_dir/slot-b/etc"
    printf 'slot=A\nrelease=%s\n' "$SLOT_A_RELEASE" > "$work_dir/slot-a/etc/awesome-slot"
    printf 'slot=B\nrelease=%s\n' "$SLOT_B_RELEASE" > "$work_dir/slot-b/etc/awesome-slot"
    mkdir -p "$work_dir/cas/v11/files" "$work_dir/cas/v11/links"

    sync
    printf '[awesome-install] Install complete on %s (active slot: A)\n' "$dev"
}

do_switch_slot() {
    slot="$1"
    esp="$2"

    case "$slot" in
        A) entry=1 ;;
        B) entry=2 ;;
        *) die "invalid slot '$slot' (expected A or B)" ;;
    esac

    conf=""
    if [ -f "$esp/boot/limine/limine.conf" ]; then
        conf="$esp/boot/limine/limine.conf"
    elif [ -f "$esp/limine.conf" ]; then
        conf="$esp/limine.conf"
    else
        die "limine.conf not found under '$esp/boot/limine/limine.conf' or '$esp/limine.conf'"
    fi

    tmp="${conf}.tmp.$$"
    awk -v target="$entry" '
        BEGIN { replaced = 0; inserted = 0 }
        /^[[:space:]]*default_entry[[:space:]]*:/ {
            if (!replaced) {
                print "default_entry: " target
                replaced = 1
            }
            next
        }
        /^[[:space:]]*\// && !replaced && !inserted {
            print "default_entry: " target
            print ""
            inserted = 1
        }
        { print }
        END {
            if (!replaced && !inserted) {
                print "default_entry: " target
            }
        }
    ' "$conf" > "$tmp"

    mv "$tmp" "$conf"
    # Keep root ESP mirror in sync if both exist
    if [ "$conf" = "$esp/boot/limine/limine.conf" ] && [ -f "$esp/limine.conf" ]; then
        cp "$conf" "$esp/limine.conf.tmp.$$"
        mv "$esp/limine.conf.tmp.$$" "$esp/limine.conf"
    fi
    sync 2>/dev/null || true
    printf '[awesome-install] Switched active boot slot to %s (default_entry: %s) in %s\n' "$slot" "$entry" "$conf"
}

if [ "$#" -eq 0 ]; then
    usage
    exit 1
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run)
            [ "$#" -ge 2 ] || die "--dry-run requires a <device> argument"
            MODE="dry-run"
            TARGET_DEV="$2"
            shift 2
            ;;
        --target)
            [ "$#" -ge 2 ] || die "--target requires a <device> argument"
            MODE="install"
            TARGET_DEV="$2"
            shift 2
            ;;
        --yes)
            CONFIRM_YES=1
            shift
            ;;
        --switch-slot)
            [ "$#" -ge 2 ] || die "--switch-slot requires <A|B>"
            MODE="switch-slot"
            SWITCH_SLOT="$2"
            shift 2
            ;;
        --esp-mount)
            [ "$#" -ge 2 ] || die "--esp-mount requires a directory path"
            ESP_MOUNT="$2"
            shift 2
            ;;
        --slot-a-release)
            [ "$#" -ge 2 ] || die "--slot-a-release requires a value"
            SLOT_A_RELEASE="$2"
            shift 2
            ;;
        --slot-b-release)
            [ "$#" -ge 2 ] || die "--slot-b-release requires a value"
            SLOT_B_RELEASE="$2"
            shift 2
            ;;
        --kernel)
            [ "$#" -ge 2 ] || die "--kernel requires a path"
            KERNEL_SRC="$2"
            shift 2
            ;;
        --initramfs)
            [ "$#" -ge 2 ] || die "--initramfs requires a path"
            INITRAMFS_SRC="$2"
            shift 2
            ;;
        --limine-efi)
            [ "$#" -ge 2 ] || die "--limine-efi requires a path"
            LIMINE_EFI_SRC="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

case "$MODE" in
    dry-run)
        validate_device "$TARGET_DEV"
        print_plan "$TARGET_DEV"
        ;;
    install)
        do_install "$TARGET_DEV"
        ;;
    switch-slot)
        do_switch_slot "$SWITCH_SLOT" "$ESP_MOUNT"
        ;;
    *)
        usage
        exit 1
        ;;
esac
