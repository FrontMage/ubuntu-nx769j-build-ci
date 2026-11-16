// SPDX-License-Identifier: GPL-2.0-only
/*
 * nx769j_perf.c — 3-Mode Performance & Thermal Profile Switcher + Daemon
 * Target: Nubia RedMagic 9 Pro (NX769J, Snapdragon 8 Gen 3 SM8650, Adreno 750,
 *         built-in 25.0 kHz PWM Turbo Cooling Fan) on Omarchy Mobile (7.1.1-nx769j+)
 *
 * Modes:
 *   - powersave   ("省电" / Eco)          : ~60% sweet-spot OPP cap on Cortex-X4 & A720,
 *                                           Adreno 750 capped at 500 MHz, silent fan (<62 C)
 *   - balanced    ("均衡" / Balanced)     : Full base range on all 4 CPU clusters (schedutil)
 *                                           & Adreno 750 (231..834 MHz), quiet fan (<48 C = 0 RPM)
 *   - performance ("狂暴" / Diablo-Turbo) : Boost=1 unlocking Cortex-X4 3.30 GHz + ~50% floor,
 *                                           Adreno 750 500..834 MHz (20 ms poll), proactive
 *                                           Turbo Fan (~5,500+ RPM active floor -> 21,500 RPM)
 *
 * Screen-Off Standby Harmony (coordinated with nx769j-powerkey):
 *   - `nx769j-perf standby` enters deep `powersave` (365..499 MHz, boost=0, GPU 231..366 MHz)
 *     and silences fan to 0 RPM (<65 C, 10 C cooldown hyst) WITHOUT altering saved mode.
 *   - `nx769j-perf apply` clears standby and restores whichever mode (`powersave`,
 *     `balanced`, or `performance`) the user persisted in `/var/lib/nx769j-perf/mode`.
 */

#define _POSIX_C_SOURCE 200809L

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX_POLICIES   8
#define MAX_OPPS       64

enum perf_mode {
    PERF_POWERSAVE   = 0,
    PERF_BALANCED    = 1,
    PERF_PERFORMANCE = 2,
};

enum fan_override {
    FAN_OVR_PROFILE = 0,
    FAN_OVR_LOW     = 1,
    FAN_OVR_MED     = 2,
    FAN_OVR_HIGH    = 3,
    FAN_OVR_MAX     = 4,
    FAN_OVR_OFF     = 5,
};

struct runtime_paths {
    char sysfs_root[PATH_MAX];
    char cpu_root[PATH_MAX];
    char cpufreq_root[PATH_MAX];
    char devfreq_root[PATH_MAX];
    char hwmon_root[PATH_MAX];
    char thermal_root[PATH_MAX];
    char run_dir[PATH_MAX];
    char ctrl_sock[PATH_MAX];
    char state_json[PATH_MAX];
    char state_dir[PATH_MAX];
    char mode_file[PATH_MAX];
};

struct cpu_cluster {
    char policy_name[32];
    int policy_idx;
    int first_cpu;
    long capacity;
    long avail[MAX_OPPS];
    int n_avail;
    long base_min;
    long base_max;
    long boost_max;
    long cur_min;
    long cur_max;
    long cur_freq;
    char cur_gov[32];
};

struct gpu_devfreq {
    bool present;
    char dir[PATH_MAX];
    long avail[MAX_OPPS];
    int n_avail;
    long min_opp;
    long max_opp;
    long cur_min;
    long cur_max;
    long cur_freq;
    char cur_gov[32];
};

struct fan_hw {
    bool present;
    char hwmon_dir[PATH_MAX];
    bool has_tz_cpuss0;
    char tz_cpuss0_dir[PATH_MAX];
};

struct perf_telemetry {
    enum perf_mode mode;
    bool standby;
    enum fan_override fan_ovr;
    int boost;
    long cpuss0_mc;
    long peak_cpu_mc;
    long peak_gpu_mc;
    long hotspot_mc;
    long fan_pwm;
    int fan_duty_pct;
    long fan_rpm;
    char fan_mode_desc[32];
    struct gpu_devfreq gpu;
    struct cpu_cluster clusters[MAX_POLICIES];
    int n_clusters;
};

static volatile sig_atomic_t g_stop = 0;

static void on_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void init_paths(struct runtime_paths *rp)
{
    memset(rp, 0, sizeof(*rp));
    const char *sysfs = getenv("NX769J_PERF_SYSFS_ROOT");
    if (!sysfs || !*sysfs)
        sysfs = "/sys";
    const char *rundir = getenv("NX769J_PERF_RUN_DIR");
    if (!rundir || !*rundir)
        rundir = "/run/nx769j-perf";
    const char *stdir = getenv("NX769J_PERF_STATE_DIR");
    if (!stdir || !*stdir)
        stdir = "/var/lib/nx769j-perf";

    snprintf(rp->sysfs_root, sizeof(rp->sysfs_root), "%s", sysfs);
    snprintf(rp->cpu_root, sizeof(rp->cpu_root), "%s/devices/system/cpu", sysfs);
    snprintf(rp->cpufreq_root, sizeof(rp->cpufreq_root), "%s/devices/system/cpu/cpufreq", sysfs);
    snprintf(rp->devfreq_root, sizeof(rp->devfreq_root), "%s/class/devfreq", sysfs);
    snprintf(rp->hwmon_root, sizeof(rp->hwmon_root), "%s/class/hwmon", sysfs);
    snprintf(rp->thermal_root, sizeof(rp->thermal_root), "%s/class/thermal", sysfs);
    snprintf(rp->run_dir, sizeof(rp->run_dir), "%s", rundir);
    snprintf(rp->ctrl_sock, sizeof(rp->ctrl_sock), "%s/ctrl.sock", rundir);
    snprintf(rp->state_json, sizeof(rp->state_json), "%s/state.json", rundir);
    snprintf(rp->state_dir, sizeof(rp->state_dir), "%s", stdir);
    snprintf(rp->mode_file, sizeof(rp->mode_file), "%s/mode", stdir);
}

static const char *mode_to_id(enum perf_mode m)
{
    switch (m) {
    case PERF_POWERSAVE:   return "powersave";
    case PERF_BALANCED:    return "balanced";
    case PERF_PERFORMANCE: return "performance";
    }
    return "balanced";
}

static const char *mode_to_zh(enum perf_mode m)
{
    switch (m) {
    case PERF_POWERSAVE:   return "省电";
    case PERF_BALANCED:    return "均衡";
    case PERF_PERFORMANCE: return "狂暴";
    }
    return "均衡";
}

static const char *mode_to_en(enum perf_mode m)
{
    switch (m) {
    case PERF_POWERSAVE:   return "Eco";
    case PERF_BALANCED:    return "Balanced";
    case PERF_PERFORMANCE: return "Turbo";
    }
    return "Balanced";
}

static bool parse_mode_str(const char *s, enum perf_mode *out)
{
    if (!s || !out)
        return false;
    if (strcasecmp(s, "powersave") == 0 || strcasecmp(s, "power-saver") == 0 ||
        strcasecmp(s, "eco") == 0) {
        *out = PERF_POWERSAVE;
        return true;
    }
    if (strcasecmp(s, "balanced") == 0 || strcasecmp(s, "normal") == 0 ||
        strcasecmp(s, "default") == 0) {
        *out = PERF_BALANCED;
        return true;
    }
    if (strcasecmp(s, "performance") == 0 || strcasecmp(s, "turbo") == 0 ||
        strcasecmp(s, "diablo") == 0) {
        *out = PERF_PERFORMANCE;
        return true;
    }
    return false;
}

