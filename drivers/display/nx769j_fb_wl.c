// nx769j_fb_wl.c - Zero-dependency Wayland-to-LinuxFB (/dev/fb0) nested host compositor
// Engineered for Omarchy Mobile (Hyprland 0.57 + Aquamarine 0.16 + Quickshell) & KDE KWin
// on Nubia RedMagic 9 Pro (NX769J, SM8650, Adreno 750 /dev/dri/renderD128, 1116x2480 AMOLED).
//
// Capabilities:
//   1. Hardware GPU Zero-Shader-Overhead Scanout Bridge (zwp_linux_dmabuf_v1 v4 + wl_shm v1):
//      - Advertises DRM_FORMAT_XRGB8888 & DRM_FORMAT_ARGB8888 with DRM_FORMAT_MOD_LINEAR (0x0)
//        and main_device = st_rdev(/dev/dri/renderD128) via v4 feedback tranches + format_table.
//      - Imports Freedreno FD750 linear GBM DMA-BUFs (e.g. stride 4608 -> fb0 stride 4464) via
//        SCM_RIGHTS + create_immed, wraps CPU reads in DMA_BUF_IOCTL_SYNC(START|READ..END|READ),
//        and blits/rotates into cached shadow RAM + /dev/fb0. Keeps wl_shm 100% compatible.
//   2. True 10-Finger Native Multi-Touch (wl_touch v9) + Single-Finger wl_pointer Fallback:
//      - Advertises wl_seat v9 (POINTER | KEYBOARD | TOUCH = 0x7) and xdg_wm_base v6.
//      - Emits simultaneous 10-slot (0..9) wl_touch.{down,up,motion,frame,cancel} transformed
//        across all 4 IMU orientations when client binds wl_touch; falls back to wl_pointer
//        only when wl_touch is unbound. Cleanly emits wl_touch.cancel on sleep & rotation.
//   3. Native Evdev/Uinput Keyboard & Hardware Button Forwarding (wl_keyboard v9):
//      - Sends self-contained XKB v1 keymap memfd + repeat_info(25, 400) + enter/key/modifiers.
//      - Hot-scans /dev/input/event* for volume keys (pmic_resin / gpio-keys), USB/BT keyboards,
//        and uinput OSKs (omarchy-osk / ydotool), while strictly excluding pmic_pwrkey (owned by
//        nx769j-powerkey), Goodix touchscreen, Headset Jack, and LSM6DSO IMU.
//   4. AMOLED 0x000000 Standby, Software Brightness (1..100%), & 4-Way IMU Auto-Rotation:
//      - Controlled over /run/nx769j-wl/power.sock: OFF, ON, TOGGLE, STATUS,
//        ROTATE auto|normal|right-up|left-up|bottom-up, BRIGHTNESS <1..100>.

#define _GNU_SOURCE
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
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/dma-buf.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/sysmacros.h>
#else
// Portable definitions for macOS host compilation & full socket integration tests
#define ABS_MAX             0x3f
#define KEY_MAX             0x2ff
#define EV_SYN              0x00
#define EV_KEY              0x01
#define EV_ABS              0x03
#define SYN_REPORT          0
#define ABS_MT_SLOT         0x2f
#define ABS_MT_POSITION_X   0x35
#define ABS_MT_POSITION_Y   0x36
#define ABS_MT_TRACKING_ID  0x39
#define KEY_ESC             1
#define KEY_ENTER           28
#define KEY_LEFTSHIFT       42
#define KEY_RIGHTSHIFT      54
#define KEY_LEFTCTRL        29
#define KEY_RIGHTCTRL       97
#define KEY_LEFTALT         56
#define KEY_RIGHTALT        100
#define KEY_CAPSLOCK        58
#define KEY_LEFTMETA        125
#define KEY_RIGHTMETA       126
#define KEY_A               30
#define KEY_MUTE            113
#define KEY_VOLUMEDOWN      114
#define KEY_VOLUMEUP        115
#define KEY_POWER           116
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
struct input_absinfo {
    int32_t value, minimum, maximum, fuzz, flat, resolution;
};
#ifndef makedev
#define makedev(maj, min)   ((dev_t)(((uint32_t)(maj) << 24) | (uint32_t)(min)))
#endif
#endif

#ifdef __linux__
#ifndef GPIO_V2_GET_LINE_IOCTL
#define GPIO_MAX_NAME_SIZE 32
#define GPIO_V2_LINES_MAX 64
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10
#define GPIO_V2_LINE_FLAG_USED          (1ULL << 0)
#define GPIO_V2_LINE_FLAG_ACTIVE_LOW    (1ULL << 1)
#define GPIO_V2_LINE_FLAG_INPUT         (1ULL << 2)
#define GPIO_V2_LINE_FLAG_EDGE_RISING   (1ULL << 5)
#define GPIO_V2_LINE_FLAG_EDGE_FALLING  (1ULL << 6)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_UP  (1ULL << 8)
struct gpio_v2_line_attribute {
    uint32_t id;
    uint32_t padding;
    union {
        uint64_t flags;
        uint64_t values;
        uint32_t debounce_period_us;
    };
};
struct gpio_v2_line_config_attribute {
    struct gpio_v2_line_attribute attr;
    uint64_t mask;
};
struct gpio_v2_line_config {
    uint64_t flags;
    uint32_t num_attrs;
    uint32_t padding[5];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
};
struct gpio_v2_line_request {
    uint32_t offsets[GPIO_V2_LINES_MAX];
    char consumer[GPIO_MAX_NAME_SIZE];
    struct gpio_v2_line_config config;
    uint32_t num_lines;
    uint32_t event_buffer_size;
    uint32_t padding[5];
    int32_t fd;
};
struct gpio_v2_line_info {
    char name[GPIO_MAX_NAME_SIZE];
    char consumer[GPIO_MAX_NAME_SIZE];
    uint32_t offset;
    uint32_t num_attrs;
    uint64_t flags;
    struct gpio_v2_line_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
    uint32_t padding[4];
};
struct gpio_v2_line_values {
    uint64_t bits;
    uint64_t mask;
};
struct gpio_v2_line_event {
    uint64_t timestamp_ns;
    uint32_t id;
    uint32_t offset;
    uint32_t seqno;
    uint32_t line_seqno;
    uint32_t padding[6];
};
#define GPIO_V2_GET_LINEINFO_IOCTL      _IOWR(0xB4, 0x05, struct gpio_v2_line_info)
#define GPIO_V2_GET_LINE_IOCTL          _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#define GPIO_V2_LINE_GET_VALUES_IOCTL   _IOWR(0xB4, 0x0E, struct gpio_v2_line_values)
#endif
#ifndef GPIO_V2_LINE_FLAG_EDGE_BOTH
#define GPIO_V2_LINE_FLAG_EDGE_BOTH (GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING)
#endif
#endif

#ifndef DMA_BUF_IOCTL_SYNC
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ   (1ULL << 0)
#define DMA_BUF_SYNC_START  (0ULL << 2)
#define DMA_BUF_SYNC_END    (1ULL << 2)
#define DMA_BUF_BASE        'b'
#define DMA_BUF_IOCTL_SYNC  _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DRM_FMT_XRGB8888    0x34325258u
#define DRM_FMT_ARGB8888    0x34325241u
#define DRM_FMT_XBGR8888    0x34324258u
#define DRM_FMT_ABGR8888    0x34324241u
#define DRM_MOD_LINEAR      0x0ULL

#define MAX_OBJS            65536
#define MAX_FDS             64
#define MAX_CBS             64
#define MAX_SLOTS           10
#define MAX_KBDS            8
#define DEFAULT_TOUCH_SLOP_PX 18
#define IMU_STATE_PATH      "/run/nx769j-imu/state.json"
#define DEFAULT_DRM_NODE    "/dev/dri/renderD128"
#define IMU_POLL_MS         150U
#define HW_VOL_POLL_MS      25U
#define ROT_DEBOUNCE_MS     400U
#define CTRL_RECV_WAIT_MS   100
#define DEFAULT_RESIN_REGMAP_PATH "/sys/kernel/debug/regmap/0-00/registers"

// Self-contained XKB v1 keymap (evdev keycodes = linux keycode + 8) covering full US QWERTY,
// modifiers, navigation, function keys, and XF86 volume/brightness/power keys.
static const char g_xkb_keymap_str[] =
    "xkb_keymap {\n"
    "xkb_keycodes \"evdev\" {\n"
    "  minimum = 8;\n"
    "  maximum = 255;\n"
    "  <ESC> = 9; <AE01> = 10; <AE02> = 11; <AE03> = 12; <AE04> = 13;\n"
    "  <AE05> = 14; <AE06> = 15; <AE07> = 16; <AE08> = 17; <AE09> = 18;\n"
    "  <AE10> = 19; <AE11> = 20; <AE12> = 21; <BKSP> = 22; <TAB> = 23;\n"
    "  <AD01> = 24; <AD02> = 25; <AD03> = 26; <AD04> = 27; <AD05> = 28;\n"
    "  <AD06> = 29; <AD07> = 30; <AD08> = 31; <AD09> = 32; <AD10> = 33;\n"
    "  <AD11> = 34; <AD12> = 35; <RTRN> = 36; <LCTL> = 37; <AC01> = 38;\n"
    "  <AC02> = 39; <AC03> = 40; <AC04> = 41; <AC05> = 42; <AC06> = 43;\n"
    "  <AC07> = 44; <AC08> = 45; <AC09> = 46; <AC10> = 47; <AC11> = 48;\n"
    "  <TLDE> = 49; <LFSH> = 50; <BKSL> = 51; <AB01> = 52; <AB02> = 53;\n"
    "  <AB03> = 54; <AB04> = 55; <AB05> = 56; <AB06> = 57; <AB07> = 58;\n"
    "  <AB08> = 59; <AB09> = 60; <AB10> = 61; <RTSH> = 62; <SPCE> = 65;\n"
    "  <CAPS> = 66; <FK01> = 67; <FK02> = 68; <FK03> = 69; <FK04> = 70;\n"
    "  <FK05> = 71; <FK06> = 72; <FK07> = 73; <FK08> = 74; <FK09> = 75;\n"
    "  <FK10> = 76; <FK11> = 95; <FK12> = 96; <LALT> = 64; <RCTL> = 105;\n"
    "  <RALT> = 108; <HOME> = 110; <UP> = 111; <PGUP> = 112; <LEFT> = 113;\n"
    "  <RGHT> = 114; <END> = 115; <DOWN> = 116; <PGDN> = 117; <INS> = 118;\n"
    "  <DELE> = 119; <MUTE> = 121; <VOL-> = 122; <VOL+> = 123; <POWR> = 124;\n"
    "  <LWIN> = 133; <RWIN> = 134; <MON-> = 232; <MON+> = 233;\n"
    "};\n"
    "xkb_types \"minimal\" {\n"
    "  virtual_modifiers Alt,Super;\n"
    "  type \"ONE_LEVEL\" { modifiers= none; level_name[Level1]= \"Any\"; };\n"
    "  type \"TWO_LEVEL\" { modifiers= Shift; map[Shift]= Level2; level_name[Level1]= \"Base\"; level_name[Level2]= \"Shift\"; };\n"
    "  type \"ALPHABETIC\" { modifiers= Shift+Lock; map[Shift]= Level2; map[Lock]= Level2; level_name[Level1]= \"Base\"; level_name[Level2]= \"Caps\"; };\n"
    "};\n"
    "xkb_compatibility \"minimal\" {\n"
    "  virtual_modifiers Alt,Super;\n"
    "  interpret Shift_L { action= SetMods(modifiers=Shift); };\n"
    "  interpret Shift_R { action= SetMods(modifiers=Shift); };\n"
    "  interpret Control_L { action= SetMods(modifiers=Control); };\n"
    "  interpret Control_R { action= SetMods(modifiers=Control); };\n"
    "  interpret Alt_L { action= SetMods(modifiers=Mod1); };\n"
    "  interpret Alt_R { action= SetMods(modifiers=Mod1); };\n"
    "  interpret Super_L { action= SetMods(modifiers=Mod4); };\n"
    "  interpret Super_R { action= SetMods(modifiers=Mod4); };\n"
    "  interpret Caps_Lock { action= LockMods(modifiers=Lock); };\n"
    "};\n"
    "xkb_symbols \"us\" {\n"
    "  name[group1]=\"English (US)\";\n"
    "  key <ESC>  { [ Escape ] };\n"
    "  key <TLDE> { [ grave, asciitilde ] };\n"
    "  key <AE01> { [ 1, exclam ] };\n"
    "  key <AE02> { [ 2, at ] };\n"
    "  key <AE03> { [ 3, numbersign ] };\n"
    "  key <AE04> { [ 4, dollar ] };\n"
    "  key <AE05> { [ 5, percent ] };\n"
    "  key <AE06> { [ 6, asciicircum ] };\n"
    "  key <AE07> { [ 7, ampersand ] };\n"
    "  key <AE08> { [ 8, asterisk ] };\n"
    "  key <AE09> { [ 9, parenleft ] };\n"
    "  key <AE10> { [ 0, parenright ] };\n"
    "  key <AE11> { [ minus, underscore ] };\n"
    "  key <AE12> { [ equal, plus ] };\n"
    "  key <BKSP> { [ BackSpace ] };\n"
    "  key <TAB>  { [ Tab, ISO_Left_Tab ] };\n"
    "  key <AD01> { [ q, Q ] }; key <AD02> { [ w, W ] }; key <AD03> { [ e, E ] };\n"
    "  key <AD04> { [ r, R ] }; key <AD05> { [ t, T ] }; key <AD06> { [ y, Y ] };\n"
    "  key <AD07> { [ u, U ] }; key <AD08> { [ i, I ] }; key <AD09> { [ o, O ] };\n"
    "  key <AD10> { [ p, P ] };\n"
    "  key <AD11> { [ bracketleft, braceleft ] };\n"
    "  key <AD12> { [ bracketright, braceright ] };\n"
    "  key <RTRN> { [ Return ] };\n"
    "  key <AC01> { [ a, A ] }; key <AC02> { [ s, S ] }; key <AC03> { [ d, D ] };\n"
    "  key <AC04> { [ f, F ] }; key <AC05> { [ g, G ] }; key <AC06> { [ h, H ] };\n"
    "  key <AC07> { [ j, J ] }; key <AC08> { [ k, K ] }; key <AC09> { [ l, L ] };\n"
    "  key <AC10> { [ semicolon, colon ] };\n"
    "  key <AC11> { [ apostrophe, quotedbl ] };\n"
    "  key <BKSL> { [ backslash, bar ] };\n"
    "  key <AB01> { [ z, Z ] }; key <AB02> { [ x, X ] }; key <AB03> { [ c, C ] };\n"
    "  key <AB04> { [ v, V ] }; key <AB05> { [ b, B ] }; key <AB06> { [ n, N ] };\n"
    "  key <AB07> { [ m, M ] };\n"
    "  key <AB08> { [ comma, less ] };\n"
    "  key <AB09> { [ period, greater ] };\n"
    "  key <AB10> { [ slash, question ] };\n"
    "  key <SPCE> { [ space ] };\n"
    "  key <LFSH> { [ Shift_L ] }; key <RTSH> { [ Shift_R ] };\n"
    "  key <LCTL> { [ Control_L ] }; key <RCTL> { [ Control_R ] };\n"
    "  key <LALT> { [ Alt_L ] }; key <RALT> { [ Alt_R ] };\n"
    "  key <LWIN> { [ Super_L ] }; key <RWIN> { [ Super_R ] };\n"
    "  key <CAPS> { [ Caps_Lock ] };\n"
    "  key <FK01> { [ F1 ] }; key <FK02> { [ F2 ] }; key <FK03> { [ F3 ] };\n"
    "  key <FK04> { [ F4 ] }; key <FK05> { [ F5 ] }; key <FK06> { [ F6 ] };\n"
    "  key <FK07> { [ F7 ] }; key <FK08> { [ F8 ] }; key <FK09> { [ F9 ] };\n"
    "  key <FK10> { [ F10 ] }; key <FK11> { [ F11 ] }; key <FK12> { [ F12 ] };\n"
    "  key <HOME> { [ Home ] }; key <UP>   { [ Up ] };   key <PGUP> { [ Prior ] };\n"
    "  key <LEFT> { [ Left ] }; key <RGHT> { [ Right ] }; key <END> { [ End ] };\n"
    "  key <DOWN> { [ Down ] }; key <PGDN> { [ Next ] }; key <INS>  { [ Insert ] };\n"
    "  key <DELE> { [ Delete ] };\n"
    "  key <MUTE> { [ XF86AudioMute ] };\n"
    "  key <VOL-> { [ XF86AudioLowerVolume ] };\n"
    "  key <VOL+> { [ XF86AudioRaiseVolume ] };\n"
    "  key <POWR> { [ XF86PowerOff ] };\n"
    "  key <MON-> { [ XF86MonBrightnessDown ] };\n"
    "  key <MON+> { [ XF86MonBrightnessUp ] };\n"
    "  modifier_map Shift   { <LFSH>, <RTSH> };\n"
    "  modifier_map Lock    { <CAPS> };\n"
    "  modifier_map Control { <LCTL>, <RCTL> };\n"
    "  modifier_map Mod1    { <LALT>, <RALT> };\n"
    "  modifier_map Mod4    { <LWIN>, <RWIN> };\n"
    "};\n"
    "xkb_geometry \"minimal\" { width= 100; height= 100; };\n"
    "};\n";

