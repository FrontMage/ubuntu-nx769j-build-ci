// nx769j_powerkey.c - Display-safe Power Button & Low-Power Standby Daemon for NX769J
//
// Owns physical power button (pmic_pwrkey, /dev/input/event0) on Nubia RedMagic 9 Pro
// running mainline Linux 7.1.1-nx769j+ + Omarchy Mobile Wayland over U-Boot efifb:
//   - Exclusively grabs pmic_pwrkey via EVIOCGRAB so DTB's KEY_ENTER (0x1c) or KEY_POWER
//     (0x74) never leaks into Linux VT kbd, Hyprland, or systemd-logind.
//   - NEVER opens or EVIOCGRABs /dev/input/event2 (Goodix touchscreen): nx769j-fb-wl is
//     the sole reader of event2, gating touch internally while screen_off=1 and resyncing
//     MT Protocol B slots cleanly on wake without stolen-event slot desynchronization.
//   - Short press (< 1.5s): toggles Instant Screen-Off & Low-Power Standby WITHOUT deep
//     kernel suspend (keeping MMCX / smmu-splash-keepalive / efifb 100% intact):
//       1) Coordinates with nx769j-fb-wl (/run/nx769j-wl/power.sock) using verified
//          poll-framed replies ("OK screen_off=0|1 ...") with retry so state never drifts.
//       2) Coordinates with nx769j-perf:
//          - Screen-OFF calls `nx769j-perf standby` (deep `powersave` clocks + `boost=0`
//            + Adreno 750 idle cap + 0 RPM silent fan <65 C, preserving user's saved mode).
//          - Screen-ON calls `nx769j-perf apply` to restore whichever profile
//            (`powersave` / `balanced` / `performance`) the user selected!
//          - Retains built-in direct cpufreq fallback if nx769j-perf is absent.
//   - Long press (>= 2.5s held): blanks display, restores active profile, and executes a
//     clean `systemctl poweroff`.

#define _GNU_SOURCE

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fb.h>
#include <linux/input.h>
#else
#define EV_KEY    0x01
#define KEY_ENTER 28
#define KEY_POWER 116
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define PWRKEY_BY_PATH   "/dev/input/by-path/platform-c400000.spmi-platform-c400000.spmi:pmic@0:pon@1300:pwrkey-event"
#define TOUCH_BY_PATH    "/dev/input/by-path/platform-a90000.spi-cs-0-event"
#define FB_WL_PWR_SOCK   "/run/nx769j-wl/power.sock"
#define PERF_CTRL_SOCK   "/run/nx769j-perf/ctrl.sock"
#define PERF_BIN_PATH    "/usr/local/bin/nx769j-perf"
#define PERF_MODE_PATH   "/var/lib/nx769j-perf/mode"
#define RUN_DIR          "/run/nx769j-powerkey"
#define CTRL_SOCK_PATH   "/run/nx769j-powerkey/ctrl.sock"
#define STATE_JSON       "/run/nx769j-powerkey/state.json"
#define STATE_JSON_TMP   "/run/nx769j-powerkey/state.json.tmp"

#define DEBOUNCE_MS       25ULL
#define SHORT_MAX_MS      1500ULL
#define LONG_MIN_MS       2500ULL
#define CTRL_RECV_WAIT_MS 100
#define FB_REPLY_WAIT_MS  250
#define PERF_REPLY_MS     600
#define FB_NOTIFY_RETRIES 5

#define MAX_POLICIES   16

struct policy_save {
    char name[32];
    char saved_gov[32];
};

static volatile sig_atomic_t g_stop = 0;

static int g_pwr_fd = -1;
static int g_pwr_grabbed = 0;
static char g_pwr_path[256];

static int g_screen_off = 0;
static uint64_t g_transitions = 0;
static const char *g_fb_wl_pwr_sock = FB_WL_PWR_SOCK;
static const char *g_perf_ctrl_sock = PERF_CTRL_SOCK;
static const char *g_perf_bin_path  = PERF_BIN_PATH;
static const char *g_perf_mode_path = PERF_MODE_PATH;