static enum perf_mode next_cycle_mode(enum perf_mode cur)
{
    switch (cur) {
    case PERF_POWERSAVE:   return PERF_BALANCED;
    case PERF_BALANCED:    return PERF_PERFORMANCE;
    case PERF_PERFORMANCE: return PERF_POWERSAVE;
    }
    return PERF_BALANCED;
}

static const char *fan_ovr_to_str(enum fan_override ovr)
{
    switch (ovr) {
    case FAN_OVR_PROFILE: return "profile";
    case FAN_OVR_LOW:     return "low";
    case FAN_OVR_MED:     return "med";
    case FAN_OVR_HIGH:    return "high";
    case FAN_OVR_MAX:     return "max";
    case FAN_OVR_OFF:     return "off";
    }
    return "profile";
}

static bool parse_fan_ovr(const char *s, enum fan_override *out)
{
    if (!s || !out)
        return false;
    if (strcasecmp(s, "profile") == 0 || strcasecmp(s, "auto") == 0) {
        *out = FAN_OVR_PROFILE;
        return true;
    }
    if (strcasecmp(s, "low") == 0)  { *out = FAN_OVR_LOW;  return true; }
    if (strcasecmp(s, "med") == 0)  { *out = FAN_OVR_MED;  return true; }
    if (strcasecmp(s, "high") == 0) { *out = FAN_OVR_HIGH; return true; }
    if (strcasecmp(s, "max") == 0)  { *out = FAN_OVR_MAX;  return true; }
    if (strcasecmp(s, "off") == 0)  { *out = FAN_OVR_OFF;  return true; }
    return false;
}

static int mkdir_p(const char *dir, mode_t mode)
{
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s", dir);
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        return -ENAMETOOLONG;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -errno;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -errno;
    return 0;
}

static int read_trimmed(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ssize_t n = read(fd, buf, bufsz - 1);
    int saved = errno;
    close(fd);
    if (n < 0)
        return -saved;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' '  || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    return 0;
}

static int read_long_at(const char *dir, const char *leaf, long *out)
{
    char path[PATH_MAX];
    char buf[128];
    int rc = snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;
    rc = read_trimmed(path, buf, sizeof(buf));
    if (rc < 0)
        return rc;
    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || end == buf)
        return -EINVAL;
    *out = v;
    return 0;
}

static int write_str_path(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    size_t len = strlen(val);
    ssize_t wr = write(fd, val, len);
    int saved = errno;
    close(fd);
    if (wr < 0)
        return -saved;
    return ((size_t)wr == len) ? 0 : -EIO;
}

static int write_str_at(const char *dir, const char *leaf, const char *val)
{
    char path[PATH_MAX];
    int rc = snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;
    return write_str_path(path, val);
}

static int write_long_at(const char *dir, const char *leaf, long val)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%ld\n", val);
    return write_str_at(dir, leaf, buf);
}

static int atomic_write_file(const char *path, const char *content, mode_t mode)
{
    char tmp[PATH_MAX];
    int rc = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (rc < 0 || (size_t)rc >= sizeof(tmp))
        return -ENAMETOOLONG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -errno;
    size_t len = strlen(content);
    ssize_t wr = write(fd, content, len);
    int saved = errno;
    (void)fchmod(fd, mode);
    close(fd);
    if (wr < 0 || (size_t)wr != len) {
        unlink(tmp);
        return wr < 0 ? -saved : -EIO;
    }
    if (rename(tmp, path) < 0) {
        saved = errno;
        unlink(tmp);
        return -saved;
    }
    return 0;
}

static enum perf_mode load_saved_mode(const struct runtime_paths *rp)
{
    char buf[64];
    enum perf_mode m = PERF_BALANCED;
    if (read_trimmed(rp->mode_file, buf, sizeof(buf)) == 0)
        (void)parse_mode_str(buf, &m);
    return m;
}

static int save_selected_mode(const struct runtime_paths *rp, enum perf_mode m)
{
    int rc = mkdir_p(rp->state_dir, 0755);
    if (rc < 0)
        return rc;
    char line[32];
    snprintf(line, sizeof(line), "%s\n", mode_to_id(m));
    return atomic_write_file(rp->mode_file, line, 0644);
}

static int cmp_long(const void *a, const void *b)
{
    long la = *(const long *)a;
    long lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

static int parse_long_list(const char *text, long *out, int max_items)
{
    int count = 0;
    const char *p = text;
    while (*p && count < max_items) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        errno = 0;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (errno != 0 || end == p)
            break;
        if (v > 0)
            out[count++] = v;
        p = end;
    }
    if (count > 1) {
        qsort(out, (size_t)count, sizeof(long), cmp_long);
        int uniq = 0;
        for (int i = 0; i < count; i++) {
            if (uniq == 0 || out[i] != out[uniq - 1])
                out[uniq++] = out[i];
        }
        count = uniq;
    }
    return count;
}

static long nearest_opp(const long *avail, int n, long target)
{
    if (n <= 0)
        return target;
    long best = avail[0];
    long best_diff = labs(avail[0] - target);
    for (int i = 1; i < n; i++) {
        long d = labs(avail[i] - target);
        if (d <= best_diff) {
            best = avail[i];
            best_diff = d;
        }
    }
    return best;
}

static int cmp_cluster(const void *a, const void *b)
{
    const struct cpu_cluster *ca = (const struct cpu_cluster *)a;
    const struct cpu_cluster *cb = (const struct cpu_cluster *)b;
    return (ca->policy_idx > cb->policy_idx) - (ca->policy_idx < cb->policy_idx);
}