enum screen_rot {
    ROT_NORMAL    = 0, // Portrait (+Y top/camera up): logical 1116 x 2480
    ROT_RIGHT_UP  = 1, // Landscape 90 CCW (+X right/pwrkey edge up): logical 2480 x 1116
    ROT_LEFT_UP   = 2, // Landscape 90 CW (-X left edge up): logical 2480 x 1116
    ROT_BOTTOM_UP = 3, // Portrait 180 (-Y bottom/USB-C edge up): logical 1116 x 2480
};

enum obj_type {
    OBJ_NONE = 0,
    OBJ_DISPLAY,
    OBJ_REGISTRY,
    OBJ_COMPOSITOR,
    OBJ_SUBCOMPOSITOR,
    OBJ_SHM,
    OBJ_OUTPUT,
    OBJ_SEAT,
    OBJ_XDG_WM_BASE,
    OBJ_SURFACE,
    OBJ_REGION,
    OBJ_SUBSURFACE,
    OBJ_SHM_POOL,
    OBJ_BUFFER,
    OBJ_XDG_SURFACE,
    OBJ_XDG_TOPLEVEL,
    OBJ_XDG_POPUP,
    OBJ_XDG_POSITIONER,
    OBJ_POINTER,
    OBJ_KEYBOARD,
    OBJ_TOUCH,
    OBJ_SINGLE_PIXEL_MGR,
    OBJ_SINGLE_PIXEL_BUF,
    OBJ_VIEWPORTER,
    OBJ_VIEWPORT,
    OBJ_PTR_CONSTRAINTS,
    OBJ_LOCKED_PTR,
    OBJ_CONFINED_PTR,
    OBJ_REL_PTR_MGR,
    OBJ_REL_PTR,
    OBJ_PRESENTATION,
    OBJ_PRESENTATION_FB,
    OBJ_TOPLEVEL_ICON_MGR,
    OBJ_TOPLEVEL_ICON,
    OBJ_SHORTCUTS_INHIBIT_MGR,
    OBJ_SHORTCUTS_INHIBITOR,
    OBJ_DMABUF,
    OBJ_DMABUF_FEEDBACK,
    OBJ_DMABUF_PARAMS,
};

struct shm_pool {
    int fd;
    size_t size;
    uint8_t *map;
    int refcnt;
    int is_dmabuf;
};

struct wl_buf {
    struct shm_pool *pool;
    int32_t offset, width, height, stride;
    uint32_t format;
    uint32_t flags;
};

struct dmabuf_params {
    int fd;
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
    int has_plane0;
};

struct wl_surf {
    uint32_t id;
    uint32_t xdg_surf_id;
    uint32_t xdg_top_id;
    uint32_t pending_buf;
    int has_attach;
    int is_toplevel;
    int sent_initial_configure;
    int sent_wm_caps;
    int configured;
    uint32_t frame_cbs[MAX_CBS];
    int n_frame_cbs;
    uint32_t pres_fbs[MAX_CBS];
    int n_pres_fbs;
};

struct wl_obj {
    enum obj_type type;
    uint32_t version;
    union {
        struct shm_pool *pool;
        struct wl_buf buf;
        struct wl_surf *surf;
        struct dmabuf_params *dmabuf_p;
        uint32_t link_id;
    } u;
};

struct mt_state {
    int fd;
    int max_x, max_y;
    int slop_px;
    int cur_slot;
    int ptr_slot;
    int active[MAX_SLOTS];
    int prev_active[MAX_SLOTS];
    int tracking_id[MAX_SLOTS];
    int pending_down[MAX_SLOTS];
    int slot_suppressed[MAX_SLOTS];
    int wl_touch_down[MAX_SLOTS];
    int x[MAX_SLOTS];
    int y[MAX_SLOTS];
    int down_x[MAX_SLOTS];
    int down_y[MAX_SLOTS];
    int drag_unlocked[MAX_SLOTS];
    int32_t last_wx[MAX_SLOTS];
    int32_t last_wy[MAX_SLOTS];
    int moved[MAX_SLOTS];
};

struct kbd_state {
    int fds[MAX_KBDS];
    int dev_nums[MAX_KBDS];
    int fd_has_volup[MAX_KBDS];
    int fd_has_voldown[MAX_KBDS];
    int n_fds;
    int has_evdev_volup;
    int has_evdev_voldown;
    uint64_t evdev_voldown_events;
    uint64_t total_vol_events;
    int lshift, rshift, lctrl, rctrl, lalt, ralt, lmeta, rmeta;
    int capslock;
};

struct hw_vol_state {
    int volup_line_fd;
    int volup_mock_fd;
    int volup_pressed;
    int volup_raw_prev;
    uint32_t volup_line_offset;
    char volup_src[24];
    int resin_regmap_fd;
    off_t resin_regmap_off;
    int resin_is_mock;
    int resin_probed_unavail;
    int voldown_pressed;
    int voldown_raw_prev;
    char voldown_src[24];
};

struct mt_slot_req {
    uint32_t code;
    int32_t values[MAX_SLOTS];
};

#pragma pack(push, 1)
struct dmabuf_fmt_entry {
    uint32_t drm_format;
    uint32_t pad;
    uint64_t modifier;
};
#pragma pack(pop)

static struct wl_obj g_objs[MAX_OBJS];
static uint8_t *g_fb_map = NULL;
static uint8_t *g_saved_fb = NULL;
static size_t g_fb_len = 0;
static int g_fb_w = 1116;
static int g_fb_h = 2480;
static int g_fb_stride = 4464;
static int g_fb_swap_rb = 0;
static int g_screen_off = 0;
static int g_brightness_pct = 100;
static enum screen_rot g_rot = ROT_NORMAL;
static int g_auto_rotate = 1;
static int g_cand_rot = -1;
static uint32_t g_cand_since_ms = 0;
static const char *g_imu_state_path = IMU_STATE_PATH;
static const char *g_drm_node_path = DEFAULT_DRM_NODE;

static uint64_t g_blit_frames = 0;
static uint64_t g_dmabuf_frames = 0;
static uint32_t g_serial = 1;
static uint64_t g_frame_seq = 0;
static uint32_t g_top_surf_id = 0;
static uint32_t g_ptr_id = 0;
static uint32_t g_kbd_id = 0;
static uint32_t g_touch_id = 0;
static int g_ptr_entered = 0;
static int g_kbd_entered = 0;

static inline void hw_write_barrier(void) {
#if defined(__aarch64__) && defined(__linux__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static int create_anon_ro_fd(const char *tag, const void *data, size_t len) {
    int fd = -1;
#if defined(__linux__) && defined(MFD_CLOEXEC)
    fd = memfd_create(tag ? tag : "nx769j-anon", MFD_CLOEXEC);
#endif
    if (fd < 0) {
        char tmpl[128];
        snprintf(tmpl, sizeof(tmpl), "/tmp/nx769j-%s-XXXXXX", tag ? tag : "anon");
        fd = mkstemp(tmpl);
        if (fd >= 0) {
            unlink(tmpl);
            int fdfl = fcntl(fd, F_GETFD, 0);
            if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
        }
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) < 0) {
        close(fd);
        return -1;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        p += w;
        rem -= (size_t)w;
    }
    (void)lseek(fd, 0, SEEK_SET);
    return fd;
}

static dev_t get_drm_render_dev(void) {
    struct stat st;
    if (g_drm_node_path && *g_drm_node_path && stat(g_drm_node_path, &st) == 0) {
        if (S_ISCHR(st.st_mode)) {
            return st.st_rdev;
        }
        if (S_ISREG(st.st_mode) && st.st_size >= (off_t)sizeof(dev_t)) {
            int fd = open(g_drm_node_path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                dev_t d = 0;
                ssize_t r = read(fd, &d, sizeof(d));
                close(fd);
                if (r == (ssize_t)sizeof(d)) return d;
            }
        }
    }
    return makedev(226, 128); // /dev/dri/renderD128 default (226:128)
}

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

static int accept_cloexec_blocking(int lfd) {
#ifdef __linux__
    return accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
#else
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0) {
        int fdfl = fcntl(fd, F_GETFD, 0);
        if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    }
    return fd;
#endif
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static const char *rot_name(enum screen_rot r) {
    switch (r) {
    case ROT_NORMAL:    return "normal";
    case ROT_RIGHT_UP:  return "right-up";
    case ROT_LEFT_UP:   return "left-up";
    case ROT_BOTTOM_UP: return "bottom-up";
    default:            return "normal";
    }
}

static int parse_rot_name(const char *s, enum screen_rot *out) {
    if (!s || !out) return -1;
    if (strcasecmp(s, "normal") == 0)    { *out = ROT_NORMAL;    return 0; }
    if (strcasecmp(s, "right-up") == 0)  { *out = ROT_RIGHT_UP;  return 0; }
    if (strcasecmp(s, "left-up") == 0)   { *out = ROT_LEFT_UP;   return 0; }
    if (strcasecmp(s, "bottom-up") == 0) { *out = ROT_BOTTOM_UP; return 0; }
    return -1;
}

static void logical_dims_for_rot(enum screen_rot r, int fb_w, int fb_h, int *lw, int *lh) {
    if (r == ROT_RIGHT_UP || r == ROT_LEFT_UP) {
        *lw = fb_h;
        *lh = fb_w;
    } else {
        *lw = fb_w;
        *lh = fb_h;
    }
}

static inline uint32_t swap_rb_u32(uint32_t v) {
    return (v & 0xff00ff00u) | ((v & 0x00ff0000u) >> 16) | ((v & 0x000000ffu) << 16);
}

static void send_all(int cfd, const void *buf, size_t len) {
    if (cfd < 0) return;
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t w = send(cfd, p, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return;
        }
        p += w;
        len -= (size_t)w;
    }
}

static ssize_t recv_line_timeout(int fd, char *buf, size_t bufsz, int timeout_ms) {
    if (fd < 0 || !buf || bufsz < 2) return -1;
    size_t total = 0;
    uint32_t start = now_ms();
    buf[0] = '\0';

    while (total < bufsz - 1) {
        ssize_t r = recv(fd, buf + total, bufsz - 1 - total, 0);
        if (r > 0) {
            total += (size_t)r;
            buf[total] = '\0';
            if (memchr(buf, '\n', total) != NULL) {
                return (ssize_t)total;
            }
            continue;
        }
        if (r == 0) break;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return total > 0 ? (ssize_t)total : -1;
        }
        uint32_t elapsed = now_ms() - start;
        if (elapsed >= (uint32_t)timeout_ms) break;
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

struct msg_builder {
    uint32_t words[256];
    int nwords;
};

static void mb_init(struct msg_builder *m, uint32_t id) {
    m->words[0] = id;
    m->words[1] = 0;
    m->nwords = 2;
}

static void mb_u32(struct msg_builder *m, uint32_t v) {
    m->words[m->nwords++] = v;
}

static void mb_i32(struct msg_builder *m, int32_t v) {
    m->words[m->nwords++] = (uint32_t)v;
}

static void mb_str(struct msg_builder *m, const char *s) {
    if (!s) {
        mb_u32(m, 0);
        return;
    }
    uint32_t len = (uint32_t)strlen(s) + 1;
    mb_u32(m, len);
    uint32_t nwords = (len + 3) / 4;
    memset(&m->words[m->nwords], 0, nwords * 4);
    memcpy(&m->words[m->nwords], s, len);
    m->nwords += (int)nwords;
}

static void mb_arr(struct msg_builder *m, const void *data, uint32_t byte_len) {
    mb_u32(m, byte_len);
    uint32_t nwords = (byte_len + 3) / 4;
    if (nwords > 0) {
        memset(&m->words[m->nwords], 0, nwords * 4);
        memcpy(&m->words[m->nwords], data, byte_len);
        m->nwords += (int)nwords;
    }
}

static void mb_send(struct msg_builder *m, int cfd, uint16_t opcode) {
    uint32_t bytes = (uint32_t)(m->nwords * 4);
    m->words[1] = (bytes << 16) | opcode;
    send_all(cfd, m->words, bytes);
}

static void mb_send_fd(struct msg_builder *m, int cfd, uint16_t opcode, int pass_fd) {
    if (cfd < 0 || pass_fd < 0) return;
    uint32_t bytes = (uint32_t)(m->nwords * 4);
    m->words[1] = (bytes << 16) | opcode;

    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct iovec iov = { .iov_base = m->words, .iov_len = bytes };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));

    for (;;) {
        ssize_t w = sendmsg(cfd, &msg, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return;
        }
        if ((size_t)w < bytes) {
            send_all(cfd, ((const uint8_t *)m->words) + w, bytes - (size_t)w);
        }
        return;
    }
}

static void send_delete_id(int cfd, uint32_t id) {
    struct msg_builder m;
    mb_init(&m, 1);
    mb_u32(&m, id);
    mb_send(&m, cfd, 1);
}

static void send_callback_done(int cfd, uint32_t cb_id, uint32_t data) {
    struct msg_builder m;
    mb_init(&m, cb_id);
    mb_u32(&m, data);
    mb_send(&m, cfd, 0);
    send_delete_id(cfd, cb_id);
}

static void send_global(int cfd, uint32_t reg_id, uint32_t name, const char *iface, uint32_t ver) {
    struct msg_builder m;
    mb_init(&m, reg_id);
    mb_u32(&m, name);
    mb_str(&m, iface);
    mb_u32(&m, ver);
    mb_send(&m, cfd, 0);
}

static void send_output_mode_events(int cfd, uint32_t out_id, uint32_t ver, int lw, int lh) {
    struct msg_builder m;
    mb_init(&m, out_id);
    mb_u32(&m, 0x3); // WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED
    mb_i32(&m, lw);
    mb_i32(&m, lh);
    mb_i32(&m, 60000);
    mb_send(&m, cfd, 1); // wl_output.mode
    if (ver >= 2) {
        mb_init(&m, out_id);
        mb_send(&m, cfd, 2); // wl_output.done
    }
}

static void send_kbd_enter_if_needed(int cfd, const struct kbd_state *ks) {
    if (cfd < 0 || !g_kbd_id || !g_top_surf_id || g_kbd_entered) return;
    struct msg_builder m;
    mb_init(&m, g_kbd_id);
    mb_u32(&m, g_serial++);
    mb_u32(&m, g_top_surf_id);
    mb_arr(&m, NULL, 0);
    mb_send(&m, cfd, 1); // wl_keyboard.enter

    uint32_t dep = 0;
    uint32_t lck = 0;
    if (ks) {
        if (ks->lshift || ks->rshift) dep |= 1u;
        if (ks->lctrl  || ks->rctrl)  dep |= 4u;
        if (ks->lalt   || ks->ralt)   dep |= 8u;
        if (ks->lmeta  || ks->rmeta)  dep |= 64u;
        if (ks->capslock)             lck |= 2u;
    }
    mb_init(&m, g_kbd_id);
    mb_u32(&m, g_serial++);
    mb_u32(&m, dep);
    mb_u32(&m, 0);
    mb_u32(&m, lck);
    mb_u32(&m, 0);
    mb_send(&m, cfd, 4); // wl_keyboard.modifiers
    g_kbd_entered = 1;
}

static void send_kbd_init_events(int cfd, uint32_t kid, uint32_t ver, const struct kbd_state *ks) {
    size_t km_size = sizeof(g_xkb_keymap_str); // includes trailing NUL byte
    int km_fd = create_anon_ro_fd("nx769j-xkb", g_xkb_keymap_str, km_size);
    if (km_fd >= 0) {
        struct msg_builder m;
        mb_init(&m, kid);
        mb_u32(&m, 1); // WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1
        mb_u32(&m, (uint32_t)km_size);
        mb_send_fd(&m, cfd, 0, km_fd); // wl_keyboard.keymap
        close(km_fd);
    }
    if (ver >= 4) {
        struct msg_builder m;
        mb_init(&m, kid);
        mb_i32(&m, 25);  // 25 chars/sec
        mb_i32(&m, 400); // 400 ms delay
        mb_send(&m, cfd, 5); // wl_keyboard.repeat_info
    }
    send_kbd_enter_if_needed(cfd, ks);
}

static void send_dmabuf_feedback_events(int cfd, uint32_t fid) {
    struct dmabuf_fmt_entry table[2];
    memset(table, 0, sizeof(table));
    table[0].drm_format = DRM_FMT_XRGB8888;
    table[0].modifier   = DRM_MOD_LINEAR;
    table[1].drm_format = DRM_FMT_ARGB8888;
    table[1].modifier   = DRM_MOD_LINEAR;

    int tfd = create_anon_ro_fd("nx769j-dmabuf-fmts", table, sizeof(table));
    if (tfd >= 0) {
        struct msg_builder m;
        mb_init(&m, fid);
        mb_u32(&m, (uint32_t)sizeof(table));
        mb_send_fd(&m, cfd, 1, tfd); // format_table(fd, size)
        close(tfd);
    }

    dev_t rdev = get_drm_render_dev();
    uint16_t indices[2] = { 0, 1 };
    struct msg_builder m;

    mb_init(&m, fid);
    mb_arr(&m, &rdev, (uint32_t)sizeof(rdev));
    mb_send(&m, cfd, 2); // main_device

    mb_init(&m, fid);
    mb_arr(&m, &rdev, (uint32_t)sizeof(rdev));
    mb_send(&m, cfd, 4); // tranche_target_device

    mb_init(&m, fid);
    mb_arr(&m, indices, (uint32_t)sizeof(indices));
    mb_send(&m, cfd, 5); // tranche_formats

    mb_init(&m, fid);
    mb_u32(&m, 0);
    mb_send(&m, cfd, 6); // tranche_flags

    mb_init(&m, fid);
    mb_send(&m, cfd, 3); // tranche_done

    mb_init(&m, fid);
    mb_send(&m, cfd, 0); // done
}