static struct policy_save g_policies[MAX_POLICIES];
static int g_npolicies = 0;
static int g_has_boost = 0;
static int g_saved_boost = 0;

static void on_term_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

#ifdef __linux__
static inline void hw_write_barrier(void) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
#endif

static int accept_nonblock_cloexec(int lfd) {
#ifdef __linux__
    return accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int fdfl = fcntl(fd, F_GETFD, 0);
        if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    }
    return fd;
#endif
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int send_all_bytes(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static ssize_t recv_line_timeout(int fd, char *buf, size_t bufsz, int timeout_ms) {
    if (fd < 0 || !buf || bufsz < 2) return -1;
    size_t total = 0;
    uint64_t start = now_ms();
    buf[0] = '\0';

    while (total < bufsz - 1) {
        ssize_t r = recv(fd, buf + total, bufsz - 1 - total, MSG_DONTWAIT);
        if (r > 0) {
            total += (size_t)r;
            buf[total] = '\0';
            if (memchr(buf, '\n', total) != NULL) {
                return (ssize_t)total;
            }
            continue;
        }
        if (r == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return total > 0 ? (ssize_t)total : -1;
        }
        uint64_t elapsed = now_ms() - start;
        if (elapsed >= (uint64_t)timeout_ms) {
            break;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, timeout_ms - (int)elapsed);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            break;
        }
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

static int parse_fb_wl_screen_off_reply(const char *reply, int *out_off) {
    if (!reply || !out_off) return -1;
    const char *p = strstr(reply, "screen_off=");
    if (!p) return -1;
    p += strlen("screen_off=");
    if (*p == '0') { *out_off = 0; return 0; }
    if (*p == '1') { *out_off = 1; return 0; }
    return -1;
}

static int read_small_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    return 0;
}

static int write_small_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(val);
    ssize_t w = write(fd, val, len);
    close(fd);
    return (w == (ssize_t)len) ? 0 : -1;
}

static void ensure_powersave_governor_module(void) {
    char avail[256];
    if (read_small_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors",
                        avail, sizeof(avail)) == 0) {
        if (strstr(avail, "powersave") != NULL) return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl("/sbin/modprobe", "modprobe", "-q", "cpufreq_powersave", (char *)NULL);
        execl("/usr/sbin/modprobe", "modprobe", "-q", "cpufreq_powersave", (char *)NULL);
        execl("/usr/bin/modprobe", "modprobe", "-q", "cpufreq_powersave", (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

static void discover_cpufreq_policies(void) {
    g_npolicies = 0;
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_npolicies < MAX_POLICIES) {
        if (strncmp(de->d_name, "policy", 6) != 0) continue;
        if (de->d_name[6] < '0' || de->d_name[6] > '9') continue;
        struct policy_save *ps = &g_policies[g_npolicies++];
        snprintf(ps->name, sizeof(ps->name), "%.31s", de->d_name);
        snprintf(ps->saved_gov, sizeof(ps->saved_gov), "schedutil");
    }
    closedir(d);

    char bval[32];
    if (read_small_file("/sys/devices/system/cpu/cpufreq/boost", bval, sizeof(bval)) == 0) {
        g_has_boost = 1;
        g_saved_boost = atoi(bval) ? 1 : 0;
    } else {
        g_has_boost = 0;
        g_saved_boost = 0;
    }
}

/*
 * Coordinate screen-OFF (`want_standby=1` -> `STANDBY 1` / `nx769j-perf standby`)
 * and screen-ON (`want_standby=0` -> `APPLY` / `nx769j-perf apply`) with nx769j-perf.
 * Returns 0 on verified handoff, -1 if nx769j-perf is absent/unreachable.
 */
static int notify_nx769j_perf_standby(int want_standby) {
    /* 1. Fast zero-fork Unix domain socket RPC to nx769j-perf.service */
    if (g_perf_ctrl_sock && *g_perf_ctrl_sock) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(g_perf_ctrl_sock) < sizeof(addr.sun_path)) {
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_perf_ctrl_sock);
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                int fdfl = fcntl(fd, F_GETFD, 0);
                if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
                if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    const char *req = want_standby ? "STANDBY 1\n" : "APPLY\n";
                    if (send_all_bytes(fd, req, strlen(req)) == 0) {
                        char rbuf[1024];
                        ssize_t r = recv_line_timeout(fd, rbuf, sizeof(rbuf), PERF_REPLY_MS);
                        if (r > 0 && strstr(rbuf, want_standby ? "\"standby\":true" : "\"standby\":false") != NULL) {
                            close(fd);
                            return 0;
                        }
                    }
                }
                close(fd);
            }
        }
    }

    /* 2. Direct CLI execution (`nx769j-perf standby` / `nx769j-perf apply`) */
    if (g_perf_bin_path && *g_perf_bin_path && access(g_perf_bin_path, X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd >= 0) {
                (void)dup2(nullfd, STDOUT_FILENO);
                (void)dup2(nullfd, STDERR_FILENO);
                if (nullfd > 2) close(nullfd);
            }
            execl(g_perf_bin_path, "nx769j-perf", want_standby ? "standby" : "apply", (char *)NULL);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                return 0;
            }
        }
    }

    return -1;
}