static int discover_cpu_clusters(const struct runtime_paths *rp,
                                 struct cpu_cluster *clusters, int max_clusters)
{
    int n = 0;
    DIR *d = opendir(rp->cpufreq_root);
    if (!d)
        return 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max_clusters) {
        if (strncmp(de->d_name, "policy", 6) != 0 || !isdigit((unsigned char)de->d_name[6]))
            continue;
        struct cpu_cluster *c = &clusters[n];
        memset(c, 0, sizeof(*c));
        snprintf(c->policy_name, sizeof(c->policy_name), "%.31s", de->d_name);
        c->policy_idx = atoi(de->d_name + 6);
        c->first_cpu = c->policy_idx;

        char pdir[PATH_MAX], buf[1024];
        snprintf(pdir, sizeof(pdir), "%s/%s", rp->cpufreq_root, c->policy_name);

        char rel_path[PATH_MAX];
        snprintf(rel_path, sizeof(rel_path), "%s/related_cpus", pdir);
        if (read_trimmed(rel_path, buf, sizeof(buf)) == 0 && isdigit((unsigned char)buf[0]))
            c->first_cpu = atoi(buf);

        char cap_dir[PATH_MAX];
        snprintf(cap_dir, sizeof(cap_dir), "%s/cpu%d", rp->cpu_root, c->first_cpu);
        long cap = -1;
        if (read_long_at(cap_dir, "cpu_capacity", &cap) == 0 && cap > 0)
            c->capacity = cap;
        else
            c->capacity = (c->policy_idx == 0) ? 410 : 1000;

        char af_path[PATH_MAX];
        snprintf(af_path, sizeof(af_path), "%s/scaling_available_frequencies", pdir);
        if (read_trimmed(af_path, buf, sizeof(buf)) == 0)
            c->n_avail = parse_long_list(buf, c->avail, MAX_OPPS);

        long cmin = 0, cmax = 0;
        (void)read_long_at(pdir, "cpuinfo_min_freq", &cmin);
        (void)read_long_at(pdir, "cpuinfo_max_freq", &cmax);

        if (c->n_avail > 0) {
            c->base_min = c->avail[0];
            c->base_max = c->avail[c->n_avail - 1];
        } else {
            c->base_min = cmin;
            c->base_max = cmax;
        }
        c->boost_max = c->base_max;

        snprintf(af_path, sizeof(af_path), "%s/scaling_boost_frequencies", pdir);
        if (read_trimmed(af_path, buf, sizeof(buf)) == 0) {
            long boost_opps[16];
            int nb = parse_long_list(buf, boost_opps, 16);
            if (nb > 0 && boost_opps[nb - 1] > c->boost_max)
                c->boost_max = boost_opps[nb - 1];
        }

        (void)read_long_at(pdir, "scaling_min_freq", &c->cur_min);
        (void)read_long_at(pdir, "scaling_max_freq", &c->cur_max);
        if (read_long_at(pdir, "scaling_cur_freq", &c->cur_freq) < 0)
            (void)read_long_at(pdir, "cpuinfo_cur_freq", &c->cur_freq);

        snprintf(af_path, sizeof(af_path), "%s/scaling_governor", pdir);
        if (read_trimmed(af_path, c->cur_gov, sizeof(c->cur_gov)) < 0)
            snprintf(c->cur_gov, sizeof(c->cur_gov), "unknown");

        n++;
    }
    closedir(d);

    if (n > 1)
        qsort(clusters, (size_t)n, sizeof(clusters[0]), cmp_cluster);
    return n;
}

static void discover_gpu_devfreq(const struct runtime_paths *rp, struct gpu_devfreq *gpu)
{
    memset(gpu, 0, sizeof(*gpu));
    DIR *d = opendir(rp->devfreq_root);
    if (!d)
        return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (strstr(de->d_name, "gpu") == NULL && strstr(de->d_name, "kgsl") == NULL)
            continue;
        snprintf(gpu->dir, sizeof(gpu->dir), "%s/%s", rp->devfreq_root, de->d_name);
        gpu->present = true;
        if (strcmp(de->d_name, "3d00000.gpu") == 0)
            break;
    }
    closedir(d);

    if (!gpu->present)
        return;

    char p[PATH_MAX + 40], buf[1024];
    snprintf(p, sizeof(p), "%s/available_frequencies", gpu->dir);
    if (read_trimmed(p, buf, sizeof(buf)) == 0)
        gpu->n_avail = parse_long_list(buf, gpu->avail, MAX_OPPS);

    (void)read_long_at(gpu->dir, "min_freq", &gpu->cur_min);
    (void)read_long_at(gpu->dir, "max_freq", &gpu->cur_max);
    if (read_long_at(gpu->dir, "cur_freq", &gpu->cur_freq) < 0)
        (void)read_long_at(gpu->dir, "target_freq", &gpu->cur_freq);

    if (gpu->n_avail > 0) {
        gpu->min_opp = gpu->avail[0];
        gpu->max_opp = gpu->avail[gpu->n_avail - 1];
    } else {
        gpu->min_opp = gpu->cur_min;
        gpu->max_opp = gpu->cur_max;
    }

    snprintf(p, sizeof(p), "%s/governor", gpu->dir);
    if (read_trimmed(p, gpu->cur_gov, sizeof(gpu->cur_gov)) < 0)
        snprintf(gpu->cur_gov, sizeof(gpu->cur_gov), "unknown");
}

static void discover_fan_hw(const struct runtime_paths *rp, struct fan_hw *fh)
{
    memset(fh, 0, sizeof(*fh));
    DIR *d = opendir(rp->hwmon_root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "hwmon", 5) != 0)
                continue;
            char p[PATH_MAX], name[64];
            snprintf(p, sizeof(p), "%s/%s/name", rp->hwmon_root, de->d_name);
            if (read_trimmed(p, name, sizeof(name)) == 0 && strcmp(name, "pwmfan") == 0) {
                snprintf(fh->hwmon_dir, sizeof(fh->hwmon_dir), "%s/%s", rp->hwmon_root, de->d_name);
                fh->present = true;
                break;
            }
        }
        closedir(d);
    }

    d = opendir(rp->thermal_root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "thermal_zone", 12) != 0)
                continue;
            char p[PATH_MAX], type[64];
            snprintf(p, sizeof(p), "%s/%s/type", rp->thermal_root, de->d_name);
            if (read_trimmed(p, type, sizeof(type)) == 0 && strcmp(type, "cpuss0-thermal") == 0) {
                snprintf(fh->tz_cpuss0_dir, sizeof(fh->tz_cpuss0_dir), "%s/%s", rp->thermal_root, de->d_name);
                fh->has_tz_cpuss0 = true;
                break;
            }
        }
        closedir(d);
    }
}

static void read_soc_temps(const struct runtime_paths *rp,
                           long *cpuss0_mc, long *peak_cpu_mc, long *peak_gpu_mc, long *hotspot_mc)
{
    *cpuss0_mc = -1;
    *peak_cpu_mc = -1;
    *peak_gpu_mc = -1;
    *hotspot_mc = -1;

    DIR *d = opendir(rp->thermal_root);
    if (!d)
        return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "thermal_zone", 12) != 0)
            continue;
        char zdir[PATH_MAX], p[PATH_MAX + 32], type[64];
        snprintf(zdir, sizeof(zdir), "%s/%s", rp->thermal_root, de->d_name);
        snprintf(p, sizeof(p), "%s/type", zdir);
        if (read_trimmed(p, type, sizeof(type)) < 0)
            continue;
        long t = -1;
        if (read_long_at(zdir, "temp", &t) < 0 || t <= -50000 || t >= 200000)
            continue;
        if (strcmp(type, "cpuss0-thermal") == 0)
            *cpuss0_mc = t;
        if (strncmp(type, "cpu", 3) == 0 && t > *peak_cpu_mc)
            *peak_cpu_mc = t;
        if (strncmp(type, "gpuss", 5) == 0 && t > *peak_gpu_mc)
            *peak_gpu_mc = t;
    }
    closedir(d);

    long hot = *cpuss0_mc;
    if (*peak_cpu_mc > hot)
        hot = *peak_cpu_mc;
    if (*peak_gpu_mc > hot)
        hot = *peak_gpu_mc;
    *hotspot_mc = hot;
}

/*
 * Compute target PWM (0..255) for the active mode/standby/override with hysteresis.
 * Updates *io_stage in-place so hysteresis prevents rapid on/off oscillation.
 */