static void send_xdg_configure(int cfd, struct wl_surf *s) {
    int lw = g_fb_w, lh = g_fb_h;
    logical_dims_for_rot(g_rot, g_fb_w, g_fb_h, &lw, &lh);
    if (s->xdg_top_id && s->xdg_top_id < MAX_OBJS) {
        uint32_t tver = g_objs[s->xdg_top_id].version;
        if (tver >= 5 && !s->sent_wm_caps) {
            uint32_t caps[2] = { 2 /* MAXIMIZE */, 3 /* FULLSCREEN */ };
            struct msg_builder m;
            mb_init(&m, s->xdg_top_id);
            mb_arr(&m, caps, sizeof(caps));
            mb_send(&m, cfd, 3); // xdg_toplevel.wm_capabilities
            s->sent_wm_caps = 1;
        }
        if (tver >= 4) {
            struct msg_builder m;
            mb_init(&m, s->xdg_top_id);
            mb_i32(&m, lw);
            mb_i32(&m, lh);
            mb_send(&m, cfd, 2); // xdg_toplevel.configure_bounds
        }
        uint32_t states[2] = { 1 /* MAXIMIZED */, 4 /* ACTIVATED */ };
        struct msg_builder m;
        mb_init(&m, s->xdg_top_id);
        mb_i32(&m, lw);
        mb_i32(&m, lh);
        mb_arr(&m, states, sizeof(states));
        mb_send(&m, cfd, 0); // xdg_toplevel.configure
    }
    if (s->xdg_surf_id) {
        struct msg_builder m;
        mb_init(&m, s->xdg_surf_id);
        mb_u32(&m, g_serial++);
        mb_send(&m, cfd, 0); // xdg_surface.configure
    }
}

static void pool_unref(struct shm_pool *p) {
    if (!p) return;
    p->refcnt--;
    if (p->refcnt <= 0) {
        if (p->map && p->map != MAP_FAILED) munmap(p->map, p->size);
        if (p->fd >= 0) close(p->fd);
        free(p);
    }
}

static void destroy_obj(int cfd, uint32_t id) {
    if (id == 0 || id >= MAX_OBJS) return;
    struct wl_obj *o = &g_objs[id];
    if (o->type == OBJ_SHM_POOL) {
        pool_unref(o->u.pool);
    } else if (o->type == OBJ_BUFFER) {
        pool_unref(o->u.buf.pool);
    } else if (o->type == OBJ_DMABUF_PARAMS) {
        if (o->u.dmabuf_p) {
            if (o->u.dmabuf_p->fd >= 0) close(o->u.dmabuf_p->fd);
            free(o->u.dmabuf_p);
        }
    } else if (o->type == OBJ_SURFACE) {
        if (g_top_surf_id == id) {
            g_top_surf_id = 0;
            g_ptr_entered = 0;
            g_kbd_entered = 0;
        }
        free(o->u.surf);
    }
    memset(o, 0, sizeof(*o));
    send_delete_id(cfd, id);
}

static void send_presentation_presented(int cfd, uint32_t fb_id) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t sec = (uint64_t)ts.tv_sec;
    uint64_t seq = ++g_frame_seq;
    struct msg_builder m;
    mb_init(&m, fb_id);
    mb_u32(&m, (uint32_t)(sec >> 32));
    mb_u32(&m, (uint32_t)(sec & 0xffffffffu));
    mb_u32(&m, (uint32_t)ts.tv_nsec);
    mb_u32(&m, 16666666u); // 60 Hz refresh in ns
    mb_u32(&m, (uint32_t)(seq >> 32));
    mb_u32(&m, (uint32_t)(seq & 0xffffffffu));
    mb_u32(&m, 0x7u); // VSYNC | HW_CLOCK | HW_COMPLETION
    mb_send(&m, cfd, 1); // opcode 1 = presented
    send_delete_id(cfd, fb_id);
}

static void flush_saved_fb_to_panel(void) {
    if (g_screen_off || !g_fb_map || !g_saved_fb || g_fb_len == 0) return;
    size_t total_rows_bytes = (size_t)g_fb_h * (size_t)g_fb_stride;
    size_t copy_bytes = total_rows_bytes <= g_fb_len ? total_rows_bytes : g_fb_len;

    if (g_brightness_pct >= 100) {
        memcpy(g_fb_map, g_saved_fb, copy_bytes);
    } else if (g_brightness_pct <= 0) {
        memset(g_fb_map, 0, copy_bytes);
    } else {
        uint32_t scale = (uint32_t)((g_brightness_pct * 256 + 50) / 100);
        const uint32_t *sp = (const uint32_t *)g_saved_fb;
        uint32_t *dp = (uint32_t *)g_fb_map;
        size_t npx = copy_bytes / 4;
        for (size_t i = 0; i < npx; i++) {
            uint32_t px = sp[i];
            uint32_t rb = (((px & 0x00ff00ffu) * scale) >> 8) & 0x00ff00ffu;
            uint32_t g  = (((px & 0x0000ff00u) * scale) >> 8) & 0x0000ff00u;
            dp[i] = (px & 0xff000000u) | rb | g;
        }
    }
    hw_write_barrier();
}

static void blit_buffer_rotated(uint8_t *dst, int dst_stride, int fb_w, int fb_h,
                                const uint8_t *src, int src_stride, int src_w, int src_h,
                                enum screen_rot rot, int need_swap, int y_invert) {
    const int TILE = 32;

    if (rot == ROT_NORMAL) {
        int copy_w = src_w < fb_w ? src_w : fb_w;
        int copy_h = src_h < fb_h ? src_h : fb_h;
        if (!need_swap && !y_invert) {
            if (src_stride == dst_stride && copy_w * 4 == dst_stride) {
                memcpy(dst, src, (size_t)copy_h * (size_t)dst_stride);
            } else {
                for (int y = 0; y < copy_h; y++) {
                    memcpy(dst + (size_t)y * (size_t)dst_stride,
                           src + (size_t)y * (size_t)src_stride,
                           (size_t)copy_w * 4);
                }
            }
        } else {
            for (int y = 0; y < copy_h; y++) {
                int sy = y_invert ? ((src_h - 1) - y) : y;
                const uint32_t *sp = (const uint32_t *)(src + (size_t)sy * (size_t)src_stride);
                uint32_t *dp = (uint32_t *)(dst + (size_t)y * (size_t)dst_stride);
                if (!need_swap) {
                    memcpy(dp, sp, (size_t)copy_w * 4);
                } else {
                    for (int x = 0; x < copy_w; x++) dp[x] = swap_rb_u32(sp[x]);
                }
            }
        }
        return;
    }

    if (rot == ROT_BOTTOM_UP) {
        int copy_w = src_w < fb_w ? src_w : fb_w;
        int copy_h = src_h < fb_h ? src_h : fb_h;
        for (int wy = 0; wy < copy_h; wy++) {
            int sy = y_invert ? ((src_h - 1) - wy) : wy;
            const uint32_t *sp = (const uint32_t *)(src + (size_t)sy * (size_t)src_stride);
            int fy = (fb_h - 1) - wy;
            uint32_t *dp = (uint32_t *)(dst + (size_t)fy * (size_t)dst_stride);
            if (!need_swap) {
                for (int wx = 0; wx < copy_w; wx++) dp[(fb_w - 1) - wx] = sp[wx];
            } else {
                for (int wx = 0; wx < copy_w; wx++) dp[(fb_w - 1) - wx] = swap_rb_u32(sp[wx]);
            }
        }
        return;
    }

    if (rot == ROT_RIGHT_UP) {
        int max_wx = src_w < fb_h ? src_w : fb_h;
        int max_wy = src_h < fb_w ? src_h : fb_w;
        for (int wy0 = 0; wy0 < max_wy; wy0 += TILE) {
            int wy_end = (wy0 + TILE < max_wy) ? (wy0 + TILE) : max_wy;
            for (int wx0 = 0; wx0 < max_wx; wx0 += TILE) {
                int wx_end = (wx0 + TILE < max_wx) ? (wx0 + TILE) : max_wx;
                for (int wy = wy0; wy < wy_end; wy++) {
                    int sy = y_invert ? ((src_h - 1) - wy) : wy;
                    const uint32_t *sp = (const uint32_t *)(src + (size_t)sy * (size_t)src_stride);
                    int fx = (fb_w - 1) - wy;
                    if (!need_swap) {
                        for (int wx = wx0; wx < wx_end; wx++) {
                            uint32_t *dp = (uint32_t *)(dst + (size_t)wx * (size_t)dst_stride);
                            dp[fx] = sp[wx];
                        }
                    } else {
                        for (int wx = wx0; wx < wx_end; wx++) {
                            uint32_t *dp = (uint32_t *)(dst + (size_t)wx * (size_t)dst_stride);
                            dp[fx] = swap_rb_u32(sp[wx]);
                        }
                    }
                }
            }
        }
        return;
    }

    if (rot == ROT_LEFT_UP) {
        int max_wx = src_w < fb_h ? src_w : fb_h;
        int max_wy = src_h < fb_w ? src_h : fb_w;
        for (int wy0 = 0; wy0 < max_wy; wy0 += TILE) {
            int wy_end = (wy0 + TILE < max_wy) ? (wy0 + TILE) : max_wy;
            for (int wx0 = 0; wx0 < max_wx; wx0 += TILE) {
                int wx_end = (wx0 + TILE < max_wx) ? (wx0 + TILE) : max_wx;
                for (int wy = wy0; wy < wy_end; wy++) {
                    int sy = y_invert ? ((src_h - 1) - wy) : wy;
                    const uint32_t *sp = (const uint32_t *)(src + (size_t)sy * (size_t)src_stride);
                    int fx = wy;
                    if (!need_swap) {
                        for (int wx = wx0; wx < wx_end; wx++) {
                            int fy = (fb_h - 1) - wx;
                            uint32_t *dp = (uint32_t *)(dst + (size_t)fy * (size_t)dst_stride);
                            dp[fx] = sp[wx];
                        }
                    } else {
                        for (int wx = wx0; wx < wx_end; wx++) {
                            int fy = (fb_h - 1) - wx;
                            uint32_t *dp = (uint32_t *)(dst + (size_t)fy * (size_t)dst_stride);
                            dp[fx] = swap_rb_u32(sp[wx]);
                        }
                    }
                }
            }
        }
    }
}

static void flip_saved_fb_180(uint8_t *buf, int stride, int fb_w, int fb_h) {
    if (!buf || fb_w <= 0 || fb_h <= 0) return;
    int top = 0, bot = fb_h - 1;
    while (top < bot) {
        uint32_t *rt = (uint32_t *)(buf + (size_t)top * (size_t)stride);
        uint32_t *rb = (uint32_t *)(buf + (size_t)bot * (size_t)stride);
        for (int x = 0; x < fb_w; x++) {
            int rx = (fb_w - 1) - x;
            uint32_t tmp = rt[x];
            rt[x] = rb[rx];
            rb[rx] = tmp;
        }
        top++;
        bot--;
    }
    if (top == bot) {
        uint32_t *rm = (uint32_t *)(buf + (size_t)top * (size_t)stride);
        for (int x = 0; x < fb_w / 2; x++) {
            int rx = (fb_w - 1) - x;
            uint32_t tmp = rm[x];
            rm[x] = rm[rx];
            rm[rx] = tmp;
        }
    }
}

static void blit_to_fb(struct wl_buf *b) {
    static uint32_t last_blit_ms = 0;
    if (!g_fb_map || !b->pool || !b->pool->map || b->pool->map == MAP_FAILED) return;
    if (b->width <= 0 || b->height <= 0 || b->offset < 0 || b->stride < b->width * 4) return;

    size_t need = (size_t)b->offset + (size_t)(b->height - 1) * (size_t)b->stride + (size_t)b->width * 4;
    if (need > b->pool->size) return;

    uint32_t t0 = now_ms();
    int min_interval_ms = g_screen_off ? 32 : 12;
    if (last_blit_ms && (t0 - last_blit_ms) < (uint32_t)min_interval_ms) {
        usleep(((uint32_t)min_interval_ms - (t0 - last_blit_ms)) * 1000);
    }
    last_blit_ms = now_ms();

    int src_is_rgb = (b->format == DRM_FMT_XBGR8888 || b->format == DRM_FMT_ABGR8888);
    int need_swap = g_fb_swap_rb ^ src_is_rgb;
    int y_invert = (b->flags & 1u) ? 1 : 0;

    if (b->pool->is_dmabuf && b->pool->fd >= 0) {
        struct dma_buf_sync sync_start = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        (void)ioctl(b->pool->fd, DMA_BUF_IOCTL_SYNC, &sync_start);
    }

    const uint8_t *src = b->pool->map + b->offset;
    uint8_t *dst = g_saved_fb ? g_saved_fb : g_fb_map;

    blit_buffer_rotated(dst, g_fb_stride, g_fb_w, g_fb_h,
                        src, b->stride, b->width, b->height,
                        g_rot, need_swap, y_invert);

    if (b->pool->is_dmabuf && b->pool->fd >= 0) {
        struct dma_buf_sync sync_end = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        (void)ioctl(b->pool->fd, DMA_BUF_IOCTL_SYNC, &sync_end);
        g_dmabuf_frames++;
    }

    if (g_screen_off) {
        return;
    }

    if (g_saved_fb) {
        flush_saved_fb_to_panel();
    } else {
        hw_write_barrier();
    }
    g_blit_frames++;
    if (g_blit_frames <= 5 || (g_blit_frames % 120) == 0) {
        fprintf(stderr, "[nx769j-fb-wl] blitted frame #%llu (dmabuf=%llu %dx%d stride=%d rot=%s fmt=0x%x)\n",
                (unsigned long long)g_blit_frames, (unsigned long long)g_dmabuf_frames,
                b->width, b->height, b->stride, rot_name(g_rot), b->format);
    }
}

static void send_pointer_leave_if_entered(int cfd) {
    if (g_ptr_entered && cfd >= 0 && g_ptr_id && g_top_surf_id) {
        struct msg_builder m;
        mb_init(&m, g_ptr_id);
        mb_u32(&m, g_serial++);
        mb_u32(&m, g_top_surf_id);
        mb_send(&m, cfd, 1); // wl_pointer.leave
        mb_init(&m, g_ptr_id);
        mb_send(&m, cfd, 5); // wl_pointer.frame
    }
    g_ptr_entered = 0;
}

static void cancel_active_touches_and_pointer(int cfd, struct mt_state *mt) {
    if (!mt) return;
    int had_wl_touch = 0;
    for (int s = 0; s < MAX_SLOTS; s++) {
        if (mt->wl_touch_down[s]) {
            had_wl_touch = 1;
            mt->wl_touch_down[s] = 0;
            // Suppress until finger physically lifts so post-rotation/sleep drag never re-triggers
            mt->slot_suppressed[s] = 1;
        }
    }
    if (had_wl_touch && cfd >= 0 && g_touch_id) {
        struct msg_builder m;
        mb_init(&m, g_touch_id);
        mb_send(&m, cfd, 4); // wl_touch.cancel
    }

    if (mt->ptr_slot >= 0 && cfd >= 0 && g_ptr_id) {
        struct msg_builder m;
        mb_init(&m, g_ptr_id);
        mb_u32(&m, g_serial++);
        mb_u32(&m, now_ms());
        mb_u32(&m, 0x110); // BTN_LEFT
        mb_u32(&m, 0);     // released
        mb_send(&m, cfd, 3);
        mb_init(&m, g_ptr_id);
        mb_send(&m, cfd, 5);
    }
    send_pointer_leave_if_entered(cfd);
    mt->ptr_slot = -1;
}

static void init_mt_state(struct mt_state *mt) {
    if (!mt) return;
    memset(mt, 0, sizeof(*mt));
    mt->fd = -1;
    mt->ptr_slot = -1;
    mt->max_x = g_fb_w * 16 - 1;
    mt->max_y = g_fb_h * 16 - 1;
    mt->slop_px = DEFAULT_TOUCH_SLOP_PX;
    const char *slop_env = getenv("NX769J_TOUCH_SLOP_PX");
    if (slop_env && *slop_env) {
        char *endp = NULL;
        long v = strtol(slop_env, &endp, 10);
        if (endp != slop_env && v >= 0 && v <= 200) {
            mt->slop_px = (int)v;
        }
    }
    for (int s = 0; s < MAX_SLOTS; s++) {
        mt->tracking_id[s] = -1;
    }
}

static void resync_touchscreen_state(int cfd, struct mt_state *mt) {
    if (!mt) return;
    cancel_active_touches_and_pointer(cfd, mt);
    memset(mt->moved, 0, sizeof(mt->moved));
    memset(mt->pending_down, 0, sizeof(mt->pending_down));
    memset(mt->drag_unlocked, 0, sizeof(mt->drag_unlocked));
    memset(mt->wl_touch_down, 0, sizeof(mt->wl_touch_down));

    if (mt->fd >= 0) {
        struct input_event ev[64];
        while (read(mt->fd, ev, sizeof(ev)) > 0) {
            // drain
        }
#ifdef __linux__
        struct input_absinfo slot_info;
        memset(&slot_info, 0, sizeof(slot_info));
        if (ioctl(mt->fd, EVIOCGABS(ABS_MT_SLOT), &slot_info) == 0) {
            if (slot_info.value >= 0 && slot_info.value < MAX_SLOTS) {
                mt->cur_slot = slot_info.value;
            }
        }
        struct mt_slot_req req_tid, req_x, req_y;
        memset(&req_tid, 0, sizeof(req_tid));
        memset(&req_x, 0, sizeof(req_x));
        memset(&req_y, 0, sizeof(req_y));
        req_tid.code = ABS_MT_TRACKING_ID;
        req_x.code = ABS_MT_POSITION_X;
        req_y.code = ABS_MT_POSITION_Y;
        if (ioctl(mt->fd, EVIOCGMTSLOTS(sizeof(req_tid)), &req_tid) == 0 &&
            ioctl(mt->fd, EVIOCGMTSLOTS(sizeof(req_x)), &req_x) == 0 &&
            ioctl(mt->fd, EVIOCGMTSLOTS(sizeof(req_y)), &req_y) == 0) {
            for (int s = 0; s < MAX_SLOTS; s++) {
                mt->tracking_id[s] = req_tid.values[s];
                mt->active[s] = (req_tid.values[s] >= 0) ? 1 : 0;
                mt->x[s] = req_x.values[s];
                mt->y[s] = req_y.values[s];
                mt->down_x[s] = req_x.values[s];
                mt->down_y[s] = req_y.values[s];
            }
        }
#endif
    }

    for (int s = 0; s < MAX_SLOTS; s++) {
        if (!mt->active[s]) mt->tracking_id[s] = -1;
        mt->prev_active[s] = mt->active[s];
        mt->slot_suppressed[s] = mt->active[s] ? 1 : 0;
    }
}

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int json_bool_field_true(const char *json, const char *key) {
    const char *p = strstr(json, key);
    if (!p) return 0;
    const char *c = strchr(p + strlen(key), ':');
    if (!c) return 0;
    c++;
    while (*c == ' ' || *c == '\t') c++;
    return strncmp(c, "true", 4) == 0 ? 1 : 0;
}

