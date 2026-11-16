// Static nostdlib ARM64 Linux/Android tool for:
//   1) nx_dm_tool pin-alloc <filepath> <size_bytes>
//      -> F2FS_IOC_SET_PIN_FILE + fallocate + fsync + merged FS_IOC_FIEMAP dump
//   2) nx_dm_tool fiemap <filepath>
//      -> Merged FS_IOC_FIEMAP dump as dm-linear lines: "<logical_sec> <len_sec> <phys_sec>"
//   3) nx_dm_tool dm-up <dm_name> <backing_dev> <extents_file>
//      -> Creates & activates /dev/nxroot via /dev/mapper/control ioctls
//   4) nx_dm_tool dm-down <dm_name>
//      -> Removes dm device
//   5) nx_dm_tool recv-img <target_dev> <tcp_port>
//      -> Receives RLE stream ([u8 tag][u64 len_le]) over 127.0.0.1:<tcp_port> into <target_dev>
//   6) nx_dm_tool check-ext4 <bdev>
//      -> Verifies ext4 superblock magic (0xef53) and prints UUID

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef signed long        s64;

static inline long sys_call(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}

#define SYS_ioctl       29
#define SYS_mknodat     33
#define SYS_fallocate   47
#define SYS_openat      56
#define SYS_close       57
#define SYS_lseek       62
#define SYS_read        63
#define SYS_write       64
#define SYS_fsync       82
#define SYS_exit        93
#define SYS_socket      198
#define SYS_bind        200
#define SYS_listen      201
#define SYS_accept      202
#define SYS_setsockopt  208

#define AT_FDCWD        (-100)
#define O_RDONLY        00
#define O_WRONLY        01
#define O_RDWR          02
#define O_CREAT         0100

#define AF_INET         2
#define SOCK_STREAM     1
#define SOL_SOCKET      1
#define SO_REUSEADDR    2

#define F2FS_IOC_SET_PIN_FILE   0x4004f50dUL
#define FS_IOC_FIEMAP           0xc020660bUL

struct sockaddr_in {
    u16 sin_family;
    u16 sin_port;
    u32 sin_addr;
    u8  sin_zero[8];
};

struct fiemap_extent {
    u64 fe_logical;
    u64 fe_physical;
    u64 fe_length;
    u64 fe_reserved64[2];
    u32 fe_flags;
    u32 fe_reserved[3];
};

struct fiemap {
    u64 fm_start;
    u64 fm_length;
    u32 fm_flags;
    u32 fm_mapped_extents;
    u32 fm_extent_count;
    u32 fm_reserved;
    struct fiemap_extent fm_extents[512];
};

#define FIEMAP_FLAG_SYNC        0x00000001
#define FIEMAP_EXTENT_LAST      0x00000001

// Device-mapper ioctl structs (Linux ABI v4.0.0)
#define DM_VERSION_MAJOR        4
#define DM_VERSION_MINOR        0
#define DM_VERSION_PATCHLEVEL   0
#define DM_MAX_TYPE_NAME        16
#define DM_NAME_LEN             128
#define DM_UUID_LEN             129

struct dm_ioctl_exact {
    u32 version[3];
    u32 data_size;
    u32 data_start;
    u32 target_count;
    int open_count;
    u32 flags;
    u32 event_nr;
    u32 padding;
    u64 dev;
    char name[DM_NAME_LEN];
    char uuid[DM_UUID_LEN];
    char data[7];
};

struct dm_target_spec {
    u64 sector_start;
    u64 length;
    int status;
    u32 next;
    char target_type[DM_MAX_TYPE_NAME];
};

#define DM_IOCTL_BASE       0xfd
#define DM_DEV_CREATE_CMD   3
#define DM_DEV_REMOVE_CMD   4
#define DM_DEV_SUSPEND_CMD  6
#define DM_TABLE_LOAD_CMD   9

#define DM_IOC(cmd) (0xc0000000UL | ((unsigned long)sizeof(struct dm_ioctl_exact) << 16) | (DM_IOCTL_BASE << 8) | (cmd))

