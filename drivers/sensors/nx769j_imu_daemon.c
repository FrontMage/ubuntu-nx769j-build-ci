// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nx769j_imu_daemon.c — Nubia RedMagic 9 Pro (NX769J / SM8650) 6-Axis IMU Daemon
 *
 * Bridges Qualcomm ADSP QSH / SEE Sensors (SNS_CLIENT_SVC QMI service 400 over
 * AF_QIPCRTR) for the internal STMicroelectronics LSM6DSO 6-axis IMU ("accel" +
 * "gyro") to:
 *   1. Standard Linux evdev input device via /dev/cuse (/dev/input/eventN +
 *      /dev/input/by-id/platform-nx769j-lsm6dso-imu-event) with full EVIOC*
 *      support for evtest, libevdev, and udev input_id (INPUT_PROP_ACCELEROMETER).
 *   2. Atomic JSON state file (/run/nx769j-imu/state.json) + local UNIX stream
 *      socket (/run/nx769j-imu/stream.sock) feeding nx769j-sensor-proxy
 *      (net.hadess.SensorProxy for monitor-sensor & KDE Plasma 6 kwin_wayland).
 *
 * Build on target:
 *   gcc -O2 -Wall -Wextra -pthread nx769j_imu_daemon.c -lm -o /usr/local/sbin/nx769j-imu-daemon
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/fuse.h>
#include <linux/input.h>
#include <linux/qrtr.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

/* Qualcomm QSH / SNS_CLIENT_SVC (service 0x190 = 400) constants */
#define SNS_CLIENT_SVC_ID                              400U
#define QMI_CNTL_FLAG_REQ                              0x00U
#define QMI_CNTL_FLAG_RESP                             0x02U
#define QMI_CNTL_FLAG_IND                              0x04U

#define QMI_SNS_CLIENT_REQ_V01                         0x0020U
#define QMI_SNS_CLIENT_RESP_V01                        0x0020U
#define QMI_SNS_CLIENT_REPORT_IND_V01                  0x0021U
#define QMI_SNS_CLIENT_JUMBO_REPORT_IND_V01            0x0022U

#define SNS_SUID_LOOKUP_LOW                            0xababababababababULL
#define SNS_SUID_LOOKUP_HIGH                           0xababababababababULL

#define SNS_SUID_MSGID_SNS_SUID_REQ                    512U   /* 0x200 */
#define SNS_SUID_MSGID_SNS_SUID_EVENT                  768U   /* 0x300 */
#define SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_CONFIG     513U   /* 0x201 */
#define SNS_STD_SENSOR_MSGID_PHYSICAL_CONFIG_EVENT     768U   /* 0x300 */
#define SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_EVENT      1025U  /* 0x401 */

#define SNS_STD_CLIENT_PROCESSOR_APSS                  1U
#define SNS_CLIENT_DELIVERY_WAKEUP                     0U

/* Linux evdev INPUT_PROP_ACCELEROMETER scaling:
 *   ABS_X/Y/Z    : 1000 LSB / g       (±16 g   -> [-16000, +16000])
 *   ABS_RX/RY/RZ : 1000 LSB / (deg/s) (±2000 dps -> [-2000000, +2000000])
 */
#define GRAVITY_MPS2                                   9.80665f
#define RAD_TO_DEG                                     57.29577951308232f
#define ACCEL_RES_PER_G                                1000
#define ACCEL_ABS_MAX                                  16000
#define GYRO_RES_PER_DPS                               1000
#define GYRO_ABS_MAX                                   2000000

#define MAX_EVDEV_CLIENTS                              16
#define EV_RING_CAP                                    512
#define MAX_STREAM_SUBS                                16
#define STATE_RUN_DIR                                  "/run/nx769j-imu"
#define STATE_JSON_PATH                                "/run/nx769j-imu/state.json"
#define STREAM_SOCK_PATH                               "/run/nx769j-imu/stream.sock"

struct suid_pair {
    uint64_t low;
    uint64_t high;
};

struct evdev_client {
    bool active;
    uint64_t fh;
    uint32_t open_flags;
    int clock_id;
    bool poll_armed;
    uint64_t poll_kh;
    bool read_pending;
    uint64_t read_unique;
    uint32_t read_size;
    struct input_event ring[EV_RING_CAP];
    size_t head;
    size_t count;
};

static volatile sig_atomic_t g_stop = 0;
static float g_sample_rate_hz = 50.0f;
static bool g_verbose = false;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cuse_fd = -1;
static char g_evdev_name[64] = "input/event16";
static char g_evdev_path[80] = "/dev/input/event16";
static bool g_cuse_ready = false;

static struct evdev_client g_clients[MAX_EVDEV_CLIENTS];
static uint64_t g_next_fh = 1;

/* Live IMU state protected by g_lock */
static bool g_qsh_connected = false;
static uint32_t g_adsp_node = 0;
static uint32_t g_adsp_port = 0;
static bool g_have_accel_suid = false;
static bool g_have_gyro_suid = false;
static struct suid_pair g_accel_suid;
static struct suid_pair g_gyro_suid;

static float g_accel_mps2[3] = {0.0f, 0.0f, 0.0f};
static float g_gyro_rads[3] = {0.0f, 0.0f, 0.0f};
static int32_t g_abs_vals[6] = {0, 0, 0, 0, 0, 0};
static uint32_t g_accel_status = 0;
static uint32_t g_gyro_status = 0;
static uint64_t g_accel_samples = 0;
static uint64_t g_gyro_samples = 0;
static uint64_t g_dsp_timestamp = 0;
static char g_orientation[16] = "normal";
static char g_tilt[16] = "face-up";

static int g_stream_listen_fd = -1;
static int g_stream_subs[MAX_STREAM_SUBS];

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int32_t clamp_i32(long v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int32_t)v;
}

static void update_orientation_locked(float ax, float ay, float az) {
    float abs_x = fabsf(ax);
    float abs_y = fabsf(ay);
    float abs_z = fabsf(az);

    /* Tilt classification matching iio-sensor-proxy semantics */
    if (az >= 7.5f && abs_x < 5.5f && abs_y < 5.5f) {
        snprintf(g_tilt, sizeof(g_tilt), "face-up");
    } else if (az <= -7.5f && abs_x < 5.5f && abs_y < 5.5f) {
        snprintf(g_tilt, sizeof(g_tilt), "face-down");
    } else if (az > 3.0f) {
        snprintf(g_tilt, sizeof(g_tilt), "tilted-up");
    } else if (az < -3.0f) {
        snprintf(g_tilt, sizeof(g_tilt), "tilted-down");
    } else {
        snprintf(g_tilt, sizeof(g_tilt), "vertical");
    }

    /* Update portrait/landscape orientation when XY gravity projection >= 3.5 m/s^2
     * (hysteresis preserves last orientation when lying flat on a desk).
     */
    if (hypot(ax, ay) >= 3.5f && abs_z < 8.7f) {
        if (abs_y >= abs_x * 1.15f) {
            snprintf(g_orientation, sizeof(g_orientation), "%s", (ay >= 0.0f) ? "normal" : "bottom-up");
        } else if (abs_x >= abs_y * 1.15f) {
            /* Proper acceleration +X = right edge up; -X = left edge up */
            snprintf(g_orientation, sizeof(g_orientation), "%s", (ax >= 0.0f) ? "right-up" : "left-up");
        }
    }
}

/* ---------------- CUSE /dev/input/eventN Implementation ---------------- */