static int parse_imu_json_orientation(const char *json, uint64_t ref_mono_ns, enum screen_rot *out_rot) {
    if (!json || !out_rot) return -1;
    if (!json_bool_field_true(json, "\"connected\"") &&
        !json_bool_field_true(json, "\"valid\"")) {
        return -1;
    }

    const char *sp = strstr(json, "\"accel_samples\"");
    if (sp) {
        const char *sc = strchr(sp, ':');
        if (sc) {
            sc++;
            while (*sc == ' ' || *sc == '\t') sc++;
            if (strtoull(sc, NULL, 10) == 0ULL) return -1;
        }
    }

    const char *mp = strstr(json, "\"monotonic_ns\"");
    if (mp && ref_mono_ns > 0) {
        const char *mc = strchr(mp, ':');
        if (mc) {
            mc++;
            while (*mc == ' ' || *mc == '\t') mc++;
            uint64_t sample_ns = strtoull(mc, NULL, 10);
            if (sample_ns > 0 && ref_mono_ns > sample_ns &&
                (ref_mono_ns - sample_ns) > 5000000000ULL) {
                return -1;
            }
        }
    }

    const char *ap = strstr(json, "\"accel_mps2\"");
    if (ap) {
        const char *lb = strchr(ap, '[');
        if (lb) {
            char *end1 = NULL, *end2 = NULL;
            double ax = strtod(lb + 1, &end1);
            if (end1 && *end1 == ',') {
                double ay = strtod(end1 + 1, &end2);
                if (end2 && (ax * ax + ay * ay) < (3.5 * 3.5)) {
                    return -1;
                }
            }
        }
    }

    const char *op = strstr(json, "\"orientation\"");
    if (!op) return -1;
    const char *oc = strchr(op, ':');
    if (!oc) return -1;
    const char *q1 = strchr(oc, '"');
    if (!q1) return -1;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2 || (size_t)(q2 - q1) >= 32) return -1;

    char val[32];
    size_t len = (size_t)(q2 - q1);
    memcpy(val, q1, len);
    val[len] = '\0';
    return parse_rot_name(val, out_rot);
}

static int read_imu_orientation(enum screen_rot *out_rot) {
    if (!g_imu_state_path || !*g_imu_state_path) return -1;
    int fd = open(g_imu_state_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return parse_imu_json_orientation(buf, now_monotonic_ns(), out_rot);
}

static void apply_rotation(enum screen_rot new_rot, int cfd, struct mt_state *mt) {
    if (new_rot == g_rot) return;

    int old_lw = g_fb_w, old_lh = g_fb_h;
    int new_lw = g_fb_w, new_lh = g_fb_h;
    logical_dims_for_rot(g_rot, g_fb_w, g_fb_h, &old_lw, &old_lh);
    logical_dims_for_rot(new_rot, g_fb_w, g_fb_h, &new_lw, &new_lh);

    cancel_active_touches_and_pointer(cfd, mt);
    g_rot = new_rot;

    if (old_lw == new_lw && old_lh == new_lh && g_saved_fb) {
        flip_saved_fb_180(g_saved_fb, g_fb_stride, g_fb_w, g_fb_h);
        flush_saved_fb_to_panel();
    }

    if (cfd >= 0) {
        for (int id = 1; id < MAX_OBJS; id++) {
            if (g_objs[id].type == OBJ_OUTPUT) {
                send_output_mode_events(cfd, (uint32_t)id, g_objs[id].version, new_lw, new_lh);
            }
        }
        for (int id = 1; id < MAX_OBJS; id++) {
            if (g_objs[id].type == OBJ_SURFACE && g_objs[id].u.surf && g_objs[id].u.surf->is_toplevel) {
                send_xdg_configure(cfd, g_objs[id].u.surf);
            }
        }
    }

    fprintf(stderr, "[nx769j-fb-wl] rotation -> %s (logical=%dx%d)\n",
            rot_name(g_rot), new_lw, new_lh);
}

static void poll_imu_auto_rotate(uint32_t now, int cfd, struct mt_state *mt) {
    if (g_screen_off || !g_auto_rotate) {
        g_cand_rot = -1;
        return;
    }
    enum screen_rot sensed = g_rot;
    if (read_imu_orientation(&sensed) < 0 || sensed == g_rot) {
        g_cand_rot = -1;
        return;
    }
    if (g_cand_rot != (int)sensed) {
        g_cand_rot = (int)sensed;
        g_cand_since_ms = now;
        return;
    }
    if ((now - g_cand_since_ms) >= ROT_DEBOUNCE_MS) {
        g_cand_rot = -1;
        apply_rotation(sensed, cfd, mt);
    }
}

static void feed_kbd_input_event(int cfd, struct kbd_state *ks, const struct input_event *ev);
static void release_held_hw_vol_keys(int cfd, struct kbd_state *ks, struct hw_vol_state *hv);

static void set_screen_off_state(int off, int cfd, struct mt_state *mt, struct kbd_state *ks, struct hw_vol_state *hv) {
    off = off ? 1 : 0;
    if (off == g_screen_off) {
        if (off && g_fb_map && g_fb_len > 0) {
            memset(g_fb_map, 0, g_fb_len);
            hw_write_barrier();
        }
        return;
    }

    if (off) {
        if (g_saved_fb && g_fb_map && g_blit_frames == 0) {
            memcpy(g_saved_fb, g_fb_map, g_fb_len);
        }
        release_held_hw_vol_keys(cfd, ks, hv);
        g_screen_off = 1;
        g_cand_rot = -1;
        cancel_active_touches_and_pointer(cfd, mt);
        if (g_fb_map && g_fb_len > 0) {
            memset(g_fb_map, 0, g_fb_len);
            hw_write_barrier();
        }
        fprintf(stderr, "[nx769j-fb-wl] screen_off=1 (AMOLED blanked 0x000000, blits & rotation paused)\n");
    } else {
        g_screen_off = 0;
        g_cand_rot = -1;
        resync_touchscreen_state(cfd, mt);
        flush_saved_fb_to_panel();
        if (g_auto_rotate) {
            enum screen_rot sensed = g_rot;
            if (read_imu_orientation(&sensed) == 0 && sensed != g_rot) {
                apply_rotation(sensed, cfd, mt);
            }
        }
        fprintf(stderr, "[nx769j-fb-wl] screen_off=0 (shadow fb restored, touch resynced, touch_id=%u ptr_id=%u)\n",
                g_touch_id, g_ptr_id);
    }
}

static void inject_key_tap(int cfd, struct kbd_state *ks, uint16_t code) {
    if (!ks || cfd < 0) return;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = 1;
    feed_kbd_input_event(cfd, ks, &ev);
    ev.value = 0;
    feed_kbd_input_event(cfd, ks, &ev);
}

static void handle_power_ctrl_accept(int ctrl_lfd, int cfd, struct mt_state *mt,
                                     struct kbd_state *ks, struct hw_vol_state *hv) {
    if (ctrl_lfd < 0) return;
    for (;;) {
        int pcfd = accept_nonblock_cloexec(ctrl_lfd);
        if (pcfd < 0) break;
        char line[128];
        ssize_t n = recv_line_timeout(pcfd, line, sizeof(line), CTRL_RECV_WAIT_MS);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (line[i] == '\r' || line[i] == '\n') {
                    line[i] = '\0';
                    break;
                }
            }
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            size_t plen = strlen(p);
            while (plen > 0 && (p[plen - 1] == ' ' || p[plen - 1] == '\t')) {
                p[--plen] = '\0';
            }

            if (strcasecmp(p, "OFF") == 0 || strcmp(p, "1") == 0) {
                set_screen_off_state(1, cfd, mt, ks, hv);
            } else if (strcasecmp(p, "ON") == 0 || strcmp(p, "0") == 0) {
                set_screen_off_state(0, cfd, mt, ks, hv);
            } else if (strcasecmp(p, "TOGGLE") == 0) {
                set_screen_off_state(!g_screen_off, cfd, mt, ks, hv);
            } else if (strcasecmp(p, "VOLUP") == 0 || strcasecmp(p, "KEY VOLUP") == 0) {
                inject_key_tap(cfd, ks, KEY_VOLUMEUP);
            } else if (strcasecmp(p, "VOLDOWN") == 0 || strcasecmp(p, "KEY VOLDOWN") == 0) {
                inject_key_tap(cfd, ks, KEY_VOLUMEDOWN);
            } else if (strcasecmp(p, "MUTE") == 0 || strcasecmp(p, "KEY MUTE") == 0) {
                inject_key_tap(cfd, ks, KEY_MUTE);
            } else if (strncasecmp(p, "ROTATE", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
                const char *arg = p + 6;
                while (*arg == ' ' || *arg == '\t') arg++;
                enum screen_rot r = ROT_NORMAL;
                if (strcasecmp(arg, "auto") == 0) {
                    g_auto_rotate = 1;
                    g_cand_rot = -1;
                    if (!g_screen_off && read_imu_orientation(&r) == 0 && r != g_rot) {
                        apply_rotation(r, cfd, mt);
                    }
                } else if (strcasecmp(arg, "lock") == 0) {
                    g_auto_rotate = 0;
                    g_cand_rot = -1;
                } else if (parse_rot_name(arg, &r) == 0) {
                    g_auto_rotate = 0;
                    g_cand_rot = -1;
                    apply_rotation(r, cfd, mt);
                }
            } else if (strncasecmp(p, "BRIGHTNESS", 10) == 0 && (p[10] == ' ' || p[10] == '\t')) {
                const char *arg = p + 10;
                while (*arg == ' ' || *arg == '\t') arg++;
                int pct = atoi(arg);
                if (pct < 1) pct = 1;
                if (pct > 100) pct = 100;
                if (pct != g_brightness_pct) {
                    g_brightness_pct = pct;
                    flush_saved_fb_to_panel();
                }
            }
        }
        int cur_lw = g_fb_w, cur_lh = g_fb_h;
        logical_dims_for_rot(g_rot, g_fb_w, g_fb_h, &cur_lw, &cur_lh);
        const char *vup_src = (hv && hv->volup_src[0]) ? hv->volup_src : "none";
        const char *vdn_src = (hv && hv->voldown_src[0]) ? hv->voldown_src : "none";
        unsigned long long vev = ks ? (unsigned long long)ks->total_vol_events : 0ULL;
        char resp[384];
        int rlen = snprintf(
            resp, sizeof(resp),
            "OK screen_off=%d frames=%llu dmabuf_frames=%llu orient=%s auto_rotate=%d logical=%dx%d brightness=%d ptr_id=%u touch_id=%u kbd_id=%u touch_slop_px=%d volup_src=%s voldown_src=%s vol_events=%llu\n",
            g_screen_off, (unsigned long long)g_blit_frames, (unsigned long long)g_dmabuf_frames,
            rot_name(g_rot), g_auto_rotate, cur_lw, cur_lh, g_brightness_pct,
            g_ptr_id, g_touch_id, g_kbd_id, mt ? mt->slop_px : DEFAULT_TOUCH_SLOP_PX,
            vup_src, vdn_src, vev
        );
        if (rlen > 0) {
            send_all(pcfd, resp, (size_t)rlen);
        }
        close(pcfd);
    }
}

static void reset_client_state(void) {
    for (int i = 0; i < MAX_OBJS; i++) {
        if (g_objs[i].type == OBJ_SHM_POOL) pool_unref(g_objs[i].u.pool);
        else if (g_objs[i].type == OBJ_BUFFER) pool_unref(g_objs[i].u.buf.pool);
        else if (g_objs[i].type == OBJ_DMABUF_PARAMS && g_objs[i].u.dmabuf_p) {
            if (g_objs[i].u.dmabuf_p->fd >= 0) close(g_objs[i].u.dmabuf_p->fd);
            free(g_objs[i].u.dmabuf_p);
        }
        else if (g_objs[i].type == OBJ_SURFACE) free(g_objs[i].u.surf);
    }
    memset(g_objs, 0, sizeof(g_objs));
    g_objs[1].type = OBJ_DISPLAY;
    g_objs[1].version = 1;
    g_top_surf_id = 0;
    g_ptr_id = 0;
    g_kbd_id = 0;
    g_touch_id = 0;
    g_ptr_entered = 0;
    g_kbd_entered = 0;
}