static void apply_cpufreq_standby_fallback(void) {
    ensure_powersave_governor_module();
    if (g_npolicies == 0) discover_cpufreq_policies();

    char bval[32];
    if (read_small_file("/sys/devices/system/cpu/cpufreq/boost", bval, sizeof(bval)) == 0) {
        g_has_boost = 1;
        g_saved_boost = atoi(bval) ? 1 : 0;
        (void)write_small_file("/sys/devices/system/cpu/cpufreq/boost", "0\n");
    }

    for (int i = 0; i < g_npolicies; i++) {
        char path[256], cur[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/%.31s/scaling_governor",
                 g_policies[i].name);
        if (read_small_file(path, cur, sizeof(cur)) == 0 && cur[0] != '\0') {
            if (strcmp(cur, "powersave") != 0) {
                snprintf(g_policies[i].saved_gov, sizeof(g_policies[i].saved_gov), "%.31s", cur);
            }
        }
        if (write_small_file(path, "powersave\n") < 0) {
            fprintf(stderr, "[nx769j-powerkey] warning: failed to set powersave on %s: %s\n",
                    g_policies[i].name, strerror(errno));
        }
    }
}

static void restore_cpufreq_awake_fallback(void) {
    if (g_npolicies == 0) discover_cpufreq_policies();

    for (int i = 0; i < g_npolicies; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/%.31s/scaling_governor",
                 g_policies[i].name);
        const char *gov = g_policies[i].saved_gov[0] ? g_policies[i].saved_gov : "schedutil";
        char buf[48];
        snprintf(buf, sizeof(buf), "%.31s\n", gov);
        if (write_small_file(path, buf) < 0) {
            (void)write_small_file(path, "schedutil\n");
        }
    }

    if (g_has_boost) {
        (void)write_small_file("/sys/devices/system/cpu/cpufreq/boost",
                               g_saved_boost ? "1\n" : "0\n");
    }
}

static void apply_power_standby(void) {
    if (notify_nx769j_perf_standby(1) == 0) {
        return;
    }
    apply_cpufreq_standby_fallback();
}

static void restore_power_awake(void) {
    if (notify_nx769j_perf_standby(0) == 0) {
        return;
    }
    restore_cpufreq_awake_fallback();
}

static void current_governor_summary(char *out, size_t outsz) {
    if (read_small_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", out, outsz) < 0) {
        snprintf(out, outsz, "unknown");
    }
}

static void current_perf_mode_summary(char *out, size_t outsz) {
    if (!g_perf_mode_path || read_small_file(g_perf_mode_path, out, outsz) < 0 || out[0] == '\0') {
        snprintf(out, outsz, "balanced");
    }
}

static int current_boost_value(void) {
    char bval[32];
    if (read_small_file("/sys/devices/system/cpu/cpufreq/boost", bval, sizeof(bval)) == 0) {
        return atoi(bval) ? 1 : 0;
    }
    return 0;
}

