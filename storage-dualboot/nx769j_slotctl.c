/*
 * nx769j_slotctl.c — Bit-Accurate Qualcomm SM8650 (NX769J) A/B Slot & UFS Boot-LUN Controller
 *
 * Verified against live RedMagic 9 Pro (NX769J) GPT tables (/dev/sda../dev/sdf, lbs=4096),
 * /dev/bsg/ufs-bsg0 SG_IO v4 UPIU Query Attributes (bBootLunEn idn=0x00), and
 * stock Nubia/Qualcomm SM8650 libboot_control_qti.so + libgptutils.so:
 *
 *   - Dynamically discovers all same-LUN A/B partition pairs (31 pairs on NX769J:
 *     vbmeta_system_a/b on LUN0 sda + 30 boot-chain pairs on LUN4 sde), swapping
 *     PartitionTypeGUID (preserving 77036CD4-03D5-42BB-8ED1-37E5A88BAA34, UEFI
 *     active/inactive GUIDs, and vbmeta_system GUIDs) and updating byte[0x36]
 *     (AB_FLAG_OFFSET = 54) identically to QTI BootControl HAL (0x3f on newly
 *     active slot; &= ~0x04 on newly inactive slot).
 *   - Updates cross-LUN XBL A/B partition pairs across LUN1 sdb <-> LUN2 sdc
 *     (xbl_a/b, xbl_config_a/b, multiimgqti_a/b, multiimgoem_a/b: |= 0x04 / &= ~0x04).
 *   - Recomputes IEEE 802.3 CRC32 for both primary and backup GPT partition arrays
 *     and GPT headers across all modified LUNs, fsyncs, and re-verifies on disk.
 *   - Switches UFS bBootLunEn (1 = Boot LU A / slot _a, 2 = Boot LU B / slot _b)
 *     via /dev/bsg/ufs-bsg0 SG_IO and verifies hardware readback.
 *   - Supports `mark-successful [--only-if-slot=a|b]` (zero-write idempotent once
 *     already marked successful, preventing flash wear and keeping ABL tries_remaining
 *     from ever expiring across infinite normal Omarchy reboots).
 *
 * Dual-build support:
 *   1) Static zero-libc ARM64 Linux/Android ELF (-DNX_NOSTDLIB_ARM64):
 *      runs identically on Omarchy Mobile (Arch Linux ARM) and Android 14 (Magisk su).
 *   2) Standard POSIX host binary (macOS / Linux) for offline GPT fixture unit testing.
 */

#ifdef NX_NOSTDLIB_ARM64

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef int                s32;
typedef unsigned long      u64;
typedef long               s64;
typedef unsigned long      size_t;

#define NULL ((void *)0)

static inline s64 sys_call(s64 n, s64 a0, s64 a1, s64 a2, s64 a3, s64 a4, s64 a5) {
    register s64 x8 __asm__("x8") = n;
    register s64 x0 __asm__("x0") = a0;
    register s64 x1 __asm__("x1") = a1;
    register s64 x2 __asm__("x2") = a2;
    register s64 x3 __asm__("x3") = a3;
    register s64 x4 __asm__("x4") = a4;
    register s64 x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}

#define SYS_ioctl   29
#define SYS_openat  56
#define SYS_close   57
#define SYS_read    63
#define SYS_write   64
#define SYS_pread64 67
#define SYS_pwrite64 68
#define SYS_fsync   82
#define SYS_exit    93

#define AT_FDCWD    (-100)
#define O_RDONLY    00
#define O_RDWR      02

static int nx_open(const char *path, int flags) {
    s64 r = sys_call(SYS_openat, AT_FDCWD, (s64)path, flags, 0, 0, 0);
    return (r < 0) ? -1 : (int)r;
}

static int nx_close(int fd) {
    return (sys_call(SYS_close, fd, 0, 0, 0, 0, 0) < 0) ? -1 : 0;
}

static s64 nx_read(int fd, void *buf, size_t count) {
    return sys_call(SYS_read, fd, (s64)buf, (s64)count, 0, 0, 0);
}

static s64 nx_write(int fd, const void *buf, size_t count) {
    return sys_call(SYS_write, fd, (s64)buf, (s64)count, 0, 0, 0);
}

static s64 nx_pread(int fd, void *buf, size_t count, u64 offset) {
    u8 *p = (u8 *)buf;
    size_t done = 0;
    while (done < count) {
        s64 r = sys_call(SYS_pread64, fd, (s64)(p + done), (s64)(count - done), (s64)(offset + done), 0, 0);
        if (r <= 0) return (done > 0 && r == 0) ? (s64)done : -1;
        done += (size_t)r;
    }
    return (s64)done;
}