static void handle_message(int cfd, uint32_t id, uint16_t op, const uint32_t *args, int nargs,
                           int *fds, int *fd_idx, int nfds, const struct kbd_state *ks) {
    if (id >= MAX_OBJS || g_objs[id].type == OBJ_NONE) return;
    struct wl_obj *o = &g_objs[id];

    switch (o->type) {
    case OBJ_DISPLAY:
        if (op == 0 && nargs >= 1) { // sync(callback)
            send_callback_done(cfd, args[0], g_serial++);
        } else if (op == 1 && nargs >= 1) { // get_registry(registry)
            uint32_t rid = args[0];
            if (rid < MAX_OBJS) {
                g_objs[rid].type = OBJ_REGISTRY;
                g_objs[rid].version = 1;
                send_global(cfd, rid, 1, "wl_compositor", 6);
                send_global(cfd, rid, 2, "wl_subcompositor", 1);
                send_global(cfd, rid, 3, "wl_shm", 1);
                send_global(cfd, rid, 4, "wl_output", 4);
                send_global(cfd, rid, 5, "wl_seat", 9);
                send_global(cfd, rid, 6, "xdg_wm_base", 6);
                send_global(cfd, rid, 7, "wp_single_pixel_buffer_manager_v1", 1);
                send_global(cfd, rid, 8, "wp_viewporter", 1);
                send_global(cfd, rid, 9, "zwp_pointer_constraints_v1", 1);
                send_global(cfd, rid, 10, "zwp_relative_pointer_manager_v1", 1);
                send_global(cfd, rid, 11, "wp_presentation", 1);
                send_global(cfd, rid, 12, "xdg_toplevel_icon_manager_v1", 1);
                send_global(cfd, rid, 13, "zwp_keyboard_shortcuts_inhibit_manager_v1", 1);
                send_global(cfd, rid, 14, "zwp_linux_dmabuf_v1", 4);
            }
        }
        break;

    case OBJ_REGISTRY:
        if (op == 0 && nargs >= 4) { // bind(name, iface_str, version, new_id)
            uint32_t name = args[0];
            uint32_t slen = args[1];
            uint32_t swords = (slen + 3) / 4;
            if (2 + (int)swords + 2 > nargs) break;
            uint32_t ver = args[2 + swords];
            uint32_t nid = args[3 + swords];
            if (nid >= MAX_OBJS) break;
            g_objs[nid].version = ver;
            if (name == 1) g_objs[nid].type = OBJ_COMPOSITOR;
            else if (name == 2) g_objs[nid].type = OBJ_SUBCOMPOSITOR;
            else if (name == 3) {
                g_objs[nid].type = OBJ_SHM;
                struct msg_builder m;
                mb_init(&m, nid); mb_u32(&m, 0); mb_send(&m, cfd, 0); // ARGB8888
                mb_init(&m, nid); mb_u32(&m, 1); mb_send(&m, cfd, 0); // XRGB8888
            } else if (name == 4) {
                int lw = g_fb_w, lh = g_fb_h;
                logical_dims_for_rot(g_rot, g_fb_w, g_fb_h, &lw, &lh);
                g_objs[nid].type = OBJ_OUTPUT;
                struct msg_builder m;
                mb_init(&m, nid);
                mb_i32(&m, 0); mb_i32(&m, 0);
                mb_i32(&m, 71); mb_i32(&m, 158);
                mb_i32(&m, 0);
                mb_str(&m, "RedMagic");
                mb_str(&m, "NX769J-AMOLED");
                mb_i32(&m, 0);
                mb_send(&m, cfd, 0);
                mb_init(&m, nid);
                mb_u32(&m, 0x3);
                mb_i32(&m, lw);
                mb_i32(&m, lh);
                mb_i32(&m, 60000);
                mb_send(&m, cfd, 1);
                if (ver >= 2) {
                    mb_init(&m, nid); mb_i32(&m, 1); mb_send(&m, cfd, 3);
                }
                if (ver >= 4) {
                    mb_init(&m, nid); mb_str(&m, "DSI-1"); mb_send(&m, cfd, 4);
                    mb_init(&m, nid); mb_str(&m, "RedMagic 9 Pro AMOLED"); mb_send(&m, cfd, 5);
                }
                if (ver >= 2) {
                    mb_init(&m, nid); mb_send(&m, cfd, 2);
                }
            } else if (name == 5) {
                g_objs[nid].type = OBJ_SEAT;
                struct msg_builder m;
                mb_init(&m, nid);
                mb_u32(&m, 0x7); // POINTER(1) | KEYBOARD(2) | TOUCH(4)
                mb_send(&m, cfd, 0);
                if (ver >= 2) {
                    mb_init(&m, nid); mb_str(&m, "seat0"); mb_send(&m, cfd, 1);
                }
            } else if (name == 6) {
                g_objs[nid].type = OBJ_XDG_WM_BASE;
            } else if (name == 7) {
                g_objs[nid].type = OBJ_SINGLE_PIXEL_MGR;
            } else if (name == 8) {
                g_objs[nid].type = OBJ_VIEWPORTER;
            } else if (name == 9) {
                g_objs[nid].type = OBJ_PTR_CONSTRAINTS;
            } else if (name == 10) {
                g_objs[nid].type = OBJ_REL_PTR_MGR;
            } else if (name == 11) {
                g_objs[nid].type = OBJ_PRESENTATION;
                struct msg_builder m;
                mb_init(&m, nid);
                mb_u32(&m, 1); // CLOCK_MONOTONIC
                mb_send(&m, cfd, 0);
            } else if (name == 12) {
                g_objs[nid].type = OBJ_TOPLEVEL_ICON_MGR;
            } else if (name == 13) {
                g_objs[nid].type = OBJ_SHORTCUTS_INHIBIT_MGR;
            } else if (name == 14) {
                g_objs[nid].type = OBJ_DMABUF;
                if (ver < 4) {
                    uint32_t fmts[2] = { DRM_FMT_XRGB8888, DRM_FMT_ARGB8888 };
                    for (int i = 0; i < 2; i++) {
                        struct msg_builder m;
                        mb_init(&m, nid);
                        mb_u32(&m, fmts[i]);
                        mb_send(&m, cfd, 0); // format
                        if (ver >= 3) {
                            mb_init(&m, nid);
                            mb_u32(&m, fmts[i]);
                            mb_u32(&m, 0); // modifier_hi
                            mb_u32(&m, 0); // modifier_lo (DRM_FORMAT_MOD_LINEAR)
                            mb_send(&m, cfd, 1); // modifier
                        }
                    }
                }
            }
        }
        break;

    case OBJ_COMPOSITOR:
        if (op == 0 && nargs >= 1) { // create_surface(id)
            uint32_t sid = args[0];
            if (sid < MAX_OBJS) {
                struct wl_surf *s = calloc(1, sizeof(*s));
                s->id = sid;
                g_objs[sid].type = OBJ_SURFACE;
                g_objs[sid].version = o->version;
                g_objs[sid].u.surf = s;
                if (o->version >= 6) {
                    struct msg_builder m;
                    mb_init(&m, sid); mb_i32(&m, 1); mb_send(&m, cfd, 2); // preferred_buffer_scale
                    mb_init(&m, sid); mb_u32(&m, 0); mb_send(&m, cfd, 3); // preferred_buffer_transform
                }
            }
        } else if (op == 1 && nargs >= 1) { // create_region(id)
            if (args[0] < MAX_OBJS) {
                g_objs[args[0]].type = OBJ_REGION;
                g_objs[args[0]].version = o->version;
            }
        }
        break;

    case OBJ_REGION:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_SUBCOMPOSITOR:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 3) {
            if (args[0] < MAX_OBJS) {
                g_objs[args[0]].type = OBJ_SUBSURFACE;
                g_objs[args[0]].version = o->version;
            }
        }
        break;

    case OBJ_SUBSURFACE:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_SHM:
        if (op == 0 && nargs >= 2 && *fd_idx < nfds) { // create_pool(id, fd, size)
            uint32_t pid = args[0];
            int32_t sz = (int32_t)args[1];
            int pfd = fds[(*fd_idx)++];
            if (pid < MAX_OBJS && sz > 0) {
                struct shm_pool *p = calloc(1, sizeof(*p));
                p->fd = pfd;
                p->size = (size_t)sz;
                p->map = mmap(NULL, p->size, PROT_READ, MAP_SHARED, pfd, 0);
                p->refcnt = 1;
                p->is_dmabuf = 0;
                g_objs[pid].type = OBJ_SHM_POOL;
                g_objs[pid].version = o->version;
                g_objs[pid].u.pool = p;
            } else {
                close(pfd);
            }
        } else if (op == 1) { // release (since v2)
            destroy_obj(cfd, id);
        }
        break;

    case OBJ_SHM_POOL:
        if (op == 0 && nargs >= 6) { // create_buffer(id, offset, w, h, stride, format)
            uint32_t bid = args[0];
            if (bid < MAX_OBJS && o->u.pool) {
                o->u.pool->refcnt++;
                g_objs[bid].type = OBJ_BUFFER;
                g_objs[bid].version = 1;
                g_objs[bid].u.buf.pool   = o->u.pool;
                g_objs[bid].u.buf.offset = (int32_t)args[1];
                g_objs[bid].u.buf.width  = (int32_t)args[2];
                g_objs[bid].u.buf.height = (int32_t)args[3];
                g_objs[bid].u.buf.stride = (int32_t)args[4];
                g_objs[bid].u.buf.format = args[5];
                g_objs[bid].u.buf.flags  = 0;
            }
        } else if (op == 1) { // destroy
            destroy_obj(cfd, id);
        } else if (op == 2 && nargs >= 1 && o->u.pool) { // resize(size)
            size_t nsz = (size_t)(int32_t)args[0];
            if (nsz > o->u.pool->size) {
                if (o->u.pool->map && o->u.pool->map != MAP_FAILED)
                    munmap(o->u.pool->map, o->u.pool->size);
                o->u.pool->size = nsz;
                o->u.pool->map = mmap(NULL, nsz, PROT_READ, MAP_SHARED, o->u.pool->fd, 0);
            }
        }
        break;

    case OBJ_DMABUF:
        if (op == 0) { // destroy
            destroy_obj(cfd, id);
        } else if (op == 1 && nargs >= 1) { // create_params(params_id)
            uint32_t pid = args[0];
            if (pid < MAX_OBJS) {
                struct dmabuf_params *dp = calloc(1, sizeof(*dp));
                dp->fd = -1;
                g_objs[pid].type = OBJ_DMABUF_PARAMS;
                g_objs[pid].version = o->version;
                g_objs[pid].u.dmabuf_p = dp;
            }
        } else if ((op == 2 || op == 3) && nargs >= 1) { // get_default_feedback / get_surface_feedback
            uint32_t fid = args[0];
            if (fid < MAX_OBJS) {
                g_objs[fid].type = OBJ_DMABUF_FEEDBACK;
                g_objs[fid].version = o->version;
                send_dmabuf_feedback_events(cfd, fid);
            }
        }
        break;

    case OBJ_DMABUF_FEEDBACK:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_DMABUF_PARAMS: {
        struct dmabuf_params *dp = o->u.dmabuf_p;
        if (op == 0) { // destroy
            destroy_obj(cfd, id);
        } else if (op == 1 && nargs >= 5 && *fd_idx < nfds) { // add(fd, plane_idx, offset, stride, mod_hi, mod_lo)
            int pfd = fds[(*fd_idx)++];
            uint32_t plane_idx = args[0];
            if (dp && plane_idx == 0) {
                if (dp->fd >= 0) close(dp->fd);
                dp->fd = pfd;
                dp->offset = args[1];
                dp->stride = args[2];
                dp->modifier = ((uint64_t)args[3] << 32) | (uint64_t)args[4];
                dp->has_plane0 = 1;
            } else {
                close(pfd);
            }
        } else if (op == 3 && nargs >= 5) { // create_immed(buffer_id, width, height, format, flags)
            uint32_t bid = args[0];
            int32_t w = (int32_t)args[1];
            int32_t h = (int32_t)args[2];
            uint32_t fmt = args[3];
            uint32_t flg = args[4];
            if (dp && dp->has_plane0 && dp->fd >= 0 && bid < MAX_OBJS && w > 0 && h > 0 &&
                dp->stride >= (uint32_t)(w * 4)) {
                size_t min_sz = (size_t)dp->offset + (size_t)(h - 1) * (size_t)dp->stride + (size_t)w * 4;
                size_t map_sz = min_sz;
                off_t end = lseek(dp->fd, 0, SEEK_END);
                if (end > 0 && (size_t)end > map_sz) map_sz = (size_t)end;
                (void)lseek(dp->fd, 0, SEEK_SET);

                struct shm_pool *p = calloc(1, sizeof(*p));
                p->fd = dp->fd;
                dp->fd = -1;
                dp->has_plane0 = 0;
                p->size = map_sz;
                p->map = mmap(NULL, p->size, PROT_READ, MAP_SHARED, p->fd, 0);
                p->refcnt = 1;
                p->is_dmabuf = 1;

                g_objs[bid].type = OBJ_BUFFER;
                g_objs[bid].version = 1;
                g_objs[bid].u.buf.pool   = p;
                g_objs[bid].u.buf.offset = (int32_t)dp->offset;
                g_objs[bid].u.buf.width  = w;
                g_objs[bid].u.buf.height = h;
                g_objs[bid].u.buf.stride = (int32_t)dp->stride;
                g_objs[bid].u.buf.format = fmt;
                g_objs[bid].u.buf.flags  = flg;
            } else if (bid < MAX_OBJS) {
                g_objs[bid].type = OBJ_BUFFER;
                g_objs[bid].version = 1;
            }
        } else if (op == 2) { // non-immed create -> unsupported, send failed (op=1)
            struct msg_builder m;
            mb_init(&m, id);
            mb_send(&m, cfd, 1);
        }
        break;
    }

    case OBJ_BUFFER:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_SURFACE: {
        struct wl_surf *s = o->u.surf;
        if (!s) break;
        if (op == 0) {
            destroy_obj(cfd, id);
        } else if (op == 1 && nargs >= 3) { // attach(buf, x, y)
            s->pending_buf = args[0];
            s->has_attach = 1;
        } else if (op == 3 && nargs >= 1) { // frame(cb)
            if (s->n_frame_cbs < MAX_CBS) s->frame_cbs[s->n_frame_cbs++] = args[0];
        } else if (op == 6) { // commit()
            if (s->xdg_surf_id && !s->sent_initial_configure && s->pending_buf == 0) {
                s->sent_initial_configure = 1;
                send_xdg_configure(cfd, s);
            }
            if (s->has_attach && s->pending_buf != 0 && s->pending_buf < MAX_OBJS) {
                if (g_objs[s->pending_buf].type == OBJ_BUFFER) {
                    struct wl_buf *b = &g_objs[s->pending_buf].u.buf;
                    int cur_lw = g_fb_w, cur_lh = g_fb_h;
                    logical_dims_for_rot(g_rot, g_fb_w, g_fb_h, &cur_lw, &cur_lh);
                    if (s->is_toplevel && b->width == cur_lw && b->height == cur_lh) {
                        blit_to_fb(b);
                    }
                }
                if (g_objs[s->pending_buf].type == OBJ_BUFFER ||
                    g_objs[s->pending_buf].type == OBJ_SINGLE_PIXEL_BUF) {
                    struct msg_builder m;
                    mb_init(&m, s->pending_buf);
                    mb_send(&m, cfd, 0); // wl_buffer.release
                }
                s->has_attach = 0;
            }
            uint32_t t = now_ms();
            for (int i = 0; i < s->n_frame_cbs; i++) {
                send_callback_done(cfd, s->frame_cbs[i], t);
            }
            s->n_frame_cbs = 0;
            for (int i = 0; i < s->n_pres_fbs; i++) {
                send_presentation_presented(cfd, s->pres_fbs[i]);
            }
            s->n_pres_fbs = 0;
        }
        break;
    }

    case OBJ_XDG_WM_BASE:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 1) {
            if (args[0] < MAX_OBJS) {
                g_objs[args[0]].type = OBJ_XDG_POSITIONER;
                g_objs[args[0]].version = o->version;
            }
        } else if (op == 2 && nargs >= 2) { // get_xdg_surface(id, surface)
            uint32_t xid = args[0], sid = args[1];
            if (xid < MAX_OBJS && sid < MAX_OBJS && g_objs[sid].type == OBJ_SURFACE) {
                g_objs[xid].type = OBJ_XDG_SURFACE;
                g_objs[xid].version = o->version;
                g_objs[xid].u.link_id = sid;
                g_objs[sid].u.surf->xdg_surf_id = xid;
            }
        }
        // op 3 = pong(serial): accepted no-op
        break;

    case OBJ_XDG_POSITIONER:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_XDG_SURFACE:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 1) { // get_toplevel(id)
            uint32_t tid = args[0];
            uint32_t sid = o->u.link_id;
            if (tid < MAX_OBJS && sid < MAX_OBJS && g_objs[sid].type == OBJ_SURFACE) {
                struct wl_surf *s = g_objs[sid].u.surf;
                g_objs[tid].type = OBJ_XDG_TOPLEVEL;
                g_objs[tid].version = o->version;
                g_objs[tid].u.link_id = sid;
                s->xdg_top_id = tid;
                s->is_toplevel = 1;
                if (!g_top_surf_id || g_objs[g_top_surf_id].type != OBJ_SURFACE) {
                    g_top_surf_id = sid;
                    send_kbd_enter_if_needed(cfd, ks);
                }
            }
        } else if (op == 2 && nargs >= 1) {
            if (args[0] < MAX_OBJS) {
                g_objs[args[0]].type = OBJ_XDG_POPUP;
                g_objs[args[0]].version = o->version;
            }
        } else if (op == 4) { // ack_configure
            uint32_t sid = o->u.link_id;
            if (sid < MAX_OBJS && g_objs[sid].type == OBJ_SURFACE) {
                g_objs[sid].u.surf->configured = 1;
                send_kbd_enter_if_needed(cfd, ks);
            }
        }
        break;

    case OBJ_XDG_TOPLEVEL:
    case OBJ_XDG_POPUP:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_OUTPUT:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_SEAT:
        if (op == 0 && nargs >= 1 && args[0] < MAX_OBJS) { // get_pointer
            g_objs[args[0]].type = OBJ_POINTER;
            g_objs[args[0]].version = o->version;
            g_ptr_id = args[0];
            g_ptr_entered = 0;
        } else if (op == 1 && nargs >= 1 && args[0] < MAX_OBJS) { // get_keyboard
            g_objs[args[0]].type = OBJ_KEYBOARD;
            g_objs[args[0]].version = o->version;
            g_kbd_id = args[0];
            g_kbd_entered = 0;
            send_kbd_init_events(cfd, g_kbd_id, o->version, ks);
        } else if (op == 2 && nargs >= 1 && args[0] < MAX_OBJS) { // get_touch
            g_objs[args[0]].type = OBJ_TOUCH;
            g_objs[args[0]].version = o->version;
            g_touch_id = args[0];
        } else if (op == 3) { // release
            destroy_obj(cfd, id);
        }
        break;

    case OBJ_POINTER:
        if (op == 1) { // release() (since v3); op 0 = set_cursor -> no-op
            if (g_ptr_id == id) {
                g_ptr_id = 0;
                g_ptr_entered = 0;
            }
            destroy_obj(cfd, id);
        }
        break;

    case OBJ_KEYBOARD:
        if (op == 0) { // release()
            if (g_kbd_id == id) {
                g_kbd_id = 0;
                g_kbd_entered = 0;
            }
            destroy_obj(cfd, id);
        }
        break;

    case OBJ_TOUCH:
        if (op == 0) { // release()
            if (g_touch_id == id) g_touch_id = 0;
            destroy_obj(cfd, id);
        }
        break;

    case OBJ_SINGLE_PIXEL_MGR:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 5 && args[0] < MAX_OBJS) {
            g_objs[args[0]].type = OBJ_SINGLE_PIXEL_BUF;
            g_objs[args[0]].version = o->version;
        }
        break;

    case OBJ_SINGLE_PIXEL_BUF:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_VIEWPORTER:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 2 && args[0] < MAX_OBJS) {
            g_objs[args[0]].type = OBJ_VIEWPORT;
            g_objs[args[0]].version = o->version;
        }
        break;

    case OBJ_VIEWPORT:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_PTR_CONSTRAINTS:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 5 && args[0] < MAX_OBJS) g_objs[args[0]].type = OBJ_LOCKED_PTR;
        else if (op == 2 && nargs >= 5 && args[0] < MAX_OBJS) g_objs[args[0]].type = OBJ_CONFINED_PTR;
        break;

    case OBJ_LOCKED_PTR:
    case OBJ_CONFINED_PTR:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_REL_PTR_MGR:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 2 && args[0] < MAX_OBJS) g_objs[args[0]].type = OBJ_REL_PTR;
        break;

    case OBJ_REL_PTR:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_PRESENTATION:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 2) {
            uint32_t sid = args[0], fid = args[1];
            if (sid < MAX_OBJS && g_objs[sid].type == OBJ_SURFACE && g_objs[sid].u.surf) {
                struct wl_surf *s = g_objs[sid].u.surf;
                if (s->n_pres_fbs < MAX_CBS) s->pres_fbs[s->n_pres_fbs++] = fid;
            } else {
                send_presentation_presented(cfd, fid);
            }
        }
        break;

    case OBJ_TOPLEVEL_ICON_MGR:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 1 && args[0] < MAX_OBJS) g_objs[args[0]].type = OBJ_TOPLEVEL_ICON;
        break;

    case OBJ_TOPLEVEL_ICON:
        if (op == 0) destroy_obj(cfd, id);
        break;

    case OBJ_SHORTCUTS_INHIBIT_MGR:
        if (op == 0) destroy_obj(cfd, id);
        else if (op == 1 && nargs >= 3 && args[0] < MAX_OBJS) {
            uint32_t iid = args[0];
            g_objs[iid].type = OBJ_SHORTCUTS_INHIBITOR;
            struct msg_builder m;
            mb_init(&m, iid);
            mb_send(&m, cfd, 0); // active()
        }
        break;

    case OBJ_SHORTCUTS_INHIBITOR:
        if (op == 0) destroy_obj(cfd, id);
        break;

    default:
        break;
    }
}

