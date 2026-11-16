// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nx769j-hexagonrpcd: FastRPC reverse-tunnel listener daemon for SM8650 (NX769J)
 *
 * Implements remotectl (handle 0), apps_std (handle 1), and apps_mem (handle 2)
 * on top of libhexagonrpc.so.0.4 for ADSP root_pd and sensor_pd, providing:
 *   - 64 KiB page-aligned inline listener buffer (fixes >256B payload abort)
 *   - Complete apps_std file/directory/env/stat/write/rename/mkdir/remove ABI
 *   - Ephemeral RAM-backed read-write sandbox for /mnt/vendor/persist/sensors
 *     mapped strictly to /run/nx769j-fastrpc/persist_sensors (zero flash writes)
 *   - Read-only virtual VFS mapping for /usr/share/qcom/sm8650/nubia/nx769j
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* FastRPC kernel ioctls (include/uapi/misc/fastrpc.h) */
#define FASTRPC_IOCTL_INIT_ATTACH      _IO('R', 4)
#define FASTRPC_IOCTL_INIT_ATTACH_SNS  _IO('R', 8)

struct fastrpc_req_mmap {
    int32_t fd;
    uint32_t flags;
    uint64_t vaddrin;
    uint64_t size;
    uint64_t vaddrout;
};

struct fastrpc_req_munmap {
    uint64_t vaddrout;
    uint64_t size;
};

#define FASTRPC_IOCTL_MMAP    _IOWR('R', 6, struct fastrpc_req_mmap)
#define FASTRPC_IOCTL_MUNMAP  _IOWR('R', 7, struct fastrpc_req_munmap)

/* libhexagonrpc.so.0.4 ABI */
struct fastrpc_function_def {
    uint32_t msg_id;
    uint8_t in_nums;
    uint8_t in_bufs;
    uint8_t out_nums;
    uint8_t out_bufs;
};

struct fastrpc_context;

extern const struct fastrpc_function_def remotectl_open_def;
extern const struct fastrpc_function_def remotectl_close_def;
extern struct fastrpc_context *fastrpc_create_context(int fd, uint32_t handle);
extern int fastrpc(const struct fastrpc_function_def *def, struct fastrpc_context *ctx, ...);
extern int fastrpc2(const struct fastrpc_function_def *def, int fd, uint32_t handle, ...);

/* AEE / apps_std return codes */
#define AEE_SUCCESS          0U
#define AEE_EFAILED          1U
#define AEE_EBADPARM         0x0eU
#define AEE_EUNSUPPORTED     0x14U
#define APPS_STD_EBADF       0x201U

#define IN_INLINE_CAP        65536U
#define MAX_BUFS             16U
#define MAX_STREAMS          512
#define STREAM_BASE_ID       3
#define MAX_DIRS             128
#define MAX_ENVS             32

struct rpc_in_buf {
    const uint8_t *data;
    uint32_t size;
};

struct rpc_out_buf {
    bool allocated;
    uint8_t *data;
    uint32_t size;
};

struct stream_slot {
    bool used;
    bool writable;
    FILE *fp;
    char host_path[PATH_MAX];
};

struct dir_slot {
    bool used;
    bool is_virtual;
    DIR *dp;
    const char *const *virt_entries;
    size_t virt_count;
    size_t virt_idx;
};

struct env_slot {
    bool used;
    char name[128];
    char value[512];
};

static int g_fastrpc_fd = -1;
static bool g_sensorspd = false;
static bool g_verbose = false;
static char g_ro_root[PATH_MAX] = "/usr/share/qcom/sm8650/nubia/nx769j";
static char g_rw_persist[PATH_MAX] = "/run/nx769j-fastrpc/persist_sensors";

static struct stream_slot g_streams[MAX_STREAMS];
static struct dir_slot g_dirs[MAX_DIRS];
static struct env_slot g_envs[MAX_ENVS];

static uint32_t errno_to_aee(int err)
{
    return (err > 0) ? (uint32_t)err : AEE_EFAILED;
}

static bool alloc_out(struct rpc_out_buf *out, uint32_t sz)
{
    out->size = sz;
    if (sz == 0) {
        out->data = NULL;
        out->allocated = true;
        return true;
    }
    out->data = calloc(1, sz);
    out->allocated = (out->data != NULL);
    return out->allocated;
}

static bool cstr_from_in(const struct rpc_in_buf *b, char *dst, size_t cap)
{
    if (!b || b->size == 0 || !b->data)
        return false;
    size_t n = strnlen((const char *)b->data, b->size);
    if (n >= cap)
        return false;
    memcpy(dst, b->data, n);
    dst[n] = '\0';
    return true;
}

/* Normalize DSP path into canonical "/..." form with all "." and ".." resolved */
static bool normalize_dsp_path(const char *raw, char *out, size_t cap)
{
    if (!raw || raw[0] == '\0')
        return false;

    const char *segs[128];
    size_t lens[128];
    size_t depth = 0;

    const char *p = raw;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0)
                depth--;
            continue;
        }
        if (depth >= 128)
            return false;
        segs[depth] = start;
        lens[depth] = len;
        depth++;
    }

    if (depth == 0) {
        if (cap < 2)
            return false;
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    size_t pos = 0;
    for (size_t i = 0; i < depth; i++) {
        if (pos + 1 + lens[i] >= cap)
            return false;
        out[pos++] = '/';
        memcpy(out + pos, segs[i], lens[i]);
        pos += lens[i];
    }
    out[pos] = '\0';
    return true;
}

enum vdir_kind {
    VDIR_NONE = 0,
    VDIR_ROOT,
    VDIR_MNT,
    VDIR_MNT_VENDOR,
    VDIR_MNT_VENDOR_PERSIST,
    VDIR_VENDOR,
    VDIR_VENDOR_ETC,
    VDIR_VENDOR_ETC_SENSORS,
    VDIR_VENDOR_DSP,
    VDIR_SYS,
    VDIR_SYS_DEVICES,
    VDIR_USR,
    VDIR_USR_LIB,
};

struct mapped_path {
    bool ok;
    bool is_rw;
    enum vdir_kind vdir;
    char host[PATH_MAX];
};

static bool prefix_match_dir(const char *path, const char *prefix, const char **rest_out)
{
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0)
        return false;
    if (path[plen] == '\0') {
        *rest_out = "";
        return true;
    }
    if (path[plen] == '/') {
        *rest_out = path + plen + 1;
        return true;
    }
    return false;
}

static bool join_host(char *dst, size_t cap, const char *base, const char *rest)
{
    int n;
    if (!rest || rest[0] == '\0')
        n = snprintf(dst, cap, "%s", base);
    else
        n = snprintf(dst, cap, "%s/%s", base, rest);
    return n > 0 && (size_t)n < cap;
}

static bool verify_under_rw(const char *host_path)
{
    size_t wlen = strlen(g_rw_persist);
    if (strncmp(host_path, g_rw_persist, wlen) != 0)
        return false;
    if (host_path[wlen] != '\0' && host_path[wlen] != '/')
        return false;
    if (strstr(host_path, "/.."))
        return false;
    return true;
}

static struct mapped_path map_dsp_path(const char *raw_path)
{
    struct mapped_path m;
    memset(&m, 0, sizeof(m));

    char norm[PATH_MAX];
    if (!normalize_dsp_path(raw_path, norm, sizeof(norm)))
        return m;

    const char *rest = NULL;