static void write_state_json(int powering_off) {
    char gov[64];
    char pmode[32];
    current_governor_summary(gov, sizeof(gov));
    current_perf_mode_summary(pmode, sizeof(pmode));
    int boost = current_boost_value();

    (void)mkdir(RUN_DIR, 0755);
    int fd = open(STATE_JSON_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char body[1024];
    int n = snprintf(
        body, sizeof(body),
        "{\n"
        "  \"screen_off\": %s,\n"
        "  \"touch_grabbed\": false,\n"
        "  \"pwrkey_grabbed\": %s,\n"
        "  \"pwrkey_device\": \"%s\",\n"
        "  \"touch_device\": \"delegated:nx769j-fb-wl\",\n"
        "  \"perf_mode\": \"%s\",\n"
        "  \"governor\": \"%s\",\n"
        "  \"boost\": %d,\n"
        "  \"transitions\": %llu,\n"
        "  \"powering_off\": %s\n"
        "}\n",
        g_screen_off ? "true" : "false",
        g_pwr_grabbed ? "true" : "false",
        g_pwr_path,
        pmode,
        gov,
        boost,
        (unsigned long long)g_transitions,
        powering_off ? "true" : "false"
    );
    if (n > 0 && (size_t)n < sizeof(body)) {
        ssize_t wr = write(fd, body, (size_t)n);
        if (wr < 0) {
            // best-effort runtime state file
        }
    }
    close(fd);
    (void)rename(STATE_JSON_TMP, STATE_JSON);
}

static void direct_blank_fb0(void) {
#ifdef __linux__
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    size_t len = 1116UL * 2480UL * 4UL;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 &&
        ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        len = finfo.smem_len ? finfo.smem_len : (size_t)finfo.line_length * (size_t)vinfo.yres;
    }
    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map != MAP_FAILED) {
        memset(map, 0, len);
        hw_write_barrier();
        munmap(map, len);
    }
    close(fd);
#endif
}

static int notify_fb_wl_state(int want_off, char *reply, size_t reply_sz) {
    const char *cmd = want_off ? "OFF" : "ON";
    if (reply && reply_sz > 0) reply[0] = '\0';

    for (int attempt = 0; attempt < FB_NOTIFY_RETRIES; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int fdfl = fcntl(fd, F_GETFD, 0);
        if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_fb_wl_pwr_sock);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            char msg[32];
            int mlen = snprintf(msg, sizeof(msg), "%s\n", cmd);
            if (mlen > 0 && send_all_bytes(fd, msg, (size_t)mlen) == 0) {
                char rbuf[192];
                ssize_t r = recv_line_timeout(fd, rbuf, sizeof(rbuf), FB_REPLY_WAIT_MS);
                if (r > 0) {
                    while (r > 0 && (rbuf[r - 1] == '\n' || rbuf[r - 1] == '\r')) {
                        rbuf[--r] = '\0';
                    }
                    if (reply && reply_sz > 0) {
                        snprintf(reply, reply_sz, "%s", rbuf);
                    }
                    int got_off = -1;
                    if (parse_fb_wl_screen_off_reply(rbuf, &got_off) == 0 && got_off == want_off) {
                        close(fd);
                        return 0;
                    }
                }
            }
        }
        close(fd);
        usleep(20000);
    }
    return -1;
}