static __attribute__((unused)) int is_excluded_kbd_name(const char *name) {
    if (!name || !*name) return 0;
    // Strictly exclude pmic_pwrkey (owned by nx769j-powerkey), touchscreen, headset jack, and IMU
    if (strcasestr(name, "pwrkey") != NULL) return 1;
    if (strcasestr(name, "Goodix") != NULL) return 1;
    if (strcasestr(name, "TouchScreen") != NULL) return 1;
    if (strcasestr(name, "Headset") != NULL) return 1;
    if (strcasestr(name, "Jack") != NULL) return 1;
    if (strcasestr(name, "lsm6dso") != NULL) return 1;
    if (strcasestr(name, "imu") != NULL) return 1;
    return 0;
}

static void try_open_touchscreen(struct mt_state *mt) {
    if (mt->fd >= 0) return;
    const char *override_dev = getenv("NX769J_TOUCH_DEV");
    if (override_dev && *override_dev) {
        int fd = open(override_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            mt->fd = fd;
            mt->ptr_slot = -1;
            mt->max_x = g_fb_w * 16 - 1;
            mt->max_y = g_fb_h * 16 - 1;
            fprintf(stderr, "[nx769j-fb-wl] opened synthetic touch dev %s eff_max=(%d,%d) slop_px=%d\n",
                    override_dev, mt->max_x, mt->max_y, mt->slop_px);
        }
        return;
    }
#ifdef __linux__
    for (int i = 0; i < 24; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
        memset(absbits, 0, sizeof(absbits));
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
            int has_mt_x = (absbits[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long)))) & 1UL;
            if (has_mt_x) {
                struct input_absinfo ax, ay;
                memset(&ax, 0, sizeof(ax));
                memset(&ay, 0, sizeof(ay));
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
                char name[128] = "unknown";
                ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
                mt->fd = fd;
                mt->ptr_slot = -1;
                if (ax.maximum == 65535 && ay.maximum == 65535) {
                    mt->max_x = g_fb_w * 16 - 1;
                    mt->max_y = g_fb_h * 16 - 1;
                } else {
                    mt->max_x = ax.maximum > 0 ? ax.maximum : (g_fb_w - 1);
                    mt->max_y = ay.maximum > 0 ? ay.maximum : (g_fb_h - 1);
                }
                resync_touchscreen_state(-1, mt);
                memset(mt->slot_suppressed, 0, sizeof(mt->slot_suppressed));
                fprintf(stderr, "[nx769j-fb-wl] opened touchscreen %s (%s) raw_max=(%d,%d) eff_max=(%d,%d) slop_px=%d\n",
                        path, name, ax.maximum, ay.maximum, mt->max_x, mt->max_y, mt->slop_px);
                return;
            }
        }
        close(fd);
    }
#endif
}

static void try_open_keyboards(struct kbd_state *ks) {
    if (!ks) return;
    const char *override_dev = getenv("NX769J_KBD_DEV");
    if (override_dev && *override_dev) {
        if (ks->n_fds == 0) {
            int fd = open(override_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                const char *vup_env = getenv("NX769J_KBD_HAS_VOLUP");
                const char *vdn_env = getenv("NX769J_KBD_HAS_VOLDOWN");
                ks->fds[0] = fd;
                ks->dev_nums[0] = -2;
                ks->fd_has_volup[0] = (vup_env && *vup_env == '1') ? 1 : 0;
                ks->fd_has_voldown[0] = (vdn_env && *vdn_env == '1') ? 1 : 0;
                ks->has_evdev_volup = ks->fd_has_volup[0];
                ks->has_evdev_voldown = ks->fd_has_voldown[0];
                ks->n_fds = 1;
                fprintf(stderr, "[nx769j-fb-wl] opened synthetic keyboard dev %s (volup=%d voldown=%d)\n",
                        override_dev, ks->has_evdev_volup, ks->has_evdev_voldown);
            }
        }
        return;
    }
#ifdef __linux__
    for (int i = 0; i < 24 && ks->n_fds < MAX_KBDS; i++) {
        int already = 0;
        for (int j = 0; j < ks->n_fds; j++) {
            if (ks->dev_nums[j] == i) { already = 1; break; }
        }
        if (already) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        char name[128] = "";
        (void)ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        if (is_excluded_kbd_name(name)) {
            close(fd);
            continue;
        }

        unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
        memset(absbits, 0, sizeof(absbits));
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
            int has_mt_x = (absbits[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long)))) & 1UL;
            if (has_mt_x) { close(fd); continue; }
        }

        unsigned long keybits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
        memset(keybits, 0, sizeof(keybits));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
            close(fd);
            continue;
        }

        #define HAS_KBIT(k) ((keybits[(k) / (8 * sizeof(long))] >> ((k) % (8 * sizeof(long)))) & 1UL)
        int has_vup = HAS_KBIT(KEY_VOLUMEUP) ? 1 : 0;
        int has_vdn = HAS_KBIT(KEY_VOLUMEDOWN) ? 1 : 0;
        int useful = HAS_KBIT(KEY_A) || HAS_KBIT(KEY_ENTER) || HAS_KBIT(KEY_ESC) ||
                     has_vdn || has_vup;
        #undef HAS_KBIT

        if (!useful) {
            close(fd);
            continue;
        }

        int idx = ks->n_fds++;
        ks->fds[idx] = fd;
        ks->dev_nums[idx] = i;
        ks->fd_has_volup[idx] = has_vup;
        ks->fd_has_voldown[idx] = has_vdn;
        if (has_vup) ks->has_evdev_volup = 1;
        if (has_vdn) ks->has_evdev_voldown = 1;
        fprintf(stderr, "[nx769j-fb-wl] opened keyboard/key input %s (%s) [volup=%d voldown=%d]\n",
                path, name, has_vup, has_vdn);
    }
#endif
}

static void refresh_kbd_vol_caps(struct kbd_state *ks) {
    if (!ks) return;
    int vup = 0, vdn = 0;
    for (int i = 0; i < ks->n_fds; i++) {
        if (ks->fds[i] >= 0) {
            if (ks->fd_has_volup[i]) vup = 1;
            if (ks->fd_has_voldown[i]) vdn = 1;
        }
    }
    ks->has_evdev_volup = vup;
    ks->has_evdev_voldown = vdn;
}

static void feed_kbd_input_event(int cfd, struct kbd_state *ks, const struct input_event *ev) {
    if (!ks || ev->type != EV_KEY) return;
    if (ev->code == KEY_POWER) return; // safety belt: never forward KEY_POWER
    if (ev->value != 0 && ev->value != 1) return; // ignore kernel repeat=2 (Wayland uses repeat_info)

    int pressed = (ev->value == 1) ? 1 : 0;
    int mod_changed = 0;

    switch (ev->code) {
    case KEY_LEFTSHIFT:  if (ks->lshift != pressed) { ks->lshift = pressed; mod_changed = 1; } break;
    case KEY_RIGHTSHIFT: if (ks->rshift != pressed) { ks->rshift = pressed; mod_changed = 1; } break;
    case KEY_LEFTCTRL:   if (ks->lctrl  != pressed) { ks->lctrl  = pressed; mod_changed = 1; } break;
    case KEY_RIGHTCTRL:  if (ks->rctrl  != pressed) { ks->rctrl  = pressed; mod_changed = 1; } break;
    case KEY_LEFTALT:    if (ks->lalt   != pressed) { ks->lalt   = pressed; mod_changed = 1; } break;
    case KEY_RIGHTALT:   if (ks->ralt   != pressed) { ks->ralt   = pressed; mod_changed = 1; } break;
    case KEY_LEFTMETA:   if (ks->lmeta  != pressed) { ks->lmeta  = pressed; mod_changed = 1; } break;
    case KEY_RIGHTMETA:  if (ks->rmeta  != pressed) { ks->rmeta  = pressed; mod_changed = 1; } break;
    case KEY_CAPSLOCK:
        if (pressed) { ks->capslock ^= 1; mod_changed = 1; }
        break;
    default:
        break;
    }

    if (g_screen_off || cfd < 0 || !g_kbd_id || !g_top_surf_id) return;
    if (ev->code == KEY_VOLUMEUP || ev->code == KEY_VOLUMEDOWN || ev->code == KEY_MUTE) {
        ks->total_vol_events++;
    }
    send_kbd_enter_if_needed(cfd, ks);

    struct msg_builder m;
    mb_init(&m, g_kbd_id);
    mb_u32(&m, g_serial++);
    mb_u32(&m, now_ms());
    mb_u32(&m, (uint32_t)ev->code);
    mb_u32(&m, pressed ? 1u : 0u);
    mb_send(&m, cfd, 3); // wl_keyboard.key

    if (mod_changed) {
        uint32_t dep = 0;
        uint32_t lck = ks->capslock ? 2u : 0u;
        if (ks->lshift || ks->rshift) dep |= 1u;
        if (ks->lctrl  || ks->rctrl)  dep |= 4u;
        if (ks->lalt   || ks->ralt)   dep |= 8u;
        if (ks->lmeta  || ks->rmeta)  dep |= 64u;

        mb_init(&m, g_kbd_id);
        mb_u32(&m, g_serial++);
        mb_u32(&m, dep);
        mb_u32(&m, 0);
        mb_u32(&m, lck);
        mb_u32(&m, 0);
        mb_send(&m, cfd, 4); // wl_keyboard.modifiers
    }
}

static void process_kbd_events(int cfd, struct kbd_state *ks, int idx) {
    if (!ks || idx < 0 || idx >= ks->n_fds) return;
    int fd = ks->fds[idx];
    struct input_event ev[64];
    while (fd >= 0) {
        ssize_t n = read(fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            close(fd);
            ks->fds[idx] = ks->fds[ks->n_fds - 1];
            ks->dev_nums[idx] = ks->dev_nums[ks->n_fds - 1];
            ks->fd_has_volup[idx] = ks->fd_has_volup[ks->n_fds - 1];
            ks->fd_has_voldown[idx] = ks->fd_has_voldown[ks->n_fds - 1];
            ks->n_fds--;
            refresh_kbd_vol_caps(ks);
            break;
        }
        if (n == 0) {
            if (ks->dev_nums[idx] == -2) break; // synthetic EOF in test
            close(fd);
            ks->fds[idx] = ks->fds[ks->n_fds - 1];
            ks->dev_nums[idx] = ks->dev_nums[ks->n_fds - 1];
            ks->fd_has_volup[idx] = ks->fd_has_volup[ks->n_fds - 1];
            ks->fd_has_voldown[idx] = ks->fd_has_voldown[ks->n_fds - 1];
            ks->n_fds--;
            refresh_kbd_vol_caps(ks);
            break;
        }
        int cnt = (int)(n / (ssize_t)sizeof(ev[0]));
        for (int i = 0; i < cnt; i++) {
            if (ev[i].type == EV_KEY && ev[i].code == KEY_VOLUMEDOWN &&
                (ev[i].value == 0 || ev[i].value == 1)) {
                ks->evdev_voldown_events++;
            }
            feed_kbd_input_event(cfd, ks, &ev[i]);
        }
    }
}


static void init_hw_vol_state(struct hw_vol_state *hv) {
    if (!hv) return;
    memset(hv, 0, sizeof(*hv));
    hv->volup_line_fd = -1;
    hv->volup_mock_fd = -1;
    hv->resin_regmap_fd = -1;
    hv->resin_regmap_off = (off_t)-1;
    snprintf(hv->volup_src, sizeof(hv->volup_src), "none");
    snprintf(hv->voldown_src, sizeof(hv->voldown_src), "none");
}

static void emit_hw_vol_key(int cfd, struct kbd_state *ks, uint16_t code, int pressed) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = pressed ? 1 : 0;
    feed_kbd_input_event(cfd, ks, &ev);
}

static void release_held_hw_vol_keys(int cfd, struct kbd_state *ks, struct hw_vol_state *hv) {
    if (!hv) return;
    if (hv->volup_pressed) {
        hv->volup_pressed = 0;
        hv->volup_raw_prev = 0;
        emit_hw_vol_key(cfd, ks, KEY_VOLUMEUP, 0);
    }
    if (hv->voldown_pressed) {
        hv->voldown_pressed = 0;
        hv->voldown_raw_prev = 0;
        emit_hw_vol_key(cfd, ks, KEY_VOLUMEDOWN, 0);
    }
}

static __attribute__((unused)) int is_pm8550_volup_gpiochip_path(const char *real_buf) {
    if (!real_buf || !*real_buf) return 0;
    // Strictly match ONLY PM8550 (spmi@1 / 0-01) gpio@8800 — NEVER pmic@7 (PM8550B) or pmic@8 (PM8550VE)!
    if (strstr(real_buf, "pmic@1:gpio@8800") != NULL) return 1;
    if (strstr(real_buf, "/0-01/") != NULL && strstr(real_buf, "gpio@8800") != NULL) return 1;
    return 0;
}

static int read_volup_raw_level(struct hw_vol_state *hv) {
    if (!hv) return -1;
    if (hv->volup_mock_fd >= 0) {
        char buf[16];
        ssize_t n = pread(hv->volup_mock_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return -1;
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '1') return 1;
            if (buf[i] == '0') return 0;
        }
        return -1;
    }
#ifdef __linux__
    if (hv->volup_line_fd >= 0) {
        struct gpio_v2_line_values vals;
        memset(&vals, 0, sizeof(vals));
        vals.mask = 1ULL;
        if (ioctl(hv->volup_line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) == 0) {
            return (vals.bits & 1ULL) ? 1 : 0;
        }
    }
#endif
    return -1;
}

static void drain_volup_gpio_events(int cfd, struct kbd_state *ks, struct hw_vol_state *hv) {
#ifdef __linux__
    if (!hv || hv->volup_line_fd < 0) return;
    struct gpio_v2_line_event gev[16];
    for (;;) {
        ssize_t n = read(hv->volup_line_fd, gev, sizeof(gev));
        if (n <= 0) break;
    }
    int raw = read_volup_raw_level(hv);
    if (raw >= 0) {
        hv->volup_raw_prev = raw;
        if (raw != hv->volup_pressed) {
            hv->volup_pressed = raw;
            emit_hw_vol_key(cfd, ks, KEY_VOLUMEUP, raw);
        }
    }
#else
    (void)cfd;
    (void)ks;
    (void)hv;
#endif
}

static void sync_hw_volup_source(int cfd, struct kbd_state *ks, struct hw_vol_state *hv) {
    if (!ks || !hv) return;

    const char *mock_path = getenv("NX769J_VOLUP_MOCK_FILE");
    const char *force_skip_evdev = getenv("NX769J_VOLUP_FORCE_MOCK");
    if (ks->has_evdev_volup && !(mock_path && *mock_path && force_skip_evdev && *force_skip_evdev == '1')) {
        if (hv->volup_pressed) {
            hv->volup_pressed = 0;
            hv->volup_raw_prev = 0;
            emit_hw_vol_key(cfd, ks, KEY_VOLUMEUP, 0);
        }
        if (hv->volup_line_fd >= 0) {
            close(hv->volup_line_fd);
            hv->volup_line_fd = -1;
            fprintf(stderr, "[nx769j-fb-wl] released direct gpio6 line (evdev KEY_VOLUMEUP active)\n");
        }
        if (hv->volup_mock_fd >= 0) {
            close(hv->volup_mock_fd);
            hv->volup_mock_fd = -1;
        }
        snprintf(hv->volup_src, sizeof(hv->volup_src), "evdev");
        return;
    }

    if (mock_path && *mock_path) {
        if (strcmp(mock_path, "none") == 0) {
            snprintf(hv->volup_src, sizeof(hv->volup_src), "none");
            return;
        }
        if (hv->volup_mock_fd < 0) {
            int fd = open(mock_path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                hv->volup_mock_fd = fd;
                int init_v = read_volup_raw_level(hv);
                hv->volup_raw_prev = (init_v > 0) ? init_v : 0;
                hv->volup_pressed = 0;
                snprintf(hv->volup_src, sizeof(hv->volup_src), "mock");
                fprintf(stderr, "[nx769j-fb-wl] opened mock Volume+ file %s\n", mock_path);
            }
        }
        return;
    }

#ifdef __linux__
    if (hv->volup_line_fd >= 0) return;

    DIR *d = opendir("/sys/bus/gpio/devices");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "gpiochip", 8) != 0) continue;
        char sys_path[256], real_buf[512];
        snprintf(sys_path, sizeof(sys_path), "/sys/bus/gpio/devices/%.64s", de->d_name);
        if (!realpath(sys_path, real_buf)) continue;
        // Factory DT proves Volume+ is PM8550 (spmi@1) gpio@8800 pin "gpio6" (0-based offset 5)
        if (!is_pm8550_volup_gpiochip_path(real_buf)) {
            continue;
        }

        char dev_path[128];
        snprintf(dev_path, sizeof(dev_path), "/dev/%.64s", de->d_name);
        int cfd_gpio = open(dev_path, O_RDONLY | O_CLOEXEC);
        if (cfd_gpio < 0) continue;

        int found_line = -1;
        for (uint32_t off = 0; off < 16; off++) {
            struct gpio_v2_line_info linfo;
            memset(&linfo, 0, sizeof(linfo));
            linfo.offset = off;
            if (ioctl(cfd_gpio, GPIO_V2_GET_LINEINFO_IOCTL, &linfo) < 0) break;
            if (strcmp(linfo.name, "gpio6") == 0) {
                if (!(linfo.flags & GPIO_V2_LINE_FLAG_USED)) {
                    found_line = (int)off;
                }
                break;
            }
        }
        if (found_line < 0) {
            // Fallback to 0-based offset 5 (PMIC_GPIO_PHYSICAL_OFFSET=1 -> gpio6 is offset 5)
            struct gpio_v2_line_info linfo5;
            memset(&linfo5, 0, sizeof(linfo5));
            linfo5.offset = 5;
            if (ioctl(cfd_gpio, GPIO_V2_GET_LINEINFO_IOCTL, &linfo5) == 0 &&
                !(linfo5.flags & GPIO_V2_LINE_FLAG_USED)) {
                found_line = 5;
            }
        }

        if (found_line >= 0) {
            struct gpio_v2_line_request req;
            uint64_t try_flags[2] = {
                GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW |
                    GPIO_V2_LINE_FLAG_BIAS_PULL_UP | GPIO_V2_LINE_FLAG_EDGE_BOTH,
                GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW |
                    GPIO_V2_LINE_FLAG_BIAS_PULL_UP,
            };
            for (int a = 0; a < 2; a++) {
                memset(&req, 0, sizeof(req));
                req.offsets[0] = (uint32_t)found_line;
                req.num_lines = 1;
                snprintf(req.consumer, sizeof(req.consumer), "nx769j-fb-wl-volup");
                req.config.flags = try_flags[a];
                req.fd = -1;
                if (ioctl(cfd_gpio, GPIO_V2_GET_LINE_IOCTL, &req) == 0 && req.fd >= 0) {
                    int fl = fcntl(req.fd, F_GETFL, 0);
                    if (fl >= 0) (void)fcntl(req.fd, F_SETFL, fl | O_NONBLOCK);
                    int fdfl = fcntl(req.fd, F_GETFD, 0);
                    if (fdfl >= 0) (void)fcntl(req.fd, F_SETFD, fdfl | FD_CLOEXEC);
                    hv->volup_line_fd = req.fd;
                    hv->volup_line_offset = (uint32_t)found_line;
                    int init_v = read_volup_raw_level(hv);
                    hv->volup_raw_prev = (init_v > 0) ? init_v : 0;
                    hv->volup_pressed = 0;
                    snprintf(hv->volup_src, sizeof(hv->volup_src), "gpio6");
                    fprintf(stderr, "[nx769j-fb-wl] opened Volume+ PM8550 %s line=%d (gpio6, flags=0x%llx)\n",
                            dev_path, found_line, (unsigned long long)try_flags[a]);
                    break;
                }
            }
        }
        close(cfd_gpio);
        if (hv->volup_line_fd >= 0) break;
    }
    closedir(d);