    /* 1. Ephemeral RW persist sensors tree */
    if (prefix_match_dir(norm, "/mnt/vendor/persist/sensors", &rest) ||
        prefix_match_dir(norm, "/persist/sensors", &rest)) {
        if (!join_host(m.host, sizeof(m.host), g_rw_persist, rest))
            return m;
        if (!verify_under_rw(m.host))
            return m;
        m.ok = true;
        m.is_rw = true;
        return m;
    }

    /* 2. Sensor registry master config file */
    if (strcmp(norm, "/vendor/etc/sensors/sns_reg_config") == 0 ||
        strcmp(norm, "/etc/sensors/sns_reg_config") == 0) {
        if (!join_host(m.host, sizeof(m.host), g_ro_root, "sensors/sns_reg.conf"))
            return m;
        m.ok = true;
        return m;
    }

    /* 3. Sensor JSON config directory & files */
    if (prefix_match_dir(norm, "/vendor/etc/sensors/config", &rest) ||
        prefix_match_dir(norm, "/etc/sensors/config", &rest)) {
        char sub[PATH_MAX];
        if (!join_host(sub, sizeof(sub), "sensors/config", rest))
            return m;
        if (!join_host(m.host, sizeof(m.host), g_ro_root, sub))
            return m;
        m.ok = true;
        return m;
    }

    if (strcmp(norm, "/vendor/etc/sensors") == 0 ||
        strcmp(norm, "/etc/sensors") == 0) {
        m.ok = true;
        m.vdir = VDIR_VENDOR_ETC_SENSORS;
        return m;
    }

    if (prefix_match_dir(norm, "/vendor/etc/sensors", &rest) ||
        prefix_match_dir(norm, "/etc/sensors", &rest)) {
        char sub[PATH_MAX];
        if (!join_host(sub, sizeof(sub), "sensors", rest))
            return m;
        if (!join_host(m.host, sizeof(m.host), g_ro_root, sub))
            return m;
        m.ok = true;
        return m;
    }

    /* 4. SoC info attributes */
    if (prefix_match_dir(norm, "/sys/devices/soc0", &rest)) {
        char sub[PATH_MAX];
        if (join_host(sub, sizeof(sub), "socinfo", rest) &&
            join_host(m.host, sizeof(m.host), g_ro_root, sub) &&
            access(m.host, F_OK) == 0) {
            m.ok = true;
            return m;
        }
        if (!join_host(m.host, sizeof(m.host), "/sys/devices/soc0", rest))
            return m;
        m.ok = true;
        return m;
    }

    /* 5. Audio calibration DB (ACDB) */
    if (prefix_match_dir(norm, "/vendor/etc/acdbdata", &rest) ||
        prefix_match_dir(norm, "/etc/acdbdata", &rest)) {
        char sub[PATH_MAX];
        if (!join_host(sub, sizeof(sub), "acdb", rest))
            return m;
        if (!join_host(m.host, sizeof(m.host), g_ro_root, sub))
            return m;
        m.ok = true;
        return m;
    }

    /* 6. ADSP dynamic libraries / skels */
    if (prefix_match_dir(norm, "/usr/lib/qcom/adsp", &rest) ||
        prefix_match_dir(norm, "/vendor/dsp/adsp", &rest) ||
        prefix_match_dir(norm, "/dsp/adsp", &rest) ||
        prefix_match_dir(norm, "/vendor/lib/rfsa/adsp", &rest) ||
        prefix_match_dir(norm, "/system/lib/rfsa/adsp", &rest)) {
        char sub[PATH_MAX];
        if (!join_host(sub, sizeof(sub), "dsp/adsp", rest))
            return m;
        if (!join_host(m.host, sizeof(m.host), g_ro_root, sub))
            return m;
        m.ok = true;
        return m;
    }

    if (prefix_match_dir(norm, "/usr/lib/qcom", &rest)) {
        char sub[PATH_MAX];
        if (!join_host(sub, sizeof(sub), "dsp", rest))
            return m;
        if (!join_host(m.host, sizeof(m.host), g_ro_root, sub))
            return m;
        m.ok = true;
        return m;
    }

    /* 7. Virtual ancestor directories */
    if (strcmp(norm, "/") == 0) { m.ok = true; m.vdir = VDIR_ROOT; return m; }
    if (strcmp(norm, "/mnt") == 0) { m.ok = true; m.vdir = VDIR_MNT; return m; }
    if (strcmp(norm, "/mnt/vendor") == 0) { m.ok = true; m.vdir = VDIR_MNT_VENDOR; return m; }
    if (strcmp(norm, "/mnt/vendor/persist") == 0 || strcmp(norm, "/persist") == 0) {
        m.ok = true; m.vdir = VDIR_MNT_VENDOR_PERSIST; return m;
    }
    if (strcmp(norm, "/vendor") == 0) { m.ok = true; m.vdir = VDIR_VENDOR; return m; }
    if (strcmp(norm, "/vendor/etc") == 0 || strcmp(norm, "/etc") == 0) {
        m.ok = true; m.vdir = VDIR_VENDOR_ETC; return m;
    }
    if (strcmp(norm, "/vendor/dsp") == 0 || strcmp(norm, "/dsp") == 0) {
        m.ok = true; m.vdir = VDIR_VENDOR_DSP; return m;
    }
    if (strcmp(norm, "/sys") == 0) { m.ok = true; m.vdir = VDIR_SYS; return m; }
    if (strcmp(norm, "/sys/devices") == 0) { m.ok = true; m.vdir = VDIR_SYS_DEVICES; return m; }
    if (strcmp(norm, "/usr") == 0) { m.ok = true; m.vdir = VDIR_USR; return m; }
    if (strcmp(norm, "/usr/lib") == 0) { m.ok = true; m.vdir = VDIR_USR_LIB; return m; }

    return m;
}

static void get_virtual_dir_entries(enum vdir_kind kind, const char *const **entries, size_t *count)
{
    static const char *const e_root[] = { ".", "..", "mnt", "vendor", "etc", "sys", "usr" };
    static const char *const e_mnt[] = { ".", "..", "vendor" };
    static const char *const e_mnt_vendor[] = { ".", "..", "persist" };
    static const char *const e_persist[] = { ".", "..", "sensors" };
    static const char *const e_vendor[] = { ".", "..", "etc", "dsp" };
    static const char *const e_vendor_etc[] = { ".", "..", "sensors", "acdbdata" };
    static const char *const e_sensors[] = { ".", "..", "config", "sns_reg_config" };
    static const char *const e_dsp[] = { ".", "..", "adsp" };
    static const char *const e_sys[] = { ".", "..", "devices" };
    static const char *const e_sys_dev[] = { ".", "..", "soc0" };
    static const char *const e_usr[] = { ".", "..", "lib" };
    static const char *const e_usr_lib[] = { ".", "..", "qcom" };

    switch (kind) {
    case VDIR_ROOT: *entries = e_root; *count = sizeof(e_root)/sizeof(e_root[0]); break;
    case VDIR_MNT: *entries = e_mnt; *count = sizeof(e_mnt)/sizeof(e_mnt[0]); break;
    case VDIR_MNT_VENDOR: *entries = e_mnt_vendor; *count = sizeof(e_mnt_vendor)/sizeof(e_mnt_vendor[0]); break;
    case VDIR_MNT_VENDOR_PERSIST: *entries = e_persist; *count = sizeof(e_persist)/sizeof(e_persist[0]); break;
    case VDIR_VENDOR: *entries = e_vendor; *count = sizeof(e_vendor)/sizeof(e_vendor[0]); break;
    case VDIR_VENDOR_ETC: *entries = e_vendor_etc; *count = sizeof(e_vendor_etc)/sizeof(e_vendor_etc[0]); break;
    case VDIR_VENDOR_ETC_SENSORS: *entries = e_sensors; *count = sizeof(e_sensors)/sizeof(e_sensors[0]); break;
    case VDIR_VENDOR_DSP: *entries = e_dsp; *count = sizeof(e_dsp)/sizeof(e_dsp[0]); break;
    case VDIR_SYS: *entries = e_sys; *count = sizeof(e_sys)/sizeof(e_sys[0]); break;
    case VDIR_SYS_DEVICES: *entries = e_sys_dev; *count = sizeof(e_sys_dev)/sizeof(e_sys_dev[0]); break;
    case VDIR_USR: *entries = e_usr; *count = sizeof(e_usr)/sizeof(e_usr[0]); break;
    case VDIR_USR_LIB: *entries = e_usr_lib; *count = sizeof(e_usr_lib)/sizeof(e_usr_lib[0]); break;
    default: *entries = NULL; *count = 0; break;
    }
}

