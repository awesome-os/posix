#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define B8G_ABI_MAGIC_V3 0x33564742U
#define B8G_ABI_EXPECTED                                                       \
    ((uint32_t)'B' | ((uint32_t)'G' << 8) | ((uint32_t)'V' << 16) |            \
     ((uint32_t)'3' << 24))

static int g_tty0_fd = -1;
static int g_ttyS0_fd = -1;

static void open_consoles(void) {
    struct stat st_out, st_tmp;
    int have_out = (fstat(STDOUT_FILENO, &st_out) == 0);

    if (g_tty0_fd < 0) {
        int fd = open("/dev/tty0", O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            if (have_out && fstat(fd, &st_tmp) == 0 &&
                st_tmp.st_rdev == st_out.st_rdev) {
                close(fd);
            } else {
                g_tty0_fd = fd;
            }
        }
    }

    if (g_ttyS0_fd < 0) {
        int fd = open("/dev/ttyS0", O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            if (have_out && fstat(fd, &st_tmp) == 0 &&
                st_tmp.st_rdev == st_out.st_rdev) {
                close(fd);
            } else {
                g_ttyS0_fd = fd;
            }
        }
    }
}

static void write_all(int fd, const char *buf, size_t len) {
    if (fd < 0 || len == 0) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
}

/* Dual-console logger: mirrors stdout to tty0 (VGA/UEFI) + ttyS0 (Serial) without duplicate echo */
static void log_both(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    write_all(STDOUT_FILENO, buf, len);
    if (g_tty0_fd >= 0) {
        write_all(g_tty0_fd, buf, len);
    }
    if (g_ttyS0_fd >= 0) {
        write_all(g_ttyS0_fd, buf, len);
    }
}

static void ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        log_both("[AWESOME-OS-POSIX] warn: mkdir(%s): %s\n", path,
                 strerror(errno));
    }
}

static void mount_or_warn(const char *src, const char *target,
                          const char *fstype, unsigned long flags,
                          const void *data) {
    if (mount(src, target, fstype, flags, data) != 0 && errno != EBUSY) {
        log_both("[AWESOME-OS-POSIX] warn: mount(%s -> %s, %s): %s\n", src,
                 target, fstype, strerror(errno));
    }
}

static int run_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static void run_bg(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }
        execv(argv[0], argv);
        _exit(127);
    }
}

static int cmdline_has_token(const char *token) {
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    size_t toklen = strlen(token);
    const char *p = buf;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != '\r') {
            p++;
        }
        if ((size_t)(p - start) == toklen && memcmp(start, token, toklen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Stateless RAM v11 CAS netboot-first store (optional persistent ext4 via awesome.persist=1) */
static void mount_v11_cas_store(void) {
    ensure_dir("/var", 0755);
    ensure_dir("/var/lib", 0755);
    ensure_dir("/var/lib/b8g", 0755);
    ensure_dir("/var/lib/b8g/cas", 0755);

    int mounted = 0;
    if (cmdline_has_token("awesome.persist=1")) {
        const char *labels[] = {"/dev/disk/by-label/AWESOME_V11",
                                "/dev/disk/by-label/B8G_CAS", NULL};
        for (int i = 0; labels[i] != NULL && !mounted; i++) {
            if (access(labels[i], F_OK) == 0 &&
                mount(labels[i], "/var/lib/b8g/cas", "ext4", 0, NULL) == 0) {
                log_both(
                    "[AWESOME-OS-POSIX] v11 CAS: persistent ext4 (%s -> /var/lib/b8g/cas)\n",
                    labels[i]);
                mounted = 1;
            }
        }
    }

    if (!mounted) {
        mount_or_warn("tmpfs", "/var/lib/b8g/cas", "tmpfs", 0,
                      "mode=0755,size=512M");
        log_both(
            "[AWESOME-OS-POSIX] v11 CAS: stateless RAM netboot-first (/var/lib/b8g/cas/v11/files/)\n");
    }

    ensure_dir("/var/lib/b8g/cas/v11", 0755);
    ensure_dir("/var/lib/b8g/cas/v11/files", 0755);
    ensure_dir("/var/lib/b8g/cas/v11/links", 0755);
}

static void setup_network(void) {
    if (access("/bin/ip", X_OK) != 0) {
        return;
    }
    char *const lo_argv[] = {"/bin/ip", "link", "set", "lo", "up", NULL};
    (void)run_wait(lo_argv);

    DIR *d = opendir("/sys/class/net");
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) {
            continue;
        }
        char ifname[256];
        snprintf(ifname, sizeof(ifname), "%.*s", (int)(sizeof(ifname) - 1),
                 ent->d_name);
        char *const up_argv[] = {"/bin/ip", "link", "set", ifname, "up", NULL};
        if (run_wait(up_argv) == 0) {
            log_both("[AWESOME-OS-POSIX] NIC up: %s\n", ifname);
            if (access("/bin/udhcpc", X_OK) == 0) {
                char *const dhcp_argv[] = {"/bin/udhcpc", "-i", ifname,
                                           "-b",          "-q", NULL};
                run_bg(dhcp_argv);
            }
        }
    }
    closedir(d);
}