#endif
}

static int parse_hex_byte2(const char *p) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

static int read_resin_regmap_raw_level(struct hw_vol_state *hv) {
    if (!hv || hv->resin_regmap_fd < 0 || hv->resin_regmap_off < 0) return -1;
    char buf[24];
    ssize_t n = pread(hv->resin_regmap_fd, buf, 16, hv->resin_regmap_off);
    if (n < 8) return -1;
    buf[n] = '\0';
    const char *hit = strstr(buf, "1310: ");
    if (!hit || (hit - buf) + 8 > n) return -1;
    int sts = parse_hex_byte2(hit + 6);
    if (sts < 0) return -1;
    // PMK8550 pon@1300 PON_INT_RT_STS (0x1310) bit 6 (0x40) = PON_GEN3_RESIN_N_SET (1 = pressed)
    return (sts & 0x40) ? 1 : 0;
}

static void sync_hw_voldown_source(int cfd, struct kbd_state *ks, struct hw_vol_state *hv) {
    if (!ks || !hv) return;

    if (ks->evdev_voldown_events > 0) {
        if (hv->voldown_pressed) {
            hv->voldown_pressed = 0;
            hv->voldown_raw_prev = 0;
            emit_hw_vol_key(cfd, ks, KEY_VOLUMEDOWN, 0);
        }
        if (hv->resin_regmap_fd >= 0) {
            close(hv->resin_regmap_fd);
            hv->resin_regmap_fd = -1;
            hv->resin_regmap_off = (off_t)-1;
        }
        snprintf(hv->voldown_src, sizeof(hv->voldown_src), "evdev");
        return;
    }

    if (hv->resin_regmap_fd >= 0 || hv->resin_probed_unavail) {
        if (hv->resin_regmap_fd < 0 && ks->has_evdev_voldown) {
            snprintf(hv->voldown_src, sizeof(hv->voldown_src), "evdev");
        }
        return;
    }

    const char *regmap_path = getenv("NX769J_RESIN_REGMAP_FILE");
    int is_mock = 0;
    if (regmap_path && *regmap_path) {
        if (strcmp(regmap_path, "none") == 0) {
            hv->resin_probed_unavail = 1;
            snprintf(hv->voldown_src, sizeof(hv->voldown_src),
                     ks->has_evdev_voldown ? "evdev" : "none");
            return;
        }
        is_mock = 1;
    } else {
#ifdef __linux__
        regmap_path = DEFAULT_RESIN_REGMAP_PATH;
#else
        hv->resin_probed_unavail = 1;
        return;
#endif
    }

    int fd = open(regmap_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (ks->has_evdev_voldown) {
            snprintf(hv->voldown_src, sizeof(hv->voldown_src), "evdev");
        }
        return;
    }

    // Scan once up to "1310: " to record exact byte offset and prime kernel regmap-debugfs O(1) cache
    char scan[4096];
    off_t cur_off = 0;
    off_t found_off = (off_t)-1;
    char tail[16];
    size_t tail_len = 0;

    while (cur_off < 65536) {
        ssize_t n = pread(fd, scan + tail_len, sizeof(scan) - tail_len - 1, cur_off);
        if (n <= 0) break;
        size_t total = tail_len + (size_t)n;
        scan[total] = '\0';
        char *m = strstr(scan, "1310: ");
        if (m) {
            found_off = (cur_off - (off_t)tail_len) + (off_t)(m - scan);
            break;
        }
        cur_off += (off_t)n;
        tail_len = total >= 8 ? 8 : total;
        memcpy(tail, scan + total - tail_len, tail_len);
        memcpy(scan, tail, tail_len);
    }

    if (found_off >= 0) {
        hv->resin_regmap_fd = fd;
        hv->resin_regmap_off = found_off;
        hv->resin_is_mock = is_mock;
        int init_v = read_resin_regmap_raw_level(hv);
        hv->voldown_raw_prev = (init_v > 0) ? init_v : 0;
        hv->voldown_pressed = 0;
        snprintf(hv->voldown_src, sizeof(hv->voldown_src), is_mock ? "mock" : "regmap");
        fprintf(stderr, "[nx769j-fb-wl] armed Volume- PON 0x1310 O(1) regmap fallback (%s @ off=%lld)\n",
                regmap_path, (long long)found_off);
    } else {
        close(fd);
        hv->resin_probed_unavail = 1;
        if (ks->has_evdev_voldown) {
            snprintf(hv->voldown_src, sizeof(hv->voldown_src), "evdev");
        }
    }
}

static void poll_hw_vol_keys(int cfd, struct kbd_state *ks, struct hw_vol_state *hv) {
    if (g_screen_off || !ks || !hv) return;

    int raw_up = read_volup_raw_level(hv);
    if (raw_up >= 0) {
        if (raw_up == hv->volup_raw_prev && raw_up != hv->volup_pressed) {
            hv->volup_pressed = raw_up;
            emit_hw_vol_key(cfd, ks, KEY_VOLUMEUP, raw_up);
        }
        hv->volup_raw_prev = raw_up;
    }

    if (ks->evdev_voldown_events > 0 && hv->resin_regmap_fd >= 0) {
        sync_hw_voldown_source(cfd, ks, hv);
    } else if (hv->resin_regmap_fd >= 0) {
        int raw_dn = read_resin_regmap_raw_level(hv);
        if (raw_dn >= 0) {
            if (raw_dn == hv->voldown_raw_prev && raw_dn != hv->voldown_pressed) {
                hv->voldown_pressed = raw_dn;
                emit_hw_vol_key(cfd, ks, KEY_VOLUMEDOWN, raw_dn);
            }
            hv->voldown_raw_prev = raw_dn;
        }
    }
}

static int32_t scale_wl_fixed(int val, int max_v, int screen_px) {
    if (screen_px <= 1 || val <= 0) return 0;
    if (max_v <= 0) max_v = screen_px - 1;
    // Goodix GT9916R reports 0..(screen_px * 16 - 1) (17855 x 39679), i.e. exact 1/16 px units.
    int64_t denom = ((max_v + 1) == screen_px * 16 || (max_v + 1) == screen_px)
                        ? (int64_t)(max_v + 1)
                        : (int64_t)max_v;
    int64_t f = ((int64_t)val * (int64_t)screen_px * 256LL) / denom;
    int64_t lim = (int64_t)screen_px * 256LL - 16LL; // 1115.9375 px, strictly < screen_px
    if (lim < 0) lim = 0;
    if (f < 0) f = 0;
    if (f > lim) f = lim;
    return (int32_t)f;
}

static void transform_touch_to_wl_fixed(int raw_x, int raw_y, int max_x, int max_y,
                                        int fb_w, int fb_h, enum screen_rot rot,
                                        int32_t *out_wx, int32_t *out_wy) {
    int32_t px = scale_wl_fixed(raw_x, max_x, fb_w);
    int32_t py = scale_wl_fixed(raw_y, max_y, fb_h);
    int32_t max_px = scale_wl_fixed(max_x > 0 ? max_x : (fb_w - 1), max_x, fb_w);
    int32_t max_py = scale_wl_fixed(max_y > 0 ? max_y : (fb_h - 1), max_y, fb_h);

    switch (rot) {
    case ROT_NORMAL:
        *out_wx = px;
        *out_wy = py;
        break;
    case ROT_RIGHT_UP:
        *out_wx = py;
        *out_wy = max_px - px;
        break;
    case ROT_LEFT_UP:
        *out_wx = max_py - py;
        *out_wy = px;
        break;
    case ROT_BOTTOM_UP:
        *out_wx = max_px - px;
        *out_wy = max_py - py;
        break;
    default:
        *out_wx = px;
        *out_wy = py;
        break;
    }
}

static int touch_exceeds_slop(const struct mt_state *mt, int s) {
    if (!mt || mt->slop_px <= 0) return 1;
    int64_t denom_x = (mt->max_x > 0)
                          ? (((mt->max_x + 1) == g_fb_w * 16 || (mt->max_x + 1) == g_fb_w) ? (mt->max_x + 1) : mt->max_x)
                          : g_fb_w;
    int64_t denom_y = (mt->max_y > 0)
                          ? (((mt->max_y + 1) == g_fb_h * 16 || (mt->max_y + 1) == g_fb_h) ? (mt->max_y + 1) : mt->max_y)
                          : g_fb_h;
    int64_t dx16 = (((int64_t)mt->x[s] - (int64_t)mt->down_x[s]) * (int64_t)g_fb_w * 16LL) / denom_x;
    int64_t dy16 = (((int64_t)mt->y[s] - (int64_t)mt->down_y[s]) * (int64_t)g_fb_h * 16LL) / denom_y;
    int64_t thresh16 = (int64_t)mt->slop_px * 16LL;
    return (dx16 * dx16 + dy16 * dy16) >= (thresh16 * thresh16);
}

static void emit_mt_frame_to_wayland(int cfd, struct mt_state *mt, uint32_t t) {
    static uint64_t touch_log_cnt = 0;

    for (int s = 0; s < MAX_SLOTS; s++) {
        if (!mt->active[s]) {
            mt->slot_suppressed[s] = 0;
        }
    }

    if (g_screen_off || cfd < 0 || !g_top_surf_id || (!g_touch_id && !g_ptr_id)) {
        if (g_screen_off) {
            mt->ptr_slot = -1;
            memset(mt->wl_touch_down, 0, sizeof(mt->wl_touch_down));
        }
        for (int s = 0; s < MAX_SLOTS; s++) {
            mt->prev_active[s] = mt->active[s];
            mt->pending_down[s] = 0;
            mt->moved[s] = 0;
        }
        return;
    }

    // Primary Path: Native 10-slot wl_touch when client bound wl_seat.get_touch
    if (g_touch_id != 0) {
        int emitted = 0;
        for (int s = 0; s < MAX_SLOTS; s++) {
            if (mt->active[s] && !mt->prev_active[s] && !mt->slot_suppressed[s]) {
                int32_t wx = 0, wy = 0;
                transform_touch_to_wl_fixed(mt->x[s], mt->y[s], mt->max_x, mt->max_y,
                                            g_fb_w, g_fb_h, g_rot, &wx, &wy);
                struct msg_builder m;
                mb_init(&m, g_touch_id);
                mb_u32(&m, g_serial++);
                mb_u32(&m, t);
                mb_u32(&m, g_top_surf_id);
                mb_i32(&m, s);
                mb_i32(&m, wx);
                mb_i32(&m, wy);
                mb_send(&m, cfd, 0); // wl_touch.down
                mt->wl_touch_down[s] = 1;
                mt->down_x[s] = mt->x[s];
                mt->down_y[s] = mt->y[s];
                mt->drag_unlocked[s] = (mt->slop_px <= 0) ? 1 : 0;
                mt->last_wx[s] = wx;
                mt->last_wy[s] = wy;
                emitted = 1;
                if (touch_log_cnt++ < 40) {
                    fprintf(stderr, "[nx769j-fb-wl] wl_touch down slot=%d raw=(%d,%d) wl=(%d,%d) rot=%s\n",
                            s, mt->x[s], mt->y[s], wx / 256, wy / 256, rot_name(g_rot));
                }
            } else if (mt->active[s] && mt->wl_touch_down[s] && mt->moved[s]) {
                if (!mt->drag_unlocked[s] && touch_exceeds_slop(mt, s)) {
                    mt->drag_unlocked[s] = 1;
                }
                if (mt->drag_unlocked[s]) {
                    int32_t wx = 0, wy = 0;
                    transform_touch_to_wl_fixed(mt->x[s], mt->y[s], mt->max_x, mt->max_y,
                                                g_fb_w, g_fb_h, g_rot, &wx, &wy);
                    if (wx != mt->last_wx[s] || wy != mt->last_wy[s]) {
                        struct msg_builder m;
                        mb_init(&m, g_touch_id);
                        mb_u32(&m, t);
                        mb_i32(&m, s);
                        mb_i32(&m, wx);
                        mb_i32(&m, wy);
                        mb_send(&m, cfd, 2); // wl_touch.motion
                        mt->last_wx[s] = wx;
                        mt->last_wy[s] = wy;
                        emitted = 1;
                    }
                }
            } else if (!mt->active[s] && mt->wl_touch_down[s]) {
                struct msg_builder m;
                mb_init(&m, g_touch_id);
                mb_u32(&m, g_serial++);
                mb_u32(&m, t);
                mb_i32(&m, s);
                mb_send(&m, cfd, 1); // wl_touch.up
                mt->wl_touch_down[s] = 0;
                mt->drag_unlocked[s] = 0;
                emitted = 1;
                if (touch_log_cnt++ < 40) {
                    fprintf(stderr, "[nx769j-fb-wl] wl_touch up slot=%d rot=%s\n", s, rot_name(g_rot));
                }
            }
            mt->prev_active[s] = mt->active[s];
            mt->pending_down[s] = 0;
            mt->moved[s] = 0;
        }
        if (emitted) {
            struct msg_builder m;
            mb_init(&m, g_touch_id);
            mb_send(&m, cfd, 3); // wl_touch.frame
        }
        return;
    }

    // Fallback Path: Single-finger wl_pointer when client did NOT bind wl_touch
    if (mt->ptr_slot >= 0) {
        int ps = mt->ptr_slot;
        int32_t wx = 0, wy = 0;
        transform_touch_to_wl_fixed(mt->x[ps], mt->y[ps], mt->max_x, mt->max_y,
                                    g_fb_w, g_fb_h, g_rot, &wx, &wy);
        if (mt->active[ps]) {
            if (mt->moved[ps]) {
                if (!mt->drag_unlocked[ps] && touch_exceeds_slop(mt, ps)) {
                    mt->drag_unlocked[ps] = 1;
                }
                if (mt->drag_unlocked[ps] && (wx != mt->last_wx[ps] || wy != mt->last_wy[ps])) {
                    if (!g_ptr_entered) {
                        struct msg_builder m;
                        mb_init(&m, g_ptr_id);
                        mb_u32(&m, g_serial++);
                        mb_u32(&m, g_top_surf_id);
                        mb_i32(&m, wx);
                        mb_i32(&m, wy);
                        mb_send(&m, cfd, 0);
                        mb_init(&m, g_ptr_id);
                        mb_send(&m, cfd, 5);
                        g_ptr_entered = 1;
                    }
                    struct msg_builder m;
                    mb_init(&m, g_ptr_id);
                    mb_u32(&m, t);
                    mb_i32(&m, wx);
                    mb_i32(&m, wy);
                    mb_send(&m, cfd, 2);
                    mb_init(&m, g_ptr_id);
                    mb_send(&m, cfd, 5);
                    mt->last_wx[ps] = wx;
                    mt->last_wy[ps] = wy;
                }
            }
        } else {
            struct msg_builder m;
            mb_init(&m, g_ptr_id);
            mb_u32(&m, g_serial++);
            mb_u32(&m, t);
            mb_u32(&m, 0x110);
            mb_u32(&m, 0);
            mb_send(&m, cfd, 3);
            mb_init(&m, g_ptr_id);
            mb_send(&m, cfd, 5);
            mt->drag_unlocked[ps] = 0;
            mt->ptr_slot = -1;
        }
    }

    if (mt->ptr_slot < 0) {
        for (int s = 0; s < MAX_SLOTS; s++) {
            if (mt->active[s] && !mt->prev_active[s] && !mt->slot_suppressed[s]) {
                mt->ptr_slot = s;
                int32_t wx = 0, wy = 0;
                transform_touch_to_wl_fixed(mt->x[s], mt->y[s], mt->max_x, mt->max_y,
                                            g_fb_w, g_fb_h, g_rot, &wx, &wy);
                mt->down_x[s] = mt->x[s];
                mt->down_y[s] = mt->y[s];
                mt->drag_unlocked[s] = (mt->slop_px <= 0) ? 1 : 0;
                mt->last_wx[s] = wx;
                mt->last_wy[s] = wy;
                if (!g_ptr_entered) {
                    struct msg_builder m;
                    mb_init(&m, g_ptr_id);
                    mb_u32(&m, g_serial++);
                    mb_u32(&m, g_top_surf_id);
                    mb_i32(&m, wx);
                    mb_i32(&m, wy);
                    mb_send(&m, cfd, 0);
                    mb_init(&m, g_ptr_id);
                    mb_send(&m, cfd, 5);
                    g_ptr_entered = 1;
                }
                {
                    struct msg_builder m;
                    mb_init(&m, g_ptr_id);
                    mb_u32(&m, t);
                    mb_i32(&m, wx);
                    mb_i32(&m, wy);
                    mb_send(&m, cfd, 2);
                    mb_init(&m, g_ptr_id);
                    mb_send(&m, cfd, 5);
                }
                {
                    struct msg_builder m;
                    mb_init(&m, g_ptr_id);
                    mb_u32(&m, g_serial++);
                    mb_u32(&m, t);
                    mb_u32(&m, 0x110);
                    mb_u32(&m, 1);
                    mb_send(&m, cfd, 3);
                    mb_init(&m, g_ptr_id);
                    mb_send(&m, cfd, 5);
                }
                break;
            }
        }
    }

    for (int s = 0; s < MAX_SLOTS; s++) {
        mt->prev_active[s] = mt->active[s];
        mt->pending_down[s] = 0;
        mt->moved[s] = 0;
    }
}