/* Environment helpers */
static void env_init_defaults(void)
{
    memset(g_envs, 0, sizeof(g_envs));
    g_envs[0].used = true;
    snprintf(g_envs[0].name, sizeof(g_envs[0].name), "ADSP_LIBRARY_PATH");
    snprintf(g_envs[0].value, sizeof(g_envs[0].value), "/usr/lib/qcom/adsp;/vendor/dsp/adsp");

    g_envs[1].used = true;
    snprintf(g_envs[1].name, sizeof(g_envs[1].name), "ADSP_AVS_CFG_PATH");
    snprintf(g_envs[1].value, sizeof(g_envs[1].value), "/vendor/etc/acdbdata");
}

static const char *env_lookup(const char *name)
{
    for (size_t i = 0; i < MAX_ENVS; i++) {
        if (g_envs[i].used && strcmp(g_envs[i].name, name) == 0)
            return g_envs[i].value;
    }
    return NULL;
}

static bool env_set_val(const char *name, const char *val, bool override_flag)
{
    for (size_t i = 0; i < MAX_ENVS; i++) {
        if (g_envs[i].used && strcmp(g_envs[i].name, name) == 0) {
            if (override_flag) {
                snprintf(g_envs[i].value, sizeof(g_envs[i].value), "%s", val);
            }
            return true;
        }
    }
    for (size_t i = 0; i < MAX_ENVS; i++) {
        if (!g_envs[i].used) {
            g_envs[i].used = true;
            snprintf(g_envs[i].name, sizeof(g_envs[i].name), "%s", name);
            snprintf(g_envs[i].value, sizeof(g_envs[i].value), "%s", val);
            return true;
        }
    }
    return false;
}

static void env_unset_val(const char *name)
{
    for (size_t i = 0; i < MAX_ENVS; i++) {
        if (g_envs[i].used && strcmp(g_envs[i].name, name) == 0)
            g_envs[i].used = false;
    }
}

/* Stream helpers */
static struct stream_slot *stream_get(int32_t sin)
{
    int idx = (int)sin - STREAM_BASE_ID;
    if (idx < 0 || idx >= MAX_STREAMS)
        return NULL;
    if (!g_streams[idx].used || !g_streams[idx].fp)
        return NULL;
    return &g_streams[idx];
}

static bool mode_requests_write(const char *mode, char *clean_mode, size_t cap)
{
    bool wr = false;
    bool has_r = false, has_w = false, has_a = false, has_plus = false;
    for (const char *p = mode; *p; p++) {
        if (*p == 'r') has_r = true;
        else if (*p == 'w') { has_w = true; wr = true; }
        else if (*p == 'a') { has_a = true; wr = true; }
        else if (*p == '+') { has_plus = true; wr = true; }
    }
    if (has_w)
        snprintf(clean_mode, cap, has_plus ? "w+b" : "wb");
    else if (has_a)
        snprintf(clean_mode, cap, has_plus ? "a+b" : "ab");
    else if (has_r)
        snprintf(clean_mode, cap, has_plus ? "r+b" : "rb");
    else
        snprintf(clean_mode, cap, "rb");
    return wr;
}

static uint32_t open_dsp_stream(const char *dsp_path, const char *mode_str, int32_t *sin_out)
{
    struct mapped_path m = map_dsp_path(dsp_path);
    if (!m.ok || m.vdir != VDIR_NONE) {
        if (g_verbose)
            fprintf(stderr, "open unmapped/vdir '%s' -> ENOENT\n", dsp_path);
        return ENOENT;
    }

    char clean_mode[8];
    bool want_write = mode_requests_write(mode_str, clean_mode, sizeof(clean_mode));
    if (want_write && !m.is_rw) {
        fprintf(stderr, "Denied write open on RO path '%s' (mode '%s')\n", dsp_path, mode_str);
        return EACCES;
    }

    int slot_idx = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!g_streams[i].used) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0)
        return EMFILE;

    FILE *fp = fopen(m.host, clean_mode);
    if (!fp) {
        int err = errno;
        if (want_write || g_verbose)
            fprintf(stderr, "fopen('%s' -> '%s', '%s') failed: %s\n",
                    dsp_path, m.host, clean_mode, strerror(err));
        return errno_to_aee(err);
    }

    if (want_write && g_verbose) {
        fprintf(stderr, "RW open('%s' -> '%s', '%s') -> sin=%d\n",
                dsp_path, m.host, clean_mode, slot_idx + STREAM_BASE_ID);
    }

    g_streams[slot_idx].used = true;
    g_streams[slot_idx].writable = want_write;
    g_streams[slot_idx].fp = fp;
    snprintf(g_streams[slot_idx].host_path, sizeof(g_streams[slot_idx].host_path), "%s", m.host);
    *sin_out = (int32_t)(slot_idx + STREAM_BASE_ID);
    return AEE_SUCCESS;
}

/* Fill 96-byte apps_std_STAT structure */
static void fill_apps_std_stat(uint8_t *buf96, const struct stat *st)
{
    memset(buf96, 0, 96);
    uint64_t tsz = 0;
    uint64_t dev = (uint64_t)st->st_dev;
    uint64_t ino = (uint64_t)st->st_ino;
    uint32_t mode = (uint32_t)st->st_mode;
    uint32_t nlink = (uint32_t)st->st_nlink;
    uint64_t rdev = (uint64_t)st->st_rdev;
    uint64_t size = (uint64_t)st->st_size;
    int64_t blksize = (int64_t)st->st_blksize;
    int64_t blocks = (int64_t)st->st_blocks;
    int64_t atime_s = (int64_t)st->st_atim.tv_sec;
    int64_t mtime_s = (int64_t)st->st_mtim.tv_sec;
    int64_t ctime_s = (int64_t)st->st_ctim.tv_sec;

    memcpy(buf96 + 0,  &tsz, 8);
    memcpy(buf96 + 8,  &dev, 8);
    memcpy(buf96 + 16, &ino, 8);
    memcpy(buf96 + 24, &mode, 4);
    memcpy(buf96 + 28, &nlink, 4);
    memcpy(buf96 + 32, &rdev, 8);
    memcpy(buf96 + 40, &size, 8);
    memcpy(buf96 + 48, &blksize, 8);
    memcpy(buf96 + 56, &blocks, 8);
    memcpy(buf96 + 64, &atime_s, 8);
    memcpy(buf96 + 72, &mtime_s, 8);
    memcpy(buf96 + 80, &ctime_s, 8);
}

