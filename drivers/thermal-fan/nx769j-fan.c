// SPDX-License-Identifier: GPL-2.0-only
/*
 * nx769j-fan.c — Built-in Turbo Cooling Fan CLI & Thermal Init/Daemon Helper
 * Target: Nubia RedMagic 9 Pro (NX769J, SM8650) on Mainline Linux 7.1.1-nx769j+
 *
 * Hardware (verified from factory soc_fan.ko + pwm-qti-lpg.ko + live-fdt.dts):
 *   - Boost power enable : TLMM gpio173 (active-high, 100 ms startup delay)
 *                          exposed via regulator-fixed "vreg_fan_pwr" (fan-supply)
 *   - Speed PWM          : PM8550 GPIO9 func1 -> LPG channel 3 (0xeb00)
 *                          40,000 ns period (25.0 kHz), 6 levels: 0/20/40/60/80/100%
 *   - Tachometer         : TLMM gpio174 (IRQ_TYPE_EDGE_FALLING, 3 pulses/rev)
 *
 * Mainline pwm-fan.ko notes (verified from disassembly of pwm-fan.ko):
 *   - Probe defaults pwm1_enable = 1 (pwm_disable_reg_enable), keeping fan-supply
 *     enabled at pwm1 = 0, and initializes speed to max_state (255 = 100%).
 *   - Writing pwm1_enable = 3 (pwm_disable_reg_disable) instructs pwm-fan to call
 *     regulator_disable(fan-supply) whenever pwm1 == 0 (0 mA idle draw on gpio173)
 *     and regulator_enable(fan-supply) (honoring 100 ms startup-delay-us) on pwm1 > 0.
 *   - Default automatic mode uses kernel step_wise governor via cpuss0-thermal
 *     cooling-maps (zero background daemon overhead). Manual modes pause only
 *     cpuss0-thermal/mode ("disabled") while 18 sibling CPU/GPU thermal zones
 *     keep 110 C hot / 115 C critical hardware protection active.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HWMON_ROOT   "/sys/class/hwmon"
#define THERMAL_ROOT "/sys/class/thermal"

/* Factory 6-level PWM table (0%, 20%, 40%, 60%, 80%, 100% of 255) */
static const int LEVEL_PWM[6] = { 0, 51, 102, 153, 204, 255 };

/* Active thermal trip thresholds in millidegrees Celsius (5 C hysteresis) */
static const int TRIP_MC[5] = { 55000, 65000, 75000, 85000, 92000 };
#define HYST_MC 5000

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}

struct fan_paths {
    char hwmon_dir[PATH_MAX];
    char cdev_dir[PATH_MAX];
    char tz_cpuss0_dir[PATH_MAX];
    bool has_cdev;
    bool has_tz_cpuss0;
};

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
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return 0;
}

static int read_int_file(const char *dir, const char *leaf, long *out)
{
    char path[PATH_MAX];
    char buf[64];
    int rc = snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;
    rc = read_trimmed(path, buf, sizeof(buf));
    if (rc < 0)
        return rc;
    errno = 0;
    char *end = NULL;
    long val = strtol(buf, &end, 10);
    if (errno != 0 || end == buf)
        return -EINVAL;
    *out = val;
    return 0;
}

static int write_str_file(const char *dir, const char *leaf, const char *val)
{
    char path[PATH_MAX];
    int rc = snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    size_t len = strlen(val);
    ssize_t wr = write(fd, val, len);
    int saved = errno;
    close(fd);
    if (wr < 0)
        return -saved;
    if ((size_t)wr != len)
        return -EIO;
    return 0;
}

static int write_int_file(const char *dir, const char *leaf, long val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld\n", val);
    return write_str_file(dir, leaf, buf);
}