static int compute_target_fan_pwm(enum perf_mode mode, bool standby,
                                  enum fan_override ovr, long hotspot_mc,
                                  int *io_stage, char *out_desc, size_t desc_sz)
{
    if (standby) {
        static const long sb_trips[3] = { 65000, 75000, 85000 };
        static const int  sb_pwm[4]   = { 0, 64, 140, 255 };
        int st = *io_stage;
        if (st < 0) st = 0;
        if (st > 3) st = 3;
        if (hotspot_mc >= 0) {
            while (st < 3 && hotspot_mc >= sb_trips[st])
                st++;
            while (st > 0 && hotspot_mc < (sb_trips[st - 1] - 10000))
                st--;
        } else {
            st = 0;
        }
        *io_stage = st;
        snprintf(out_desc, desc_sz, "standby-silent");
        return sb_pwm[st];
    }

    if (ovr != FAN_OVR_PROFILE) {
        int pwm = 0;
        switch (ovr) {
        case FAN_OVR_LOW:  pwm = 51;  break;
        case FAN_OVR_MED:  pwm = 153; break;
        case FAN_OVR_HIGH: pwm = 204; break;
        case FAN_OVR_MAX:  pwm = 255; break;
        case FAN_OVR_OFF:  pwm = 0;   break;
        default:           pwm = 0;   break;
        }
        /* Hardware safety floor >= 85 C even if manual off/low was requested */
        if (hotspot_mc >= 85000 && pwm < 230) {
            snprintf(out_desc, desc_sz, "safety-override");
            return 255;
        }
        snprintf(out_desc, desc_sz, "manual-%s", fan_ovr_to_str(ovr));
        return pwm;
    }

    if (hotspot_mc < 0) {
        snprintf(out_desc, desc_sz, "sensor-fallback");
        return (mode == PERF_PERFORMANCE) ? 90 : 56;
    }

    if (mode == PERF_POWERSAVE) {
        static const long eco_trips[4] = { 62000, 70000, 78000, 85000 };
        static const int  eco_pwm[5]   = { 0, 64, 115, 179, 255 };
        int st = *io_stage;
        if (st < 0) st = 0;
        if (st > 4) st = 4;
        while (st < 4 && hotspot_mc >= eco_trips[st])
            st++;
        while (st > 0 && hotspot_mc < (eco_trips[st - 1] - 4000))
            st--;
        *io_stage = st;
        snprintf(out_desc, desc_sz, "eco-silent");
        return eco_pwm[st];
    }

    if (mode == PERF_PERFORMANCE) {
        /*
         * Diablo-Turbo proactive curve:
         * Active ~5,500 RPM (24% = 60/255) pre-cooling floor when screen is ON,
         * stepping at 38/44/50/56/62 C up to 100% (255/255, ~21,500 RPM).
         */
        static const long turbo_trips[5] = { 38000, 44000, 50000, 56000, 62000 };
        static const int  turbo_pwm[6]   = { 60, 90, 128, 173, 217, 255 };
        int st = *io_stage;
        if (st < 0) st = 0;
        if (st > 5) st = 5;
        while (st < 5 && hotspot_mc >= turbo_trips[st])
            st++;
        while (st > 0 && hotspot_mc < (turbo_trips[st - 1] - 3000))
            st--;
        *io_stage = st;
        snprintf(out_desc, desc_sz, "turbo-proactive");
        return turbo_pwm[st];
    }

    /* Default: PERF_BALANCED — 0 RPM below 48 C (44 C off-hyst), smooth ramp */
    static const long bal_trips[5] = { 48000, 56000, 64000, 72000, 80000 };
    static const int  bal_pwm[6]   = { 0, 56, 102, 153, 204, 255 };
    int st = *io_stage;
    if (st < 0) st = 0;
    if (st > 5) st = 5;
    while (st < 5 && hotspot_mc >= bal_trips[st])
        st++;
    while (st > 0 && hotspot_mc < (bal_trips[st - 1] - 4000))
        st--;
    *io_stage = st;
    snprintf(out_desc, desc_sz, "balanced-auto");
    return bal_pwm[st];
}

static void apply_fan_pwm(const struct fan_hw *fh, int pwm_val, bool pause_kernel_tz)
{
    if (!fh->present)
        return;
    if (pwm_val < 0)
        pwm_val = 0;
    if (pwm_val > 255)
        pwm_val = 255;

    if (fh->has_tz_cpuss0)
        (void)write_str_at(fh->tz_cpuss0_dir, "mode", pause_kernel_tz ? "disabled\n" : "enabled\n");

    long en = -1;
    if (read_long_at(fh->hwmon_dir, "pwm1_enable", &en) != 0 || en != 3)
        (void)write_long_at(fh->hwmon_dir, "pwm1_enable", 3);

    long cur_pwm = -1;
    if (read_long_at(fh->hwmon_dir, "pwm1", &cur_pwm) != 0 || cur_pwm != pwm_val)
        (void)write_long_at(fh->hwmon_dir, "pwm1", pwm_val);
}