static int cuse_send_reply(uint64_t unique, int32_t err, const void *body1, size_t len1,
                           const void *body2, size_t len2) {
    if (g_cuse_fd < 0) return -1;
    struct fuse_out_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.error = err;
    hdr.unique = unique;
    if (err < 0 && unique != 0) {
        len1 = 0;
        len2 = 0;
    }
    hdr.len = (uint32_t)(sizeof(hdr) + len1 + len2);

    struct iovec iov[3];
    int iovcnt = 1;
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    if (len1 > 0 && body1 != NULL) {
        iov[iovcnt].iov_base = (void *)body1;
        iov[iovcnt].iov_len = len1;
        iovcnt++;
    }
    if (len2 > 0 && body2 != NULL) {
        iov[iovcnt].iov_base = (void *)body2;
        iov[iovcnt].iov_len = len2;
        iovcnt++;
    }
    ssize_t wr = writev(g_cuse_fd, iov, iovcnt);
    return (wr == (ssize_t)hdr.len) ? 0 : -1;
}

static size_t drain_client_events_locked(struct evdev_client *c, uint8_t *dst, size_t max_bytes) {
    size_t max_evs = max_bytes / sizeof(struct input_event);
    size_t n = (c->count < max_evs) ? c->count : max_evs;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (c->head + i) % EV_RING_CAP;
        memcpy(dst + i * sizeof(struct input_event), &c->ring[idx], sizeof(struct input_event));
    }
    c->head = (c->head + n) % EV_RING_CAP;
    c->count -= n;
    return n * sizeof(struct input_event);
}

static void push_client_frame_locked(uint32_t msc_us) {
    for (int i = 0; i < MAX_EVDEV_CLIENTS; i++) {
        struct evdev_client *c = &g_clients[i];
        if (!c->active) continue;

        struct timespec ts;
        if (clock_gettime(c->clock_id, &ts) != 0) {
            clock_gettime(CLOCK_REALTIME, &ts);
        }

        struct input_event frame[8];
        memset(frame, 0, sizeof(frame));
        for (int k = 0; k < 8; k++) {
            frame[k].input_event_sec = ts.tv_sec;
            frame[k].input_event_usec = ts.tv_nsec / 1000;
        }
        for (int a = 0; a < 6; a++) {
            frame[a].type = EV_ABS;
            frame[a].code = (uint16_t)(ABS_X + a);
            frame[a].value = g_abs_vals[a];
        }
        frame[6].type = EV_MSC;
        frame[6].code = MSC_TIMESTAMP;
        frame[6].value = (int32_t)msc_us;
        frame[7].type = EV_SYN;
        frame[7].code = SYN_REPORT;
        frame[7].value = 0;

        if (c->count + 8 > EV_RING_CAP) {
            /* Drop oldest complete frame to avoid partial frame tearing */
            size_t drop = (c->count + 8) - EV_RING_CAP;
            if (drop < 8) drop = 8;
            if (drop > c->count) drop = c->count;
            c->head = (c->head + drop) % EV_RING_CAP;
            c->count -= drop;
        }
        for (int k = 0; k < 8; k++) {
            size_t tail = (c->head + c->count) % EV_RING_CAP;
            c->ring[tail] = frame[k];
            c->count++;
        }

        if (c->read_pending && c->count > 0) {
            uint8_t out_buf[4096];
            size_t cap = c->read_size < sizeof(out_buf) ? c->read_size : sizeof(out_buf);
            size_t bytes = drain_client_events_locked(c, out_buf, cap);
            uint64_t u = c->read_unique;
            c->read_pending = false;
            cuse_send_reply(u, 0, out_buf, bytes, NULL, 0);
        }
        if (c->poll_armed && c->count > 0) {
            struct fuse_notify_poll_wakeup_out wp = { .kh = c->poll_kh };
            c->poll_armed = false;
            cuse_send_reply(0, FUSE_NOTIFY_POLL, &wp, sizeof(wp), NULL, 0);
        }
    }
}

static struct evdev_client *find_client_locked(uint64_t fh) {
    for (int i = 0; i < MAX_EVDEV_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].fh == fh) return &g_clients[i];
    }
    return NULL;
}

static void set_bit_le(uint8_t *buf, size_t buf_len, unsigned int bit) {
    size_t byte_idx = bit / 8;
    if (byte_idx < buf_len) {
        buf[byte_idx] |= (uint8_t)(1U << (bit % 8));
    }
}