static int open_pwrkey_device(void) {
    if (g_pwr_fd >= 0) return 0;
#ifdef __linux__
    int fd = open(PWRKEY_BY_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        snprintf(g_pwr_path, sizeof(g_pwr_path), "%s", PWRKEY_BY_PATH);
    } else {
        for (int i = 0; i < 32; i++) {
            char p[64], name[128];
            snprintf(p, sizeof(p), "/dev/input/event%d", i);
            int cfd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (cfd < 0) continue;
            memset(name, 0, sizeof(name));
            if (ioctl(cfd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
                strstr(name, "pmic_pwrkey") != NULL) {
                fd = cfd;
                snprintf(g_pwr_path, sizeof(g_pwr_path), "%s", p);
                break;
            }
            close(cfd);
        }
    }

    if (fd < 0) return -1;

    g_pwr_fd = fd;
    if (ioctl(g_pwr_fd, EVIOCGRAB, 1) == 0) {
        g_pwr_grabbed = 1;
    } else {
        g_pwr_grabbed = 0;
        fprintf(stderr, "[nx769j-powerkey] warning: EVIOCGRAB on %s failed: %s\n",
                g_pwr_path, strerror(errno));
    }
    fprintf(stderr, "[nx769j-powerkey] opened %s (grab=%d)\n", g_pwr_path, g_pwr_grabbed);
    return 0;
#else
    return -1;
#endif
}

static void close_pwrkey_device(void) {
    if (g_pwr_fd >= 0) {
#ifdef __linux__
        if (g_pwr_grabbed) (void)ioctl(g_pwr_fd, EVIOCGRAB, 0);
#endif
        close(g_pwr_fd);
        g_pwr_fd = -1;
        g_pwr_grabbed = 0;
    }
}

static void enter_standby(void) {
    if (g_screen_off) return;
    g_screen_off = 1;
    g_transitions++;

    // 1. Coordinate with nx769j-fb-wl to gate touch, pause blits, and paint /dev/fb0 0x000000
    char reply[192];
    if (notify_fb_wl_state(1, reply, sizeof(reply)) < 0) {
        fprintf(stderr, "[nx769j-powerkey] warning: fb_wl OFF unconfirmed (%s), direct-blanking fb0\n",
                reply[0] ? reply : "no-reply");
        direct_blank_fb0();
    }

    // 2. Coordinate with nx769j-perf standby (powersave clocks + 0 RPM fan <65 C)
    apply_power_standby();

    write_state_json(0);
    fprintf(stderr, "[nx769j-powerkey] entered screen-off standby (#%llu) [%s]\n",
            (unsigned long long)g_transitions, reply[0] ? reply : "fallback");
}

static void exit_standby(void) {
    if (!g_screen_off) return;
    g_screen_off = 0;
    g_transitions++;

    // 1. Restore user's selected nx769j-perf profile (`powersave` / `balanced` / `performance`)
    restore_power_awake();

    // 2. Restore shadow framebuffer, resync touch MT slots, and resume live Wayland blits
    char reply[192];
    if (notify_fb_wl_state(0, reply, sizeof(reply)) < 0) {
        fprintf(stderr, "[nx769j-powerkey] warning: fb_wl ON unconfirmed (%s)\n",
                reply[0] ? reply : "no-reply");
    }

    write_state_json(0);
    fprintf(stderr, "[nx769j-powerkey] woke from standby (#%llu) [%s]\n",
            (unsigned long long)g_transitions, reply[0] ? reply : "unconfirmed");
}

static void trigger_clean_poweroff(uint64_t held_ms) {
    fprintf(stderr, "[nx769j-powerkey] long press (%llu ms >= %llu ms) -> systemctl poweroff\n",
            (unsigned long long)held_ms, (unsigned long long)LONG_MIN_MS);

    char reply[192];
    if (notify_fb_wl_state(1, reply, sizeof(reply)) < 0) {
        direct_blank_fb0();
    }
    restore_power_awake();
    write_state_json(1);
    close_pwrkey_device();

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/systemctl", "systemctl", "poweroff", (char *)NULL);
        execl("/usr/bin/systemctl", "systemctl", "poweroff", (char *)NULL);
        _exit(127);
    }
}

static void handle_ctrl_client(int lfd) {
    if (lfd < 0) return;
    for (;;) {
        int cfd = accept_nonblock_cloexec(lfd);
        if (cfd < 0) break;
        char cmd[64];
        ssize_t n = recv_line_timeout(cfd, cmd, sizeof(cmd), CTRL_RECV_WAIT_MS);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (cmd[i] == '\r' || cmd[i] == '\n' || cmd[i] == ' ' || cmd[i] == '\t') {
                    cmd[i] = '\0';
                    break;
                }
            }
            if (strcasecmp(cmd, "OFF") == 0) {
                enter_standby();
            } else if (strcasecmp(cmd, "ON") == 0) {
                exit_standby();
            } else if (strcasecmp(cmd, "TOGGLE") == 0) {
                if (g_screen_off) exit_standby();
                else enter_standby();
            }
        }
        char buf[512];
        int fd = open(STATE_JSON, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t r = read(fd, buf, sizeof(buf));
            close(fd);
            if (r > 0) (void)send_all_bytes(cfd, buf, (size_t)r);
        }
        close(cfd);
    }
}