static void ensure_powersave_gov_loaded(const struct runtime_paths *rp)
{
    char p[PATH_MAX], avail[256];
    snprintf(p, sizeof(p), "%s/policy0/scaling_available_governors", rp->cpufreq_root);
    if (read_trimmed(p, avail, sizeof(avail)) == 0 && strstr(avail, "powersave") != NULL)
        return;
    if (strcmp(rp->sysfs_root, "/sys") != 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/modprobe", "modprobe", "-q", "cpufreq_powersave", (char *)NULL);
        execl("/sbin/modprobe", "modprobe", "-q", "cpufreq_powersave", (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
    }
}

static int apply_hw_profile(const struct runtime_paths *rp, enum perf_mode mode,
                            bool standby, enum fan_override fan_ovr,
                            bool daemon_controls_fan, int *io_fan_stage)
{
    if (standby)
        ensure_powersave_gov_loaded(rp);

    struct cpu_cluster clusters[MAX_POLICIES];
    int n_clusters = discover_cpu_clusters(rp, clusters, MAX_POLICIES);

    bool want_boost = (!standby && mode == PERF_PERFORMANCE);
    if (want_boost)
        (void)write_str_at(rp->cpufreq_root, "boost", "1\n");

    for (int i = 0; i < n_clusters; i++) {
        const struct cpu_cluster *c = &clusters[i];
        char pdir[PATH_MAX];
        snprintf(pdir, sizeof(pdir), "%s/%s", rp->cpufreq_root, c->policy_name);

        bool is_silver = (c->capacity > 0 && c->capacity < 600);
        const char *target_gov = "schedutil";
        long target_min = c->base_min;
        long target_max = c->base_max;
        long rate_us = 1000;

        if (standby) {
            target_gov = "powersave";
            target_min = c->base_min;
            long pct = is_silver ? 45 : 35;
            target_max = nearest_opp(c->avail, c->n_avail, (c->base_max * pct) / 100);
            rate_us = 2000;
        } else if (mode == PERF_POWERSAVE) {
            target_gov = "schedutil";
            target_min = c->base_min;
            long pct = is_silver ? 70 : 60;
            target_max = nearest_opp(c->avail, c->n_avail, (c->base_max * pct) / 100);
            rate_us = 2000;
        } else if (mode == PERF_PERFORMANCE) {
            target_gov = "schedutil";
            target_min = nearest_opp(c->avail, c->n_avail, (c->base_max * 50) / 100);
            target_max = c->boost_max;
            rate_us = 500;
        } else {
            target_gov = "schedutil";
            target_min = c->base_min;
            target_max = c->base_max;
            rate_us = 1000;
        }

        if (target_min > target_max)
            target_min = target_max;

        char gov_line[40];
        snprintf(gov_line, sizeof(gov_line), "%s\n", target_gov);
        if (write_str_at(pdir, "scaling_governor", gov_line) < 0 && standby)
            (void)write_str_at(pdir, "scaling_governor", "schedutil\n");

        /* Non-crossing frequency update: lower min -> set max -> raise min */
        if (c->base_min > 0)
            (void)write_long_at(pdir, "scaling_min_freq", c->base_min);
        if (target_max > 0) {
            if (write_long_at(pdir, "scaling_max_freq", target_max) < 0 && target_max > c->base_max)
                (void)write_long_at(pdir, "scaling_max_freq", c->base_max);
        }
        if (target_min > 0)
            (void)write_long_at(pdir, "scaling_min_freq", target_min);

        if (strcmp(target_gov, "schedutil") == 0) {
            char su_dir[PATH_MAX + 16];
            snprintf(su_dir, sizeof(su_dir), "%s/schedutil", pdir);
            (void)write_long_at(su_dir, "rate_limit_us", rate_us);
        }
    }

    if (!want_boost)
        (void)write_str_at(rp->cpufreq_root, "boost", "0\n");

    struct gpu_devfreq gpu;
    discover_gpu_devfreq(rp, &gpu);
    if (gpu.present) {
        long gmin = gpu.min_opp;
        long gmax = gpu.max_opp;
        long poll_ms = 50;

        if (standby) {
            gmin = gpu.min_opp;
            gmax = nearest_opp(gpu.avail, gpu.n_avail, (gpu.max_opp * 45) / 100);
            poll_ms = 100;
        } else if (mode == PERF_POWERSAVE) {
            gmin = gpu.min_opp;
            gmax = nearest_opp(gpu.avail, gpu.n_avail, (gpu.max_opp * 60) / 100);
            poll_ms = 100;
        } else if (mode == PERF_PERFORMANCE) {
            gmin = nearest_opp(gpu.avail, gpu.n_avail, (gpu.max_opp * 60) / 100);
            gmax = gpu.max_opp;
            poll_ms = 20;
        } else {
            gmin = gpu.min_opp;
            gmax = gpu.max_opp;
            poll_ms = 50;
        }

        if (gmin > gmax)
            gmin = gmax;

        (void)write_str_at(gpu.dir, "governor", "simple_ondemand\n");
        if (gpu.min_opp > 0)
            (void)write_long_at(gpu.dir, "min_freq", gpu.min_opp);
        if (gmax > 0)
            (void)write_long_at(gpu.dir, "max_freq", gmax);
        if (gmin > 0)
            (void)write_long_at(gpu.dir, "min_freq", gmin);
        (void)write_long_at(gpu.dir, "polling_interval", poll_ms);
    }

    struct fan_hw fh;
    discover_fan_hw(rp, &fh);
    if (fh.present) {
        long cpuss0_mc = -1, peak_cpu_mc = -1, peak_gpu_mc = -1, hotspot_mc = -1;
        read_soc_temps(rp, &cpuss0_mc, &peak_cpu_mc, &peak_gpu_mc, &hotspot_mc);
        char desc[32];
        int stage = io_fan_stage ? *io_fan_stage : 0;
        int pwm = compute_target_fan_pwm(mode, standby, fan_ovr, hotspot_mc, &stage, desc, sizeof(desc));
        if (io_fan_stage)
            *io_fan_stage = stage;
        /* If daemon is active, daemon owns smooth multi-sensor curves; if standalone
         * oneshot without daemon, keep kernel thermal auto enabled in balanced/eco. */
        bool pause_tz = daemon_controls_fan || standby || (mode == PERF_PERFORMANCE) || (fan_ovr != FAN_OVR_PROFILE);
        apply_fan_pwm(&fh, pwm, pause_tz);
    }

    return 0;
}

static void collect_telemetry(const struct runtime_paths *rp, enum perf_mode mode,
                              bool standby, enum fan_override fan_ovr,
                              const char *fan_desc_hint, struct perf_telemetry *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = mode;
    out->standby = standby;
    out->fan_ovr = fan_ovr;

    long b = 0;
    if (read_long_at(rp->cpufreq_root, "boost", &b) == 0)
        out->boost = b ? 1 : 0;

    read_soc_temps(rp, &out->cpuss0_mc, &out->peak_cpu_mc, &out->peak_gpu_mc, &out->hotspot_mc);
    out->n_clusters = discover_cpu_clusters(rp, out->clusters, MAX_POLICIES);
    discover_gpu_devfreq(rp, &out->gpu);

    struct fan_hw fh;
    discover_fan_hw(rp, &fh);
    out->fan_pwm = -1;
    out->fan_rpm = -1;
    out->fan_duty_pct = 0;
    if (fh.present) {
        (void)read_long_at(fh.hwmon_dir, "pwm1", &out->fan_pwm);
        (void)read_long_at(fh.hwmon_dir, "fan1_input", &out->fan_rpm);
        if (out->fan_pwm >= 0)
            out->fan_duty_pct = (int)((out->fan_pwm * 100 + 127) / 255);
    }
    if (fan_desc_hint && *fan_desc_hint) {
        snprintf(out->fan_mode_desc, sizeof(out->fan_mode_desc), "%s", fan_desc_hint);
    } else {
        int dummy = 0;
        (void)compute_target_fan_pwm(mode, standby, fan_ovr, out->hotspot_mc,
                                     &dummy, out->fan_mode_desc, sizeof(out->fan_mode_desc));
    }
}

static void format_telemetry_json(const struct perf_telemetry *t, char *buf, size_t bufsz)
{
    size_t off = 0;
    int n = snprintf(
        buf, bufsz,
        "{\"mode\":\"%s\",\"label_zh\":\"%s\",\"label_en\":\"%s\","
        "\"standby\":%s,\"fan_override\":\"%s\",\"boost\":%d,"
        "\"cpuss0_c\":%.1f,\"peak_cpu_c\":%.1f,\"peak_gpu_c\":%.1f,\"hotspot_c\":%.1f,"
        "\"fan_pwm\":%ld,\"fan_duty_percent\":%d,\"fan_rpm\":%ld,\"fan_mode\":\"%s\","
        "\"gpu\":{\"present\":%s,\"governor\":\"%s\",\"min_mhz\":%ld,\"max_mhz\":%ld,\"cur_mhz\":%ld},"
        "\"clusters\":[",
        mode_to_id(t->mode), mode_to_zh(t->mode), mode_to_en(t->mode),
        t->standby ? "true" : "false",
        fan_ovr_to_str(t->fan_ovr),
        t->boost,
        t->cpuss0_mc >= 0 ? t->cpuss0_mc / 1000.0 : -1.0,
        t->peak_cpu_mc >= 0 ? t->peak_cpu_mc / 1000.0 : -1.0,
        t->peak_gpu_mc >= 0 ? t->peak_gpu_mc / 1000.0 : -1.0,
        t->hotspot_mc >= 0 ? t->hotspot_mc / 1000.0 : -1.0,
        t->fan_pwm >= 0 ? t->fan_pwm : 0L,
        t->fan_duty_pct,
        t->fan_rpm >= 0 ? t->fan_rpm : 0L,
        t->fan_mode_desc,
        t->gpu.present ? "true" : "false",
        t->gpu.present ? t->gpu.cur_gov : "none",
        t->gpu.present ? t->gpu.cur_min / 1000000L : 0L,
        t->gpu.present ? t->gpu.cur_max / 1000000L : 0L,
        t->gpu.present ? t->gpu.cur_freq / 1000000L : 0L
    );
    if (n > 0)
        off = (size_t)n < bufsz ? (size_t)n : bufsz - 1;

    for (int i = 0; i < t->n_clusters && off + 128 < bufsz; i++) {
        const struct cpu_cluster *c = &t->clusters[i];
        n = snprintf(buf + off, bufsz - off,
                     "%s{\"policy\":\"%s\",\"governor\":\"%s\",\"min_khz\":%ld,\"max_khz\":%ld,\"cur_khz\":%ld}",
                     i > 0 ? "," : "",
                     c->policy_name, c->cur_gov, c->cur_min, c->cur_max, c->cur_freq);
        if (n > 0)
            off += (size_t)n < (bufsz - off) ? (size_t)n : (bufsz - off - 1);
    }

    if (off + 4 < bufsz)
        snprintf(buf + off, bufsz - off, "]}\n");
}

static void format_telemetry_human(const struct perf_telemetry *t, FILE *fp)
{
    fprintf(fp, "NX769J Performance & Thermal Profile\n");
    fprintf(fp, "  Active Mode   : %s (%s / %s)%s\n",
            mode_to_id(t->mode), mode_to_zh(t->mode), mode_to_en(t->mode),
            t->standby ? " [SCREEN-OFF STANDBY]" : "");
    fprintf(fp, "  CPU Boost     : %d\n", t->boost);
    for (int i = 0; i < t->n_clusters; i++) {
        const struct cpu_cluster *c = &t->clusters[i];
        fprintf(fp, "  %-13s : gov=%-10s min=%7ld kHz  max=%7ld kHz  cur=%7ld kHz\n",
                c->policy_name, c->cur_gov, c->cur_min, c->cur_max, c->cur_freq);
    }
    if (t->gpu.present) {
        fprintf(fp, "  Adreno 750    : gov=%-14s min=%4ld MHz  max=%4ld MHz  cur=%4ld MHz\n",
                t->gpu.cur_gov,
                t->gpu.cur_min / 1000000L,
                t->gpu.cur_max / 1000000L,
                t->gpu.cur_freq / 1000000L);
    }
    fprintf(fp, "  Temperatures  : hotspot=%.1f C (cpuss0=%.1f C, cpu_peak=%.1f C, gpu_peak=%.1f C)\n",
            t->hotspot_mc >= 0 ? t->hotspot_mc / 1000.0 : -1.0,
            t->cpuss0_mc >= 0 ? t->cpuss0_mc / 1000.0 : -1.0,
            t->peak_cpu_mc >= 0 ? t->peak_cpu_mc / 1000.0 : -1.0,
            t->peak_gpu_mc >= 0 ? t->peak_gpu_mc / 1000.0 : -1.0);
    fprintf(fp, "  Turbo Fan     : %s | PWM=%ld/255 (%d%%) | %ld RPM (override=%s)\n",
            t->fan_mode_desc,
            t->fan_pwm >= 0 ? t->fan_pwm : 0L,
            t->fan_duty_pct,
            t->fan_rpm >= 0 ? t->fan_rpm : 0L,
            fan_ovr_to_str(t->fan_ovr));
}

static void parse_runtime_flags_from_state_json(const struct runtime_paths *rp,
                                                bool *out_standby,
                                                enum fan_override *out_ovr)
{
    *out_standby = false;
    *out_ovr = FAN_OVR_PROFILE;
    char buf[2048];
    if (read_trimmed(rp->state_json, buf, sizeof(buf)) < 0)
        return;
    if (strstr(buf, "\"standby\":true") != NULL)
        *out_standby = true;
    const char *p = strstr(buf, "\"fan_override\":\"");
    if (p) {
        p += strlen("\"fan_override\":\"");
        char val[32];
        size_t i = 0;
        while (p[i] && p[i] != '"' && i + 1 < sizeof(val)) {
            val[i] = p[i];
            i++;
        }
        val[i] = '\0';
        (void)parse_fan_ovr(val, out_ovr);
    }
}

static int write_runtime_state_json(const struct runtime_paths *rp, const struct perf_telemetry *t)
{
    int rc = mkdir_p(rp->run_dir, 0755);
    if (rc < 0)
        return rc;
    char json[4096];
    format_telemetry_json(t, json, sizeof(json));
    return atomic_write_file(rp->state_json, json, 0644);
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        off += (size_t)w;
    }
    return 0;
}

static int try_daemon_rpc(const struct runtime_paths *rp, const char *req_line,
                          char *resp_buf, size_t resp_sz)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(rp->ctrl_sock) >= sizeof(addr.sun_path))
        return -ENAMETOOLONG;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", rp->ctrl_sock);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;
    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fdfl >= 0)
        (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        return -saved;
    }

    if (send_all(fd, req_line, strlen(req_line)) < 0) {
        int saved = errno;
        close(fd);
        return -saved;
    }

    size_t total = 0;
    uint64_t start = monotonic_ms();
    while (total + 1 < resp_sz) {
        uint64_t elapsed = monotonic_ms() - start;
        if (elapsed >= 2500)
            break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, (int)(2500 - elapsed));
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR)
                continue;
            break;
        }
        ssize_t r = recv(fd, resp_buf + total, resp_sz - 1 - total, 0);
        if (r <= 0)
            break;
        total += (size_t)r;
        if (memchr(resp_buf, '\n', total) != NULL)
            break;
    }
    resp_buf[total] = '\0';
    close(fd);
    return total > 0 ? 0 : -EIO;
}