static void handle_cuse_ioctl_locked(const struct fuse_in_header *hdr,
                                     const struct fuse_ioctl_in *in,
                                     const uint8_t *in_data) {
    uint32_t cmd = in->cmd;
    unsigned int dir = _IOC_DIR(cmd);
    unsigned int type = _IOC_TYPE(cmd);
    unsigned int nr = _IOC_NR(cmd);
    unsigned int sz = _IOC_SIZE(cmd);

    if (type != 'E') {
        cuse_send_reply(hdr->unique, -ENOTTY, NULL, 0, NULL, 0);
        return;
    }

    /* EVIOCGRAB / EVIOCREVOKE / EVIOCRMFF pass an integer arg directly, not a pointer */
    if (cmd == (uint32_t)EVIOCGRAB || cmd == (uint32_t)EVIOCREVOKE) {
        struct fuse_ioctl_out iout = { .result = 0, .flags = 0, .in_iovs = 0, .out_iovs = 0 };
        cuse_send_reply(hdr->unique, 0, &iout, sizeof(iout), NULL, 0);
        return;
    }
    if (cmd == (uint32_t)EVIOCRMFF) {
        cuse_send_reply(hdr->unique, -EINVAL, NULL, 0, NULL, 0);
        return;
    }

    /* Request kernel buffer retry under CUSE_UNRESTRICTED_IOCTL when needed */
    if ((dir & _IOC_READ) && sz > 0 && in->out_size == 0) {
        struct fuse_ioctl_out iout = {
            .result = 0, .flags = FUSE_IOCTL_RETRY, .in_iovs = 0, .out_iovs = 1
        };
        struct fuse_ioctl_iovec iov = { .base = in->arg, .len = sz };
        cuse_send_reply(hdr->unique, 0, &iout, sizeof(iout), &iov, sizeof(iov));
        return;
    }
    if ((dir & _IOC_WRITE) && sz > 0 && in->in_size == 0) {
        struct fuse_ioctl_out iout = {
            .result = 0, .flags = FUSE_IOCTL_RETRY, .in_iovs = 1, .out_iovs = 0
        };
        struct fuse_ioctl_iovec iov = { .base = in->arg, .len = sz };
        cuse_send_reply(hdr->unique, 0, &iout, sizeof(iout), &iov, sizeof(iov));
        return;
    }

    if (cmd == (uint32_t)EVIOCSCLOCKID) {
        if (in->in_size < sizeof(int)) {
            cuse_send_reply(hdr->unique, -EINVAL, NULL, 0, NULL, 0);
            return;
        }
        int clk = 0;
        memcpy(&clk, in_data, sizeof(int));
        if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC && clk != CLOCK_BOOTTIME) {
            cuse_send_reply(hdr->unique, -EINVAL, NULL, 0, NULL, 0);
            return;
        }
        struct evdev_client *c = find_client_locked(in->fh);
        if (c) c->clock_id = clk;
        struct fuse_ioctl_out iout = { .result = 0, .flags = 0, .in_iovs = 0, .out_iovs = 0 };
        cuse_send_reply(hdr->unique, 0, &iout, sizeof(iout), NULL, 0);
        return;
    }

    uint8_t out_payload[512];
    memset(out_payload, 0, sizeof(out_payload));
    size_t copy_len = 0;
    int32_t ioctl_ret = 0;

    if (cmd == (uint32_t)EVIOCGVERSION) {
        int ver = EV_VERSION;
        copy_len = sizeof(ver);
        memcpy(out_payload, &ver, copy_len);
        ioctl_ret = 0;
    } else if (cmd == (uint32_t)EVIOCGID) {
        struct input_id iid = {
            .bustype = BUS_HOST,
            .vendor = 0x05c6,   /* Qualcomm */
            .product = 0x7690,  /* NX769J */
            .version = 0x0100,
        };
        copy_len = sizeof(iid);
        memcpy(out_payload, &iid, copy_len);
        ioctl_ret = 0;
    } else if (nr == 0x06 && (dir & _IOC_READ)) { /* EVIOCGNAME(len) */
        const char *s = "NX769J LSM6DSO 6-Axis IMU";
        copy_len = strlen(s) + 1;
        if (copy_len > sz) copy_len = sz;
        memcpy(out_payload, s, copy_len);
        ioctl_ret = (int32_t)copy_len;
    } else if (nr == 0x07 && (dir & _IOC_READ)) { /* EVIOCGPHYS(len) */
        const char *s = "qcom-adsp/sensor_pd/spi3/lsm6dso";
        copy_len = strlen(s) + 1;
        if (copy_len > sz) copy_len = sz;
        memcpy(out_payload, s, copy_len);
        ioctl_ret = (int32_t)copy_len;
    } else if (nr == 0x08 && (dir & _IOC_READ)) { /* EVIOCGUNIQ(len) */
        const char *s = "nx769j-lsm6dso-0";
        copy_len = strlen(s) + 1;
        if (copy_len > sz) copy_len = sz;
        memcpy(out_payload, s, copy_len);
        ioctl_ret = (int32_t)copy_len;
    } else if (nr == 0x09 && (dir & _IOC_READ)) { /* EVIOCGPROP(len) */
        copy_len = sz < sizeof(unsigned long) ? sz : sizeof(unsigned long);
        set_bit_le(out_payload, sizeof(out_payload), INPUT_PROP_ACCELEROMETER);
        ioctl_ret = (int32_t)copy_len;
    } else if ((nr >= 0x18 && nr <= 0x1b) && (dir & _IOC_READ)) {
        /* EVIOCGKEY / EVIOCGLED / EVIOCGSND / EVIOCGSW -> empty bitmask */
        copy_len = sz < sizeof(out_payload) ? sz : sizeof(out_payload);
        ioctl_ret = (int32_t)copy_len;
    } else if (nr >= 0x20 && nr < 0x40 && (dir & _IOC_READ)) { /* EVIOCGBIT(ev, len) */
        unsigned int ev = nr - 0x20;
        size_t max_mask = 64;
        copy_len = sz < max_mask ? sz : max_mask;
        if (ev == 0) {
            set_bit_le(out_payload, sizeof(out_payload), EV_SYN);
            set_bit_le(out_payload, sizeof(out_payload), EV_ABS);
            set_bit_le(out_payload, sizeof(out_payload), EV_MSC);
        } else if (ev == EV_ABS) {
            set_bit_le(out_payload, sizeof(out_payload), ABS_X);
            set_bit_le(out_payload, sizeof(out_payload), ABS_Y);
            set_bit_le(out_payload, sizeof(out_payload), ABS_Z);
            set_bit_le(out_payload, sizeof(out_payload), ABS_RX);
            set_bit_le(out_payload, sizeof(out_payload), ABS_RY);
            set_bit_le(out_payload, sizeof(out_payload), ABS_RZ);
        } else if (ev == EV_MSC) {
            set_bit_le(out_payload, sizeof(out_payload), MSC_TIMESTAMP);
        }
        ioctl_ret = (int32_t)copy_len;
    } else if (nr >= 0x40 && nr < 0x80 && (dir & _IOC_READ)) { /* EVIOCGABS(abs) */
        unsigned int axis = nr - 0x40;
        struct input_absinfo ai;
        memset(&ai, 0, sizeof(ai));
        if (axis <= ABS_Z) {
            ai.value = g_abs_vals[axis];
            ai.minimum = -ACCEL_ABS_MAX;
            ai.maximum = ACCEL_ABS_MAX;
            ai.resolution = ACCEL_RES_PER_G;
        } else if (axis >= ABS_RX && axis <= ABS_RZ) {
            ai.value = g_abs_vals[axis];
            ai.minimum = -GYRO_ABS_MAX;
            ai.maximum = GYRO_ABS_MAX;
            ai.resolution = GYRO_RES_PER_DPS;
        }
        copy_len = sz < sizeof(ai) ? sz : sizeof(ai);
        memcpy(out_payload, &ai, copy_len);
        ioctl_ret = 0;
    } else if (cmd == (uint32_t)EVIOCGEFFECTS) {
        int effects = 0;
        copy_len = sizeof(effects);
        memcpy(out_payload, &effects, copy_len);
        ioctl_ret = 0;
    } else {
        cuse_send_reply(hdr->unique, -EINVAL, NULL, 0, NULL, 0);
        return;
    }

    if (copy_len > in->out_size) copy_len = in->out_size;
    struct fuse_ioctl_out iout = {
        .result = ioctl_ret, .flags = 0, .in_iovs = 0, .out_iovs = 0
    };
    cuse_send_reply(hdr->unique, 0, &iout, sizeof(iout), out_payload, copy_len);
}

static void choose_free_evdev_name(void) {
    for (int n = 16; n < 64; n++) {
        char candidate[80];
        snprintf(candidate, sizeof(candidate), "/dev/input/event%d", n);
        if (access(candidate, F_OK) != 0) {
            snprintf(g_evdev_name, sizeof(g_evdev_name), "input/event%d", n);
            snprintf(g_evdev_path, sizeof(g_evdev_path), "/dev/input/event%d", n);
            return;
        }
    }
}