static s64 nx_pwrite(int fd, const void *buf, size_t count, u64 offset) {
    const u8 *p = (const u8 *)buf;
    size_t done = 0;
    while (done < count) {
        s64 w = sys_call(SYS_pwrite64, fd, (s64)(p + done), (s64)(count - done), (s64)(offset + done), 0, 0);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return (s64)done;
}

static int nx_fsync(int fd) {
    return (sys_call(SYS_fsync, fd, 0, 0, 0, 0, 0) < 0) ? -1 : 0;
}

static int nx_ioctl(int fd, unsigned long req, void *arg) {
    s64 r = sys_call(SYS_ioctl, fd, (s64)req, (s64)arg, 0, 0, 0);
    return (r < 0) ? -1 : (int)r;
}

#else /* POSIX host / standard libc build */

#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;

static int nx_open(const char *path, int flags) {
    return open(path, flags);
}

static int nx_close(int fd) {
    return close(fd);
}

static s64 nx_read(int fd, void *buf, size_t count) {
    return (s64)read(fd, buf, count);
}

static s64 nx_write(int fd, const void *buf, size_t count) {
    return (s64)write(fd, buf, count);
}

static s64 nx_pread(int fd, void *buf, size_t count, u64 offset) {
    u8 *p = (u8 *)buf;
    size_t done = 0;
    while (done < count) {
        ssize_t r = pread(fd, p + done, count - done, (off_t)(offset + done));
        if (r <= 0) return (done > 0 && r == 0) ? (s64)done : -1;
        done += (size_t)r;
    }
    return (s64)done;
}

static s64 nx_pwrite(int fd, const void *buf, size_t count, u64 offset) {
    const u8 *p = (const u8 *)buf;
    size_t done = 0;
    while (done < count) {
        ssize_t w = pwrite(fd, p + done, count - done, (off_t)(offset + done));
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return (s64)done;
}

static int nx_fsync(int fd) {
    return fsync(fd);
}

static int nx_ioctl(int fd, unsigned long req, void *arg) {
#ifdef __linux__
    return ioctl(fd, req, arg);
#else
    (void)fd;
    (void)req;
    (void)arg;
    return -1;
#endif
}

#endif /* NX_NOSTDLIB_ARM64 */

/* Minimal string / memory / formatting primitives (shared across nostdlib & host) */
static size_t s_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int s_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int s_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static void s_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void s_cat(char *dst, const char *src, size_t cap) {
    size_t d = s_len(dst);
    size_t i = 0;
    if (d >= cap) return;
    while (d + i + 1 < cap && src[i]) {
        dst[d + i] = src[i];
        i++;
    }
    dst[d + i] = 0;
}

static void m_zero(void *p, size_t n) {
    u8 *b = (u8 *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static void m_copy(void *dst, const void *src, size_t n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static int m_eq(const void *a, const void *b, size_t n) {
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static void out_raw(int fd, const char *s, size_t len) {
    size_t done = 0;
    while (done < len) {
        s64 w = nx_write(fd, s + done, len - done);
        if (w <= 0) break;
        done += (size_t)w;
    }
}

static void out_s(int fd, const char *s) {
    out_raw(fd, s, s_len(s));
}

static void out_u64(int fd, u64 v) {
    char buf[32];
    int i = 30;
    buf[31] = 0;
    if (v == 0) {
        out_s(fd, "0");
        return;
    }
    while (v > 0 && i >= 0) {
        buf[i--] = (char)('0' + (v % 10));
        v /= 10;
    }
    out_s(fd, &buf[i + 1]);
}

static void out_hex_u8(int fd, u8 v) {
    char buf[5];
    const char *hx = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = hx[(v >> 4) & 0xf];
    buf[3] = hx[v & 0xf];
    buf[4] = 0;
    out_s(fd, buf);
}

#ifdef NX_NOSTDLIB_ARM64
void *memset(void *dst, int c, size_t n) {
    u8 *d = (u8 *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    m_copy(dst, src, n);
    return dst;
}
#endif

/* Little-endian / big-endian helpers */
static u32 get_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 get_le64(const u8 *p) {
    return (u64)get_le32(p) | ((u64)get_le32(p + 4) << 32);
}

static void put_le32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
    p[2] = (u8)((v >> 16) & 0xff);
    p[3] = (u8)((v >> 24) & 0xff);
}

static u32 get_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)((v >> 24) & 0xff);
    p[1] = (u8)((v >> 16) & 0xff);
    p[2] = (u8)((v >> 8) & 0xff);
    p[3] = (u8)(v & 0xff);
}

/* Standard IEEE 802.3 / EFI GPT CRC32 */
static u32 gpt_crc32(const u8 *buf, size_t len) {
    u32 crc = 0xffffffffU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (u32)buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1U)
                crc = (crc >> 1) ^ 0xedb88320U;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

/* GPT & Qualcomm A/B constants */
#define MAX_LUNS            6
#define MAX_BlockSize       4096
#define MAX_PE_BYTES        32768  /* 256 * 128 (sde has 96 * 128 = 12288) */
#define MAX_PARTS_PER_LUN   128

#define GPT_HDR_SIZE_OFF    12
#define GPT_HDR_CRC_OFF     16
#define GPT_MY_LBA_OFF      24
#define GPT_ALT_LBA_OFF     32
#define GPT_PE_LBA_OFF      72
#define GPT_NUM_PE_OFF      80
#define GPT_PE_SZ_OFF       84
#define GPT_PE_CRC_OFF      88

#define GPT_ENT_TYPE_OFF    0
#define GPT_ENT_TYPE_LEN    16
#define GPT_ENT_ATTR_OFF    48
#define GPT_ENT_AB_OFF      54     /* Attribute byte 6 (bits 48..55) */
#define GPT_ENT_NAME_OFF    56
#define GPT_ENT_NAME_CHARS  36

#define AB_ATTR_PRIORITY_MASK   0x03U
#define AB_ATTR_ACTIVE          0x04U
#define AB_ATTR_TRIES_SHIFT     3
#define AB_ATTR_TRIES_MASK      0x38U
#define AB_ATTR_SUCCESSFUL      0x40U
#define AB_ATTR_UNBOOTABLE      0x80U
#define AB_SLOT_ACTIVE_INIT_VAL 0x3fU /* priority=3 | active=1 | tries=7 | successful=0 | unbootable=0 */

/* Linux BLKSSZGET = _IO(0x12, 104) = 0x1268; SG_IO = 0x2285 */
#define LINUX_BLKSSZGET     0x1268UL
#define LINUX_SG_IO         0x2285UL

struct lun_gpt {
    char name[8];
    char path[256];
    int  present;
    int  dirty;
    u32  lbs;
    u32  hsz;
    u32  bhsz;
    u64  alt_lba;
    u64  pe_lba;
    u64  bpe_lba;
    u32  num_pe;
    u32  pe_sz;
    u32  pe_bytes;
    u8   hdr[MAX_BlockSize];
    u8   bhdr[MAX_BlockSize];
    u8   pe[MAX_PE_BYTES];
    u8   bpe[MAX_PE_BYTES];
};

static struct lun_gpt g_luns[MAX_LUNS];
static const char *const k_lun_names[MAX_LUNS] = { "sda", "sdb", "sdc", "sdd", "sde", "sdf" };

struct same_lun_pair {
    int  lun_idx;
    u32  idx_a;
    u32  idx_b;
    char base[40];
};

struct cross_lun_pair {
    int  lun_a;
    u32  idx_a;
    int  lun_b;
    u32  idx_b;
    char base[40];
};

static struct same_lun_pair  g_same_pairs[64];
static int                   g_num_same_pairs = 0;
static struct cross_lun_pair g_cross_pairs[16];
static int                   g_num_cross_pairs = 0;

static void decode_part_name(const u8 *ent, char *out, size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < GPT_ENT_NAME_CHARS && pos + 1 < cap; i++) {
        u8 lo = ent[GPT_ENT_NAME_OFF + i * 2];
        u8 hi = ent[GPT_ENT_NAME_OFF + i * 2 + 1];
        if (lo == 0 && hi == 0) break;
        out[pos++] = (hi == 0) ? (char)lo : '?';
    }
    out[pos] = 0;
}

static int ent_is_used(const u8 *ent) {
    for (int i = 0; i < GPT_ENT_TYPE_LEN; i++) {
        if (ent[i] != 0) return 1;
    }
    return 0;
}

static u32 detect_lbs(int fd, const char *lun_name) {
    char sys_path[128];
    sys_path[0] = 0;
    s_cat(sys_path, "/sys/block/", sizeof(sys_path));
    s_cat(sys_path, lun_name, sizeof(sys_path));
    s_cat(sys_path, "/queue/logical_block_size", sizeof(sys_path));
    int sfd = nx_open(sys_path, O_RDONLY);
    if (sfd >= 0) {
        char buf[32];
        m_zero(buf, sizeof(buf));
        s64 r = nx_read(sfd, buf, sizeof(buf) - 1);
        nx_close(sfd);
        if (r > 0) {
            u32 v = 0;
            for (s64 i = 0; i < r && buf[i] >= '0' && buf[i] <= '9'; i++) {
                v = v * 10 + (u32)(buf[i] - '0');
            }
            if (v == 4096 || v == 512) return v;
        }
    }
    u32 ioctl_bs = 0;
    if (nx_ioctl(fd, LINUX_BLKSSZGET, &ioctl_bs) == 0 && (ioctl_bs == 4096 || ioctl_bs == 512)) {
        return ioctl_bs;
    }
    u8 probe[8];
    if (nx_pread(fd, probe, 8, 4096) == 8 && m_eq(probe, "EFI PART", 8)) return 4096;
    if (nx_pread(fd, probe, 8, 512) == 8 && m_eq(probe, "EFI PART", 8)) return 512;
    return 0;
}

static int load_and_verify_lun(struct lun_gpt *g, const char *dev_dir, const char *lun_name) {
    m_zero(g, sizeof(*g));
    s_copy(g->name, lun_name, sizeof(g->name));
    g->path[0] = 0;
    s_cat(g->path, dev_dir, sizeof(g->path));
    s_cat(g->path, "/", sizeof(g->path));
    s_cat(g->path, lun_name, sizeof(g->path));

    int fd = nx_open(g->path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    u32 lbs = detect_lbs(fd, lun_name);
    if (lbs != 4096 && lbs != 512) {
        nx_close(fd);
        out_s(2, "ERR: cannot detect GPT block size on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    g->lbs = lbs;

    if (nx_pread(fd, g->hdr, lbs, (u64)lbs) != (s64)lbs) {
        nx_close(fd);
        out_s(2, "ERR: failed to read primary GPT header on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    if (!m_eq(g->hdr, "EFI PART", 8)) {
        nx_close(fd);
        out_s(2, "ERR: bad primary GPT signature on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    g->hsz = get_le32(g->hdr + GPT_HDR_SIZE_OFF);
    u32 exp_hcrc = get_le32(g->hdr + GPT_HDR_CRC_OFF);
    u64 my_lba = get_le64(g->hdr + GPT_MY_LBA_OFF);
    g->alt_lba = get_le64(g->hdr + GPT_ALT_LBA_OFF);
    g->pe_lba = get_le64(g->hdr + GPT_PE_LBA_OFF);
    g->num_pe = get_le32(g->hdr + GPT_NUM_PE_OFF);
    g->pe_sz = get_le32(g->hdr + GPT_PE_SZ_OFF);
    u32 exp_pe_crc = get_le32(g->hdr + GPT_PE_CRC_OFF);

    if (g->hsz < 92 || g->hsz > lbs || my_lba != 1 || g->pe_sz < 128 || (g->pe_sz % 8) != 0 ||
        g->num_pe == 0 || g->num_pe > MAX_PARTS_PER_LUN || (g->num_pe * g->pe_sz) > MAX_PE_BYTES) {
        nx_close(fd);
        out_s(2, "ERR: invalid GPT geometry on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    g->pe_bytes = g->num_pe * g->pe_sz;

    u8 tmp_hdr[MAX_BlockSize];
    m_copy(tmp_hdr, g->hdr, lbs);
    put_le32(tmp_hdr + GPT_HDR_CRC_OFF, 0);
    u32 calc_hcrc = gpt_crc32(tmp_hdr, g->hsz);
    if (calc_hcrc != exp_hcrc) {
        nx_close(fd);
        out_s(2, "ERR: primary GPT header CRC mismatch on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    if (nx_pread(fd, g->pe, g->pe_bytes, g->pe_lba * (u64)lbs) != (s64)g->pe_bytes) {
        nx_close(fd);
        out_s(2, "ERR: failed to read primary GPT entries on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    u32 calc_pe_crc = gpt_crc32(g->pe, g->pe_bytes);
    if (calc_pe_crc != exp_pe_crc) {
        nx_close(fd);
        out_s(2, "ERR: primary GPT entry-array CRC mismatch on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    if (nx_pread(fd, g->bhdr, lbs, g->alt_lba * (u64)lbs) != (s64)lbs) {
        nx_close(fd);
        out_s(2, "ERR: failed to read backup GPT header on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    if (!m_eq(g->bhdr, "EFI PART", 8)) {
        nx_close(fd);
        out_s(2, "ERR: bad backup GPT signature on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    g->bhsz = get_le32(g->bhdr + GPT_HDR_SIZE_OFF);
    u32 exp_bhcrc = get_le32(g->bhdr + GPT_HDR_CRC_OFF);
    u64 bmy_lba = get_le64(g->bhdr + GPT_MY_LBA_OFF);
    u64 balt_lba = get_le64(g->bhdr + GPT_ALT_LBA_OFF);
    g->bpe_lba = get_le64(g->bhdr + GPT_PE_LBA_OFF);
    u32 bnum_pe = get_le32(g->bhdr + GPT_NUM_PE_OFF);
    u32 bpe_sz = get_le32(g->bhdr + GPT_PE_SZ_OFF);
    u32 exp_bpe_crc = get_le32(g->bhdr + GPT_PE_CRC_OFF);

    if (g->bhsz != g->hsz || bmy_lba != g->alt_lba || balt_lba != 1 ||
        bnum_pe != g->num_pe || bpe_sz != g->pe_sz) {
        nx_close(fd);
        out_s(2, "ERR: backup GPT header geometry mismatch on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    m_copy(tmp_hdr, g->bhdr, lbs);
    put_le32(tmp_hdr + GPT_HDR_CRC_OFF, 0);
    u32 calc_bhcrc = gpt_crc32(tmp_hdr, g->bhsz);
    if (calc_bhcrc != exp_bhcrc) {
        nx_close(fd);
        out_s(2, "ERR: backup GPT header CRC mismatch on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    if (nx_pread(fd, g->bpe, g->pe_bytes, g->bpe_lba * (u64)lbs) != (s64)g->pe_bytes) {
        nx_close(fd);
        out_s(2, "ERR: failed to read backup GPT entries on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    u32 calc_bpe_crc = gpt_crc32(g->bpe, g->pe_bytes);
    if (calc_bpe_crc != exp_bpe_crc) {
        nx_close(fd);
        out_s(2, "ERR: backup GPT entry-array CRC mismatch on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    if (!m_eq(g->pe, g->bpe, g->pe_bytes)) {
        nx_close(fd);
        out_s(2, "ERR: primary vs backup GPT entries diverge on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    nx_close(fd);
    g->present = 1;
    return 0;
}

static int commit_lun(struct lun_gpt *g) {
    if (!g->present || !g->dirty) return 0;

    u32 pe_crc = gpt_crc32(g->pe, g->pe_bytes);
    u32 bpe_crc = gpt_crc32(g->bpe, g->pe_bytes);
    if (pe_crc != bpe_crc || !m_eq(g->pe, g->bpe, g->pe_bytes)) {
        out_s(2, "ERR: internal primary/backup mismatch before commit on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    put_le32(g->hdr + GPT_PE_CRC_OFF, pe_crc);
    put_le32(g->hdr + GPT_HDR_CRC_OFF, 0);
    u32 hcrc = gpt_crc32(g->hdr, g->hsz);
    put_le32(g->hdr + GPT_HDR_CRC_OFF, hcrc);

    put_le32(g->bhdr + GPT_PE_CRC_OFF, bpe_crc);
    put_le32(g->bhdr + GPT_HDR_CRC_OFF, 0);
    u32 bhcrc = gpt_crc32(g->bhdr, g->bhsz);
    put_le32(g->bhdr + GPT_HDR_CRC_OFF, bhcrc);

    int fd = nx_open(g->path, O_RDWR);
    if (fd < 0) {
        out_s(2, "ERR: open O_RDWR failed on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }

    /* Update backup table first, then primary table (crash-resilient order), fsyncing cleanly */
    if (nx_pwrite(fd, g->bpe, g->pe_bytes, g->bpe_lba * (u64)g->lbs) != (s64)g->pe_bytes ||
        nx_pwrite(fd, g->bhdr, g->lbs, g->alt_lba * (u64)g->lbs) != (s64)g->lbs ||
        nx_fsync(fd) != 0 ||
        nx_pwrite(fd, g->pe, g->pe_bytes, g->pe_lba * (u64)g->lbs) != (s64)g->pe_bytes ||
        nx_pwrite(fd, g->hdr, g->lbs, (u64)g->lbs) != (s64)g->lbs ||
        nx_fsync(fd) != 0) {
        nx_close(fd);
        out_s(2, "ERR: write/fsync failed committing GPT on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    nx_close(fd);
    g->dirty = 0;

    /* Re-read and verify committed GPT from storage */
    struct lun_gpt verify;
    char dir_buf[256];
    s_copy(dir_buf, g->path, sizeof(dir_buf));
    size_t plen = s_len(dir_buf);
    while (plen > 0 && dir_buf[plen - 1] != '/') plen--;
    if (plen > 0) dir_buf[plen - 1] = 0;
    if (load_and_verify_lun(&verify, dir_buf, g->name) != 0) {
        out_s(2, "ERR: post-commit GPT readback verification failed on ");
        out_s(2, g->path);
        out_s(2, "\n");
        return -1;
    }
    return 0;
}

static int find_part_on_lun(const struct lun_gpt *g, const char *want_name, u32 *out_idx) {
    for (u32 i = 0; i < g->num_pe; i++) {
        const u8 *ent = g->pe + i * g->pe_sz;
        if (!ent_is_used(ent)) continue;
        char nm[40];
        decode_part_name(ent, nm, sizeof(nm));
        if (s_eq(nm, want_name)) {
            *out_idx = i;
            return 0;
        }
    }
    return -1;
}

static int discover_ab_pairs(const char *dev_dir) {
    g_num_same_pairs = 0;
    g_num_cross_pairs = 0;

    for (int li = 0; li < MAX_LUNS; li++) {
        if (load_and_verify_lun(&g_luns[li], dev_dir, k_lun_names[li]) != 0) {
            return -1;
        }
    }

    for (int li = 0; li < MAX_LUNS; li++) {
        const struct lun_gpt *g = &g_luns[li];
        for (u32 i = 0; i < g->num_pe; i++) {
            const u8 *ent = g->pe + i * g->pe_sz;
            if (!ent_is_used(ent)) continue;
            char nm[40];
            decode_part_name(ent, nm, sizeof(nm));
            size_t len = s_len(nm);
            if (len < 3 || nm[len - 2] != '_' || nm[len - 1] != 'a') continue;

            char base[40];
            s_copy(base, nm, sizeof(base));
            base[len - 2] = 0;

            char partner_b[40];
            s_copy(partner_b, nm, sizeof(partner_b));
            partner_b[len - 1] = 'b';

            u32 idx_b = 0;
            if (find_part_on_lun(g, partner_b, &idx_b) == 0) {
                if (g_num_same_pairs >= 64) {
                    out_s(2, "ERR: too many same-LUN A/B pairs\n");
                    return -1;
                }
                g_same_pairs[g_num_same_pairs].lun_idx = li;
                g_same_pairs[g_num_same_pairs].idx_a = i;
                g_same_pairs[g_num_same_pairs].idx_b = idx_b;
                s_copy(g_same_pairs[g_num_same_pairs].base, base, sizeof(g_same_pairs[0].base));
                g_num_same_pairs++;
            } else {
                /* Check cross-LUN partner (e.g. sdb xbl_a <-> sdc xbl_b) */
                for (int lj = 0; lj < MAX_LUNS; lj++) {
                    if (lj == li) continue;
                    if (find_part_on_lun(&g_luns[lj], partner_b, &idx_b) == 0) {
                        if (g_num_cross_pairs >= 16) {
                            out_s(2, "ERR: too many cross-LUN A/B pairs\n");
                            return -1;
                        }
                        g_cross_pairs[g_num_cross_pairs].lun_a = li;
                        g_cross_pairs[g_num_cross_pairs].idx_a = i;
                        g_cross_pairs[g_num_cross_pairs].lun_b = lj;
                        g_cross_pairs[g_num_cross_pairs].idx_b = idx_b;
                        s_copy(g_cross_pairs[g_num_cross_pairs].base, base, sizeof(g_cross_pairs[0].base));
                        g_num_cross_pairs++;
                        break;
                    }
                }
            }
        }
    }

    if (g_num_same_pairs == 0) {
        out_s(2, "ERR: zero same-LUN A/B pairs found\n");
        return -1;
    }
    return 0;
}

/* UFS BSG SG_IO v4 exact wire layout (160-byte sg_io_v4, 36-byte req, 40-byte rsp) */
struct nx_sg_io_v4 {
    s32 guard;
    u32 protocol;
    u32 subprotocol;
    u32 request_len;
    u64 request;
    u64 request_tag;
    u32 request_attr;
    u32 request_priority;
    u32 request_extra;
    u32 max_response_len;
    u64 response;
    u32 dout_iovec_count;
    u32 dout_xfer_len;
    u32 din_iovec_count;
    u32 din_xfer_len;
    u64 dout_xferp;
    u64 din_xferp;
    u32 timeout;
    u32 flags;
    u64 usr_ptr;
    u32 spare_in;
    u32 driver_status;
    u32 transport_status;
    u32 device_status;
    u32 retry_delay;
    u32 info;
    u32 duration;
    u32 response_len;
    s32 din_resid;
    s32 dout_resid;
    u64 generated_tag;
    u32 spare_out;
    u32 padding;
};

static const char *const k_bsg_paths[] = {
    "/dev/bsg/ufs-bsg0",
    "/dev/ufs-bsg0",
    "/dev/ufs-bsg",
    NULL
};

static int open_bsg_dev(int rw) {
    for (int i = 0; k_bsg_paths[i]; i++) {
        int fd = nx_open(k_bsg_paths[i], rw ? O_RDWR : O_RDONLY);
        if (fd >= 0) return fd;
    }
    return -1;
}

static int ufs_read_boot_lun(const char *mock_bsg_file) {
    if (mock_bsg_file && mock_bsg_file[0]) {
        int fd = nx_open(mock_bsg_file, O_RDONLY);
        if (fd < 0) return -1;
        char buf[16];
        m_zero(buf, sizeof(buf));
        s64 r = nx_read(fd, buf, sizeof(buf) - 1);
        nx_close(fd);
        if (r <= 0) return -1;
        if (buf[0] == '1') return 1;
        if (buf[0] == '2') return 2;
        return -1;
    }

    int fd = open_bsg_dev(0);
    if (fd >= 0) {
        u8 req[36];
        u8 rsp[40];
        struct nx_sg_io_v4 sg;
        m_zero(req, sizeof(req));
        m_zero(rsp, sizeof(rsp));
        m_zero(&sg, sizeof(sg));

        put_le32(req + 0, 0x16U);           /* UTP_UPIU_QUERY_REQ */
        put_be32(req + 4, 0x16000000U);     /* transaction_code = 0x16 */
        put_be32(req + 8, 0x00010000U);     /* QUERY_REQ_FUNC_STD_READ = 0x01 */
        put_be32(req + 12, 0x00000000U);
        req[16] = 0x03U;                    /* QUERY_REQ_OP_READ_ATTR */
        req[17] = 0x00U;                    /* QUERY_ATTR_IDN_BOOT_LU_EN */

        sg.guard = 'Q';
        sg.protocol = 0;                    /* BSG_PROTOCOL_SCSI */
        sg.subprotocol = 2;                 /* BSG_SUB_PROTOCOL_SCSI_TRANSPORT */
        sg.request_len = sizeof(req);
        sg.request = (u64)(size_t)req;
        sg.max_response_len = sizeof(rsp);
        sg.response = (u64)(size_t)rsp;
        sg.timeout = 5000;

        int rc = nx_ioctl(fd, LINUX_SG_IO, &sg);
        nx_close(fd);
        s32 lld_res = (s32)get_le32(rsp);
        if (rc == 0 && sg.info == 0 && lld_res == 0) {
            u32 val = get_be32(rsp + 28);
            if (val == 1 || val == 2) return (int)val;
        }
    }

    /* Read-only sysfs fallback */
    int sfd = nx_open("/sys/bus/platform/drivers/ufshcd-qcom/1d84000.ufshc/attributes/boot_lun_enabled", O_RDONLY);
    if (sfd >= 0) {
        char buf[32];
        m_zero(buf, sizeof(buf));
        s64 r = nx_read(sfd, buf, sizeof(buf) - 1);
        nx_close(sfd);
        if (r > 0) {
            for (s64 i = r - 1; i >= 0; i--) {
                if (buf[i] == '1') return 1;
                if (buf[i] == '2') return 2;
            }
        }
    }
    return -1;
}

static int ufs_write_boot_lun(u8 lun_id, const char *mock_bsg_file) {
    if (lun_id != 1 && lun_id != 2) return -1;

    if (mock_bsg_file && mock_bsg_file[0]) {
        int fd = nx_open(mock_bsg_file, O_RDWR);
        if (fd < 0) {
            out_s(2, "ERR: failed to open mock BSG file for write\n");
            return -1;
        }
        char ch = (char)('0' + lun_id);
        if (nx_pwrite(fd, &ch, 1, 0) != 1 || nx_fsync(fd) != 0) {
            nx_close(fd);
            return -1;
        }
        nx_close(fd);
        return (ufs_read_boot_lun(mock_bsg_file) == (int)lun_id) ? 0 : -1;
    }

    int fd = open_bsg_dev(1);
    if (fd < 0) {
        out_s(2, "ERR: unable to open /dev/bsg/ufs-bsg0 O_RDWR\n");
        return -1;
    }

    u8 req[36];
    u8 rsp[40];
    struct nx_sg_io_v4 sg;
    m_zero(req, sizeof(req));
    m_zero(rsp, sizeof(rsp));
    m_zero(&sg, sizeof(sg));

    put_le32(req + 0, 0x16U);               /* UTP_UPIU_QUERY_REQ */
    put_be32(req + 4, 0x16000000U);         /* transaction_code = 0x16 */
    put_be32(req + 8, 0x00810000U);         /* QUERY_REQ_FUNC_STD_WRITE = 0x81 */
    put_be32(req + 12, 0x00000000U);
    req[16] = 0x04U;                        /* QUERY_REQ_OP_WRITE_ATTR */
    req[17] = 0x00U;                        /* QUERY_ATTR_IDN_BOOT_LU_EN */
    put_be32(req + 24, (u32)lun_id);        /* qr.value at header(12)+msgcode(4)+8 = offset 24 */

    sg.guard = 'Q';
    sg.protocol = 0;
    sg.subprotocol = 2;
    sg.request_len = sizeof(req);
    sg.request = (u64)(size_t)req;
    sg.max_response_len = sizeof(rsp);
    sg.response = (u64)(size_t)rsp;
    sg.timeout = 5000;

    int rc = nx_ioctl(fd, LINUX_SG_IO, &sg);
    nx_close(fd);
    s32 lld_res = (s32)get_le32(rsp);
    if (rc != 0 || sg.info != 0 || lld_res != 0) {
        out_s(2, "ERR: SG_IO WRITE_ATTR bBootLunEn failed\n");
        return -1;
    }

    int rb = ufs_read_boot_lun(mock_bsg_file);
    if (rb != (int)lun_id) {
        out_s(2, "ERR: bBootLunEn readback mismatch after SG_IO write\n");
        return -1;
    }
    return 0;
}

static const struct same_lun_pair *find_boot_pair(void) {
    for (int i = 0; i < g_num_same_pairs; i++) {
        if (s_eq(g_same_pairs[i].base, "boot")) return &g_same_pairs[i];
    }
    return NULL;
}

static int get_active_slot_from_gpt(u8 *out_b6_a, u8 *out_b6_b) {
    const struct same_lun_pair *bp = find_boot_pair();
    if (!bp) {
        out_s(2, "ERR: boot_a/boot_b pair not found in GPT\n");
        return -1;
    }
    const struct lun_gpt *g = &g_luns[bp->lun_idx];
    u8 b6_a = g->pe[bp->idx_a * g->pe_sz + GPT_ENT_AB_OFF];
    u8 b6_b = g->pe[bp->idx_b * g->pe_sz + GPT_ENT_AB_OFF];
    if (out_b6_a) *out_b6_a = b6_a;
    if (out_b6_b) *out_b6_b = b6_b;

    int act_a = (b6_a & AB_ATTR_ACTIVE) ? 1 : 0;
    int act_b = (b6_b & AB_ATTR_ACTIVE) ? 1 : 0;
    if (act_a && !act_b) return 0;
    if (act_b && !act_a) return 1;
    out_s(2, "ERR: ambiguous active bit on boot_a/boot_b\n");
    return -1;
}

static int parse_slot_arg(const char *s) {
    if (!s) return -1;
    if (s_eq(s, "a") || s_eq(s, "A") || s_eq(s, "_a") || s_eq(s, "0")) return 0;
    if (s_eq(s, "b") || s_eq(s, "B") || s_eq(s, "_b") || s_eq(s, "1")) return 1;
    return -1;
}

static int cmd_status(int json_mode, const char *mock_bsg_file) {
    u8 b6_a = 0, b6_b = 0;
    int active_slot = get_active_slot_from_gpt(&b6_a, &b6_b);
    if (active_slot < 0) return 1;
    int boot_lun = ufs_read_boot_lun(mock_bsg_file);

    u32 prio_a = b6_a & AB_ATTR_PRIORITY_MASK;
    u32 act_a  = (b6_a & AB_ATTR_ACTIVE) ? 1U : 0U;
    u32 tries_a = (b6_a & AB_ATTR_TRIES_MASK) >> AB_ATTR_TRIES_SHIFT;
    u32 succ_a = (b6_a & AB_ATTR_SUCCESSFUL) ? 1U : 0U;
    u32 unb_a  = (b6_a & AB_ATTR_UNBOOTABLE) ? 1U : 0U;

    u32 prio_b = b6_b & AB_ATTR_PRIORITY_MASK;
    u32 act_b  = (b6_b & AB_ATTR_ACTIVE) ? 1U : 0U;
    u32 tries_b = (b6_b & AB_ATTR_TRIES_MASK) >> AB_ATTR_TRIES_SHIFT;
    u32 succ_b = (b6_b & AB_ATTR_SUCCESSFUL) ? 1U : 0U;
    u32 unb_b  = (b6_b & AB_ATTR_UNBOOTABLE) ? 1U : 0U;

    if (json_mode) {
        out_s(1, "{\"active_slot\":\"");
        out_s(1, active_slot == 0 ? "a" : "b");
        out_s(1, "\",\"boot_lun_enabled\":");
        if (boot_lun < 0) out_s(1, "-1"); else out_u64(1, (u64)boot_lun);
        out_s(1, ",\"same_lun_pairs\":");
        out_u64(1, (u64)g_num_same_pairs);
        out_s(1, ",\"cross_lun_pairs\":");
        out_u64(1, (u64)g_num_cross_pairs);
        out_s(1, ",\"all_crc_valid\":true,\"slots\":{\"a\":{\"byte6\":\"");
        out_hex_u8(1, b6_a);
        out_s(1, "\",\"priority\":"); out_u64(1, prio_a);
        out_s(1, ",\"active\":"); out_u64(1, act_a);
        out_s(1, ",\"tries_remaining\":"); out_u64(1, tries_a);
        out_s(1, ",\"successful\":"); out_u64(1, succ_a);
        out_s(1, ",\"unbootable\":"); out_u64(1, unb_a);
        out_s(1, "},\"b\":{\"byte6\":\"");
        out_hex_u8(1, b6_b);
        out_s(1, "\",\"priority\":"); out_u64(1, prio_b);
        out_s(1, ",\"active\":"); out_u64(1, act_b);
        out_s(1, ",\"tries_remaining\":"); out_u64(1, tries_b);
        out_s(1, ",\"successful\":"); out_u64(1, succ_b);
        out_s(1, ",\"unbootable\":"); out_u64(1, unb_b);
        out_s(1, "}}}\n");
    } else {
        out_s(1, "Active slot      : _");
        out_s(1, active_slot == 0 ? "a" : "b");
        out_s(1, "\nUFS bBootLunEn   : ");
        if (boot_lun < 0) out_s(1, "unknown"); else out_u64(1, (u64)boot_lun);
        out_s(1, "\nSame-LUN pairs   : "); out_u64(1, (u64)g_num_same_pairs);
        out_s(1, "\nCross-LUN pairs  : "); out_u64(1, (u64)g_num_cross_pairs);
        out_s(1, "\nSLOT _a (boot_a) : byte6="); out_hex_u8(1, b6_a);
        out_s(1, " active="); out_u64(1, act_a);
        out_s(1, " priority="); out_u64(1, prio_a);
        out_s(1, " tries="); out_u64(1, tries_a);
        out_s(1, " successful="); out_u64(1, succ_a);
        out_s(1, " unbootable="); out_u64(1, unb_a);
        out_s(1, "\nSLOT _b (boot_b) : byte6="); out_hex_u8(1, b6_b);
        out_s(1, " active="); out_u64(1, act_b);
        out_s(1, " priority="); out_u64(1, prio_b);
        out_s(1, " tries="); out_u64(1, tries_b);
        out_s(1, " successful="); out_u64(1, succ_b);
        out_s(1, " unbootable="); out_u64(1, unb_b);
        out_s(1, "\nGPT CRC32 health : OK (all 6 LUNs primary + backup verified)\n");
    }
    return 0;
}

static int cmd_set_active(int target_slot, const char *mock_bsg_file) {
    if (target_slot != 0 && target_slot != 1) return 1;

    /* Pre-check BSG write availability BEFORE modifying any GPT bytes */
    if (!mock_bsg_file || !mock_bsg_file[0]) {
        int bfd = open_bsg_dev(1);
        if (bfd < 0) {
            out_s(2, "ERR: cannot open /dev/bsg/ufs-bsg0 O_RDWR; refusing partial slot switch\n");
            return 1;
        }
        nx_close(bfd);
    } else {
        int bfd = nx_open(mock_bsg_file, O_RDWR);
        if (bfd < 0) {
            out_s(2, "ERR: cannot open mock BSG file O_RDWR\n");
            return 1;
        }
        nx_close(bfd);
    }

    /* Validate every same-LUN A/B pair has a single active member before mutating */
    for (int i = 0; i < g_num_same_pairs; i++) {
        const struct same_lun_pair *sp = &g_same_pairs[i];
        const struct lun_gpt *g = &g_luns[sp->lun_idx];
        u8 b6_a = g->pe[sp->idx_a * g->pe_sz + GPT_ENT_AB_OFF];
        u8 b6_b = g->pe[sp->idx_b * g->pe_sz + GPT_ENT_AB_OFF];
        int act_a = (b6_a & AB_ATTR_ACTIVE) ? 1 : 0;
        int act_b = (b6_b & AB_ATTR_ACTIVE) ? 1 : 0;
        if (act_a == act_b) {
            out_s(2, "ERR: pair ");
            out_s(2, sp->base);
            out_s(2, " has invalid active state (both active or both inactive)\n");
            return 1;
        }
    }

    /* 1) Update all same-LUN A/B pairs (swap active/inactive TypeGUID + set QTI AB flags) */
    for (int i = 0; i < g_num_same_pairs; i++) {
        const struct same_lun_pair *sp = &g_same_pairs[i];
        struct lun_gpt *g = &g_luns[sp->lun_idx];
        u8 *pa  = g->pe  + sp->idx_a * g->pe_sz;
        u8 *pba = g->bpe + sp->idx_a * g->pe_sz;
        u8 *pb  = g->pe  + sp->idx_b * g->pe_sz;
        u8 *pbb = g->bpe + sp->idx_b * g->pe_sz;

        u8 active_guid[GPT_ENT_TYPE_LEN];
        u8 inactive_guid[GPT_ENT_TYPE_LEN];
        if (pa[GPT_ENT_AB_OFF] & AB_ATTR_ACTIVE) {
            m_copy(active_guid, pa, GPT_ENT_TYPE_LEN);
            m_copy(inactive_guid, pb, GPT_ENT_TYPE_LEN);
        } else {
            m_copy(active_guid, pb, GPT_ENT_TYPE_LEN);
            m_copy(inactive_guid, pa, GPT_ENT_TYPE_LEN);
        }

        if (target_slot == 0) {
            m_copy(pa,  active_guid, GPT_ENT_TYPE_LEN);
            m_copy(pba, active_guid, GPT_ENT_TYPE_LEN);
            pa[GPT_ENT_AB_OFF]  = AB_SLOT_ACTIVE_INIT_VAL;
            pba[GPT_ENT_AB_OFF] = AB_SLOT_ACTIVE_INIT_VAL;

            m_copy(pb,  inactive_guid, GPT_ENT_TYPE_LEN);
            m_copy(pbb, inactive_guid, GPT_ENT_TYPE_LEN);
            pb[GPT_ENT_AB_OFF]  = (u8)(pb[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
            pbb[GPT_ENT_AB_OFF] = (u8)(pbb[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
        } else {
            m_copy(pb,  active_guid, GPT_ENT_TYPE_LEN);
            m_copy(pbb, active_guid, GPT_ENT_TYPE_LEN);
            pb[GPT_ENT_AB_OFF]  = AB_SLOT_ACTIVE_INIT_VAL;
            pbb[GPT_ENT_AB_OFF] = AB_SLOT_ACTIVE_INIT_VAL;

            m_copy(pa,  inactive_guid, GPT_ENT_TYPE_LEN);
            m_copy(pba, inactive_guid, GPT_ENT_TYPE_LEN);
            pa[GPT_ENT_AB_OFF]  = (u8)(pa[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
            pba[GPT_ENT_AB_OFF] = (u8)(pba[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
        }
        g->dirty = 1;
    }

    /* 2) Update cross-LUN XBL A/B pairs (sdb <-> sdc: toggle AB_ATTR_ACTIVE bit 2) */
    for (int i = 0; i < g_num_cross_pairs; i++) {
        const struct cross_lun_pair *cp = &g_cross_pairs[i];
        struct lun_gpt *ga = &g_luns[cp->lun_a];
        struct lun_gpt *gb = &g_luns[cp->lun_b];
        u8 *pa  = ga->pe  + cp->idx_a * ga->pe_sz;
        u8 *pba = ga->bpe + cp->idx_a * ga->pe_sz;
        u8 *pb  = gb->pe  + cp->idx_b * gb->pe_sz;
        u8 *pbb = gb->bpe + cp->idx_b * gb->pe_sz;

        if (target_slot == 0) {
            pa[GPT_ENT_AB_OFF]  = (u8)(pa[GPT_ENT_AB_OFF] | AB_ATTR_ACTIVE);
            pba[GPT_ENT_AB_OFF] = (u8)(pba[GPT_ENT_AB_OFF] | AB_ATTR_ACTIVE);
            pb[GPT_ENT_AB_OFF]  = (u8)(pb[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
            pbb[GPT_ENT_AB_OFF] = (u8)(pbb[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
        } else {
            pb[GPT_ENT_AB_OFF]  = (u8)(pb[GPT_ENT_AB_OFF] | AB_ATTR_ACTIVE);
            pbb[GPT_ENT_AB_OFF] = (u8)(pbb[GPT_ENT_AB_OFF] | AB_ATTR_ACTIVE);
            pa[GPT_ENT_AB_OFF]  = (u8)(pa[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
            pba[GPT_ENT_AB_OFF] = (u8)(pba[GPT_ENT_AB_OFF] & ~AB_ATTR_ACTIVE);
        }
        ga->dirty = 1;
        gb->dirty = 1;
    }

    /* 3) Commit modified GPTs & verify CRCs */
    for (int li = 0; li < MAX_LUNS; li++) {
        if (commit_lun(&g_luns[li]) != 0) return 1;
    }

    /* 4) Switch UFS bBootLunEn (1 for _a, 2 for _b) & verify hardware readback */
    u8 want_lun = (u8)(target_slot == 0 ? 1 : 2);
    if (ufs_write_boot_lun(want_lun, mock_bsg_file) != 0) {
        return 1;
    }

    out_s(1, "OK: active slot switched to _");
    out_s(1, target_slot == 0 ? "a" : "b");
    out_s(1, " (bBootLunEn=");
    out_u64(1, want_lun);
    out_s(1, ", same_lun_pairs=");
    out_u64(1, (u64)g_num_same_pairs);
    out_s(1, ", cross_lun_pairs=");
    out_u64(1, (u64)g_num_cross_pairs);
    out_s(1, ")\n");
    return 0;
}

static int cmd_mark_successful(int target_slot, int only_if_slot, const char *mock_bsg_file) {
    u8 b6_a = 0, b6_b = 0;
    int active_slot = get_active_slot_from_gpt(&b6_a, &b6_b);
    if (active_slot < 0) return 1;

    if (only_if_slot >= 0 && active_slot != only_if_slot) {
        out_s(1, "SKIP: active slot is _");
        out_s(1, active_slot == 0 ? "a" : "b");
        out_s(1, " (only-if-slot=_");
        out_s(1, only_if_slot == 0 ? "a" : "b");
        out_s(1, "); 0 writes performed\n");
        return 0;
    }

    int slot = (target_slot >= 0) ? target_slot : active_slot;
    int boot_lun = ufs_read_boot_lun(mock_bsg_file);
    if (slot == active_slot && boot_lun > 0 && boot_lun != (slot == 0 ? 1 : 2)) {
        out_s(2, "ERR: active GPT slot does not match UFS bBootLunEn; refusing mark-successful\n");
        return 1;
    }

    u32 changed_entries = 0;
    for (int i = 0; i < g_num_same_pairs; i++) {
        const struct same_lun_pair *sp = &g_same_pairs[i];
        struct lun_gpt *g = &g_luns[sp->lun_idx];
        u32 idx = (slot == 0) ? sp->idx_a : sp->idx_b;
        u8 *p  = g->pe  + idx * g->pe_sz;
        u8 *pb = g->bpe + idx * g->pe_sz;

        u8 old_b6 = p[GPT_ENT_AB_OFF];
        /* Clear UNBOOTABLE, set BOOT_SUCCESSFUL, refresh tries_remaining=7 so ABL never expires */
        u8 new_b6 = (u8)((old_b6 & ~AB_ATTR_UNBOOTABLE) | AB_ATTR_SUCCESSFUL | AB_ATTR_TRIES_MASK);
        if ((old_b6 & AB_ATTR_PRIORITY_MASK) == 0 && (old_b6 & AB_ATTR_ACTIVE)) {
            new_b6 |= AB_ATTR_PRIORITY_MASK;
        }
        if (old_b6 != new_b6 || pb[GPT_ENT_AB_OFF] != new_b6) {
            p[GPT_ENT_AB_OFF] = new_b6;
            pb[GPT_ENT_AB_OFF] = new_b6;
            g->dirty = 1;
            changed_entries++;
        }
    }

    if (changed_entries == 0) {
        out_s(1, "OK: slot _");
        out_s(1, slot == 0 ? "a" : "b");
        out_s(1, " already marked boot-successful (0 writes needed)\n");
        return 0;
    }

    for (int li = 0; li < MAX_LUNS; li++) {
        if (commit_lun(&g_luns[li]) != 0) return 1;
    }

    out_s(1, "OK: marked slot _");
    out_s(1, slot == 0 ? "a" : "b");
    out_s(1, " boot-successful (updated_entries=");
    out_u64(1, changed_entries);
    out_s(1, ")\n");
    return 0;
}

static const char *autodetect_dev_dir(void) {
    int fd = nx_open("/dev/sda", O_RDONLY);
    if (fd >= 0) {
        nx_close(fd);
        return "/dev";
    }
    fd = nx_open("/dev/block/sda", O_RDONLY);
    if (fd >= 0) {
        nx_close(fd);
        return "/dev/block";
    }
    return "/dev";
}

static void print_usage(void) {
    out_s(2,
        "Usage:\n"
        "  nx769j-slotctl status [--json]\n"
        "  nx769j-slotctl set-active <a|b|0|1>\n"
        "  nx769j-slotctl mark-successful [a|b|0|1|active] [--only-if-slot=a|b]\n"
        "  nx769j-slotctl [-a|-c|-s <slot>|-m [slot]|-b <slot>|-n <slot>]  (qbootctl compat)\n"
        "Options:\n"
        "  --dev-dir <dir>         Override block device directory (default: /dev or /dev/block)\n"
        "  --mock-bsg-file <path>  Mock UFS bBootLunEn file for offline unit testing\n");
}

int slotctl_main(int argc, char **argv) {
    const char *dev_dir = NULL;
    const char *mock_bsg = NULL;
    int json_mode = 0;
    int only_if_slot = -1;
    const char *pos[8];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (s_eq(a, "--json")) {
            json_mode = 1;
        } else if (s_eq(a, "--dev-dir") && i + 1 < argc) {
            dev_dir = argv[++i];
        } else if (s_starts(a, "--dev-dir=")) {
            dev_dir = a + s_len("--dev-dir=");
        } else if (s_eq(a, "--mock-bsg-file") && i + 1 < argc) {
            mock_bsg = argv[++i];
        } else if (s_starts(a, "--mock-bsg-file=")) {
            mock_bsg = a + s_len("--mock-bsg-file=");
        } else if (s_starts(a, "--only-if-slot=")) {
            only_if_slot = parse_slot_arg(a + s_len("--only-if-slot="));
            if (only_if_slot < 0) {
                out_s(2, "ERR: invalid --only-if-slot value\n");
                return 1;
            }
        } else if (s_eq(a, "-h") || s_eq(a, "--help")) {
            print_usage();
            return 0;
        } else {
            if (npos < 8) pos[npos++] = a;
        }
    }

    if (!dev_dir) dev_dir = autodetect_dev_dir();

    if (discover_ab_pairs(dev_dir) != 0) {
        return 1;
    }

    if (npos == 0 || s_eq(pos[0], "status")) {
        return cmd_status(json_mode, mock_bsg);
    }

    if (s_eq(pos[0], "-a") || s_eq(pos[0], "-c")) {
        int act = get_active_slot_from_gpt(NULL, NULL);
        if (act < 0) return 1;
        out_s(1, act == 0 ? "_a\n" : "_b\n");
        return 0;
    }

    if (s_eq(pos[0], "-b") || s_eq(pos[0], "-n")) {
        if (npos < 2) { print_usage(); return 1; }
        int sl = parse_slot_arg(pos[1]);
        if (sl < 0) { print_usage(); return 1; }
        u8 b6_a = 0, b6_b = 0;
        if (get_active_slot_from_gpt(&b6_a, &b6_b) < 0) return 1;
        u8 b6 = (sl == 0) ? b6_a : b6_b;
        if (s_eq(pos[0], "-b")) {
            int bootable = (b6 & AB_ATTR_UNBOOTABLE) ? 0 : 1;
            out_s(1, bootable ? "bootable\n" : "unbootable\n");
            return bootable ? 0 : 1;
        } else {
            int succ = (b6 & AB_ATTR_SUCCESSFUL) ? 1 : 0;
            out_s(1, succ ? "successful\n" : "unsuccessful\n");
            return succ ? 0 : 1;
        }
    }

    if (s_eq(pos[0], "set-active") || s_eq(pos[0], "-s")) {
        if (npos < 2) {
            out_s(2, "ERR: set-active requires slot argument (a or b)\n");
            return 1;
        }
        int sl = parse_slot_arg(pos[1]);
        if (sl < 0) {
            out_s(2, "ERR: invalid slot argument\n");
            return 1;
        }
        return cmd_set_active(sl, mock_bsg);
    }

    if (s_eq(pos[0], "mark-successful") || s_eq(pos[0], "-m")) {
        int sl = -1;
        if (npos >= 2 && !s_eq(pos[1], "active")) {
            sl = parse_slot_arg(pos[1]);
            if (sl < 0) {
                out_s(2, "ERR: invalid slot argument for mark-successful\n");
                return 1;
            }
        }
        return cmd_mark_successful(sl, only_if_slot, mock_bsg);
    }

    print_usage();
    return 1;
}

#ifdef NX_NOSTDLIB_ARM64
__asm__(
".text\n"
".globl _start\n"
"_start:\n"
"    ldr x0, [sp]\n"
"    add x1, sp, #8\n"
"    mov x29, #0\n"
"    bl  slotctl_main\n"
"    mov x8, #93\n"
"    svc #0\n"
);
#else
int main(int argc, char **argv) {
    return slotctl_main(argc, argv);
}
#endif