static uint32_t stat_dsp_path(const char *dsp_path, struct stat *st_out)
{
    struct mapped_path m = map_dsp_path(dsp_path);
    if (!m.ok)
        return ENOENT;

    if (m.vdir != VDIR_NONE) {
        memset(st_out, 0, sizeof(*st_out));
        st_out->st_mode = S_IFDIR | 0555;
        st_out->st_nlink = 2;
        st_out->st_blksize = 4096;
        return AEE_SUCCESS;
    }

    if (stat(m.host, st_out) != 0)
        return errno_to_aee(errno);
    return AEE_SUCCESS;
}

/* Handle 0: remotectl */
static uint32_t handle_remotectl(uint32_t method,
                                 uint32_t n_in, const struct rpc_in_buf *in,
                                 uint32_t n_out, struct rpc_out_buf *out)
{
    if (method == 0) {
        /* open(in string name, rout uint32 handle, rout int32 dlerr, rout char errstr[]) */
        if (n_in < 2 || n_out < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        uint32_t err_len = 0;
        memcpy(&err_len, in[0].data + 4, 4);

        char name[128];
        if (!cstr_from_in(&in[1], name, sizeof(name)))
            return AEE_EBADPARM;

        if (!alloc_out(&out[0], 8) || !alloc_out(&out[1], err_len))
            return AEE_EFAILED;

        uint32_t h_ret = 0xffffffffU;
        int32_t dl_err = -5;
        uint32_t ret = AEE_SUCCESS;

        if (strcmp(name, "remotectl") == 0) {
            h_ret = 0; dl_err = 0;
        } else if (strcmp(name, "apps_std") == 0) {
            h_ret = 1; dl_err = 0;
        } else if (strcmp(name, "apps_mem") == 0) {
            h_ret = 2; dl_err = 0;
        } else {
            fprintf(stderr, "remotectl: unknown local interface '%s'\n", name);
            if (err_len > 0)
                snprintf((char *)out[1].data, err_len, "unknown interface %s", name);
            ret = (uint32_t)-5;
        }

        memcpy(out[0].data + 0, &h_ret, 4);
        memcpy(out[0].data + 4, &dl_err, 4);
        return ret;
    }

    if (method == 1) {
        /* close(in uint32 handle, rout int32 dlerr, rout char errstr[]) */
        if (n_in < 1 || n_out < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        uint32_t err_len = 0;
        memcpy(&err_len, in[0].data + 4, 4);
        if (!alloc_out(&out[0], 4) || !alloc_out(&out[1], err_len))
            return AEE_EFAILED;
        return AEE_SUCCESS;
    }

    fprintf(stderr, "remotectl: unsupported method %u\n", method);
    return AEE_EUNSUPPORTED;
}

/* Handle 2: apps_mem */
static uint32_t handle_apps_mem(uint32_t method,
                                uint32_t n_in, const struct rpc_in_buf *in,
                                uint32_t n_out, struct rpc_out_buf *out)
{
    if (method == 2) {
        /* request_map64(in int heapid, in uint32 ion_flags, in uint32 rflags,
                         in uint64 vin, in int64 len, rout uint64 vapps, rout uint64 vdsp) */
        if (n_in < 1 || n_out < 1 || in[0].size < 32)
            return AEE_EBADPARM;
        uint32_t rflags = 0;
        uint64_t len = 0;
        memcpy(&rflags, in[0].data + 8, 4);
        memcpy(&len, in[0].data + 24, 8);

        if (!alloc_out(&out[0], 16))
            return AEE_EFAILED;

        if (!(rflags & 0x1000U)) {
            fprintf(stderr, "apps_mem: unsupported DMA map rflags=0x%x\n", rflags);
            return AEE_EUNSUPPORTED;
        }

        struct fastrpc_req_mmap req;
        memset(&req, 0, sizeof(req));
        req.fd = -1;
        req.flags = rflags;
        req.vaddrin = 0;
        req.size = len;
        req.vaddrout = 0;

        if (ioctl(g_fastrpc_fd, FASTRPC_IOCTL_MMAP, &req) < 0) {
            fprintf(stderr, "apps_mem: FASTRPC_IOCTL_MMAP(size=%" PRIu64 ", rflags=0x%x) failed: %s\n",
                    len, rflags, strerror(errno));
            return AEE_EFAILED;
        }

        uint64_t vapps = 0;
        memcpy(out[0].data + 0, &vapps, 8);
        memcpy(out[0].data + 8, &req.vaddrout, 8);
        return AEE_SUCCESS;
    }

    if (method == 3) {
        /* request_unmap64(in uint64 vdsp, in int64 len) */
        if (n_in < 1 || in[0].size < 16)
            return AEE_EBADPARM;
        struct fastrpc_req_munmap req;
        memcpy(&req.vaddrout, in[0].data + 0, 8);
        memcpy(&req.size, in[0].data + 8, 8);
        if (ioctl(g_fastrpc_fd, FASTRPC_IOCTL_MUNMAP, &req) < 0) {
            fprintf(stderr, "apps_mem: FASTRPC_IOCTL_MUNMAP failed: %s\n", strerror(errno));
            return AEE_EFAILED;
        }
        return AEE_SUCCESS;
    }

    fprintf(stderr, "apps_mem: unsupported method %u\n", method);
    return AEE_EUNSUPPORTED;
}

/* Handle 1: apps_std */
static uint32_t handle_apps_std_ext(uint32_t n_in, const struct rpc_in_buf *in,
                                    uint32_t n_out, struct rpc_out_buf *out)
{
    if (n_in < 1 || in[0].size < 4)
        return AEE_EBADPARM;

    uint32_t ext_id = 0;
    memcpy(&ext_id, in[0].data, 4);

    if (ext_id == 31 || (ext_id == 32 && n_in >= 2 && n_out >= 1)) {
        /* stat / lstat(in string name, rout apps_std_STAT st) */
        if (n_in < 2 || n_out < 1)
            return AEE_EBADPARM;
        char path[PATH_MAX];
        if (!cstr_from_in(&in[1], path, sizeof(path)))
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 96))
            return AEE_EFAILED;
        struct stat st;
        uint32_t rc = stat_dsp_path(path, &st);
        if (rc != AEE_SUCCESS)
            return rc;
        fill_apps_std_stat(out[0].data, &st);
        return AEE_SUCCESS;
    }

    if (ext_id == 33) {
        /* frename(in string oldname, in string newname) */
        if (n_in < 3)
            return AEE_EBADPARM;
        char old_dsp[PATH_MAX], new_dsp[PATH_MAX];
        if (!cstr_from_in(&in[1], old_dsp, sizeof(old_dsp)) ||
            !cstr_from_in(&in[2], new_dsp, sizeof(new_dsp)))
            return AEE_EBADPARM;

        struct mapped_path mo = map_dsp_path(old_dsp);
        struct mapped_path mn = map_dsp_path(new_dsp);
        if (!mo.ok || !mn.ok || mo.vdir != VDIR_NONE || mn.vdir != VDIR_NONE)
            return ENOENT;
        if (!mo.is_rw || !mn.is_rw) {
            fprintf(stderr, "Denied rename outside RW sandbox: '%s' -> '%s'\n", old_dsp, new_dsp);
            return EACCES;
        }
        if (g_verbose)
            fprintf(stderr, "RW rename('%s' -> '%s')\n", mo.host, mn.host);
        else if (strstr(mn.host, "parsed_file_list.csv") != NULL)
            fprintf(stderr, "Sensor registry commit complete (%s)\n", mn.host);
        if (rename(mo.host, mn.host) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    if (ext_id == 35 || (ext_id == 32 && n_in == 1 && n_out == 0)) {
        /* ftrunc(in int32 sin, in int32 offset) */
        if (in[0].size < 12)
            return AEE_EBADPARM;
        int32_t sin = 0, offset = 0;
        memcpy(&sin, in[0].data + 4, 4);
        memcpy(&offset, in[0].data + 8, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        if (!s->writable)
            return EACCES;
        fflush(s->fp);
        if (ftruncate(fileno(s->fp), (off_t)offset) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    fprintf(stderr, "apps_std: unsupported ext_id=%u (n_in=%u, n_out=%u)\n", ext_id, n_in, n_out);
    return AEE_EUNSUPPORTED;
}

static uint32_t handle_apps_std(uint32_t method,
                                uint32_t n_in, const struct rpc_in_buf *in,
                                uint32_t n_out, struct rpc_out_buf *out)
{
    switch (method) {
    case 0: {
        /* fopen(in string name, in string mode, rout int32 sout) */
        if (n_in < 3 || n_out < 1)
            return AEE_EBADPARM;
        char path[PATH_MAX], mode[32];
        if (!cstr_from_in(&in[1], path, sizeof(path)) ||
            !cstr_from_in(&in[2], mode, sizeof(mode)))
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 4))
            return AEE_EFAILED;
        int32_t sout = -1;
        uint32_t rc = open_dsp_stream(path, mode, &sout);
        if (rc == AEE_SUCCESS)
            memcpy(out[0].data, &sout, 4);
        return rc;
    }

    case 1: {
        /* freopen(in int32 sin, in string name, in string mode, rout int32 sout) */
        if (n_in < 3 || n_out < 1 || in[0].size < 12)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (s) {
            fclose(s->fp);
            s->fp = NULL;
            s->used = false;
        }
        char path[PATH_MAX], mode[32];
        if (!cstr_from_in(&in[1], path, sizeof(path)) ||
            !cstr_from_in(&in[2], mode, sizeof(mode)))
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 4))
            return AEE_EFAILED;
        int32_t sout = -1;
        uint32_t rc = open_dsp_stream(path, mode, &sout);
        if (rc == AEE_SUCCESS)
            memcpy(out[0].data, &sout, 4);
        return rc;
    }

    case 2: {
        /* fflush(in int32 sin) */
        if (n_in < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        if (sin == 1 || sin == 2) {
            fflush(stderr);
            return AEE_SUCCESS;
        }
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        if (fflush(s->fp) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 3: {
        /* fclose(in int32 sin) */
        if (n_in < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        int rc = fclose(s->fp);
        s->fp = NULL;
        s->used = false;
        return (rc == 0) ? AEE_SUCCESS : errno_to_aee(errno);
    }

    case 4: {
        /* fread(in int32 sin, rout byte buf[], rout int32 bytesRead, rout int32 bEOF) */
        if (n_in < 1 || n_out < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        int32_t sin = 0;
        uint32_t req_len = 0;
        memcpy(&sin, in[0].data + 0, 4);
        memcpy(&req_len, in[0].data + 4, 4);

        if (!alloc_out(&out[0], 8) || !alloc_out(&out[1], req_len))
            return AEE_EFAILED;
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;

        size_t n = (req_len > 0) ? fread(out[1].data, 1, req_len, s->fp) : 0;
        if (n == 0 && ferror(s->fp))
            return errno_to_aee(errno);

        int32_t bytes_read = (int32_t)n;
        int32_t b_eof = (n < req_len || feof(s->fp)) ? 1 : 0;
        memcpy(out[0].data + 0, &bytes_read, 4);
        memcpy(out[0].data + 4, &b_eof, 4);
        return AEE_SUCCESS;
    }

    case 5: {
        /* fwrite(in int32 sin, in byte buf[], rout int32 bytesWritten, rout int32 bEOF) */
        if (n_in < 2 || n_out < 1 || in[0].size < 8)
            return AEE_EBADPARM;
        int32_t sin = 0;
        uint32_t buf_len = 0;
        memcpy(&sin, in[0].data + 0, 4);
        memcpy(&buf_len, in[0].data + 4, 4);
        if (buf_len > in[1].size)
            buf_len = in[1].size;

        if (!alloc_out(&out[0], 8))
            return AEE_EFAILED;

        if (sin == 1 || sin == 2) {
            fwrite(in[1].data, 1, buf_len, stderr);
            int32_t bw = (int32_t)buf_len, beof = 0;
            memcpy(out[0].data + 0, &bw, 4);
            memcpy(out[0].data + 4, &beof, 4);
            return AEE_SUCCESS;
        }

        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        if (!s->writable)
            return EACCES;

        size_t n = (buf_len > 0) ? fwrite(in[1].data, 1, buf_len, s->fp) : 0;
        if (n < buf_len && ferror(s->fp))
            return errno_to_aee(errno);

        int32_t bytes_written = (int32_t)n;
        int32_t b_eof = 0;
        memcpy(out[0].data + 0, &bytes_written, 4);
        memcpy(out[0].data + 4, &b_eof, 4);
        return AEE_SUCCESS;
    }

    case 6: {
        /* fgetpos(in int32 sin, rout byte pos[], rout int32 lenReq) */
        if (n_in < 1 || n_out < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        int32_t sin = 0;
        uint32_t pos_len = 0;
        memcpy(&sin, in[0].data + 0, 4);
        memcpy(&pos_len, in[0].data + 4, 4);
        if (!alloc_out(&out[0], 4) || !alloc_out(&out[1], pos_len))
            return AEE_EFAILED;
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        off_t cur = ftello(s->fp);
        if (cur < 0)
            return errno_to_aee(errno);
        int64_t val = (int64_t)cur;
        int32_t req = (int32_t)sizeof(val);
        memcpy(out[0].data, &req, 4);
        if (pos_len >= sizeof(val))
            memcpy(out[1].data, &val, sizeof(val));
        return AEE_SUCCESS;
    }

    case 7: {
        /* fsetpos(in int32 sin, in byte pos[]) */
        if (n_in < 2 || in[0].size < 8 || in[1].size < 8)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        int64_t val = 0;
        memcpy(&val, in[1].data, 8);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        if (fseeko(s->fp, (off_t)val, SEEK_SET) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 8: {
        /* ftell(in int32 sin, rout int32 pos) */
        if (n_in < 1 || n_out < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        if (!alloc_out(&out[0], 4))
            return AEE_EFAILED;
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        off_t cur = ftello(s->fp);
        if (cur < 0)
            return errno_to_aee(errno);
        int32_t p32 = (int32_t)cur;
        memcpy(out[0].data, &p32, 4);
        return AEE_SUCCESS;
    }

    case 9: {
        /* fseek(in int32 sin, in int32 offset, in int32 whence) */
        if (n_in < 1 || in[0].size < 12)
            return AEE_EBADPARM;
        int32_t sin = 0, offset = 0, whence = 0;
        memcpy(&sin, in[0].data + 0, 4);
        memcpy(&offset, in[0].data + 4, 4);
        memcpy(&whence, in[0].data + 8, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        int w = (whence == 0) ? SEEK_SET : (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : -1;
        if (w < 0)
            return EINVAL;
        if (fseeko(s->fp, (off_t)offset, w) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 10: {
        /* flen(in int32 sin, rout uint64 len) */
        if (n_in < 1 || n_out < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 8))
            return AEE_EFAILED;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        fflush(s->fp);
        struct stat st;
        if (fstat(fileno(s->fp), &st) != 0)
            return errno_to_aee(errno);
        uint64_t fsz = (uint64_t)st.st_size;
        memcpy(out[0].data, &fsz, 8);
        return AEE_SUCCESS;
    }

    case 11: {
        /* rewind(in int32 sin) */
        if (n_in < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        rewind(s->fp);
        return AEE_SUCCESS;
    }

    case 12: {
        /* feof(in int32 sin, rout int32 bEOF) */
        if (n_in < 1 || n_out < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 4))
            return AEE_EFAILED;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        int32_t e = feof(s->fp) ? 1 : 0;
        memcpy(out[0].data, &e, 4);
        return AEE_SUCCESS;
    }

    case 13: {
        /* ferror(in int32 sin, rout int32 err) */
        if (n_in < 1 || n_out < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 4))
            return AEE_EFAILED;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        int32_t err = ferror(s->fp) ? 1 : 0;
        memcpy(out[0].data, &err, 4);
        return AEE_SUCCESS;
    }

    case 14: {
        /* clearerr(in int32 sin) */
        if (n_in < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        clearerr(s->fp);
        return AEE_SUCCESS;
    }

    case 15: {
        /* print(in string str) */
        if (n_in < 2)
            return AEE_EBADPARM;
        char msg[1024];
        if (cstr_from_in(&in[1], msg, sizeof(msg)))
            fprintf(stderr, "[DSP] %s\n", msg);
        return AEE_SUCCESS;
    }

    case 16: {
        /* getenv(in string name, rout char val[], rout int32 valLenReq) */
        if (n_in < 2 || n_out < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        uint32_t val_cap = 0;
        memcpy(&val_cap, in[0].data + 4, 4);
        char name[128];
        if (!cstr_from_in(&in[1], name, sizeof(name)))
            return AEE_EBADPARM;

        const char *val = env_lookup(name);
        if (!alloc_out(&out[0], 4) || !alloc_out(&out[1], val_cap))
            return AEE_EFAILED;
        if (!val)
            return ENOENT;

        int32_t req = (int32_t)(strlen(val) + 1);
        memcpy(out[0].data, &req, 4);
        if (val_cap > 0)
            snprintf((char *)out[1].data, val_cap, "%s", val);
        return AEE_SUCCESS;
    }

    case 17: {
        /* setenv(in string name, in string val, in int32 override) */
        if (n_in < 3 || in[0].size < 12)
            return AEE_EBADPARM;
        int32_t ov = 0;
        memcpy(&ov, in[0].data + 8, 4);
        char name[128], val[512];
        if (!cstr_from_in(&in[1], name, sizeof(name)) ||
            !cstr_from_in(&in[2], val, sizeof(val)))
            return AEE_EBADPARM;
        return env_set_val(name, val, ov != 0) ? AEE_SUCCESS : ENOMEM;
    }

    case 18: {
        /* unsetenv(in string name) */
        if (n_in < 2)
            return AEE_EBADPARM;
        char name[128];
        if (!cstr_from_in(&in[1], name, sizeof(name)))
            return AEE_EBADPARM;
        env_unset_val(name);
        return AEE_SUCCESS;
    }

    case 19: {
        /* fopen_with_env(in string envvarname, in string delim, in string name,
                          in string mode, rout int32 sout) */
        if (n_in < 5 || n_out < 1)
            return AEE_EBADPARM;
        char envname[128], delim[32], name[PATH_MAX], mode[32];
        if (!cstr_from_in(&in[1], envname, sizeof(envname)) ||
            !cstr_from_in(&in[2], delim, sizeof(delim)) ||
            !cstr_from_in(&in[3], name, sizeof(name)) ||
            !cstr_from_in(&in[4], mode, sizeof(mode)))
            return AEE_EBADPARM;

        if (!alloc_out(&out[0], 4))
            return AEE_EFAILED;

        int32_t sout = -1;
        if (name[0] == '/') {
            uint32_t rc = open_dsp_stream(name, mode, &sout);
            if (rc == AEE_SUCCESS)
                memcpy(out[0].data, &sout, 4);
            return rc;
        }

        const char *envval = env_lookup(envname);
        if (!envval || envval[0] == '\0')
            return ENOENT;

        char copy[512];
        snprintf(copy, sizeof(copy), "%s", envval);
        const char *sep = (delim[0] != '\0') ? delim : ";:";
        char *saveptr = NULL;
        uint32_t last_rc = ENOENT;

        for (char *tok = strtok_r(copy, sep, &saveptr); tok; tok = strtok_r(NULL, sep, &saveptr)) {
            if (tok[0] == '\0')
                continue;
            char candidate[PATH_MAX];
            if (!join_host(candidate, sizeof(candidate), tok, name))
                continue;
            last_rc = open_dsp_stream(candidate, mode, &sout);
            if (last_rc == AEE_SUCCESS) {
                memcpy(out[0].data, &sout, 4);
                return AEE_SUCCESS;
            }
        }
        return last_rc;
    }

    case 20: {
        /* fgets(in int32 sin, rout byte buf[], rout int32 bEOF) */
        if (n_in < 1 || n_out < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        int32_t sin = 0;
        uint32_t cap = 0;
        memcpy(&sin, in[0].data + 0, 4);
        memcpy(&cap, in[0].data + 4, 4);
        if (!alloc_out(&out[0], 4) || !alloc_out(&out[1], cap))
            return AEE_EFAILED;
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        if (cap > 0) {
            if (!fgets((char *)out[1].data, (int)cap, s->fp) && ferror(s->fp))
                return errno_to_aee(errno);
        }
        int32_t b_eof = feof(s->fp) ? 1 : 0;
        memcpy(out[0].data, &b_eof, 4);
        return AEE_SUCCESS;
    }

    case 22: {
        /* fileExists(in string path, rout boolean exists) */
        if (n_in < 2 || n_out < 1)
            return AEE_EBADPARM;
        char path[PATH_MAX];
        if (!cstr_from_in(&in[1], path, sizeof(path)))
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 1))
            return AEE_EFAILED;
        struct stat st;
        out[0].data[0] = (stat_dsp_path(path, &st) == AEE_SUCCESS) ? 1 : 0;
        return AEE_SUCCESS;
    }

    case 23: {
        /* fsync(in int32 sin) */
        if (n_in < 1 || in[0].size < 4)
            return AEE_EBADPARM;
        int32_t sin = 0;
        memcpy(&sin, in[0].data, 4);
        struct stream_slot *s = stream_get(sin);
        if (!s)
            return APPS_STD_EBADF;
        fflush(s->fp);
        if (fsync(fileno(s->fp)) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 24: {
        /* fremove(in string name) */
        if (n_in < 2)
            return AEE_EBADPARM;
        char path[PATH_MAX];
        if (!cstr_from_in(&in[1], path, sizeof(path)))
            return AEE_EBADPARM;
        struct mapped_path m = map_dsp_path(path);
        if (!m.ok || m.vdir != VDIR_NONE)
            return ENOENT;
        if (!m.is_rw) {
            fprintf(stderr, "Denied fremove on RO path '%s'\n", path);
            return EACCES;
        }
        if (g_verbose)
            fprintf(stderr, "RW fremove('%s')\n", m.host);
        if (unlink(m.host) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 26: {
        /* opendir(in string name, rout apps_std_DIR dir) */
        if (n_in < 2 || n_out < 1)
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 8))
            return AEE_EFAILED;
        char path[PATH_MAX];
        if (!cstr_from_in(&in[1], path, sizeof(path)))
            return AEE_EBADPARM;

        struct mapped_path m = map_dsp_path(path);
        if (!m.ok)
            return ENOENT;

        int slot_idx = -1;
        for (int i = 0; i < MAX_DIRS; i++) {
            if (!g_dirs[i].used) {
                slot_idx = i;
                break;
            }
        }
        if (slot_idx < 0)
            return EMFILE;

        if (m.vdir != VDIR_NONE) {
            g_dirs[slot_idx].used = true;
            g_dirs[slot_idx].is_virtual = true;
            g_dirs[slot_idx].dp = NULL;
            get_virtual_dir_entries(m.vdir, &g_dirs[slot_idx].virt_entries, &g_dirs[slot_idx].virt_count);
            g_dirs[slot_idx].virt_idx = 0;
        } else {
            DIR *dp = opendir(m.host);
            if (!dp)
                return errno_to_aee(errno);
            g_dirs[slot_idx].used = true;
            g_dirs[slot_idx].is_virtual = false;
            g_dirs[slot_idx].dp = dp;
            g_dirs[slot_idx].virt_entries = NULL;
            g_dirs[slot_idx].virt_count = 0;
            g_dirs[slot_idx].virt_idx = 0;
        }

        uint64_t dh = (uint64_t)(slot_idx + 1);
        memcpy(out[0].data, &dh, 8);
        return AEE_SUCCESS;
    }

    case 27: {
        /* closedir(in apps_std_DIR dir) */
        if (n_in < 1 || in[0].size < 8)
            return AEE_EBADPARM;
        uint64_t dh = 0;
        memcpy(&dh, in[0].data, 8);
        if (dh == 0 || dh > MAX_DIRS || !g_dirs[dh - 1].used)
            return APPS_STD_EBADF;
        struct dir_slot *d = &g_dirs[dh - 1];
        int rc = 0;
        if (!d->is_virtual && d->dp)
            rc = closedir(d->dp);
        memset(d, 0, sizeof(*d));
        return (rc == 0) ? AEE_SUCCESS : errno_to_aee(errno);
    }

    case 28: {
        /* readdir(in apps_std_DIR dir, rout apps_std_DIRENT dirent, rout int32 bEOF) */
        if (n_in < 1 || n_out < 1 || in[0].size < 8)
            return AEE_EBADPARM;
        if (!alloc_out(&out[0], 264))
            return AEE_EFAILED;
        uint64_t dh = 0;
        memcpy(&dh, in[0].data, 8);
        if (dh == 0 || dh > MAX_DIRS || !g_dirs[dh - 1].used)
            return APPS_STD_EBADF;

        struct dir_slot *d = &g_dirs[dh - 1];
        if (d->is_virtual) {
            if (d->virt_idx >= d->virt_count) {
                int32_t b_eof = 1;
                memcpy(out[0].data + 260, &b_eof, 4);
                return AEE_SUCCESS;
            }
            int32_t ino = (int32_t)(d->virt_idx + 1);
            const char *nm = d->virt_entries[d->virt_idx++];
            int32_t b_eof = 0;
            memcpy(out[0].data + 0, &ino, 4);
            snprintf((char *)(out[0].data + 4), 255, "%s", nm);
            memcpy(out[0].data + 260, &b_eof, 4);
            return AEE_SUCCESS;
        }

        errno = 0;
        struct dirent *de = readdir(d->dp);
        if (!de) {
            if (errno != 0)
                return errno_to_aee(errno);
            int32_t b_eof = 1;
            memcpy(out[0].data + 260, &b_eof, 4);
            return AEE_SUCCESS;
        }

        int32_t ino = (int32_t)de->d_ino;
        int32_t b_eof = 0;
        memcpy(out[0].data + 0, &ino, 4);
        snprintf((char *)(out[0].data + 4), 255, "%.254s", de->d_name);
        memcpy(out[0].data + 260, &b_eof, 4);
        return AEE_SUCCESS;
    }

    case 29: {
        /* mkdir(in string name, in int mode) */
        if (n_in < 2 || in[0].size < 8)
            return AEE_EBADPARM;
        uint32_t mode = 0755;
        memcpy(&mode, in[0].data + 4, 4);
        char path[PATH_MAX];
        if (!cstr_from_in(&in[1], path, sizeof(path)))
            return AEE_EBADPARM;

        struct mapped_path m = map_dsp_path(path);
        if (!m.ok)
            return ENOENT;
        if (m.vdir != VDIR_NONE)
            return EEXIST;
        if (!m.is_rw) {
            struct stat st;
            if (stat(m.host, &st) == 0 && S_ISDIR(st.st_mode))
                return EEXIST;
            fprintf(stderr, "Denied mkdir on RO path '%s'\n", path);
            return EACCES;
        }

        if (g_verbose)
            fprintf(stderr, "RW mkdir('%s', 0%o)\n", m.host, mode & 0777U);
        if (mkdir(m.host, (mode_t)(mode & 0777U) | 0700) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 30: {
        /* rmdir(in string name) */
        if (n_in < 2)
            return AEE_EBADPARM;
        char path[PATH_MAX];
        if (!cstr_from_in(&in[1], path, sizeof(path)))
            return AEE_EBADPARM;
        struct mapped_path m = map_dsp_path(path);
        if (!m.ok || m.vdir != VDIR_NONE)
            return ENOENT;
        if (!m.is_rw)
            return EACCES;
        fprintf(stderr, "RW rmdir('%s')\n", m.host);
        if (rmdir(m.host) != 0)
            return errno_to_aee(errno);
        return AEE_SUCCESS;
    }

    case 31:
        return handle_apps_std_ext(n_in, in, n_out, out);

    default:
        fprintf(stderr, "apps_std: unsupported method %u (n_in=%u, n_out=%u)\n", method, n_in, n_out);
        return AEE_EUNSUPPORTED;
    }
}

static int register_default_listener(int fd)
{
    static const struct fastrpc_function_def reg_def = { 0, 0, 0, 0, 0 };
    static const struct fastrpc_function_def init_def = { 3, 0, 0, 0, 0 };

    const char *lname = "adsp_default_listener";
    uint32_t dl_h = 0, dl_err = 0;
    char dl_str[256];
    memset(dl_str, 0, sizeof(dl_str));

    int ret = fastrpc2(&remotectl_open_def, fd, 0,
                       (uint32_t)(strlen(lname) + 1), lname,
                       &dl_h, &dl_err, (uint32_t)sizeof(dl_str), dl_str);
    if (ret == 0 && dl_err == 0) {
        struct fastrpc_context *ctx = fastrpc_create_context(fd, dl_h);
        if (ctx) {
            ret = fastrpc(&reg_def, ctx);
            if (ret != 0)
                fprintf(stderr, "adsp_default_listener register returned %d\n", ret);
            free(ctx);
        }
        fastrpc2(&remotectl_close_def, fd, 0, dl_h, &dl_err, (uint32_t)sizeof(dl_str), dl_str);
    } else {
        fprintf(stderr, "remotectl_open(adsp_default_listener) ret=%d dl_err=%u (%s)\n",
                ret, dl_err, dl_str);
    }

    ret = fastrpc2(&init_def, fd, 3);
    if (ret != 0) {
        fprintf(stderr, "listener init (handle 3, msg 3) failed: %d\n", ret);
        return -1;
    }
    return 0;
}

static int run_listener_loop(int fd)
{
    static const struct fastrpc_function_def next2_def = { 4, 2, 1, 4, 1 };

    uint8_t *in_inline = aligned_alloc(4096, IN_INLINE_CAP);
    if (!in_inline) {
        perror("aligned_alloc");
        return -1;
    }

    uint32_t ctx_id = 0;
    uint32_t handle = 0;
    uint32_t sc = 0;
    uint32_t in_total_size = 0;
    uint32_t result = 0xffffffffU;

    struct rpc_out_buf prev_out[MAX_BUFS];
    memset(prev_out, 0, sizeof(prev_out));
    uint32_t prev_n_out = 0;

    for (;;) {
        uint32_t enc_size = 0;
        uint8_t *enc_buf = NULL;

        if (prev_n_out > 0) {
            uint32_t alloc_ub = 0;
            for (uint32_t i = 0; i < prev_n_out; i++) {
                alloc_ub += 4;
                alloc_ub = (alloc_ub + 7U) & ~7U;
                alloc_ub += prev_out[i].size;
            }
            if (alloc_ub > 0) {
                enc_buf = calloc(1, alloc_ub);
                if (!enc_buf) {
                    perror("calloc enc_buf");
                    free(in_inline);
                    return -1;
                }
                uint32_t pos = 0;
                for (uint32_t i = 0; i < prev_n_out; i++) {
                    uint32_t sz = prev_out[i].size;
                    memcpy(enc_buf + pos, &sz, 4);
                    pos += 4;
                    if (sz > 0) {
                        pos = (pos + 7U) & ~7U;
                        memcpy(enc_buf + pos, prev_out[i].data, sz);
                        pos += sz;
                    }
                }
                enc_size = alloc_ub;
            }
            for (uint32_t i = 0; i < prev_n_out; i++) {
                free(prev_out[i].data);
                prev_out[i].data = NULL;
                prev_out[i].size = 0;
                prev_out[i].allocated = false;
            }
            prev_n_out = 0;
        }

        memset(in_inline, 0, 64);
        int ret = fastrpc2(&next2_def, fd, 3,
                           ctx_id, result,
                           enc_size, enc_buf,
                           &ctx_id, &handle, &sc, &in_total_size,
                           (uint32_t)IN_INLINE_CAP, in_inline);
        free(enc_buf);

        if (ret != 0) {
            if (ret == -1 && errno == EINTR)
                continue;
            fprintf(stderr, "fastrpc2(next2) exited: ret=%d errno=%d (%s)\n",
                    ret, errno, strerror(errno));
            free(in_inline);
            return (ret == -1) ? 0 : -1;
        }

        if (in_total_size > IN_INLINE_CAP) {
            fprintf(stderr, "Input payload %u exceeds %u cap (handle=%u sc=0x%08x)\n",
                    in_total_size, IN_INLINE_CAP, handle, sc);
            result = AEE_EBADPARM;
            prev_n_out = 0;
            continue;
        }

        uint32_t n_in = (sc >> 16) & 0xffU;
        uint32_t n_out = (sc >> 8) & 0xffU;
        uint32_t n_handles = sc & 0xffU;
        uint32_t method = (sc >> 24) & 0x7fU;

        if (n_handles != 0 || n_in > MAX_BUFS || n_out > MAX_BUFS) {
            fprintf(stderr, "Unsupported sc=0x%08x on handle=%u\n", sc, handle);
            result = AEE_EUNSUPPORTED;
            prev_n_out = 0;
            continue;
        }

        struct rpc_in_buf in_bufs[MAX_BUFS];
        memset(in_bufs, 0, sizeof(in_bufs));
        uint32_t pos = 0;
        bool decode_ok = true;

        for (uint32_t i = 0; i < n_in; i++) {
            if (pos + 4 > in_total_size) {
                decode_ok = false;
                break;
            }
            uint32_t sz = 0;
            memcpy(&sz, in_inline + pos, 4);
            pos += 4;
            if (sz > 0) {
                pos = (pos + 7U) & ~7U;
                if (pos + sz > in_total_size) {
                    decode_ok = false;
                    break;
                }
                in_bufs[i].data = in_inline + pos;
                in_bufs[i].size = sz;
                pos += sz;
            } else {
                in_bufs[i].data = (const uint8_t *)"";
                in_bufs[i].size = 0;
            }
        }

        if (!decode_ok) {
            fprintf(stderr, "Failed to decode input buffers (in_total_size=%u, sc=0x%08x)\n",
                    in_total_size, sc);
            result = AEE_EBADPARM;
            prev_n_out = 0;
            continue;
        }

        if (handle == 0) {
            result = handle_remotectl(method, n_in, in_bufs, n_out, prev_out);
        } else if (handle == 1) {
            result = handle_apps_std(method, n_in, in_bufs, n_out, prev_out);
        } else if (handle == 2) {
            result = handle_apps_mem(method, n_in, in_bufs, n_out, prev_out);
        } else {
            fprintf(stderr, "Unsupported listener handle=%u method=%u sc=0x%08x\n",
                    handle, method, sc);
            result = AEE_EUNSUPPORTED;
        }

        bool all_out_alloc = (n_out > 0);
        for (uint32_t i = 0; i < n_out; i++) {
            if (!prev_out[i].allocated) {
                all_out_alloc = false;
                break;
            }
        }

        if (result == AEE_SUCCESS || all_out_alloc) {
            prev_n_out = n_out;
        } else {
            for (uint32_t i = 0; i < n_out; i++) {
                free(prev_out[i].data);
                prev_out[i].data = NULL;
                prev_out[i].size = 0;
                prev_out[i].allocated = false;
            }
            prev_n_out = 0;
        }
    }
}

static void strip_trailing_slashes(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') {
        p[n - 1] = '\0';
        n--;
    }
}

int main(int argc, char **argv)
{
    const char *dev_path = "/dev/fastrpc-adsp";
    const char *dsp_name = "adsp";

    int opt;
    while ((opt = getopt(argc, argv, "f:d:R:W:sv")) != -1) {
        switch (opt) {
        case 'f':
            dev_path = optarg;
            break;
        case 'd':
            dsp_name = optarg;
            break;
        case 'R':
            snprintf(g_ro_root, sizeof(g_ro_root), "%s", optarg);
            break;
        case 'W':
            snprintf(g_rw_persist, sizeof(g_rw_persist), "%s", optarg);
            break;
        case 's':
            g_sensorspd = true;
            break;
        case 'v':
            g_verbose = true;
            break;
        default:
            fprintf(stderr, "Usage: %s -f DEV [-d adsp] [-s] [-R RO_DIR] [-W RW_PERSIST_DIR] [-v]\n", argv[0]);
            return 2;
        }
    }

    setvbuf(stderr, NULL, _IOLBF, 0);
    strip_trailing_slashes(g_ro_root);
    strip_trailing_slashes(g_rw_persist);
    env_init_defaults();

    fprintf(stderr, "nx769j-hexagonrpcd starting: dev=%s dsp=%s pd=%s ro=%s rw=%s\n",
            dev_path, dsp_name, g_sensorspd ? "sensorspd" : "rootpd", g_ro_root, g_rw_persist);

    g_fastrpc_fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (g_fastrpc_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
        return 1;
    }

    unsigned long attach_ioctl = g_sensorspd ? FASTRPC_IOCTL_INIT_ATTACH_SNS : FASTRPC_IOCTL_INIT_ATTACH;
    if (ioctl(g_fastrpc_fd, attach_ioctl, NULL) < 0) {
        fprintf(stderr, "Failed to attach (%s) on %s: %s\n",
                g_sensorspd ? "INIT_ATTACH_SNS" : "INIT_ATTACH", dev_path, strerror(errno));
        close(g_fastrpc_fd);
        return 1;
    }

    if (register_default_listener(g_fastrpc_fd) != 0) {
        close(g_fastrpc_fd);
        return 1;
    }

    fprintf(stderr, "nx769j-hexagonrpcd attached and registered (%s)\n",
            g_sensorspd ? "sensorspd" : "rootpd");

    int rc = run_listener_loop(g_fastrpc_fd);
    close(g_fastrpc_fd);
    return (rc == 0) ? 0 : 1;
}