static void *cuse_thread_main(void *arg) {
    (void)arg;
    uint8_t req_buf[16384];

    while (!g_stop) {
        struct pollfd pfd = { .fd = g_cuse_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;

        ssize_t n = read(g_cuse_fd, req_buf, sizeof(req_buf));
        if (n < (ssize_t)sizeof(struct fuse_in_header)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }

        const struct fuse_in_header *hdr = (const struct fuse_in_header *)req_buf;
        const uint8_t *body = req_buf + sizeof(struct fuse_in_header);

        pthread_mutex_lock(&g_lock);
        switch (hdr->opcode) {
        case CUSE_INIT: {
            const struct cuse_init_in *ci = (const struct cuse_init_in *)body;
            struct cuse_init_out co;
            memset(&co, 0, sizeof(co));
            co.major = FUSE_KERNEL_VERSION;
            co.minor = ci->minor < 23 ? ci->minor : 23;
            co.flags = CUSE_UNRESTRICTED_IOCTL;
            co.max_read = 4096;
            co.max_write = 4096;
            co.dev_major = 0; /* dynamic major via alloc_chrdev_region */
            co.dev_minor = 0;

            char devinfo[96];
            int dlen = snprintf(devinfo, sizeof(devinfo), "DEVNAME=%s", g_evdev_name) + 1;
            cuse_send_reply(hdr->unique, 0, &co, sizeof(co), devinfo, (size_t)dlen);
            g_cuse_ready = true;
            fprintf(stderr, "[nx769j-imu] Registered CUSE evdev %s\n", g_evdev_path);
            break;
        }
        case FUSE_OPEN: {
            const struct fuse_open_in *oi = (const struct fuse_open_in *)body;
            int slot = -1;
            for (int i = 0; i < MAX_EVDEV_CLIENTS; i++) {
                if (!g_clients[i].active) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                cuse_send_reply(hdr->unique, -EMFILE, NULL, 0, NULL, 0);
                break;
            }
            struct evdev_client *c = &g_clients[slot];
            memset(c, 0, sizeof(*c));
            c->active = true;
            c->fh = g_next_fh++;
            c->open_flags = oi->flags;
            c->clock_id = CLOCK_REALTIME;

            struct fuse_open_out oo;
            memset(&oo, 0, sizeof(oo));
            oo.fh = c->fh;
            oo.open_flags = FOPEN_DIRECT_IO | FOPEN_NONSEEKABLE;
            cuse_send_reply(hdr->unique, 0, &oo, sizeof(oo), NULL, 0);
            break;
        }
        case FUSE_RELEASE: {
            const struct fuse_release_in *ri = (const struct fuse_release_in *)body;
            struct evdev_client *c = find_client_locked(ri->fh);
            if (c) {
                if (c->read_pending) {
                    cuse_send_reply(c->read_unique, -ECANCELED, NULL, 0, NULL, 0);
                }
                memset(c, 0, sizeof(*c));
            }
            cuse_send_reply(hdr->unique, 0, NULL, 0, NULL, 0);
            break;
        }
        case FUSE_FLUSH:
        case FUSE_FSYNC: {
            cuse_send_reply(hdr->unique, 0, NULL, 0, NULL, 0);
            break;
        }
        case FUSE_READ: {
            const struct fuse_read_in *ri = (const struct fuse_read_in *)body;
            struct evdev_client *c = find_client_locked(ri->fh);
            if (!c) {
                cuse_send_reply(hdr->unique, -EBADF, NULL, 0, NULL, 0);
                break;
            }
            if (ri->size < sizeof(struct input_event)) {
                cuse_send_reply(hdr->unique, -EINVAL, NULL, 0, NULL, 0);
                break;
            }
            if (c->count > 0) {
                uint8_t out_buf[4096];
                size_t cap = ri->size < sizeof(out_buf) ? ri->size : sizeof(out_buf);
                size_t bytes = drain_client_events_locked(c, out_buf, cap);
                cuse_send_reply(hdr->unique, 0, out_buf, bytes, NULL, 0);
            } else if ((ri->flags | c->open_flags) & O_NONBLOCK) {
                cuse_send_reply(hdr->unique, -EAGAIN, NULL, 0, NULL, 0);
            } else {
                c->read_pending = true;
                c->read_unique = hdr->unique;
                c->read_size = ri->size;
            }
            break;
        }
        case FUSE_INTERRUPT: {
            const struct fuse_interrupt_in *ii = (const struct fuse_interrupt_in *)body;
            for (int i = 0; i < MAX_EVDEV_CLIENTS; i++) {
                if (g_clients[i].active && g_clients[i].read_pending &&
                    g_clients[i].read_unique == ii->unique) {
                    g_clients[i].read_pending = false;
                    cuse_send_reply(ii->unique, -EINTR, NULL, 0, NULL, 0);
                }
            }
            break;
        }
        case FUSE_POLL: {
            const struct fuse_poll_in *pi = (const struct fuse_poll_in *)body;
            struct evdev_client *c = find_client_locked(pi->fh);
            struct fuse_poll_out po;
            memset(&po, 0, sizeof(po));
            if (c && c->count > 0) {
                po.revents = POLLIN | POLLRDNORM;
            } else if (c && (pi->flags & FUSE_POLL_SCHEDULE_NOTIFY)) {
                c->poll_armed = true;
                c->poll_kh = pi->kh;
            }
            cuse_send_reply(hdr->unique, 0, &po, sizeof(po), NULL, 0);
            break;
        }
        case FUSE_IOCTL: {
            const struct fuse_ioctl_in *ii = (const struct fuse_ioctl_in *)body;
            const uint8_t *in_data = body + sizeof(struct fuse_ioctl_in);
            handle_cuse_ioctl_locked(hdr, ii, in_data);
            break;
        }
        case FUSE_WRITE: {
            const struct fuse_write_in *wi = (const struct fuse_write_in *)body;
            struct fuse_write_out wo = { .size = wi->size, .padding = 0 };
            cuse_send_reply(hdr->unique, 0, &wo, sizeof(wo), NULL, 0);
            break;
        }
        default:
            cuse_send_reply(hdr->unique, -ENOSYS, NULL, 0, NULL, 0);
            break;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ---------------- Minimal Protobuf Wire Codec for QSH ---------------- */

static size_t pb_put_varint(uint8_t *dst, uint64_t v) {
    size_t n = 0;
    while (v >= 0x80U) {
        dst[n++] = (uint8_t)((v & 0x7fU) | 0x80U);
        v >>= 7;
    }
    dst[n++] = (uint8_t)v;
    return n;
}

static size_t pb_put_u32_le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xffU);
    dst[1] = (uint8_t)((v >> 8) & 0xffU);
    dst[2] = (uint8_t)((v >> 16) & 0xffU);
    dst[3] = (uint8_t)((v >> 24) & 0xffU);
    return 4;
}

static size_t pb_put_u64_le(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; i++) dst[i] = (uint8_t)((v >> (8 * i)) & 0xffU);
    return 8;
}

static bool pb_get_varint(const uint8_t **p, const uint8_t *end, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    while (*p < end && shift < 64) {
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7fU) << shift;
        if ((b & 0x80U) == 0) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip_field(const uint8_t **p, const uint8_t *end, uint32_t wire_type) {
    uint64_t v = 0;
    switch (wire_type) {
    case 0:
        return pb_get_varint(p, end, &v);
    case 1:
        if ((size_t)(end - *p) < 8) return false;
        *p += 8;
        return true;
    case 2:
        if (!pb_get_varint(p, end, &v) || v > (uint64_t)(end - *p)) return false;
        *p += (size_t)v;
        return true;
    case 5:
        if ((size_t)(end - *p) < 4) return false;
        *p += 4;
        return true;
    default:
        return false;
    }
}

static size_t build_client_req_pb(uint8_t *out, struct suid_pair suid, uint32_t msg_id,
                                  const uint8_t *inner_payload, size_t inner_len,
                                  bool include_zero_batch) {
    uint8_t suid_buf[18];
    suid_buf[0] = 0x09; /* 1: fixed64 suid_low */
    pb_put_u64_le(&suid_buf[1], suid.low);
    suid_buf[9] = 0x11; /* 2: fixed64 suid_high */
    pb_put_u64_le(&suid_buf[10], suid.high);

    uint8_t susp_buf[4] = {
        0x08, SNS_STD_CLIENT_PROCESSOR_APSS, /* 1: client_proc_type */
        0x10, SNS_CLIENT_DELIVERY_WAKEUP     /* 2: delivery_type */
    };

    uint8_t std_req[256];
    size_t sr_len = 0;
    if (include_zero_batch) {
        /* 1: batch_spec { 1: batch_period = 0 } */
        std_req[sr_len++] = 0x0a;
        std_req[sr_len++] = 0x02;
        std_req[sr_len++] = 0x08;
        std_req[sr_len++] = 0x00;
    }
    if (inner_len > 0) {
        std_req[sr_len++] = 0x12; /* 2: bytes payload */
        sr_len += pb_put_varint(&std_req[sr_len], inner_len);
        memcpy(&std_req[sr_len], inner_payload, inner_len);
        sr_len += inner_len;
    }

    size_t n = 0;
    out[n++] = 0x0a; /* 1: sns_std_suid suid */
    n += pb_put_varint(&out[n], sizeof(suid_buf));
    memcpy(&out[n], suid_buf, sizeof(suid_buf));
    n += sizeof(suid_buf);

    out[n++] = 0x15; /* 2: fixed32 msg_id */
    n += pb_put_u32_le(&out[n], msg_id);

    out[n++] = 0x1a; /* 3: suspend_config susp_config */
    n += pb_put_varint(&out[n], sizeof(susp_buf));
    memcpy(&out[n], susp_buf, sizeof(susp_buf));
    n += sizeof(susp_buf);

    out[n++] = 0x22; /* 4: sns_std_request request */
    n += pb_put_varint(&out[n], sr_len);
    memcpy(&out[n], std_req, sr_len);
    n += sr_len;
    return n;
}

static int send_qmi_sns_req(int qfd, const struct sockaddr_qrtr *srv, uint16_t *txn_io,
                            const uint8_t *pb, size_t pb_len) {
    uint8_t pkt[1100];
    uint16_t txn = (*txn_io)++;
    if (txn == 0) txn = (*txn_io)++;

    size_t tlv1_len = 2 + pb_len;
    size_t total_tlv_len = (3 + tlv1_len) + (3 + 1);

    pkt[0] = QMI_CNTL_FLAG_REQ;
    pkt[1] = (uint8_t)(txn & 0xffU);
    pkt[2] = (uint8_t)(txn >> 8);
    pkt[3] = (uint8_t)(QMI_SNS_CLIENT_REQ_V01 & 0xffU);
    pkt[4] = (uint8_t)(QMI_SNS_CLIENT_REQ_V01 >> 8);
    pkt[5] = (uint8_t)(total_tlv_len & 0xffU);
    pkt[6] = (uint8_t)(total_tlv_len >> 8);

    size_t p = 7;
    /* Mandatory TLV 0x01: variable-length uint8 array (uint16 count + bytes) */
    pkt[p++] = 0x01;
    pkt[p++] = (uint8_t)(tlv1_len & 0xffU);
    pkt[p++] = (uint8_t)(tlv1_len >> 8);
    pkt[p++] = (uint8_t)(pb_len & 0xffU);
    pkt[p++] = (uint8_t)(pb_len >> 8);
    memcpy(&pkt[p], pb, pb_len);
    p += pb_len;

    /* Optional TLV 0x10: use_jumbo_report = 1 */
    pkt[p++] = 0x10;
    pkt[p++] = 0x01;
    pkt[p++] = 0x00;
    pkt[p++] = 0x01;

    ssize_t wr = sendto(qfd, pkt, p, 0, (const struct sockaddr *)srv, sizeof(*srv));
    return (wr == (ssize_t)p) ? 0 : -1;
}

static int send_suid_lookup(int qfd, const struct sockaddr_qrtr *srv, uint16_t *txn_io,
                            const char *data_type, bool default_only) {
    uint8_t inner[64];
    size_t ilen = 0;
    size_t dlen = strlen(data_type);
    inner[ilen++] = 0x0a; /* 1: string data_type */
    ilen += pb_put_varint(&inner[ilen], dlen);
    memcpy(&inner[ilen], data_type, dlen);
    ilen += dlen;
    inner[ilen++] = 0x10; /* 2: bool register_updates = true */
    inner[ilen++] = 0x01;
    inner[ilen++] = 0x18; /* 3: bool default_only */
    inner[ilen++] = default_only ? 0x01 : 0x00;

    uint8_t pb[256];
    struct suid_pair lookup_suid = { SNS_SUID_LOOKUP_LOW, SNS_SUID_LOOKUP_HIGH };
    size_t pb_len = build_client_req_pb(pb, lookup_suid, SNS_SUID_MSGID_SNS_SUID_REQ,
                                        inner, ilen, false);
    return send_qmi_sns_req(qfd, srv, txn_io, pb, pb_len);
}

static int send_sensor_stream_cfg(int qfd, const struct sockaddr_qrtr *srv, uint16_t *txn_io,
                                  struct suid_pair suid, float rate_hz) {
    uint8_t inner[16];
    uint32_t rate_bits = 0;
    memcpy(&rate_bits, &rate_hz, sizeof(rate_bits));
    inner[0] = 0x0d; /* 1: float sample_rate */
    pb_put_u32_le(&inner[1], rate_bits);

    uint8_t pb[256];
    size_t pb_len = build_client_req_pb(pb, suid, SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_CONFIG,
                                        inner, 5, true);
    return send_qmi_sns_req(qfd, srv, txn_io, pb, pb_len);
}

static bool parse_suid_msg(const uint8_t *buf, size_t len, struct suid_pair *out) {
    const uint8_t *p = buf, *end = buf + len;
    bool got_lo = false, got_hi = false;
    while (p < end) {
        uint64_t tag = 0;
        if (!pb_get_varint(&p, end, &tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7U);
        if (fn == 1 && wt == 1 && (size_t)(end - p) >= 8) {
            memcpy(&out->low, p, 8);
            p += 8;
            got_lo = true;
        } else if (fn == 2 && wt == 1 && (size_t)(end - p) >= 8) {
            memcpy(&out->high, p, 8);
            p += 8;
            got_hi = true;
        } else if (!pb_skip_field(&p, end, wt)) {
            return false;
        }
    }
    return got_lo && got_hi;
}

static void handle_suid_event_pb(int qfd, const struct sockaddr_qrtr *srv, uint16_t *txn_io,
                                 const uint8_t *buf, size_t len) {
    const uint8_t *p = buf, *end = buf + len;
    char dtype[64] = {0};
    struct suid_pair first_suid = {0, 0};
    bool have_suid = false;

    while (p < end) {
        uint64_t tag = 0, slen = 0;
        if (!pb_get_varint(&p, end, &tag)) return;
        uint32_t fn = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7U);
        if (fn == 1 && wt == 2) {
            if (!pb_get_varint(&p, end, &slen) || slen > (uint64_t)(end - p)) return;
            size_t cpy = slen < sizeof(dtype) - 1 ? (size_t)slen : sizeof(dtype) - 1;
            memcpy(dtype, p, cpy);
            dtype[cpy] = '\0';
            p += (size_t)slen;
        } else if (fn == 2 && wt == 2) {
            if (!pb_get_varint(&p, end, &slen) || slen > (uint64_t)(end - p)) return;
            struct suid_pair sp = {0, 0};
            if (!have_suid && parse_suid_msg(p, (size_t)slen, &sp) && (sp.low || sp.high)) {
                first_suid = sp;
                have_suid = true;
            }
            p += (size_t)slen;
        } else if (!pb_skip_field(&p, end, wt)) {
            return;
        }
    }

    if (!have_suid) return;

    pthread_mutex_lock(&g_lock);
    if (strcmp(dtype, "accel") == 0 && !g_have_accel_suid) {
        g_accel_suid = first_suid;
        g_have_accel_suid = true;
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[nx769j-imu] Discovered accel SUID: 0x%016" PRIx64 "%016" PRIx64 "\n",
                first_suid.high, first_suid.low);
        send_sensor_stream_cfg(qfd, srv, txn_io, first_suid, g_sample_rate_hz);
        return;
    }
    if (strcmp(dtype, "gyro") == 0 && !g_have_gyro_suid) {
        g_gyro_suid = first_suid;
        g_have_gyro_suid = true;
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[nx769j-imu] Discovered gyro  SUID: 0x%016" PRIx64 "%016" PRIx64 "\n",
                first_suid.high, first_suid.low);
        send_sensor_stream_cfg(qfd, srv, txn_io, first_suid, g_sample_rate_hz);
        return;
    }
    pthread_mutex_unlock(&g_lock);
}