static void maybe_desktop_notify(const struct perf_telemetry *t)
{
    pid_t pid = fork();
    if (pid != 0)
        return;
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        (void)dup2(nullfd, STDOUT_FILENO);
        (void)dup2(nullfd, STDERR_FILENO);
        if (nullfd > 2)
            close(nullfd);
    }
    char title[128], body[256];
    snprintf(title, sizeof(title), "性能模式: %s (%s)",
             mode_to_zh(t->mode), mode_to_en(t->mode));
    if (t->fan_rpm > 0) {
        snprintf(body, sizeof(body), "CPU %.0f°C · Fan %ld RPM (%d%%)",
                 t->hotspot_mc >= 0 ? t->hotspot_mc / 1000.0 : 0.0,
                 t->fan_rpm, t->fan_duty_pct);
    } else {
        snprintf(body, sizeof(body), "CPU %.0f°C · Fan Silent (0 RPM)",
                 t->hotspot_mc >= 0 ? t->hotspot_mc / 1000.0 : 0.0);
    }
    execlp("notify-send", "notify-send", "-a", "NX769J Performance", "-t", "2200", title, body, (char *)NULL);
    _exit(0);
}

static int run_daemon(const struct runtime_paths *rp)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    long tick_ms = 2000;
    const char *env_tick = getenv("NX769J_PERF_DAEMON_TICK_MS");
    if (env_tick && *env_tick) {
        long v = atol(env_tick);
        if (v >= 20 && v <= 60000)
            tick_ms = v;
    }

    int rc = mkdir_p(rp->run_dir, 0755);
    if (rc < 0) {
        fprintf(stderr, "nx769j-perf: mkdir %s failed: %s\n", rp->run_dir, strerror(-rc));
        return 1;
    }
    (void)mkdir_p(rp->state_dir, 0755);

    /* Wait briefly at boot for pwmfan hwmon if modules are still probing */
    struct fan_hw fh;
    for (int i = 0; i < 30; i++) {
        discover_fan_hw(rp, &fh);
        if (fh.present || strcmp(rp->sysfs_root, "/sys") != 0)
            break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
    }

    unlink(rp->ctrl_sock);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("nx769j-perf: socket");
        return 1;
    }
    int fl = fcntl(lfd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(lfd, F_SETFL, fl | O_NONBLOCK);
    int fdfl = fcntl(lfd, F_GETFD, 0);
    if (fdfl >= 0)
        (void)fcntl(lfd, F_SETFD, fdfl | FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(rp->ctrl_sock) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "nx769j-perf: ctrl_sock path too long\n");
        close(lfd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", rp->ctrl_sock);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("nx769j-perf: bind");
        close(lfd);
        return 1;
    }

    if (geteuid() == 0) {
        struct group *grp = getgrnam("wheel");
        if (grp) {
            if (chown(rp->ctrl_sock, 0, grp->gr_gid) == 0)
                (void)chmod(rp->ctrl_sock, 0660);
            else
                (void)chmod(rp->ctrl_sock, 0666);
        } else {
            (void)chmod(rp->ctrl_sock, 0666);
        }
    } else {
        (void)chmod(rp->ctrl_sock, 0666);
    }

    if (listen(lfd, 16) < 0) {
        perror("nx769j-perf: listen");
        close(lfd);
        unlink(rp->ctrl_sock);
        return 1;
    }

    enum perf_mode cur_mode = load_saved_mode(rp);
    (void)save_selected_mode(rp, cur_mode);
    bool standby = false;
    enum fan_override fan_ovr = FAN_OVR_PROFILE;
    int fan_stage = 0;

    (void)apply_hw_profile(rp, cur_mode, standby, fan_ovr, true, &fan_stage);
    struct perf_telemetry tel;
    collect_telemetry(rp, cur_mode, standby, fan_ovr, NULL, &tel);
    (void)write_runtime_state_json(rp, &tel);

    while (!g_stop) {
        int wait_ms = (standby && tel.fan_pwm <= 0) ? (int)(tick_ms * 2) : (int)tick_ms;
        struct pollfd pfd = { .fd = lfd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0)
                    break;
                int cfl = fcntl(cfd, F_GETFD, 0);
                if (cfl >= 0)
                    (void)fcntl(cfd, F_SETFD, cfl | FD_CLOEXEC);

                char cmd[128];
                ssize_t n = 0;
                struct pollfd cpfd = { .fd = cfd, .events = POLLIN, .revents = 0 };
                if (poll(&cpfd, 1, 200) > 0)
                    n = recv(cfd, cmd, sizeof(cmd) - 1, 0);
                if (n > 0) {
                    cmd[n] = '\0';
                    for (ssize_t i = 0; i < n; i++) {
                        if (cmd[i] == '\r' || cmd[i] == '\n') {
                            cmd[i] = '\0';
                            break;
                        }
                    }

                    bool reapply = false;
                    if (strncmp(cmd, "MODE ", 5) == 0) {
                        enum perf_mode req;
                        if (parse_mode_str(cmd + 5, &req)) {
                            cur_mode = req;
                            standby = false;
                            fan_ovr = FAN_OVR_PROFILE;
                            fan_stage = 0;
                            (void)save_selected_mode(rp, cur_mode);
                            reapply = true;
                        }
                    } else if (strcasecmp(cmd, "CYCLE") == 0) {
                        cur_mode = next_cycle_mode(cur_mode);
                        standby = false;
                        fan_ovr = FAN_OVR_PROFILE;
                        fan_stage = 0;
                        (void)save_selected_mode(rp, cur_mode);
                        reapply = true;
                    } else if (strcasecmp(cmd, "APPLY") == 0 || strcasecmp(cmd, "STANDBY 0") == 0) {
                        cur_mode = load_saved_mode(rp);
                        standby = false;
                        fan_stage = 0;
                        reapply = true;
                    } else if (strcasecmp(cmd, "STANDBY 1") == 0 || strcasecmp(cmd, "STANDBY") == 0) {
                        standby = true;
                        fan_stage = 0;
                        reapply = true;
                    } else if (strncmp(cmd, "FAN ", 4) == 0) {
                        enum fan_override req_ovr;
                        if (parse_fan_ovr(cmd + 4, &req_ovr)) {
                            fan_ovr = req_ovr;
                            fan_stage = 0;
                            reapply = true;
                        }
                    }

                    if (reapply)
                        (void)apply_hw_profile(rp, cur_mode, standby, fan_ovr, true, &fan_stage);

                    collect_telemetry(rp, cur_mode, standby, fan_ovr, NULL, &tel);
                    (void)write_runtime_state_json(rp, &tel);

                    char json[4096];
                    format_telemetry_json(&tel, json, sizeof(json));
                    (void)send_all(cfd, json, strlen(json));
                }
                close(cfd);
            }
        } else {
            /* Periodic thermal curve tick */
            discover_fan_hw(rp, &fh);
            char desc[32];
            long cpuss0_mc = -1, peak_cpu_mc = -1, peak_gpu_mc = -1, hotspot_mc = -1;
            read_soc_temps(rp, &cpuss0_mc, &peak_cpu_mc, &peak_gpu_mc, &hotspot_mc);
            int pwm = compute_target_fan_pwm(cur_mode, standby, fan_ovr, hotspot_mc,
                                             &fan_stage, desc, sizeof(desc));
            apply_fan_pwm(&fh, pwm, true);
            collect_telemetry(rp, cur_mode, standby, fan_ovr, desc, &tel);
            (void)write_runtime_state_json(rp, &tel);
        }
    }

    /* Clean exit: restore kernel cpuss0-thermal autonomous fan governance */
    discover_fan_hw(rp, &fh);
    if (fh.present && fh.has_tz_cpuss0)
        (void)write_str_at(fh.tz_cpuss0_dir, "mode", "enabled\n");

    close(lfd);
    unlink(rp->ctrl_sock);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <command> [--json] [--notify]\n"
            "\n"
            "Commands:\n"
            "  status [--json]                     Show active profile, clocks, temps & fan RPM\n"
            "  powersave | balanced | performance  Switch to Eco / Balanced / Diablo-Turbo\n"
            "  cycle                               Cycle powersave -> balanced -> performance\n"
            "  set <mode>                          Set mode explicitly\n"
            "  apply                               Re-apply saved mode (wakes from standby)\n"
            "  standby [on|off]                    Screen-off deep powersave + 0 RPM (<65 C)\n"
            "  fan-override <profile|low|med|high|max|off>\n"
            "  is-mode <mode>                      Exit 0 if saved mode matches, else 1\n"
            "  daemon                              Run persistent thermal & profile service\n",
            prog);
}