static int discover_fan(struct fan_paths *fp)
{
    memset(fp, 0, sizeof(*fp));

    DIR *d = opendir(HWMON_ROOT);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "hwmon", 5) != 0)
                continue;
            char p[PATH_MAX], name[64];
            snprintf(p, sizeof(p), "%s/%s/name", HWMON_ROOT, de->d_name);
            if (read_trimmed(p, name, sizeof(name)) == 0 && strcmp(name, "pwmfan") == 0) {
                snprintf(fp->hwmon_dir, sizeof(fp->hwmon_dir), "%s/%s", HWMON_ROOT, de->d_name);
                break;
            }
        }
        closedir(d);
    }

    if (fp->hwmon_dir[0] == '\0')
        return -ENOENT;

    d = opendir(THERMAL_ROOT);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "cooling_device", 14) == 0 && !fp->has_cdev) {
                char p[PATH_MAX], type[64];
                snprintf(p, sizeof(p), "%s/%s/type", THERMAL_ROOT, de->d_name);
                if (read_trimmed(p, type, sizeof(type)) == 0 && strcmp(type, "pwm-fan") == 0) {
                    snprintf(fp->cdev_dir, sizeof(fp->cdev_dir), "%s/%s", THERMAL_ROOT, de->d_name);
                    fp->has_cdev = true;
                }
            } else if (strncmp(de->d_name, "thermal_zone", 12) == 0 && !fp->has_tz_cpuss0) {
                char p[PATH_MAX], type[64];
                snprintf(p, sizeof(p), "%s/%s/type", THERMAL_ROOT, de->d_name);
                if (read_trimmed(p, type, sizeof(type)) == 0 && strcmp(type, "cpuss0-thermal") == 0) {
                    snprintf(fp->tz_cpuss0_dir, sizeof(fp->tz_cpuss0_dir), "%s/%s", THERMAL_ROOT, de->d_name);
                    fp->has_tz_cpuss0 = true;
                }
            }
        }
        closedir(d);
    }

    return 0;
}

static void read_soc_temps(long *cpuss0_mc, long *peak_cpu_mc, long *peak_gpu_mc)
{
    *cpuss0_mc = -1;
    *peak_cpu_mc = -1;
    *peak_gpu_mc = -1;

    DIR *d = opendir(THERMAL_ROOT);
    if (!d)
        return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "thermal_zone", 12) != 0)
            continue;
        char zdir[PATH_MAX], p[PATH_MAX + 32], type[64];
        snprintf(zdir, sizeof(zdir), "%s/%s", THERMAL_ROOT, de->d_name);
        snprintf(p, sizeof(p), "%s/type", zdir);
        if (read_trimmed(p, type, sizeof(type)) < 0)
            continue;
        long t = -1;
        if (read_int_file(zdir, "temp", &t) < 0 || t <= -50000 || t >= 200000)
            continue;
        if (strcmp(type, "cpuss0-thermal") == 0)
            *cpuss0_mc = t;
        if (strncmp(type, "cpu", 3) == 0 && t > *peak_cpu_mc)
            *peak_cpu_mc = t;
        if (strncmp(type, "gpuss", 5) == 0 && t > *peak_gpu_mc)
            *peak_gpu_mc = t;
    }
    closedir(d);
}