static bool parse_sensor_sample_pb(const uint8_t *buf, size_t len, float vals[3],
                                   size_t *nvals, uint32_t *status_out) {
    const uint8_t *p = buf, *end = buf + len;
    *nvals = 0;
    *status_out = 0;
    while (p < end) {
        uint64_t tag = 0;
        if (!pb_get_varint(&p, end, &tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7U);
        if (fn == 1 && wt == 5 && (size_t)(end - p) >= 4) {
            float f = 0.0f;
            memcpy(&f, p, 4);
            p += 4;
            if (*nvals < 3) vals[(*nvals)++] = f;
        } else if (fn == 1 && wt == 2) {
            uint64_t plen = 0;
            if (!pb_get_varint(&p, end, &plen) || plen > (uint64_t)(end - p)) return false;
            const uint8_t *pe = p + (size_t)plen;
            while (p + 4 <= pe) {
                float f = 0.0f;
                memcpy(&f, p, 4);
                p += 4;
                if (*nvals < 3) vals[(*nvals)++] = f;
            }
            p = pe;
        } else if (fn == 2 && wt == 0) {
            uint64_t st = 0;
            if (!pb_get_varint(&p, end, &st)) return false;
            *status_out = (uint32_t)st;
        } else if (!pb_skip_field(&p, end, wt)) {
            return false;
        }
    }
    return (*nvals >= 3);
}

/* ---------------- State File & Local Stream Broadcast ---------------- */

static void accept_stream_subscribers(void) {
    if (g_stream_listen_fd < 0) return;
    for (;;) {
        int cfd = accept4(g_stream_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) break;
        bool placed = false;
        for (int i = 0; i < MAX_STREAM_SUBS; i++) {
            if (g_stream_subs[i] < 0) {
                g_stream_subs[i] = cfd;
                placed = true;
                break;
            }
        }
        if (!placed) close(cfd);
    }
}

static void publish_state_snapshot(void) {
    char json_line[1024];
    pthread_mutex_lock(&g_lock);
    int len = snprintf(
        json_line, sizeof(json_line),
        "{\"connected\":%s,\"adsp_node\":%u,\"adsp_port\":%u,"
        "\"evdev_path\":\"%s\",\"rate_hz\":%.1f,"
        "\"accel_suid\":\"%016" PRIx64 "%016" PRIx64 "\","
        "\"gyro_suid\":\"%016" PRIx64 "%016" PRIx64 "\","
        "\"accel_samples\":%" PRIu64 ",\"gyro_samples\":%" PRIu64 ","
        "\"accel_status\":%u,\"gyro_status\":%u,"
        "\"accel_mps2\":[%.5f,%.5f,%.5f],"
        "\"accel_mg\":[%d,%d,%d],"
        "\"gyro_rads\":[%.6f,%.6f,%.6f],"
        "\"gyro_mdps\":[%d,%d,%d],"
        "\"orientation\":\"%s\",\"tilt\":\"%s\","
        "\"dsp_timestamp\":%" PRIu64 ",\"monotonic_ns\":%" PRIu64 "}\n",
        g_qsh_connected ? "true" : "false", g_adsp_node, g_adsp_port,
        g_evdev_path, g_sample_rate_hz,
        g_accel_suid.high, g_accel_suid.low,
        g_gyro_suid.high, g_gyro_suid.low,
        g_accel_samples, g_gyro_samples,
        g_accel_status, g_gyro_status,
        g_accel_mps2[0], g_accel_mps2[1], g_accel_mps2[2],
        g_abs_vals[0], g_abs_vals[1], g_abs_vals[2],
        g_gyro_rads[0], g_gyro_rads[1], g_gyro_rads[2],
        g_abs_vals[3], g_abs_vals[4], g_abs_vals[5],
        g_orientation, g_tilt,
        g_dsp_timestamp, monotonic_ns());
    pthread_mutex_unlock(&g_lock);
    if (len <= 0 || (size_t)len >= sizeof(json_line)) return;

    if (g_cuse_ready && access(g_evdev_path, F_OK) == 0 &&
        access("/dev/input/by-id/platform-nx769j-lsm6dso-imu-event", F_OK) != 0) {
        mkdir("/dev/input/by-id", 0755);
        int sr = symlink(g_evdev_path, "/dev/input/by-id/platform-nx769j-lsm6dso-imu-event");
        (void)sr;
    }

    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", STATE_JSON_PATH);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t wr = write(fd, json_line, (size_t)len);
        (void)wr;
        close(fd);
        rename(tmp_path, STATE_JSON_PATH);
    }

    accept_stream_subscribers();
    for (int i = 0; i < MAX_STREAM_SUBS; i++) {
        if (g_stream_subs[i] >= 0) {
            ssize_t wr = send(g_stream_subs[i], json_line, (size_t)len, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (wr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close(g_stream_subs[i]);
                g_stream_subs[i] = -1;
            }
        }
    }
}

static void handle_client_event_msg_pb(int qfd, const struct sockaddr_qrtr *srv, uint16_t *txn_io,
                                       const uint8_t *buf, size_t len) {
    const uint8_t *p = buf, *end = buf + len;
    struct suid_pair sender = {0, 0};
    bool have_sender = false;

    /* Pass 1: extract sender SUID (field 1) */
    const uint8_t *scan = buf;
    while (scan < end) {
        uint64_t tag = 0, slen = 0;
        if (!pb_get_varint(&scan, end, &tag)) return;
        uint32_t fn = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7U);
        if (fn == 1 && wt == 2) {
            if (!pb_get_varint(&scan, end, &slen) || slen > (uint64_t)(end - scan)) return;
            if (parse_suid_msg(scan, (size_t)slen, &sender)) have_sender = true;
            scan += (size_t)slen;
        } else if (!pb_skip_field(&scan, end, wt)) {
            return;
        }
    }
    if (!have_sender) return;

    /* Pass 2: process repeated events (field 2) */
    while (p < end) {
        uint64_t tag = 0, elen = 0;
        if (!pb_get_varint(&p, end, &tag)) return;
        uint32_t fn = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7U);
        if (fn != 2 || wt != 2) {
            if (!pb_skip_field(&p, end, wt)) return;
            continue;
        }
        if (!pb_get_varint(&p, end, &elen) || elen > (uint64_t)(end - p)) return;
        const uint8_t *ep = p, *eend = p + (size_t)elen;
        p = eend;

        uint32_t msg_id = 0;
        uint64_t dsp_ts = 0;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;

        while (ep < eend) {
            uint64_t etag = 0;
            if (!pb_get_varint(&ep, eend, &etag)) break;
            uint32_t efn = (uint32_t)(etag >> 3), ewt = (uint32_t)(etag & 7U);
            if (efn == 1 && ewt == 5 && (size_t)(eend - ep) >= 4) {
                memcpy(&msg_id, ep, 4);
                ep += 4;
            } else if (efn == 2 && ewt == 1 && (size_t)(eend - ep) >= 8) {
                memcpy(&dsp_ts, ep, 8);
                ep += 8;
            } else if (efn == 3 && ewt == 2) {
                uint64_t plen = 0;
                if (!pb_get_varint(&ep, eend, &plen) || plen > (uint64_t)(eend - ep)) break;
                payload = ep;
                payload_len = (size_t)plen;
                ep += payload_len;
            } else if (!pb_skip_field(&ep, eend, ewt)) {
                break;
            }
        }

        if (!payload) continue;

        if (sender.low == SNS_SUID_LOOKUP_LOW && sender.high == SNS_SUID_LOOKUP_HIGH) {
            if (msg_id == SNS_SUID_MSGID_SNS_SUID_EVENT) {
                handle_suid_event_pb(qfd, srv, txn_io, payload, payload_len);
            }
            continue;
        }

        if (msg_id != SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_EVENT) continue;

        float v[3] = {0.0f, 0.0f, 0.0f};
        size_t nv = 0;
        uint32_t st = 0;
        if (!parse_sensor_sample_pb(payload, payload_len, v, &nv, &st)) continue;
        if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) continue;

        pthread_mutex_lock(&g_lock);
        bool is_accel = g_have_accel_suid &&
                        sender.low == g_accel_suid.low && sender.high == g_accel_suid.high;
        bool is_gyro = g_have_gyro_suid &&
                       sender.low == g_gyro_suid.low && sender.high == g_gyro_suid.high;

        if (is_accel) {
            g_accel_mps2[0] = v[0];
            g_accel_mps2[1] = v[1];
            g_accel_mps2[2] = v[2];
            g_abs_vals[0] = clamp_i32(lroundf((v[0] / GRAVITY_MPS2) * ACCEL_RES_PER_G),
                                      -ACCEL_ABS_MAX, ACCEL_ABS_MAX);
            g_abs_vals[1] = clamp_i32(lroundf((v[1] / GRAVITY_MPS2) * ACCEL_RES_PER_G),
                                      -ACCEL_ABS_MAX, ACCEL_ABS_MAX);
            g_abs_vals[2] = clamp_i32(lroundf((v[2] / GRAVITY_MPS2) * ACCEL_RES_PER_G),
                                      -ACCEL_ABS_MAX, ACCEL_ABS_MAX);
            g_accel_status = st;
            g_accel_samples++;
            g_dsp_timestamp = dsp_ts;
            update_orientation_locked(v[0], v[1], v[2]);
            /* Emit synchronized 6-axis frame paced on accelerometer ticks */
            uint32_t msc_us = (uint32_t)((monotonic_ns() / 1000ULL) & 0xffffffffULL);
            push_client_frame_locked(msc_us);
        } else if (is_gyro) {
            g_gyro_rads[0] = v[0];
            g_gyro_rads[1] = v[1];
            g_gyro_rads[2] = v[2];
            g_abs_vals[3] = clamp_i32(lroundf(v[0] * RAD_TO_DEG * GYRO_RES_PER_DPS),
                                      -GYRO_ABS_MAX, GYRO_ABS_MAX);
            g_abs_vals[4] = clamp_i32(lroundf(v[1] * RAD_TO_DEG * GYRO_RES_PER_DPS),
                                      -GYRO_ABS_MAX, GYRO_ABS_MAX);
            g_abs_vals[5] = clamp_i32(lroundf(v[2] * RAD_TO_DEG * GYRO_RES_PER_DPS),
                                      -GYRO_ABS_MAX, GYRO_ABS_MAX);
            g_gyro_status = st;
            g_gyro_samples++;
            g_dsp_timestamp = dsp_ts;
            if (!g_have_accel_suid) {
                uint32_t msc_us = (uint32_t)((monotonic_ns() / 1000ULL) & 0xffffffffULL);
                push_client_frame_locked(msc_us);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
}

static void handle_qmi_packet(int qfd, const struct sockaddr_qrtr *srv, uint16_t *txn_io,
                              const uint8_t *pkt, size_t len) {
    if (len < 7) return;
    uint8_t cntl = pkt[0];
    uint16_t msg_id = (uint16_t)pkt[3] | ((uint16_t)pkt[4] << 8);
    uint16_t msg_len = (uint16_t)pkt[5] | ((uint16_t)pkt[6] << 8);
    if ((size_t)msg_len + 7 < len) len = (size_t)msg_len + 7;

    if (cntl == QMI_CNTL_FLAG_RESP && msg_id == QMI_SNS_CLIENT_RESP_V01) {
        size_t pos = 7;
        while (pos + 3 <= len) {
            uint8_t tag = pkt[pos];
            uint16_t tlen = (uint16_t)pkt[pos + 1] | ((uint16_t)pkt[pos + 2] << 8);
            pos += 3;
            if (pos + tlen > len) break;
            if (tag == 0x02 && tlen >= 4) {
                uint16_t res = (uint16_t)pkt[pos] | ((uint16_t)pkt[pos + 1] << 8);
                uint16_t err = (uint16_t)pkt[pos + 2] | ((uint16_t)pkt[pos + 3] << 8);
                if (res != 0 && g_verbose) {
                    fprintf(stderr, "[nx769j-imu] QMI resp error: result=%u err=%u\n", res, err);
                }
            }
            pos += tlen;
        }
        return;
    }

    if (cntl != QMI_CNTL_FLAG_IND) return;
    if (msg_id != QMI_SNS_CLIENT_REPORT_IND_V01 &&
        msg_id != QMI_SNS_CLIENT_JUMBO_REPORT_IND_V01) {
        return;
    }

    size_t pos = 7;
    while (pos + 3 <= len) {
        uint8_t tag = pkt[pos];
        uint16_t tlen = (uint16_t)pkt[pos + 1] | ((uint16_t)pkt[pos + 2] << 8);
        pos += 3;
        if (pos + tlen > len) break;
        if (tag == 0x02 && tlen >= 2) {
            uint16_t pb_len16 = (uint16_t)pkt[pos] | ((uint16_t)pkt[pos + 1] << 8);
            if ((size_t)pb_len16 + 2 <= tlen) {
                handle_client_event_msg_pb(qfd, srv, txn_io, &pkt[pos + 2], pb_len16);
            } else if (tlen >= 4) {
                uint32_t pb_len32 = (uint32_t)pkt[pos] | ((uint32_t)pkt[pos + 1] << 8) |
                                    ((uint32_t)pkt[pos + 2] << 16) | ((uint32_t)pkt[pos + 3] << 24);
                if (pb_len32 + 4 <= tlen) {
                    handle_client_event_msg_pb(qfd, srv, txn_io, &pkt[pos + 4], pb_len32);
                }
            }
        }
        pos += tlen;
    }
}

static int setup_stream_socket(void) {
    mkdir(STATE_RUN_DIR, 0755);
    chmod(STATE_RUN_DIR, 0755);
    unlink(STREAM_SOCK_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", STREAM_SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return -1;
    }
    chmod(STREAM_SOCK_PATH, 0666);
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "r:v")) != -1) {
        switch (opt) {
        case 'r':
            g_sample_rate_hz = strtof(optarg, NULL);
            if (g_sample_rate_hz < 5.0f) g_sample_rate_hz = 5.0f;
            if (g_sample_rate_hz > 500.0f) g_sample_rate_hz = 500.0f;
            break;
        case 'v':
            g_verbose = true;
            break;
        default:
            fprintf(stderr, "Usage: %s [-r rate_hz] [-v]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < MAX_STREAM_SUBS; i++) g_stream_subs[i] = -1;
    g_stream_listen_fd = setup_stream_socket();

    choose_free_evdev_name();
    g_cuse_fd = open("/dev/cuse", O_RDWR | O_CLOEXEC);
    pthread_t cuse_tid = 0;
    bool cuse_started = false;
    if (g_cuse_fd >= 0) {
        if (pthread_create(&cuse_tid, NULL, cuse_thread_main, NULL) == 0) {
            cuse_started = true;
        }
    } else {
        fprintf(stderr, "[nx769j-imu] WARNING: open(/dev/cuse) failed (%s); running state/socket only\n",
                strerror(errno));
    }

    int qfd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (qfd < 0) {
        fprintf(stderr, "[nx769j-imu] socket(AF_QIPCRTR) failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_qrtr local_sq;
    socklen_t slen = sizeof(local_sq);
    memset(&local_sq, 0, sizeof(local_sq));
    local_sq.sq_family = AF_QIPCRTR;
    if (getsockname(qfd, (struct sockaddr *)&local_sq, &slen) < 0) {
        fprintf(stderr, "[nx769j-imu] getsockname(AF_QIPCRTR) failed: %s\n", strerror(errno));
        return 1;
    }

    /* Register QRTR control lookup for QSH SNS_CLIENT_SVC (service 400 = 0x190) */
    struct sockaddr_qrtr ctrl_sq = {
        .sq_family = AF_QIPCRTR,
        .sq_node = QRTR_NODE_BCAST,
        .sq_port = QRTR_PORT_CTRL,
    };
    struct qrtr_ctrl_pkt lookup_pkt;
    memset(&lookup_pkt, 0, sizeof(lookup_pkt));
    lookup_pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    lookup_pkt.server.service = SNS_CLIENT_SVC_ID;
    lookup_pkt.server.instance = 0;
    if (sendto(qfd, &lookup_pkt, sizeof(lookup_pkt), 0,
               (struct sockaddr *)&ctrl_sq, sizeof(ctrl_sq)) < 0) {
        fprintf(stderr, "[nx769j-imu] QRTR lookup sendto failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[nx769j-imu] Waiting for ADSP QSH SNS_CLIENT_SVC (service=%u) on QRTR...\n",
            SNS_CLIENT_SVC_ID);

    uint8_t *rx_buf = malloc(65536);
    if (!rx_buf) return 1;

    struct sockaddr_qrtr qsh_srv;
    memset(&qsh_srv, 0, sizeof(qsh_srv));
    uint16_t txn_id = 1;
    uint64_t last_publish_ns = 0;
    uint64_t connected_since_ns = 0;
    bool sent_fallback_lookup = false;

    publish_state_snapshot();

    while (!g_stop) {
        struct pollfd pfds[2];
        int nfds = 1;
        pfds[0].fd = qfd;
        pfds[0].events = POLLIN;
        if (g_stream_listen_fd >= 0) {
            pfds[1].fd = g_stream_listen_fd;
            pfds[1].events = POLLIN;
            nfds = 2;
        }

        int pr = poll(pfds, (nfds_t)nfds, 100);
        if (pr > 0 && nfds == 2 && (pfds[1].revents & POLLIN)) {
            accept_stream_subscribers();
        }
        if (pr > 0 && (pfds[0].revents & POLLIN)) {
            struct sockaddr_qrtr src;
            socklen_t srclen = sizeof(src);
            ssize_t rlen = recvfrom(qfd, rx_buf, 65536, 0, (struct sockaddr *)&src, &srclen);
            if (rlen > 0) {
                if (src.sq_port == QRTR_PORT_CTRL && (size_t)rlen >= sizeof(struct qrtr_ctrl_pkt)) {
                    const struct qrtr_ctrl_pkt *cp = (const struct qrtr_ctrl_pkt *)rx_buf;
                    if (cp->cmd == QRTR_TYPE_NEW_SERVER &&
                        cp->server.service == SNS_CLIENT_SVC_ID &&
                        (cp->server.node != 0 || cp->server.port != 0)) {
                        qsh_srv.sq_family = AF_QIPCRTR;
                        qsh_srv.sq_node = cp->server.node;
                        qsh_srv.sq_port = cp->server.port;

                        pthread_mutex_lock(&g_lock);
                        g_qsh_connected = true;
                        g_adsp_node = cp->server.node;
                        g_adsp_port = cp->server.port;
                        g_have_accel_suid = false;
                        g_have_gyro_suid = false;
                        pthread_mutex_unlock(&g_lock);

                        connected_since_ns = monotonic_ns();
                        sent_fallback_lookup = false;
                        fprintf(stderr, "[nx769j-imu] Connected to SNS_CLIENT_SVC at node=%u port=%u\n",
                                cp->server.node, cp->server.port);
                        send_suid_lookup(qfd, &qsh_srv, &txn_id, "accel", true);
                        send_suid_lookup(qfd, &qsh_srv, &txn_id, "gyro", true);
                    } else if (cp->cmd == QRTR_TYPE_DEL_SERVER &&
                               cp->server.service == SNS_CLIENT_SVC_ID &&
                               cp->server.node == g_adsp_node &&
                               cp->server.port == g_adsp_port) {
                        pthread_mutex_lock(&g_lock);
                        g_qsh_connected = false;
                        g_have_accel_suid = false;
                        g_have_gyro_suid = false;
                        pthread_mutex_unlock(&g_lock);
                        fprintf(stderr, "[nx769j-imu] SNS_CLIENT_SVC disconnected; awaiting restart\n");
                    }
                } else if (g_qsh_connected && src.sq_node == qsh_srv.sq_node &&
                           src.sq_port == qsh_srv.sq_port) {
                    handle_qmi_packet(qfd, &qsh_srv, &txn_id, rx_buf, (size_t)rlen);
                }
            }
        }

        uint64_t now_ns = monotonic_ns();
        /* If default_sensors registry hasn't bound default_only=true within 2s, query all SUIDs */
        if (g_qsh_connected && !sent_fallback_lookup &&
            (now_ns - connected_since_ns) > 2000000000ULL) {
            pthread_mutex_lock(&g_lock);
            bool need_a = !g_have_accel_suid;
            bool need_g = !g_have_gyro_suid;
            pthread_mutex_unlock(&g_lock);
            if (need_a) send_suid_lookup(qfd, &qsh_srv, &txn_id, "accel", false);
            if (need_g) send_suid_lookup(qfd, &qsh_srv, &txn_id, "gyro", false);
            sent_fallback_lookup = true;
        }

        if (now_ns - last_publish_ns >= 100000000ULL) { /* 10 Hz state/socket publish */
            publish_state_snapshot();
            last_publish_ns = now_ns;
        }
    }

    free(rx_buf);
    close(qfd);
    if (g_cuse_fd >= 0) {
        close(g_cuse_fd);
        g_cuse_fd = -1;
    }
    if (cuse_started) pthread_join(cuse_tid, NULL);
    if (g_stream_listen_fd >= 0) close(g_stream_listen_fd);
    unlink(STREAM_SOCK_PATH);
    unlink(STATE_JSON_PATH);
    return 0;
}