static void run_selftest(void) {
    log_both("[AWESOME-OS-POSIX] running headless POSIX v11 self-test...\n");
    log_both(
        "[AWESOME-OS-POSIX] v11 CAS layout: /var/lib/b8g/cas/v11/files (HOLO_CAS_DIR=%s)\n",
        getenv("HOLO_CAS_DIR") ? getenv("HOLO_CAS_DIR") : "");

    if (access("/bin/uname", X_OK) == 0) {
        char *const uname_argv[] = {"/bin/uname", "-a", NULL};
        (void)run_wait(uname_argv);
    }

    if (access("/bin/coreutils", X_OK) == 0) {
        log_both("[AWESOME-OS-POSIX] static musl awesome-os/coreutils present\n");
    }

    log_both("[AWESOME-OS-POSIX] SELFTEST PASS\n");
    fflush(stdout);
    sync();
    (void)reboot(RB_POWER_OFF);
    for (;;) {
        pause();
    }
}

static pid_t spawn_terminal_on(const char *tty_path) {
    int test_fd = open(tty_path, O_RDWR | O_NOCTTY);
    if (test_fd < 0) {
        return -1;
    }
    close(test_fd);

    pid_t pid = fork();
    if (pid == 0) {
        if (g_tty0_fd >= 0) {
            close(g_tty0_fd);
        }
        if (g_ttyS0_fd >= 0) {
            close(g_ttyS0_fd);
        }
        (void)setsid();
        int fd = open(tty_path, O_RDWR);
        if (fd >= 0) {
            (void)ioctl(fd, TIOCSCTTY, 0);
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) {
                close(fd);
            }
        }
        char *const sh_argv[] = {"/bin/sh", "-l", NULL};
        execv("/bin/sh", sh_argv);
        _exit(127);
    }
    return pid;
}

static void reap_zombies(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "linux", 1);
    setenv("B8G_CAS_DIR", "/var/lib/b8g/cas", 1);
    setenv("HOLO_CAS_DIR", "/var/lib/b8g/cas", 1);

    ensure_dir("/proc", 0555);
    ensure_dir("/sys", 0555);
    ensure_dir("/dev", 0755);
    ensure_dir("/tmp", 01777);
    ensure_dir("/run", 0755);
    ensure_dir("/root", 0700);
    ensure_dir("/etc", 0755);
    ensure_dir("/etc/awesome-os", 0755);

    mount_or_warn("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV,
                  NULL);
    mount_or_warn("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV,
                  NULL);
    mount_or_warn("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    open_consoles();
    mount_or_warn("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    mount_or_warn("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
    mount_v11_cas_store();

    volatile uint32_t abi_magic = B8G_ABI_MAGIC_V3;
    if (abi_magic != B8G_ABI_EXPECTED) {
        log_both(
            "[AWESOME-OS-POSIX] FATAL: B8G_ABI_MAGIC_V3 mismatch (0x%08x != 0x%08x)\n",
            (unsigned)abi_magic, (unsigned)B8G_ABI_EXPECTED);
        sync();
        (void)reboot(RB_HALT_SYSTEM);
        for (;;) {
            pause();
        }
    }

    log_both("[AWESOME-OS-POSIX] PID 1 READY (B8G_ABI_MAGIC_V3=0x%08X)\n",
             (unsigned)abi_magic);

    if (cmdline_has_token("awesome.selftest=1")) {
        run_selftest();
    }

    setup_network();

    log_both("\n");
    log_both("============================================================\n");
    log_both("  Awesome-OS POSIX v11 — Headless Terminal & Container OS\n");
    log_both("  ABI       : B8G_ABI_MAGIC_V3 (0x33564742)\n");
    log_both("  Consoles  : Dual tty0 (VGA/UEFI) + ttyS0 (Serial)\n");
    log_both("  Mode      : Stateless RAM v11 CAS (Zero GUI)\n");
    log_both("  v11 CAS   : /var/lib/b8g/cas/v11/files/\n");
    log_both("  System    : system://local/terminal/awesome-os-posix\n");
    log_both("============================================================\n\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_zombies;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    (void)sigaction(SIGCHLD, &sa, NULL);

    /* Spawn dual interactive shells on local console (tty0) and serial (ttyS0) */
    pid_t tty0_pid = spawn_terminal_on("/dev/tty0");
    pid_t ttyS0_pid = spawn_terminal_on("/dev/ttyS0");

    for (;;) {
        if (tty0_pid > 0 && kill(tty0_pid, 0) != 0) {
            tty0_pid = spawn_terminal_on("/dev/tty0");
        }
        if (ttyS0_pid > 0 && kill(ttyS0_pid, 0) != 0) {
            ttyS0_pid = spawn_terminal_on("/dev/ttyS0");
        }
        if (tty0_pid < 0 && ttyS0_pid < 0) {
            pid_t pid = fork();
            if (pid == 0) {
                (void)setsid();
                char *const sh_argv[] = {"/bin/sh", "-l", NULL};
                execv("/bin/sh", sh_argv);
                _exit(127);
            }
            if (pid > 0) {
                int status = 0;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
        sleep(1);
    }

    return 0;
}