static int level_for_temp_with_hyst(long temp_mc, int prev_level)
{
    if (temp_mc < 0)
        return 1; /* Safe conservative fallback (20%) if sensors unreadable */

    int lvl = prev_level;
    if (lvl < 0)
        lvl = 0;
    if (lvl > 5)
        lvl = 5;

    /* Step up immediately upon crossing active trip thresholds */
    while (lvl < 5 && temp_mc >= TRIP_MC[lvl])
        lvl++;

    /* Step down only once temperature drops below (trip - hysteresis) */
    while (lvl > 0 && temp_mc < (TRIP_MC[lvl - 1] - HYST_MC))
        lvl--;

    return lvl;
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int ensure_reg_off_at_zero(const struct fan_paths *fp)
{
    long en = -1;
    if (read_int_file(fp->hwmon_dir, "pwm1_enable", &en) == 0 && en == 3)
        return 0;
    return write_int_file(fp->hwmon_dir, "pwm1_enable", 3);
}

static int set_kernel_auto_mode(const struct fan_paths *fp, bool enable)
{
    if (!fp->has_tz_cpuss0)
        return 0;
    return write_str_file(fp->tz_cpuss0_dir, "mode", enable ? "enabled\n" : "disabled\n");
}

static int print_status(const struct fan_paths *fp, bool json_out)
{
    long pwm = -1, pwm_en = -1, rpm = -1, cur_state = -1, max_state = -1;
    long cpuss0_mc = -1, peak_cpu_mc = -1, peak_gpu_mc = -1;
    char tz_mode[32] = "unknown";

    read_int_file(fp->hwmon_dir, "pwm1", &pwm);
    read_int_file(fp->hwmon_dir, "pwm1_enable", &pwm_en);
    read_int_file(fp->hwmon_dir, "fan1_input", &rpm);

    if (fp->has_cdev) {
        read_int_file(fp->cdev_dir, "cur_state", &cur_state);
        read_int_file(fp->cdev_dir, "max_state", &max_state);
    }
    if (fp->has_tz_cpuss0) {
        char p[PATH_MAX + 32];
        snprintf(p, sizeof(p), "%s/mode", fp->tz_cpuss0_dir);
        read_trimmed(p, tz_mode, sizeof(tz_mode));
    }

    read_soc_temps(&cpuss0_mc, &peak_cpu_mc, &peak_gpu_mc);

    const char *ctrl_mode = "manual";
    if (pwm_en == 0)
        ctrl_mode = "forced-off";
    else if (strcmp(tz_mode, "enabled") == 0)
        ctrl_mode = "auto";

    int duty_pct = (pwm >= 0) ? (int)((pwm * 100 + 127) / 255) : -1;

    if (json_out) {
        printf("{\"mode\":\"%s\",\"tz_mode\":\"%s\",\"pwm\":%ld,\"duty_percent\":%d,"
               "\"pwm_enable\":%ld,\"level\":%ld,\"max_level\":%ld,\"rpm\":%ld,"
               "\"cpuss0_c\":%.1f,\"peak_cpu_c\":%.1f,\"peak_gpu_c\":%.1f,"
               "\"hwmon\":\"%s\",\"cooling_device\":\"%s\"}\n",
               ctrl_mode, tz_mode, pwm, duty_pct, pwm_en, cur_state, max_state, rpm,
               cpuss0_mc >= 0 ? cpuss0_mc / 1000.0 : -1.0,
               peak_cpu_mc >= 0 ? peak_cpu_mc / 1000.0 : -1.0,
               peak_gpu_mc >= 0 ? peak_gpu_mc / 1000.0 : -1.0,
               fp->hwmon_dir, fp->has_cdev ? fp->cdev_dir : "");
        return 0;
    }

    printf("NX769J Turbo Fan Status\n");
    printf("  Control mode   : %s (cpuss0-thermal=%s, pwm1_enable=%ld)\n",
           ctrl_mode, tz_mode, pwm_en);
    printf("  Cooling state  : %ld / %ld\n", cur_state, max_state);
    printf("  PWM duty       : %ld / 255 (%d%% @ 25.0 kHz)\n", pwm, duty_pct);
    if (rpm >= 0)
        printf("  Tachometer     : %ld RPM\n", rpm);
    else
        printf("  Tachometer     : n/a\n");
    printf("  Temperatures   : cpuss0=%.1f C, peak_cpu=%.1f C, peak_gpu=%.1f C\n",
           cpuss0_mc >= 0 ? cpuss0_mc / 1000.0 : -1.0,
           peak_cpu_mc >= 0 ? peak_cpu_mc / 1000.0 : -1.0,
           peak_gpu_mc >= 0 ? peak_gpu_mc / 1000.0 : -1.0);
    printf("  Sysfs paths    : %s | %s\n",
           fp->hwmon_dir, fp->has_cdev ? fp->cdev_dir : "(no cdev)");
    return 0;
}

static int apply_manual_pwm(const struct fan_paths *fp, long pwm_val)
{
    if (pwm_val < 0 || pwm_val > 255)
        return -EINVAL;

    int rc = set_kernel_auto_mode(fp, false);
    if (rc < 0)
        fprintf(stderr, "warning: could not pause cpuss0-thermal (%s)\n", strerror(-rc));

    rc = ensure_reg_off_at_zero(fp);
    if (rc < 0) {
        fprintf(stderr, "error: failed writing pwm1_enable=3 (%s)\n", strerror(-rc));
        return rc;
    }

    rc = write_int_file(fp->hwmon_dir, "pwm1", pwm_val);
    if (rc < 0) {
        fprintf(stderr, "error: failed writing pwm1=%ld (%s)\n", pwm_val, strerror(-rc));
        return rc;
    }
    return 0;
}

static int apply_auto(const struct fan_paths *fp)
{
    int rc = ensure_reg_off_at_zero(fp);
    if (rc < 0) {
        fprintf(stderr, "error: failed writing pwm1_enable=3 (%s)\n", strerror(-rc));
        return rc;
    }

    long cpuss0_mc = -1, peak_cpu_mc = -1, peak_gpu_mc = -1;
    read_soc_temps(&cpuss0_mc, &peak_cpu_mc, &peak_gpu_mc);
    long ref_mc = cpuss0_mc >= 0 ? cpuss0_mc : peak_cpu_mc;
    int init_lvl = level_for_temp_with_hyst(ref_mc, 0);

    rc = write_int_file(fp->hwmon_dir, "pwm1", LEVEL_PWM[init_lvl]);
    if (rc < 0) {
        fprintf(stderr, "error: failed syncing initial pwm1 (%s)\n", strerror(-rc));
        return rc;
    }

    rc = set_kernel_auto_mode(fp, true);
    if (rc < 0) {
        fprintf(stderr, "error: failed enabling cpuss0-thermal (%s)\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int run_daemon(const struct fan_paths *fp)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int rc = set_kernel_auto_mode(fp, false);
    if (rc < 0)
        fprintf(stderr, "warning: could not pause cpuss0-thermal (%s)\n", strerror(-rc));

    rc = ensure_reg_off_at_zero(fp);
    if (rc < 0) {
        fprintf(stderr, "error: failed writing pwm1_enable=3 (%s)\n", strerror(-rc));
        return rc;
    }

    int cur_lvl = -1;
    while (!g_stop) {
        long cpuss0_mc = -1, peak_cpu_mc = -1, peak_gpu_mc = -1;
        read_soc_temps(&cpuss0_mc, &peak_cpu_mc, &peak_gpu_mc);
        long hot_mc = peak_cpu_mc;
        if (peak_gpu_mc > hot_mc)
            hot_mc = peak_gpu_mc;
        if (cpuss0_mc > hot_mc)
            hot_mc = cpuss0_mc;

        int next_lvl = level_for_temp_with_hyst(hot_mc, cur_lvl < 0 ? 0 : cur_lvl);
        if (next_lvl != cur_lvl) {
            if (write_int_file(fp->hwmon_dir, "pwm1", LEVEL_PWM[next_lvl]) == 0)
                cur_lvl = next_lvl;
        }
        sleep_ms(2000);
    }

    /* Restore autonomous kernel step_wise governance on clean exit */
    (void)apply_auto(fp);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <command> [args]\n"
            "\n"
            "Commands:\n"
            "  status [--json]   Show fan mode, PWM duty, RPM, and SoC temperatures\n"
            "  init              Boot init: set pwm1_enable=3 (0 mA boost off at idle) + auto\n"
            "  auto              Hand fan control to kernel thermal cooling-maps (default)\n"
            "  off               Turn fan & boost rail completely OFF (0%% PWM, gpio173=0)\n"
            "  low               Manual Level 1 (20%% PWM, 51/255)\n"
            "  med               Manual Level 3 (60%% PWM, 153/255)\n"
            "  high              Manual Level 4 (80%% PWM, 204/255)\n"
            "  max               Manual Level 5 (100%% PWM, 255/255)\n"
            "  level <0..5>      Manual discrete factory cooling state (0..5)\n"
            "  pwm <0..255>      Manual raw 8-bit PWM duty (0..255)\n"
            "  daemon            Optional multi-sensor (CPU+GPU max) 2s hysteresis loop\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *cmd = (argc >= 2) ? argv[1] : "status";
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }

    struct fan_paths fp;
    int max_tries = (strcmp(cmd, "init") == 0) ? 50 : 1;
    int rc = -ENOENT;
    for (int i = 0; i < max_tries; i++) {
        rc = discover_fan(&fp);
        if (rc == 0)
            break;
        sleep_ms(100);
    }
    if (rc < 0) {
        fprintf(stderr, "nx769j-fan: pwmfan hwmon device not found (are leds-qcom-lpg & pwm-fan loaded?)\n");
        return 1;
    }

    if (strcmp(cmd, "status") == 0) {
        bool json_out = (argc >= 3 && strcmp(argv[2], "--json") == 0);
        return print_status(&fp, json_out) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "init") == 0 || strcmp(cmd, "auto") == 0) {
        if (apply_auto(&fp) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "off") == 0) {
        if (apply_manual_pwm(&fp, 0) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "low") == 0) {
        if (apply_manual_pwm(&fp, LEVEL_PWM[1]) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "med") == 0) {
        if (apply_manual_pwm(&fp, LEVEL_PWM[3]) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "high") == 0) {
        if (apply_manual_pwm(&fp, LEVEL_PWM[4]) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "max") == 0) {
        if (apply_manual_pwm(&fp, LEVEL_PWM[5]) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "level") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }
        char *end = NULL;
        long lvl = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || lvl < 0 || lvl > 5) {
            fprintf(stderr, "error: level must be 0..5\n");
            return 2;
        }
        if (apply_manual_pwm(&fp, LEVEL_PWM[lvl]) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "pwm") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }
        char *end = NULL;
        long pwm_val = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || pwm_val < 0 || pwm_val > 255) {
            fprintf(stderr, "error: pwm must be 0..255\n");
            return 2;
        }
        if (apply_manual_pwm(&fp, pwm_val) < 0)
            return 1;
        return print_status(&fp, false) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "daemon") == 0)
        return run_daemon(&fp) == 0 ? 0 : 1;

    usage(argv[0]);
    return 2;
}