static void feed_mt_input_event(int cfd, struct mt_state *mt, const struct input_event *ev) {
    int s = mt->cur_slot;
    if (s < 0 || s >= MAX_SLOTS) s = 0;

    if (ev->type == EV_ABS) {
        if (ev->code == ABS_MT_SLOT) {
            if (ev->value >= 0 && ev->value < MAX_SLOTS) mt->cur_slot = ev->value;
        } else if (ev->code == ABS_MT_TRACKING_ID) {
            if (ev->value < 0) {
                // Ultra-fast tap (0 -> 1 -> 0 within a single SYN_REPORT):
                // flush the pending down frame before marking the slot released!
                if (mt->pending_down[s] && mt->active[s]) {
                    emit_mt_frame_to_wayland(cfd, mt, now_ms());
                }
                mt->active[s] = 0;
                mt->tracking_id[s] = -1;
                mt->pending_down[s] = 0;
                mt->drag_unlocked[s] = 0;
            } else if (!(mt->prev_active[s] && mt->active[s] && mt->tracking_id[s] == ev->value)) {
                // Same-SYN_REPORT release + re-press (1 -> 0 -> 1) or direct tracking_id handoff:
                // flush wl_touch.up (or pointer release) cleanly before starting the new touch!
                if (mt->pending_down[s] && mt->active[s]) {
                    emit_mt_frame_to_wayland(cfd, mt, now_ms());
                    mt->active[s] = 0;
                    mt->tracking_id[s] = -1;
                    emit_mt_frame_to_wayland(cfd, mt, now_ms());
                } else if (mt->prev_active[s]) {
                    mt->active[s] = 0;
                    mt->tracking_id[s] = -1;
                    emit_mt_frame_to_wayland(cfd, mt, now_ms());
                }
                mt->active[s] = 1;
                mt->tracking_id[s] = ev->value;
                mt->pending_down[s] = 1;
                mt->drag_unlocked[s] = 0;
                mt->slot_suppressed[s] = 0;
            }
        } else if (ev->code == ABS_MT_POSITION_X) {
            mt->x[s] = ev->value;
            mt->moved[s] = 1;
        } else if (ev->code == ABS_MT_POSITION_Y) {
            mt->y[s] = ev->value;
            mt->moved[s] = 1;
        }
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        emit_mt_frame_to_wayland(cfd, mt, now_ms());
    }
}

static void process_touch_events(int cfd, struct mt_state *mt) {
    struct input_event ev[64];
    int is_synth = (getenv("NX769J_TOUCH_DEV") != NULL);
    while (mt->fd >= 0) {
        ssize_t n = read(mt->fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            close(mt->fd);
            mt->fd = -1;
            mt->ptr_slot = -1;
            memset(mt->wl_touch_down, 0, sizeof(mt->wl_touch_down));
            break;
        }
        if (n == 0) {
            if (!is_synth) {
                close(mt->fd);
                mt->fd = -1;
            }
            break;
        }
        int cnt = (int)(n / (ssize_t)sizeof(ev[0]));
        for (int i = 0; i < cnt; i++) {
            feed_mt_input_event(cfd, mt, &ev[i]);
        }
    }
}

#ifndef NX769J_FB_WL_TEST
int main(int argc, char **argv) {
    const char *sock_path = argc > 1 ? argv[1] : "/run/nx769j-wl/wayland-fb0";
    const char *fb_path = getenv("NX769J_FB_PATH");
    if (!fb_path || !*fb_path) fb_path = "/dev/fb0";

    const char *power_sock_path = getenv("NX769J_FB_POWER_SOCK");
    if (!power_sock_path || !*power_sock_path) {
        power_sock_path = "/run/nx769j-wl/power.sock";
    }
    const char *imu_env = getenv("NX769J_IMU_STATE_JSON");
    if (imu_env && *imu_env) {
        g_imu_state_path = imu_env;
    }
    const char *drm_env = getenv("NX769J_DRM_NODE");
    if (drm_env && *drm_env) {
        g_drm_node_path = drm_env;
    }

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

#ifdef __linux__
    if (!getenv("NX769J_NO_TTY")) {
        int tty_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (tty_fd >= 0) {
            ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
        }
    }
#endif

    int fb_fd = open(fb_path, O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) {
        perror("open fb0");
        return 1;
    }
#ifdef __linux__
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0 &&
        ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        g_fb_w = (int)vinfo.xres;
        g_fb_h = (int)vinfo.yres;
        g_fb_stride = (int)finfo.line_length;
        g_fb_len = finfo.smem_len ? finfo.smem_len : (size_t)g_fb_stride * (size_t)g_fb_h;
        g_fb_swap_rb = (vinfo.red.offset == 0 && vinfo.blue.offset == 16) ? 1 : 0;
        fprintf(stderr, "[nx769j-fb-wl] fb0 rgb offsets: r=%u g=%u b=%u swap_rb=%d\n",
                vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset, g_fb_swap_rb);
    } else
#endif
    {
        g_fb_len = (size_t)g_fb_stride * (size_t)g_fb_h;
    }
    g_fb_map = mmap(NULL, g_fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (g_fb_map == MAP_FAILED) {
        perror("mmap fb0");
        return 1;
    }
    g_saved_fb = calloc(1, g_fb_len);
    if (g_saved_fb) {
        memcpy(g_saved_fb, g_fb_map, g_fb_len);
    }
    fprintf(stderr, "[nx769j-fb-wl] mapped %s %dx%d stride=%d len=%zu drm=%s\n",
            fb_path, g_fb_w, g_fb_h, g_fb_stride, g_fb_len, g_drm_node_path);

    struct mt_state mt;
    init_mt_state(&mt);
    try_open_touchscreen(&mt);

    struct kbd_state ks;
    memset(&ks, 0, sizeof(ks));
    for (int i = 0; i < MAX_KBDS; i++) ks.fds[i] = -1;
    try_open_keyboards(&ks);

    struct hw_vol_state hv;
    init_hw_vol_state(&hv);
    sync_hw_volup_source(-1, &ks, &hv);
    sync_hw_voldown_source(-1, &ks, &hv);

    if (g_auto_rotate) {
        enum screen_rot init_rot = ROT_NORMAL;
        if (read_imu_orientation(&init_rot) == 0) {
            g_rot = init_rot;
        }
    }

    unlink(sock_path);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket wayland");
        return 1;
    }
    {
        int fl = fcntl(sfd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(sfd, F_SETFL, fl | O_NONBLOCK);
        int fdfl = fcntl(sfd, F_GETFD, 0);
        if (fdfl >= 0) (void)fcntl(sfd, F_SETFD, fdfl | FD_CLOEXEC);
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind wayland socket");
        return 1;
    }
    chmod(sock_path, 0777);
    listen(sfd, 4);

    unlink(power_sock_path);
    int ctrl_lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_lfd >= 0) {
        int fl = fcntl(ctrl_lfd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(ctrl_lfd, F_SETFL, fl | O_NONBLOCK);
        int fdfl = fcntl(ctrl_lfd, F_GETFD, 0);
        if (fdfl >= 0) (void)fcntl(ctrl_lfd, F_SETFD, fdfl | FD_CLOEXEC);

        struct sockaddr_un paddr;
        memset(&paddr, 0, sizeof(paddr));
        paddr.sun_family = AF_UNIX;
        snprintf(paddr.sun_path, sizeof(paddr.sun_path), "%s", power_sock_path);
        if (bind(ctrl_lfd, (struct sockaddr *)&paddr, sizeof(paddr)) < 0) {
            perror("bind power control socket");
            close(ctrl_lfd);
            ctrl_lfd = -1;
        } else {
            chmod(power_sock_path, 0666);
            listen(ctrl_lfd, 8);
            fprintf(stderr, "[nx769j-fb-wl] power control socket on %s\n", power_sock_path);
        }
    }
    fprintf(stderr, "[nx769j-fb-wl] listening on %s (initial orient=%s)\n",
            sock_path, rot_name(g_rot));

    while (1) {
        struct pollfd opfds[2];
        opfds[0].fd = sfd;
        opfds[0].events = POLLIN;
        opfds[0].revents = 0;
        int onpfds = 1;
        if (ctrl_lfd >= 0) {
            opfds[1].fd = ctrl_lfd;
            opfds[1].events = POLLIN;
            opfds[1].revents = 0;
            onpfds = 2;
        }
        int opr = poll(opfds, (nfds_t)onpfds, -1);
        if (opr < 0) {
            if (errno == EINTR) continue;
            usleep(10000);
            continue;
        }
        if (onpfds == 2 && (opfds[1].revents & POLLIN)) {
            handle_power_ctrl_accept(ctrl_lfd, -1, &mt, &ks, &hv);
        }
        if (!(opfds[0].revents & POLLIN)) {
            continue;
        }

        int cfd = accept_cloexec_blocking(sfd);
        if (cfd < 0) continue;
        fprintf(stderr, "[nx769j-fb-wl] client connected!\n");
        reset_client_state();
        memset(mt.wl_touch_down, 0, sizeof(mt.wl_touch_down));
        mt.ptr_slot = -1;
        if (!g_screen_off && g_auto_rotate) {
            enum screen_rot init_rot = g_rot;
            if (read_imu_orientation(&init_rot) == 0) {
                g_rot = init_rot;
            }
        }

        uint8_t rbuf[65536];
        size_t rlen = 0;
        int fds[MAX_FDS];
        int nfds = 0;
        uint32_t last_scan_ms = 0;
        uint32_t last_imu_ms = now_ms();
        uint32_t last_vol_ms = now_ms();

        while (1) {
            uint32_t now = now_ms();
            if (!g_screen_off && (now - last_scan_ms) >= 2000) {
                last_scan_ms = now;
                if (mt.fd < 0) try_open_touchscreen(&mt);
                try_open_keyboards(&ks);
                sync_hw_volup_source(cfd, &ks, &hv);
                sync_hw_voldown_source(cfd, &ks, &hv);
            }
            if (!g_screen_off && g_auto_rotate && (now - last_imu_ms) >= IMU_POLL_MS) {
                last_imu_ms = now;
                poll_imu_auto_rotate(now, cfd, &mt);
            }
            int need_vol_poll = (!g_screen_off &&
                                 (hv.volup_line_fd >= 0 || hv.volup_mock_fd >= 0 || hv.resin_regmap_fd >= 0));
            if (need_vol_poll && (now - last_vol_ms) >= HW_VOL_POLL_MS) {
                last_vol_ms = now;
                poll_hw_vol_keys(cfd, &ks, &hv);
            }

            struct pollfd pfds[4 + MAX_KBDS];
            pfds[0].fd = cfd;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            int npfds = 1;
            int mt_idx = -1;
            int ctrl_idx = -1;
            int volup_idx = -1;
            int kbd_base_idx = -1;
            int kbd_cnt = 0;

            if (mt.fd >= 0) {
                mt_idx = npfds++;
                pfds[mt_idx].fd = mt.fd;
                pfds[mt_idx].events = POLLIN;
                pfds[mt_idx].revents = 0;
            }
            if (ctrl_lfd >= 0) {
                ctrl_idx = npfds++;
                pfds[ctrl_idx].fd = ctrl_lfd;
                pfds[ctrl_idx].events = POLLIN;
                pfds[ctrl_idx].revents = 0;
            }
            if (!g_screen_off && hv.volup_line_fd >= 0) {
                volup_idx = npfds++;
                pfds[volup_idx].fd = hv.volup_line_fd;
                pfds[volup_idx].events = POLLIN;
                pfds[volup_idx].revents = 0;
            }
            if (ks.n_fds > 0) {
                kbd_base_idx = npfds;
                kbd_cnt = ks.n_fds;
                for (int k = 0; k < kbd_cnt; k++) {
                    pfds[npfds].fd = ks.fds[k];
                    pfds[npfds].events = POLLIN;
                    pfds[npfds].revents = 0;
                    npfds++;
                }
            }

            int poll_timeout = -1;
            if (!g_screen_off && g_auto_rotate) {
                uint32_t since_imu = now - last_imu_ms;
                poll_timeout = (since_imu < IMU_POLL_MS) ? (int)(IMU_POLL_MS - since_imu) : 1;
            } else if (!g_screen_off && mt.fd < 0) {
                poll_timeout = 2000;
            }
            if (need_vol_poll) {
                uint32_t since_vol = now - last_vol_ms;
                int vol_wait = (since_vol < HW_VOL_POLL_MS) ? (int)(HW_VOL_POLL_MS - since_vol) : 1;
                if (poll_timeout < 0 || vol_wait < poll_timeout) {
                    poll_timeout = vol_wait;
                }
            }

            int pr = poll(pfds, (nfds_t)npfds, poll_timeout);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ctrl_idx >= 0 && (pfds[ctrl_idx].revents & POLLIN)) {
                handle_power_ctrl_accept(ctrl_lfd, cfd, &mt, &ks, &hv);
            }
            if (volup_idx >= 0 && (pfds[volup_idx].revents & POLLIN)) {
                drain_volup_gpio_events(cfd, &ks, &hv);
            }
            if (mt_idx >= 0 && (pfds[mt_idx].revents & (POLLIN | POLLERR | POLLHUP))) {
                process_touch_events(cfd, &mt);
            }
            if (kbd_base_idx >= 0) {
                // Iterate backwards in case a disconnected keyboard fd shrinks ks.n_fds
                for (int k = kbd_cnt - 1; k >= 0; k--) {
                    if (pfds[kbd_base_idx + k].revents & (POLLIN | POLLERR | POLLHUP)) {
                        process_kbd_events(cfd, &ks, k);
                    }
                }
            }
            if (!(pfds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
                continue;
            }

            uint8_t cmsg_buf[CMSG_SPACE(sizeof(int) * 16)];
            struct iovec iov = { .iov_base = rbuf + rlen, .iov_len = sizeof(rbuf) - rlen };
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);

            ssize_t n = recvmsg(cfd, &msg, MSG_DONTWAIT);
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                break;
            }
            rlen += (size_t)n;

            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                    int cnt = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    int *rfd = (int *)CMSG_DATA(cm);
                    for (int i = 0; i < cnt && nfds < MAX_FDS; i++) fds[nfds++] = rfd[i];
                }
            }

            size_t pos = 0;
            int fd_idx = 0;
            while (rlen - pos >= 8) {
                uint32_t obj_id, sz_op;
                memcpy(&obj_id, rbuf + pos, 4);
                memcpy(&sz_op, rbuf + pos + 4, 4);
                uint32_t mlen = sz_op >> 16;
                uint16_t op = (uint16_t)(sz_op & 0xffff);
                if (mlen < 8 || (mlen & 3)) { pos = rlen; break; }
                if (rlen - pos < mlen) break;

                const uint32_t *args = (const uint32_t *)(rbuf + pos + 8);
                int nargs = (int)((mlen - 8) / 4);
                handle_message(cfd, obj_id, op, args, nargs, fds, &fd_idx, nfds, &ks);
                pos += mlen;
            }
            if (pos > 0) {
                memmove(rbuf, rbuf + pos, rlen - pos);
                rlen -= pos;
            }
            if (fd_idx > 0) {
                memmove(fds, fds + fd_idx, (size_t)(nfds - fd_idx) * sizeof(int));
                nfds -= fd_idx;
            }
        }
        for (int i = 0; i < nfds; i++) close(fds[i]);
        close(cfd);
        fprintf(stderr, "[nx769j-fb-wl] client disconnected\n");
    }
    return 0;
}
#endif