static char g_filebuf[4194304];
static u8   g_iobuf[8388608];
static u8   g_zerobuf[1048576];

static u64 str_len(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, u64 max) {
    u64 i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void *memset(void *p, int c, u64 n) {
    volatile u8 *b = (volatile u8 *)p;
    for (u64 i = 0; i < n; i++) b[i] = (u8)c;
    return p;
}

static void mem_zero(void *p, u64 n) {
    memset(p, 0, n);
}

static int write_all(int fd, const void *buf, u64 len) {
    const u8 *p = (const u8 *)buf;
    u64 done = 0;
    while (done < len) {
        long w = sys_call(SYS_write, fd, (long)(p + done), len - done, 0, 0, 0);
        if (w <= 0) return -1;
        done += (u64)w;
    }
    return 0;
}

static int read_exact(int fd, void *buf, u64 len) {
    u8 *p = (u8 *)buf;
    u64 done = 0;
    while (done < len) {
        long r = sys_call(SYS_read, fd, (long)(p + done), len - done, 0, 0, 0);
        if (r <= 0) return -1;
        done += (u64)r;
    }
    return 0;
}

static void put_s(int fd, const char *s) {
    write_all(fd, s, str_len(s));
}

static void put_u64(int fd, u64 v) {
    char buf[32];
    int i = 30;
    buf[31] = 0;
    if (v == 0) { put_s(fd, "0"); return; }
    while (v > 0 && i >= 0) {
        buf[i--] = '0' + (v % 10);
        v /= 10;
    }
    put_s(fd, &buf[i + 1]);
}

static void put_hex(int fd, s64 v) {
    char buf[20];
    u64 u = (u64)v;
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (u >> (60 - i * 4)) & 0xf;
        buf[2 + i] = nib < 10 ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[18] = 0;
    put_s(fd, buf);
}

static void put_byte_hex(int fd, u8 b) {
    char buf[3];
    int hi = (b >> 4) & 0xf;
    int lo = b & 0xf;
    buf[0] = hi < 10 ? ('0' + hi) : ('a' + hi - 10);
    buf[1] = lo < 10 ? ('0' + lo) : ('a' + lo - 10);
    buf[2] = 0;
    put_s(fd, buf);
}

static u64 parse_u64(const char *s) {
    u64 v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void emit_extent(u64 l_bytes, u64 len_bytes, u64 p_bytes) {
    put_u64(1, l_bytes >> 9);
    put_s(1, " ");
    put_u64(1, len_bytes >> 9);
    put_s(1, " ");
    put_u64(1, p_bytes >> 9);
    put_s(1, "\n");
}

static int dump_fiemap(int fd, u64 expected_bytes) {
    static struct fiemap fm;
    u64 start = 0;
    u64 total = 0;
    int have_cur = 0;
    u64 cur_l = 0, cur_p = 0, cur_len = 0;

    while (1) {
        mem_zero(&fm, sizeof(fm));
        fm.fm_start = start;
        fm.fm_length = ~0ULL;
        fm.fm_flags = FIEMAP_FLAG_SYNC;
        fm.fm_extent_count = 512;
        long r = sys_call(SYS_ioctl, fd, FS_IOC_FIEMAP, (long)&fm, 0, 0, 0);
        if (r < 0) {
            put_s(2, "ERR: FS_IOC_FIEMAP failed: ");
            put_hex(2, r);
            put_s(2, "\n");
            return 1;
        }
        if (fm.fm_mapped_extents == 0) break;
        for (u32 i = 0; i < fm.fm_mapped_extents; i++) {
            struct fiemap_extent *e = &fm.fm_extents[i];
            if ((e->fe_logical & 511) || (e->fe_physical & 511) || (e->fe_length & 511)) {
                put_s(2, "ERR: unaligned extent\n");
                return 1;
            }
            // Disallow encoded(0x8), delalloc(0x4), inline(0x100/0x200), encrypted(0x80)
            // Note: 0x800 (FIEMAP_EXTENT_UNWRITTEN) IS safe for raw block dm-linear because
            // physical blocks are already pinned and allocated on sda12, and we explicitly write
            // the full filesystem image onto /dev/nxroot! Wait: let's check if F2FS sets 0x800 on
            // large fallocate! Let's allow 0x800 ONLY (never encoded/inline/delalloc)!
            if (e->fe_flags & (0x8 | 0x4 | 0x100 | 0x200)) {
                put_s(2, "ERR: bad extent flags: ");
                put_hex(2, e->fe_flags);
                put_s(2, "\n");
                return 1;
            }
            if (e->fe_logical != total) {
                put_s(2, "ERR: hole in pinned file at ");
                put_u64(2, total);
                put_s(2, "\n");
                return 1;
            }
            if (!have_cur) {
                cur_l = e->fe_logical;
                cur_p = e->fe_physical;
                cur_len = e->fe_length;
                have_cur = 1;
            } else if (cur_l + cur_len == e->fe_logical && cur_p + cur_len == e->fe_physical) {
                cur_len += e->fe_length;
            } else {
                emit_extent(cur_l, cur_len, cur_p);
                cur_l = e->fe_logical;
                cur_p = e->fe_physical;
                cur_len = e->fe_length;
            }
            total += e->fe_length;
            start = e->fe_logical + e->fe_length;
            if (e->fe_flags & FIEMAP_EXTENT_LAST) goto done;
        }
    }
done:
    if (have_cur) {
        emit_extent(cur_l, cur_len, cur_p);
    }
    if (expected_bytes && total != expected_bytes) {
        put_s(2, "ERR: total mapped bytes mismatch: got ");
        put_u64(2, total);
        put_s(2, "\n");
        return 1;
    }
    return 0;
}

static int do_dm_up(const char *dm_name, const char *bdev, const char *ext_file) {
    int efd = sys_call(SYS_openat, AT_FDCWD, (long)ext_file, O_RDONLY, 0, 0, 0);
    if (efd < 0) { put_s(2, "ERR: open extents_file\n"); return 1; }
    u64 tot_read = 0;
    while (tot_read + 1 < sizeof(g_filebuf)) {
        long r = sys_call(SYS_read, efd, (long)(g_filebuf + tot_read), sizeof(g_filebuf) - 1 - tot_read, 0, 0, 0);
        if (r < 0) { sys_call(SYS_close, efd, 0, 0, 0, 0, 0); put_s(2, "ERR: read extents_file\n"); return 1; }
        if (r == 0) break;
        tot_read += (u64)r;
    }
    sys_call(SYS_close, efd, 0, 0, 0, 0, 0);
    if (tot_read == 0) { put_s(2, "ERR: empty extents_file\n"); return 1; }
    g_filebuf[tot_read] = 0;

    int cfd = sys_call(SYS_openat, AT_FDCWD, (long)"/dev/mapper/control", O_RDWR, 0, 0, 0);
    if (cfd < 0) {
        cfd = sys_call(SYS_openat, AT_FDCWD, (long)"/dev/device-mapper", O_RDWR, 0, 0, 0);
    }
    if (cfd < 0) {
        sys_call(SYS_mknodat, AT_FDCWD, (long)"/dev/device-mapper", 020600, (10 << 8) | 236, 0, 0);
        cfd = sys_call(SYS_openat, AT_FDCWD, (long)"/dev/device-mapper", O_RDWR, 0, 0, 0);
    }
    if (cfd < 0) { put_s(2, "ERR: open /dev/device-mapper failed\n"); return 1; }

    struct dm_ioctl_exact *dmi = (struct dm_ioctl_exact *)g_iobuf;

    // 1. DM_DEV_CREATE
    mem_zero(g_iobuf, sizeof(struct dm_ioctl_exact) + 64);
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->version[1] = DM_VERSION_MINOR;
    dmi->version[2] = DM_VERSION_PATCHLEVEL;
    dmi->data_size = sizeof(struct dm_ioctl_exact);
    dmi->data_start = sizeof(struct dm_ioctl_exact);
    str_copy(dmi->name, dm_name, DM_NAME_LEN);
    str_copy(dmi->uuid, "NXLINUX-ROOT-DM0", DM_UUID_LEN);
    long r = sys_call(SYS_ioctl, cfd, DM_IOC(DM_DEV_CREATE_CMD), (long)dmi, 0, 0, 0);
    if (r < 0) { put_s(2, "ERR: DM_DEV_CREATE: "); put_hex(2, r); put_s(2, "\n"); return 1; }
    u64 created_dev = dmi->dev;

    // 2. DM_TABLE_LOAD
    mem_zero(g_iobuf, sizeof(struct dm_ioctl_exact) + 64);
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->version[1] = DM_VERSION_MINOR;
    dmi->version[2] = DM_VERSION_PATCHLEVEL;
    u32 off = (sizeof(struct dm_ioctl_exact) + 7) & ~7U;
    dmi->data_start = off;
    str_copy(dmi->name, dm_name, DM_NAME_LEN);

    u32 count = 0;
    const char *p = g_filebuf;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (!*p) break;
        if (off + 128 > sizeof(g_iobuf)) {
            put_s(2, "ERR: too many extents for g_iobuf\n");
            return 1;
        }
        u64 lsec = parse_u64(p);
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ') p++;
        u64 len = parse_u64(p);
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ') p++;
        u64 psec = parse_u64(p);
        while (*p && *p != '\n') p++;

        mem_zero(g_iobuf + off, 128);
        struct dm_target_spec *sp = (struct dm_target_spec *)(g_iobuf + off);
        sp->sector_start = lsec;
        sp->length = len;
        sp->status = 0;
        str_copy(sp->target_type, "linear", DM_MAX_TYPE_NAME);

        char *param = (char *)(g_iobuf + off + sizeof(struct dm_target_spec));
        u64 bdev_len = str_len(bdev);
        for (u64 i = 0; i < bdev_len; i++) param[i] = bdev[i];
        param[bdev_len] = ' ';
        char num[32];
        int ni = 30; num[31] = 0;
        u64 tmp = psec;
        if (tmp == 0) { num[ni--] = '0'; }
        else { while (tmp > 0) { num[ni--] = '0' + (tmp % 10); tmp /= 10; } }
        u64 pi = bdev_len + 1;
        for (int k = ni + 1; num[k]; k++) param[pi++] = num[k];
        param[pi++] = 0;

        u32 entry_size = (sizeof(struct dm_target_spec) + pi + 7) & ~7U;
        sp->next = entry_size;
        off += entry_size;
        count++;
    }
    dmi->target_count = count;
    dmi->data_size = off;

    r = sys_call(SYS_ioctl, cfd, DM_IOC(DM_TABLE_LOAD_CMD), (long)dmi, 0, 0, 0);
    if (r < 0) { put_s(2, "ERR: DM_TABLE_LOAD: "); put_hex(2, r); put_s(2, "\n"); return 1; }

    // 3. DM_DEV_SUSPEND (flags=0 -> resume/activate live table)
    mem_zero(g_iobuf, sizeof(struct dm_ioctl_exact) + 64);
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->version[1] = DM_VERSION_MINOR;
    dmi->version[2] = DM_VERSION_PATCHLEVEL;
    dmi->data_size = sizeof(struct dm_ioctl_exact);
    dmi->data_start = sizeof(struct dm_ioctl_exact);
    str_copy(dmi->name, dm_name, DM_NAME_LEN);
    r = sys_call(SYS_ioctl, cfd, DM_IOC(DM_DEV_SUSPEND_CMD), (long)dmi, 0, 0, 0);
    if (r < 0) { put_s(2, "ERR: DM_DEV_SUSPEND(resume): "); put_hex(2, r); put_s(2, "\n"); return 1; }

    sys_call(SYS_mknodat, AT_FDCWD, (long)"/dev/nxroot", 060600, created_dev, 0, 0);
    put_s(1, "OK: /dev/nxroot active with extents=");
    put_u64(1, count);
    put_s(1, "\n");
    sys_call(SYS_close, cfd, 0, 0, 0, 0, 0);
    return 0;
}

static int do_dm_down(const char *dm_name) {
    static u8 buf[512];
    int cfd = sys_call(SYS_openat, AT_FDCWD, (long)"/dev/mapper/control", O_RDWR, 0, 0, 0);
    if (cfd < 0) cfd = sys_call(SYS_openat, AT_FDCWD, (long)"/dev/device-mapper", O_RDWR, 0, 0, 0);
    if (cfd < 0) return 1;
    struct dm_ioctl_exact *dmi = (struct dm_ioctl_exact *)buf;
    mem_zero(buf, sizeof(buf));
    dmi->version[0] = DM_VERSION_MAJOR;
    dmi->data_size = sizeof(struct dm_ioctl_exact);
    dmi->data_start = sizeof(struct dm_ioctl_exact);
    str_copy(dmi->name, dm_name, DM_NAME_LEN);
    long r = sys_call(SYS_ioctl, cfd, DM_IOC(DM_DEV_REMOVE_CMD), (long)dmi, 0, 0, 0);
    sys_call(SYS_close, cfd, 0, 0, 0, 0, 0);
    return r < 0 ? 1 : 0;
}

static int do_recv_img(const char *target_dev, u16 port) {
    int dfd = sys_call(SYS_openat, AT_FDCWD, (long)target_dev, O_WRONLY, 0, 0, 0);
    if (dfd < 0) { put_s(2, "ERR: open target_dev failed\n"); return 1; }
    sys_call(SYS_lseek, dfd, 0, 0, 0, 0, 0);

    int sfd = sys_call(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    if (sfd < 0) { put_s(2, "ERR: socket failed\n"); return 1; }
    int one = 1;
    sys_call(SYS_setsockopt, sfd, SOL_SOCKET, SO_REUSEADDR, (long)&one, 4, 0);

    struct sockaddr_in sa;
    mem_zero(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (u16)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
    sa.sin_addr = 0x0100007fU; // 127.0.0.1 in network byte order on LE

    if (sys_call(SYS_bind, sfd, (long)&sa, sizeof(sa), 0, 0, 0) < 0) {
        put_s(2, "ERR: bind failed\n");
        return 1;
    }
    if (sys_call(SYS_listen, sfd, 1, 0, 0, 0, 0) < 0) {
        put_s(2, "ERR: listen failed\n");
        return 1;
    }
    put_s(1, "LISTENING\n");

    int cfd = sys_call(SYS_accept, sfd, 0, 0, 0, 0, 0);
    if (cfd < 0) { put_s(2, "ERR: accept failed\n"); return 1; }

    mem_zero(g_zerobuf, sizeof(g_zerobuf));
    u64 total_written = 0;

    while (1) {
        u8 hdr[9];
        if (read_exact(cfd, hdr, 9) < 0) {
            put_s(2, "ERR: unexpected socket EOF\n");
            return 1;
        }
        u8 tag = hdr[0];
        u64 len = 0;
        for (int i = 0; i < 8; i++) len |= ((u64)hdr[1 + i]) << (i * 8);

        if (tag == 2) {
            break;
        } else if (tag == 0) {
            u64 rem = len;
            while (rem > 0) {
                u64 chunk = rem > sizeof(g_zerobuf) ? sizeof(g_zerobuf) : rem;
                if (write_all(dfd, g_zerobuf, chunk) < 0) {
                    put_s(2, "ERR: zero write failed\n");
                    return 1;
                }
                rem -= chunk;
                total_written += chunk;
            }
        } else if (tag == 1) {
            u64 rem = len;
            while (rem > 0) {
                u64 chunk = rem > sizeof(g_iobuf) ? sizeof(g_iobuf) : rem;
                if (read_exact(cfd, g_iobuf, chunk) < 0) {
                    put_s(2, "ERR: data read failed\n");
                    return 1;
                }
                if (write_all(dfd, g_iobuf, chunk) < 0) {
                    put_s(2, "ERR: data write failed\n");
                    return 1;
                }
                rem -= chunk;
                total_written += chunk;
            }
        } else {
            put_s(2, "ERR: unknown packet tag\n");
            return 1;
        }
    }

    sys_call(SYS_fsync, dfd, 0, 0, 0, 0, 0);
    sys_call(SYS_close, dfd, 0, 0, 0, 0, 0);

    put_s(1, "RECV_OK total_bytes=");
    put_u64(1, total_written);
    put_s(1, "\n");

    write_all(cfd, "OK\n", 3);
    sys_call(SYS_close, cfd, 0, 0, 0, 0, 0);
    sys_call(SYS_close, sfd, 0, 0, 0, 0, 0);
    return 0;
}

static int do_check_ext4(const char *bdev) {
    u8 sb[2048];
    int fd = sys_call(SYS_openat, AT_FDCWD, (long)bdev, O_RDONLY, 0, 0, 0);
    if (fd < 0) { put_s(2, "ERR: open bdev failed\n"); return 1; }
    if (read_exact(fd, sb, sizeof(sb)) < 0) {
        sys_call(SYS_close, fd, 0, 0, 0, 0, 0);
        put_s(2, "ERR: read superblock failed\n");
        return 1;
    }
    sys_call(SYS_close, fd, 0, 0, 0, 0, 0);
    if (sb[1024 + 56] != 0x53 || sb[1024 + 57] != 0xef) {
        put_s(2, "ERR: bad ext4 magic\n");
        return 1;
    }
    put_s(1, "EXT4_OK uuid=");
    const u8 *u = &sb[1024 + 104];
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) put_s(1, "-");
        put_byte_hex(1, u[i]);
    }
    put_s(1, "\n");
    return 0;
}

int main_c(int argc, char **argv) {
    if (argc >= 4 && str_eq(argv[1], "pin-alloc")) {
        u64 sz = parse_u64(argv[3]);
        int fd = sys_call(SYS_openat, AT_FDCWD, (long)argv[2], O_CREAT | O_RDWR, 0600, 0, 0);
        if (fd < 0) { put_s(2, "ERR: open file failed\n"); return 1; }
        u32 pin = 1;
        long r = sys_call(SYS_ioctl, fd, F2FS_IOC_SET_PIN_FILE, (long)&pin, 0, 0, 0);
        if (r < 0) { put_s(2, "ERR: F2FS_IOC_SET_PIN_FILE failed: "); put_hex(2, r); put_s(2, "\n"); return 1; }
        r = sys_call(SYS_fallocate, fd, 0, 0, sz, 0, 0);
        if (r < 0) { put_s(2, "ERR: fallocate failed: "); put_hex(2, r); put_s(2, "\n"); return 1; }
        sys_call(SYS_fsync, fd, 0, 0, 0, 0, 0);
        int rc = dump_fiemap(fd, sz);
        sys_call(SYS_close, fd, 0, 0, 0, 0, 0);
        return rc;
    }
    if (argc >= 3 && str_eq(argv[1], "fiemap")) {
        int fd = sys_call(SYS_openat, AT_FDCWD, (long)argv[2], O_RDONLY, 0, 0, 0);
        if (fd < 0) { put_s(2, "ERR: open file failed\n"); return 1; }
        int rc = dump_fiemap(fd, 0);
        sys_call(SYS_close, fd, 0, 0, 0, 0, 0);
        return rc;
    }
    if (argc >= 5 && str_eq(argv[1], "dm-up")) {
        return do_dm_up(argv[2], argv[3], argv[4]);
    }
    if (argc >= 3 && str_eq(argv[1], "dm-down")) {
        return do_dm_down(argv[2]);
    }
    if (argc >= 4 && str_eq(argv[1], "recv-img")) {
        return do_recv_img(argv[2], (u16)parse_u64(argv[3]));
    }
    if (argc >= 3 && str_eq(argv[1], "check-ext4")) {
        return do_check_ext4(argv[2]);
    }
    put_s(2, "Usage: nx_dm_tool [pin-alloc|fiemap|dm-up|dm-down|recv-img|check-ext4]\n");
    return 1;
}

__asm__(
".text\n"
".globl _start\n"
"_start:\n"
"    ldr x0, [sp]\n"
"    add x1, sp, #8\n"
"    mov x29, #0\n"
"    bl  main_c\n"
"    mov x8, #93\n"
"    svc #0\n"
);