#ifndef NX769J_PERF_LIBRARY_ONLY
int main(int argc, char **argv)
{
    struct runtime_paths rp;
    init_paths(&rp);

    bool json_out = false;
    bool notify = false;
    bool escalated = false;
    const char *pos[8];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "-j") == 0) {
            json_out = true;
        } else if (strcmp(argv[i], "--notify") == 0) {
            notify = true;
        } else if (strcmp(argv[i], "--escalated") == 0) {
            escalated = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (npos < 8) {
            pos[npos++] = argv[i];
        }
    }

    const char *cmd = (npos >= 1) ? pos[0] : "status";

    if (strcmp(cmd, "daemon") == 0)
        return run_daemon(&rp);

    if (strcmp(cmd, "is-mode") == 0) {
        if (npos < 2)
            return 2;
        enum perf_mode want;
        if (!parse_mode_str(pos[1], &want))
            return 2;
        return (load_saved_mode(&rp) == want) ? 0 : 1;
    }

    if (strcmp(cmd, "status") == 0) {
        enum perf_mode m = load_saved_mode(&rp);
        bool sb = false;
        enum fan_override ovr = FAN_OVR_PROFILE;
        parse_runtime_flags_from_state_json(&rp, &sb, &ovr);
        struct perf_telemetry tel;
        collect_telemetry(&rp, m, sb, ovr, NULL, &tel);
        if (json_out) {
            char json[4096];
            format_telemetry_json(&tel, json, sizeof(json));
            fputs(json, stdout);
        } else {
            format_telemetry_human(&tel, stdout);
        }
        return 0;
    }

    /* Determine mutating action */
    char daemon_req[64] = "";
    enum perf_mode target_mode = load_saved_mode(&rp);
    bool target_standby = false;
    bool update_saved_mode = false;
    enum fan_override target_ovr = FAN_OVR_PROFILE;
    bool is_fan_ovr = false;

    enum perf_mode parsed_direct;
    if (parse_mode_str(cmd, &parsed_direct)) {
        target_mode = parsed_direct;
        update_saved_mode = true;
        snprintf(daemon_req, sizeof(daemon_req), "MODE %s\n", mode_to_id(target_mode));
    } else if (strcmp(cmd, "set") == 0) {
        if (npos < 2 || !parse_mode_str(pos[1], &target_mode)) {
            usage(argv[0]);
            return 2;
        }
        update_saved_mode = true;
        snprintf(daemon_req, sizeof(daemon_req), "MODE %s\n", mode_to_id(target_mode));
    } else if (strcmp(cmd, "cycle") == 0) {
        target_mode = next_cycle_mode(target_mode);
        update_saved_mode = true;
        snprintf(daemon_req, sizeof(daemon_req), "CYCLE\n");
    } else if (strcmp(cmd, "apply") == 0) {
        target_standby = false;
        snprintf(daemon_req, sizeof(daemon_req), "APPLY\n");
    } else if (strcmp(cmd, "standby") == 0) {
        if (npos >= 2 && strcasecmp(pos[1], "off") == 0) {
            target_standby = false;
            snprintf(daemon_req, sizeof(daemon_req), "STANDBY 0\n");
        } else {
            target_standby = true;
            snprintf(daemon_req, sizeof(daemon_req), "STANDBY 1\n");
        }
    } else if (strcmp(cmd, "fan-override") == 0) {
        if (npos < 2 || !parse_fan_ovr(pos[1], &target_ovr)) {
            usage(argv[0]);
            return 2;
        }
        is_fan_ovr = true;
        snprintf(daemon_req, sizeof(daemon_req), "FAN %s\n", fan_ovr_to_str(target_ovr));
    } else {
        usage(argv[0]);
        return 2;
    }

    /* 1. Fast path: talk to running nx769j-perf.service Unix socket */
    char resp[4096];
    if (try_daemon_rpc(&rp, daemon_req, resp, sizeof(resp)) == 0) {
        enum perf_mode final_m = load_saved_mode(&rp);
        bool sb = false;
        enum fan_override ovr = FAN_OVR_PROFILE;
        parse_runtime_flags_from_state_json(&rp, &sb, &ovr);
        struct perf_telemetry tel;
        collect_telemetry(&rp, final_m, sb, ovr, NULL, &tel);
        if (notify)
            maybe_desktop_notify(&tel);
        if (json_out) {
            fputs(resp, stdout);
        } else {
            format_telemetry_human(&tel, stdout);
        }
        return 0;
    }

    /* 2. Fallback path when daemon socket is offline: escalate via sudo -n if unprivileged */
    if (geteuid() != 0 && strcmp(rp.sysfs_root, "/sys") == 0 && !escalated) {
        char *sudo_argv[16];
        int k = 0;
        sudo_argv[k++] = "sudo";
        sudo_argv[k++] = "-n";
        sudo_argv[k++] = "/usr/local/bin/nx769j-perf";
        sudo_argv[k++] = "--escalated";
        for (int i = 0; i < npos && k < 13; i++)
            sudo_argv[k++] = (char *)pos[i];
        if (json_out)
            sudo_argv[k++] = "--json";
        sudo_argv[k] = NULL;

        pid_t pid = fork();
        if (pid == 0) {
            execvp("sudo", sudo_argv);
            _exit(127);
        } else if (pid > 0) {
            int st = 0;
            if (waitpid(pid, &st, 0) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                if (notify) {
                    struct perf_telemetry tel;
                    collect_telemetry(&rp, load_saved_mode(&rp), target_standby, target_ovr, NULL, &tel);
                    maybe_desktop_notify(&tel);
                }
                return 0;
            }
        }
        fprintf(stderr, "nx769j-perf: neither daemon socket (%s) nor sudo available\n", rp.ctrl_sock);
        return 1;
    }

    /* 3. Direct apply (root or synthetic test sysfs) */
    bool prev_sb = false;
    enum fan_override prev_ovr = FAN_OVR_PROFILE;
    parse_runtime_flags_from_state_json(&rp, &prev_sb, &prev_ovr);

    if (update_saved_mode) {
        int rc = save_selected_mode(&rp, target_mode);
        if (rc < 0) {
            fprintf(stderr, "nx769j-perf: failed saving mode to %s: %s\n",
                    rp.mode_file, strerror(-rc));
            return 1;
        }
        prev_ovr = FAN_OVR_PROFILE;
    }
    if (is_fan_ovr) {
        prev_ovr = target_ovr;
        target_standby = prev_sb;
    }

    int stage = 0;
    (void)apply_hw_profile(&rp, target_mode, target_standby, prev_ovr, false, &stage);
    struct perf_telemetry tel;
    collect_telemetry(&rp, target_mode, target_standby, prev_ovr, NULL, &tel);
    (void)write_runtime_state_json(&rp, &tel);

    if (notify)
        maybe_desktop_notify(&tel);

    if (json_out) {
        char json[4096];
        format_telemetry_json(&tel, json, sizeof(json));
        fputs(json, stdout);
    } else {
        format_telemetry_human(&tel, stdout);
    }
    return 0;
}
#endif