static int run_cli_cmd(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CTRL_SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect " CTRL_SOCK_PATH);
        close(fd);
        return 1;
    }
    char req[32];
    int rlen = snprintf(req, sizeof(req), "%s\n", cmd);
    (void)send_all_bytes(fd, req, (size_t)rlen);
    char resp[512];
    ssize_t total = 0;
    while ((size_t)total < sizeof(resp) - 1) {
        ssize_t n = recv(fd, resp + total, sizeof(resp) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    if (total > 0) {
        resp[total] = '\0';
        fputs(resp, stdout);
        return 0;
    }
    return 1;
}

static int run_device_check(void) {
    discover_cpufreq_policies();
    char gov[64];
    char pmode[32];
    current_governor_summary(gov, sizeof(gov));
    current_perf_mode_summary(pmode, sizeof(pmode));
    printf("policies=%d gov=%s perf_mode=%s has_boost=%d boost=%d\n",
           g_npolicies, gov, pmode, g_has_boost, current_boost_value());
    printf("pwrkey_by_path=%s exists=%d\n",
           PWRKEY_BY_PATH, access(PWRKEY_BY_PATH, R_OK) == 0 ? 1 : 0);
    printf("touch_by_path=%s exists=%d (delegated to nx769j-fb-wl, no EVIOCGRAB)\n",
           TOUCH_BY_PATH, access(TOUCH_BY_PATH, R_OK) == 0 ? 1 : 0);
    return (access(PWRKEY_BY_PATH, R_OK) == 0) ? 0 : 1;
}

#ifndef NX769J_POWERKEY_TEST
int main(int argc, char **argv) {
    const char *env_pwr = getenv("NX769J_FB_POWER_SOCK");
    if (env_pwr && *env_pwr) {
        g_fb_wl_pwr_sock = env_pwr;
    }
    const char *env_psock = getenv("NX769J_PERF_SOCK");
    if (env_psock && *env_psock) {
        g_perf_ctrl_sock = env_psock;
    }
    const char *env_pbin = getenv("NX769J_PERF_BIN");
    if (env_pbin && *env_pbin) {
        g_perf_bin_path = env_pbin;
    }
    const char *env_pmode = getenv("NX769J_PERF_MODE_FILE");
    if (env_pmode && *env_pmode) {
        g_perf_mode_path = env_pmode;
    }

    if (argc > 1) {
        if (strcmp(argv[1], "--status") == 0) return run_cli_cmd("STATUS");
        if (strcmp(argv[1], "--off") == 0) return run_cli_cmd("OFF");
        if (strcmp(argv[1], "--on") == 0) return run_cli_cmd("ON");
        if (strcmp(argv[1], "--toggle") == 0) return run_cli_cmd("TOGGLE");
        if (strcmp(argv[1], "--check") == 0) return run_device_check();
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: %s [--status|--off|--on|--toggle|--check]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    ensure_powersave_governor_module();
    discover_cpufreq_policies();

    (void)mkdir(RUN_DIR, 0755);
    unlink(CTRL_SOCK_PATH);
    int ctrl_lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_lfd >= 0) {
        int fl = fcntl(ctrl_lfd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(ctrl_lfd, F_SETFL, fl | O_NONBLOCK);
        int fdfl = fcntl(ctrl_lfd, F_GETFD, 0);
        if (fdfl >= 0) (void)fcntl(ctrl_lfd, F_SETFD, fdfl | FD_CLOEXEC);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CTRL_SOCK_PATH);
        if (bind(ctrl_lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind " CTRL_SOCK_PATH);
            close(ctrl_lfd);
            ctrl_lfd = -1;
        } else {
            chmod(CTRL_SOCK_PATH, 0660);
            listen(ctrl_lfd, 8);
        }
    }

    (void)open_pwrkey_device();
    write_state_json(0);

    int key_pressed = 0;
    int long_fired = 0;
    uint64_t press_ms = 0;

    while (!g_stop) {
        (void)waitpid(-1, NULL, WNOHANG);

        if (g_pwr_fd < 0) {
            (void)open_pwrkey_device();
            write_state_json(0);
        }

        int timeout_ms = -1;
        if (key_pressed && !long_fired) {
            uint64_t elapsed = now_ms() - press_ms;
            if (elapsed >= LONG_MIN_MS) {
                long_fired = 1;
                trigger_clean_poweroff(elapsed);
                break;
            }
            timeout_ms = (int)(LONG_MIN_MS - elapsed);
        } else if (g_pwr_fd < 0) {
            timeout_ms = 1000;
        }

        struct pollfd pfds[2];
        int npfds = 0;
        int pwr_idx = -1, ctrl_idx = -1;

        if (g_pwr_fd >= 0) {
            pwr_idx = npfds++;
            pfds[pwr_idx].fd = g_pwr_fd;
            pfds[pwr_idx].events = POLLIN;
            pfds[pwr_idx].revents = 0;
        }
        if (ctrl_lfd >= 0) {
            ctrl_idx = npfds++;
            pfds[ctrl_idx].fd = ctrl_lfd;
            pfds[ctrl_idx].events = POLLIN;
            pfds[ctrl_idx].revents = 0;
        }

        int pr = poll(pfds, (nfds_t)npfds, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            usleep(10000);
            continue;
        }

        if (key_pressed && !long_fired) {
            uint64_t elapsed = now_ms() - press_ms;
            if (elapsed >= LONG_MIN_MS) {
                long_fired = 1;
                trigger_clean_poweroff(elapsed);
                break;
            }
        }

        if (ctrl_idx >= 0 && (pfds[ctrl_idx].revents & POLLIN)) {
            handle_ctrl_client(ctrl_lfd);
        }
        if (pwr_idx >= 0 && (pfds[pwr_idx].revents & (POLLIN | POLLERR | POLLHUP))) {
            struct input_event ev[32];
            for (;;) {
                ssize_t n = read(g_pwr_fd, ev, sizeof(ev));
                if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR) break;
                    close_pwrkey_device();
                    key_pressed = 0;
                    break;
                }
                if (n == 0) break;
                int cnt = (int)(n / (ssize_t)sizeof(ev[0]));
                for (int i = 0; i < cnt; i++) {
                    if (ev[i].type != EV_KEY) continue;
                    // Live DTB maps pon@1300:pwrkey to KEY_ENTER (28 = 0x1c); also accept KEY_POWER (116)
                    if (ev[i].code != KEY_POWER && ev[i].code != KEY_ENTER) continue;

                    if (ev[i].value == 1) {
                        if (!key_pressed) {
                            key_pressed = 1;
                            long_fired = 0;
                            press_ms = now_ms();
                        }
                    } else if (ev[i].value == 0) {
                        if (key_pressed) {
                            uint64_t held_ms = now_ms() - press_ms;
                            key_pressed = 0;
                            if (!long_fired) {
                                if (held_ms >= LONG_MIN_MS) {
                                    long_fired = 1;
                                    trigger_clean_poweroff(held_ms);
                                    g_stop = 1;
                                    break;
                                } else if (held_ms >= DEBOUNCE_MS && held_ms < SHORT_MAX_MS) {
                                    if (g_screen_off) exit_standby();
                                    else enter_standby();
                                } else if (held_ms >= SHORT_MAX_MS) {
                                    fprintf(stderr,
                                            "[nx769j-powerkey] ignored mid-press (%llu ms in 1.5s..2.5s deadband)\n",
                                            (unsigned long long)held_ms);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Never leave display black or CPUs in powersave on daemon exit/restart
    if (g_screen_off) {
        exit_standby();
    }
    close_pwrkey_device();
    if (ctrl_lfd >= 0) close(ctrl_lfd);
    unlink(CTRL_SOCK_PATH);
    return 0;
}
#endif
