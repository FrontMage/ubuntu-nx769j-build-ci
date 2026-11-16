// nx769j_vkbd.c - Zero-dependency Wayland zwp_input_method_v1 + zwp_input_panel_v1
// Breeze-Dark On-Screen Touch Keyboard for KDE Plasma 6 on RedMagic 9 Pro.
//
// Features:
//   - Connects directly via KWin's privileged WAYLAND_SOCKET fd.
//   - Pre-allocates a single 2560x720 ARGB8888 wl_shm_pool once at startup and dynamically
//     adapts surface/buffer geometry and key layout to live wl_output.mode updates:
//       * Portrait  (1116x2480): 1116 x 684 panel (104 px rows)
//       * Landscape (2480x1116): 2480 x 468 panel (71 px rows, full 2480 px width)
//     If the keyboard is currently visible during a portrait <-> landscape rotation, it
//     immediately recreates & commits the resized InputPanelV1Window at the top of Layer 9.
//   - Strictly recreates wl_surface + zwp_input_panel_surface_v1 on zwp_input_method_v1.activate()
//     (tearing it down on deactivate()) so InputPanelV1Window stacks ABOVE kscreenlocker_greet
//     as well as normal desktop windows.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#define DEFAULT_OUT_W   1116
#define DEFAULT_OUT_H   2480
#define DEFAULT_PANEL_W 1116
#define DEFAULT_PANEL_H 684

#define MAX_PANEL_W     2560
#define MAX_PANEL_H     720
#define MAX_PANEL_BYTES (MAX_PANEL_W * MAX_PANEL_H * 4)

#define XKB_KEY_BackSpace 0xff08
#define XKB_KEY_Tab       0xff09
#define XKB_KEY_Return    0xff0d
#define XKB_KEY_Escape    0xff1b

// 8x8 ASCII bitmap font for 0x20..0x7f (MSB = leftmost pixel)
static const uint8_t g_font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3c,0x3c,0x18,0x18,0x00,0x18,0x00}, //  !
    {0x6c,0x6c,0x24,0x00,0x00,0x00,0x00,0x00}, {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00}, // "#
    {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0x00}, {0x00,0xc6,0xcc,0x18,0x30,0x66,0xc6,0x00}, // $%
    {0x38,0x6c,0x38,0x76,0xdc,0xce,0x76,0x00}, {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // &'
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00}, {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00}, // ()
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00}, // *+
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, {0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00}, // ,-
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00}, // ./
    {0x7c,0xc6,0xce,0xde,0xf6,0xe6,0x7c,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0x00}, // 01
    {0x7c,0xc6,0x06,0x1c,0x30,0x66,0xfe,0x00}, {0x7c,0xc6,0x06,0x3c,0x06,0xc6,0x7c,0x00}, // 23
    {0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00}, {0xfe,0xc0,0xfc,0x06,0x06,0xc6,0x7c,0x00}, // 45
    {0x38,0x60,0xc0,0xfc,0xc6,0xc6,0x7c,0x00}, {0xfe,0xc6,0x0c,0x18,0x30,0x30,0x30,0x00}, // 67
    {0x7c,0xc6,0xc6,0x7c,0xc6,0xc6,0x7c,0x00}, {0x7c,0xc6,0xc6,0x7e,0x06,0x0c,0x78,0x00}, // 89
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // :;
    {0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x00}, {0x00,0x00,0x7e,0x00,0x00,0x7e,0x00,0x00}, // <=
    {0x60,0x30,0x18,0x0c,0x18,0x30,0x60,0x00}, {0x7c,0xc6,0x0c,0x18,0x18,0x00,0x18,0x00}, // >?
    {0x7c,0xc6,0xde,0xde,0xde,0xc0,0x7c,0x00}, {0x38,0x6c,0xc6,0xfe,0xc6,0xc6,0xc6,0x00}, // @A
    {0xfc,0xc6,0xc6,0xfc,0xc6,0xc6,0xfc,0x00}, {0x3c,0x66,0xc0,0xc0,0xc0,0x66,0x3c,0x00}, // BC
    {0xf8,0xcc,0xc6,0xc6,0xc6,0xcc,0xf8,0x00}, {0xfe,0xc2,0xc8,0xf8,0xc8,0xc2,0xfe,0x00}, // DE
    {0xfe,0xc2,0xc8,0xf8,0xc8,0xc0,0xc0,0x00}, {0x3c,0x66,0xc0,0xc0,0xce,0x66,0x3e,0x00}, // FG
    {0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0x00}, {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, // HI
    {0x1e,0x0c,0x0c,0x0c,0xcc,0xce,0x78,0x00}, {0xe6,0x66,0x6c,0x78,0x6c,0x66,0xe6,0x00}, // JK
    {0xf0,0x60,0x60,0x60,0x62,0x66,0xfe,0x00}, {0xc6,0xee,0xfe,0xfe,0xd6,0xc6,0xc6,0x00}, // LM
    {0xc6,0xe6,0xf6,0xde,0xce,0xc6,0xc6,0x00}, {0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, // NO
    {0xfc,0xc6,0xc6,0xfc,0xc0,0xc0,0xc0,0x00}, {0x7c,0xc6,0xc6,0xc6,0xd6,0xde,0x7c,0x06}, // PQ
    {0xfc,0xc6,0xc6,0xfc,0xca,0xc6,0xc6,0x00}, {0x7c,0xc6,0xc0,0x7c,0x06,0xc6,0x7c,0x00}, // RS
    {0x7e,0x5a,0x18,0x18,0x18,0x18,0x3c,0x00}, {0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, // TU
    {0xc6,0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x00}, {0xc6,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0x00}, // VW
    {0xc6,0xc6,0x6c,0x38,0x6c,0xc6,0xc6,0x00}, {0x66,0x66,0x66,0x3c,0x18,0x18,0x3c,0x00}, // XY
    {0xfe,0xc6,0x8c,0x18,0x32,0x66,0xfe,0x00}, {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00}, // Z[
    {0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00}, {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00}, // \]
    {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff}, // ^_
    {0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x78,0x0c,0x7c,0xcc,0x76,0x00}, // `a
    {0xe0,0x60,0x7c,0x66,0x66,0x66,0xdc,0x00}, {0x00,0x00,0x7c,0xc6,0xc0,0xc6,0x7c,0x00}, // bc
    {0x1c,0x0c,0x7c,0xcc,0x66,0x66,0x76,0x00}, {0x00,0x00,0x7c,0xc6,0xfe,0xc0,0x7c,0x00}, // de
    {0x3c,0x66,0x60,0xf8,0x60,0x60,0xf0,0x00}, {0x00,0x00,0x76,0xcc,0xcc,0x7c,0x0c,0xf8}, // fg
    {0xe0,0x60,0x6c,0x76,0x66,0x66,0xe6,0x00}, {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00}, // hi
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3c}, {0xe0,0x60,0x66,0x6c,0x78,0x6c,0xe6,0x00}, // jk
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, {0x00,0x00,0xec,0xfe,0xd6,0xd6,0xc6,0x00}, // lm
    {0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x00}, {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7c,0x00}, // no
    {0x00,0x00,0xdc,0x66,0x66,0x7c,0x60,0xf0}, {0x00,0x00,0x76,0xcc,0x66,0x3c,0x0c,0x1e}, // pq
    {0x00,0x00,0xdc,0x76,0x60,0x60,0xf0,0x00}, {0x00,0x00,0x7c,0xc0,0x7c,0x06,0xfc,0x00}, // rs
    {0x30,0x30,0xfc,0x30,0x30,0x36,0x1c,0x00}, {0x00,0x00,0xcc,0xce,0x66,0x66,0x76,0x00}, // tu
    {0x00,0x00,0xc6,0xc6,0xc6,0x6c,0x38,0x00}, {0x00,0x00,0xc6,0xd6,0xfe,0xfe,0xc6,0x00}, // vw
    {0x00,0x00,0xc6,0x6c,0x38,0x6c,0xc6,0x00}, {0x00,0x00,0xc6,0xc6,0xce,0x76,0x06,0x7c}, // xy
    {0x00,0x00,0xfe,0x8c,0x18,0x32,0xfe,0x00}, {0x0e,0x18,0x18,0x70,0x18,0x18,0x0e,0x00}, // z{
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, {0x70,0x18,0x18,0x0e,0x18,0x18,0x70,0x00}, // |}
    {0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}  // ~DEL
};

enum key_act {
    ACT_CHAR = 0,
    ACT_SHIFT,
    ACT_SYM,
    ACT_BKSP,
    ACT_ENTER,
    ACT_SPACE,
    ACT_HIDE,
};

struct key_def {
    int x, y, w, h;
    enum key_act act;
    const char *norm;
    const char *shft;
    const char *sym;
};

static int g_out_w = DEFAULT_OUT_W;
static int g_out_h = DEFAULT_OUT_H;
static int g_panel_w = DEFAULT_PANEL_W;
static int g_panel_h = DEFAULT_PANEL_H;

static struct key_def g_keys[80];
static int g_nkeys = 0;
static int g_shift = 0;
static int g_sym = 0;
static int g_pressed_idx = -1;

static void compute_panel_dims(int out_w, int out_h, int *pw, int *ph) {
    int w = out_w;
    if (w < 640) w = 640;
    if (w > MAX_PANEL_W) w = MAX_PANEL_W;

    int h;
    if (out_w > out_h) {
        // Landscape (e.g. 2480x1116 -> 2480x468, ~42% height so top 58% remains unobstructed)
        h = (out_h * 42) / 100;
        if (h < 360) h = 360;
    } else {
        // Portrait (e.g. 1116x2480 -> 1116x684)
        h = (out_h >= 1600) ? DEFAULT_PANEL_H : ((out_h * 45) / 100);
        if (h < 420) h = 420;
    }
    if (h > MAX_PANEL_H) h = MAX_PANEL_H;

    *pw = w;
    *ph = h;
}

static void build_layout(void) {
    g_nkeys = 0;
    int pw = g_panel_w;
    int ph = g_panel_h;
    int pad = (ph >= 600) ? 8 : 6;
    int row_h = (ph - pad * 7) / 6;

    // Row 0: Quick Wi-Fi Symbols + Hide
    const char *r0[11] = {"!", "@", "#", "$", "%", "^", "&", "*", "-", "_", "Hide"};
    int y0 = pad;
    int w0_hide = (pw * 140) / 1000;
    int w0 = (pw - pad * 12 - w0_hide) / 10;
    int cx = pad;
    for (int i = 0; i < 11; i++) {
        int kw = (i == 10) ? (pw - pad - cx) : w0;
        g_keys[g_nkeys++] = (struct key_def){
            cx, y0, kw, row_h,
            (i == 10) ? ACT_HIDE : ACT_CHAR,
            r0[i], r0[i], r0[i]
        };
        cx += kw + pad;
    }

    // Row 1: Digits 1..0
    const char *r1[10] = {"1","2","3","4","5","6","7","8","9","0"};
    int y1 = y0 + row_h + pad;
    int w1 = (pw - pad * 11) / 10;
    cx = pad;
    for (int i = 0; i < 10; i++) {
        int kw = (i == 9) ? (pw - pad - cx) : w1;
        g_keys[g_nkeys++] = (struct key_def){cx, y1, kw, row_h, ACT_CHAR, r1[i], r1[i], r1[i]};
        cx += kw + pad;
    }

    // Row 2: qwertyuiop / QWERTYUIOP / symbols
    const char *r2n[10] = {"q","w","e","r","t","y","u","i","o","p"};
    const char *r2s[10] = {"Q","W","E","R","T","Y","U","I","O","P"};
    const char *r2y[10] = {"[","]","{","}","(",")","=","+","\\","|"};
    int y2 = y1 + row_h + pad;
    cx = pad;
    for (int i = 0; i < 10; i++) {
        int kw = (i == 9) ? (pw - pad - cx) : w1;
        g_keys[g_nkeys++] = (struct key_def){cx, y2, kw, row_h, ACT_CHAR, r2n[i], r2s[i], r2y[i]};
        cx += kw + pad;
    }

    // Row 3: asdfghjkl (centered with proportional side margin)
    const char *r3n[9] = {"a","s","d","f","g","h","j","k","l"};
    const char *r3s[9] = {"A","S","D","F","G","H","J","K","L"};
    const char *r3y[9] = {";",":","'","\"","<",">","?","~","`"};
    int y3 = y2 + row_h + pad;
    int m3 = (pw * 47) / 1000;
    int w3 = (pw - m3 * 2 - pad * 8) / 9;
    cx = m3;
    for (int i = 0; i < 9; i++) {
        int kw = (i == 8) ? (pw - m3 - cx) : w3;
        g_keys[g_nkeys++] = (struct key_def){cx, y3, kw, row_h, ACT_CHAR, r3n[i], r3s[i], r3y[i]};
        cx += kw + pad;
    }

    // Row 4: [Shift] z x c v b n m [Bksp]
    const char *r4n[7] = {"z","x","c","v","b","n","m"};
    const char *r4s[7] = {"Z","X","C","V","B","N","M"};
    const char *r4y[7] = {"!","@","#","$","%","&","*"};
    int y4 = y3 + row_h + pad;
    int w_mod = (pw * 136) / 1000;
    int w4 = (pw - pad * 10 - w_mod * 2) / 7;
    cx = pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y4, w_mod, row_h, ACT_SHIFT, "Shift", "SHIFT", "Shift"};
    cx += w_mod + pad;
    for (int i = 0; i < 7; i++) {
        g_keys[g_nkeys++] = (struct key_def){cx, y4, w4, row_h, ACT_CHAR, r4n[i], r4s[i], r4y[i]};
        cx += w4 + pad;
    }
    g_keys[g_nkeys++] = (struct key_def){cx, y4, pw - pad - cx, row_h, ACT_BKSP, "Bksp", "Bksp", "Bksp"};

    // Row 5: [?123] [,] [.] [   SPACE   ] [/] [-] [Enter]
    int y5 = y4 + row_h + pad;
    int w_sym = (pw * 129) / 1000;
    int w_ent = (pw * 158) / 1000;
    int w_punc = (pw * 79) / 1000;
    int w_spc = pw - pad * 8 - w_sym - w_ent - w_punc * 4;
    cx = pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, w_sym, row_h, ACT_SYM, "?123", "?123", "ABC"};
    cx += w_sym + pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, w_punc, row_h, ACT_CHAR, ",", ",", ","};
    cx += w_punc + pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, w_punc, row_h, ACT_CHAR, ".", ".", "."};
    cx += w_punc + pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, w_spc, row_h, ACT_SPACE, "Space", "Space", "Space"};
    cx += w_spc + pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, w_punc, row_h, ACT_CHAR, "/", "/", "/"};
    cx += w_punc + pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, w_punc, row_h, ACT_CHAR, "-", "_", "-"};
    cx += w_punc + pad;
    g_keys[g_nkeys++] = (struct key_def){cx, y5, pw - pad - cx, row_h, ACT_ENTER, "Enter", "Enter", "Enter"};
}

static void fill_rect(uint32_t *px, int x, int y, int w, int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_panel_w) w = g_panel_w - x;
    if (y + h > g_panel_h) h = g_panel_h - y;
    if (w <= 0 || h <= 0) return;
    for (int r = y; r < y + h; r++) {
        uint32_t *row = px + (size_t)r * (size_t)g_panel_w + x;
        for (int c = 0; c < w; c++) row[c] = argb;
    }
}

static void draw_text(uint32_t *px, int cx, int cy, const char *s, int scale, uint32_t argb) {
    int len = (int)strlen(s);
    int tw = len * 8 * scale;
    int th = 8 * scale;
    int x0 = cx - tw / 2;
    int y0 = cy - th / 2;
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7f) ch = '?';
        const uint8_t *glyph = g_font8x8[ch - 0x20];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (bits & (0x80 >> gx)) {
                    fill_rect(px, x0 + (i * 8 + gx) * scale, y0 + gy * scale, scale, scale, argb);
                }
            }
        }
    }
}

static const char *key_label(const struct key_def *k) {
    if (g_sym) return k->sym;
    if (g_shift) return k->shft;
    return k->norm;
}

static void render_keyboard(uint32_t *px) {
    // Breeze Dark panel background + subtle top accent line
    fill_rect(px, 0, 0, g_panel_w, g_panel_h, 0xff1e2229);
    fill_rect(px, 0, 0, g_panel_w, 3, 0xff3daee9);

    for (int i = 0; i < g_nkeys; i++) {
        const struct key_def *k = &g_keys[i];
        uint32_t bg = 0xff31363b;
        if (k->act == ACT_ENTER) bg = 0xff2980b9;
        else if (k->act == ACT_HIDE) bg = 0xff3b424a;
        else if (k->act == ACT_SHIFT && g_shift) bg = 0xff3daee9;
        else if (k->act == ACT_SYM && g_sym) bg = 0xff3daee9;
        else if (k->act != ACT_CHAR && k->act != ACT_SPACE) bg = 0xff262b31;
        if (i == g_pressed_idx) bg = 0xff3daee9;

        // Key body + crisp 2px highlight/border
        fill_rect(px, k->x, k->y + 2, k->w, k->h - 2, 0xff14171c);
        fill_rect(px, k->x + 1, k->y, k->w - 2, k->h - 3, bg);
        fill_rect(px, k->x + 2, k->y + 1, k->w - 4, 2, 0xff454d56);

        const char *lbl = key_label(k);
        int scale;
        if (k->h >= 90) {
            scale = (strlen(lbl) == 1) ? 4 : 3;
        } else {
            scale = (strlen(lbl) == 1) ? 3 : 2;
        }
        draw_text(px, k->x + k->w / 2, k->y + k->h / 2, lbl, scale, 0xfffcfcfc);
    }
}

// Minimal Wayland client wire-protocol helpers
static int g_wl_fd = -1;
static uint32_t g_next_id = 2; // 1 = wl_display

static uint32_t g_reg_id = 0;
static uint32_t g_comp_id = 0;
static uint32_t g_shm_id = 0;
static uint32_t g_out_id = 0;
static uint32_t g_out_name = 0;
static uint32_t g_out_ver = 0;
static uint32_t g_seat_id = 0;
static uint32_t g_ptr_id = 0;
static uint32_t g_touch_id = 0;
static uint32_t g_ipanel_id = 0;
static uint32_t g_im_id = 0;

static uint32_t g_ctx_id = 0;
static uint32_t g_im_serial = 0;

static uint32_t g_surf_id = 0;
static uint32_t g_ipsurf_id = 0;
static uint32_t g_pool_id = 0;
static uint32_t g_buf_id = 0;
static int g_buf_w = 0;
static int g_buf_h = 0;
static int g_shm_fd = -1;
static uint32_t *g_shm_map = NULL;
static int g_panel_mapped = 0;

static int g_ptr_x = -1, g_ptr_y = -1;
static int g_ptr_on_panel = 0;
static int g_touch_on_panel = 0;

struct msg_buf {
    uint32_t w[128];
    int nw;
};

static void m_init(struct msg_buf *m, uint32_t obj) {
    m->w[0] = obj;
    m->w[1] = 0;
    m->nw = 2;
}
static void m_u32(struct msg_buf *m, uint32_t v) { m->w[m->nw++] = v; }
static void m_i32(struct msg_buf *m, int32_t v) { m->w[m->nw++] = (uint32_t)v; }
static void m_str(struct msg_buf *m, const char *s) {
    uint32_t len = (uint32_t)strlen(s) + 1;
    m_u32(m, len);
    uint32_t nw = (len + 3) / 4;
    memset(&m->w[m->nw], 0, nw * 4);
    memcpy(&m->w[m->nw], s, len);
    m->nw += (int)nw;
}
static void m_send_fd(struct msg_buf *m, uint16_t op, int pass_fd) {
    if (g_wl_fd < 0) return;
    uint32_t bytes = (uint32_t)(m->nw * 4);
    m->w[1] = (bytes << 16) | op;
    struct iovec iov = { .iov_base = m->w, .iov_len = bytes };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    if (pass_fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    while (sendmsg(g_wl_fd, &msg, MSG_NOSIGNAL) < 0) {
        if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; }
        break;
    }
}
static void m_send(struct msg_buf *m, uint16_t op) { m_send_fd(m, op, -1); }

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static int create_anon_shm_fd(void) {
#if defined(__linux__) && defined(MFD_CLOEXEC)
    return memfd_create("nx769j-vkbd-shm", MFD_CLOEXEC);
#else
    char tmpl[] = "/tmp/nx769j-vkbd-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
        int fl = fcntl(fd, F_GETFD, 0);
        if (fl >= 0) (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
    return fd;
#endif
}

static void reg_bind(uint32_t name, const char *iface, uint32_t ver, uint32_t nid) {
    struct msg_buf m;
    m_init(&m, g_reg_id);
    m_u32(&m, name);
    m_str(&m, iface);
    m_u32(&m, ver);
    m_u32(&m, nid);
    m_send(&m, 0); // wl_registry.bind
}

static void ensure_shm_buffer(void) {
    if (!g_shm_id) return;
    if (!g_shm_map) {
        g_shm_fd = create_anon_shm_fd();
        if (g_shm_fd < 0) return;
        if (ftruncate(g_shm_fd, MAX_PANEL_BYTES) < 0) {
            close(g_shm_fd);
            g_shm_fd = -1;
            return;
        }
        g_shm_map = mmap(NULL, MAX_PANEL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
        if (g_shm_map == MAP_FAILED) {
            g_shm_map = NULL;
            close(g_shm_fd);
            g_shm_fd = -1;
            return;
        }
        g_pool_id = g_next_id++;
        struct msg_buf m;
        m_init(&m, g_shm_id);
        m_u32(&m, g_pool_id);
        m_i32(&m, MAX_PANEL_BYTES);
        m_send_fd(&m, 0, g_shm_fd); // wl_shm.create_pool
    }

    if (g_buf_id != 0 && (g_buf_w != g_panel_w || g_buf_h != g_panel_h)) {
        struct msg_buf m;
        m_init(&m, g_buf_id);
        m_send(&m, 0); // wl_buffer.destroy
        g_buf_id = 0;
    }

    if (g_buf_id == 0 && g_pool_id != 0) {
        g_buf_id = g_next_id++;
        g_buf_w = g_panel_w;
        g_buf_h = g_panel_h;
        struct msg_buf m;
        m_init(&m, g_pool_id);
        m_u32(&m, g_buf_id);
        m_i32(&m, 0);
        m_i32(&m, g_panel_w);
        m_i32(&m, g_panel_h);
        m_i32(&m, g_panel_w * 4);
        m_u32(&m, 0); // WL_SHM_FORMAT_ARGB8888 = 0
        m_send(&m, 0); // wl_shm_pool.create_buffer
    }
}

static void commit_panel_frame(void) {
    if (!g_panel_mapped || !g_surf_id || !g_buf_id || !g_shm_map) return;
    render_keyboard(g_shm_map);
    struct msg_buf m;
    // wl_surface.attach(buf, 0, 0) = op 1
    m_init(&m, g_surf_id);
    m_u32(&m, g_buf_id);
    m_i32(&m, 0);
    m_i32(&m, 0);
    m_send(&m, 1);
    // wl_surface.damage(0, 0, g_panel_w, g_panel_h) = op 2
    m_init(&m, g_surf_id);
    m_i32(&m, 0);
    m_i32(&m, 0);
    m_i32(&m, g_panel_w);
    m_i32(&m, g_panel_h);
    m_send(&m, 2);
    // wl_surface.commit() = op 6
    m_init(&m, g_surf_id);
    m_send(&m, 6);
}

static void destroy_panel_surface(void) {
    g_pressed_idx = -1;
    g_ptr_on_panel = 0;
    g_touch_on_panel = 0;
    g_panel_mapped = 0;
    if (g_surf_id != 0) {
        struct msg_buf m;
        m_init(&m, g_surf_id);
        m_send(&m, 0); // wl_surface.destroy -> triggers InputPanelSurfaceV1Interface::aboutToBeDestroyed
        g_surf_id = 0;
        g_ipsurf_id = 0;
    }
}

static void recreate_and_show_panel(void) {
    if (!g_comp_id || !g_ipanel_id || !g_out_id || !g_shm_id) return;
    ensure_shm_buffer();
    if (!g_buf_id) return;

    // Tear down any older panel surface first so KWin creates a fresh InputPanelV1Window
    // at the very top of Layer 9 (OverlayLayer), above kscreenlocker_greet!
    destroy_panel_surface();

    g_surf_id = g_next_id++;
    struct msg_buf m;
    m_init(&m, g_comp_id);
    m_u32(&m, g_surf_id);
    m_send(&m, 0); // wl_compositor.create_surface

    g_ipsurf_id = g_next_id++;
    m_init(&m, g_ipanel_id);
    m_u32(&m, g_ipsurf_id);
    m_u32(&m, g_surf_id);
    m_send(&m, 0); // zwp_input_panel_v1.get_input_panel_surface

    m_init(&m, g_ipsurf_id);
    m_u32(&m, g_out_id);
    m_u32(&m, 0); // ZWP_INPUT_PANEL_SURFACE_V1_POSITION_CENTER_BOTTOM = 0
    m_send(&m, 0); // zwp_input_panel_surface_v1.set_toplevel

    g_panel_mapped = 1;
    commit_panel_frame();
}

static void update_output_dimensions(int out_w, int out_h) {
    if (out_w <= 0 || out_h <= 0) return;
    int new_pw = g_panel_w, new_ph = g_panel_h;
    compute_panel_dims(out_w, out_h, &new_pw, &new_ph);
    g_out_w = out_w;
    g_out_h = out_h;
    if (new_pw == g_panel_w && new_ph == g_panel_h) {
        return;
    }
    int was_mapped = g_panel_mapped;
    g_panel_w = new_pw;
    g_panel_h = new_ph;
    build_layout();
    ensure_shm_buffer();
    fprintf(stderr, "[nx769j-vkbd] output mode %dx%d -> keyboard panel %dx%d (mapped=%d)\n",
            out_w, out_h, g_panel_w, g_panel_h, was_mapped);
    if (was_mapped) {
        recreate_and_show_panel();
    }
}

static void request_kwin_hide(void) {
#ifndef NX769J_VKBD_TEST
    if (fork() == 0) {
        execl("/usr/bin/busctl", "busctl", "--user", "set-property",
              "org.kde.KWin", "/VirtualKeyboard",
              "org.kde.kwin.VirtualKeyboard", "active", "b", "false", (char *)NULL);
        _exit(0);
    }
#endif
}

static void request_kwin_show(void) {
#ifndef NX769J_VKBD_TEST
    if (fork() == 0) {
        execl("/usr/bin/busctl", "busctl", "--user", "call",
              "org.kde.KWin", "/VirtualKeyboard",
              "org.kde.kwin.VirtualKeyboard", "forceActivate", (char *)NULL);
        _exit(0);
    }
#endif
}

static void send_commit_str(const char *s) {
    if (!g_ctx_id) return;
    struct msg_buf m;
    m_init(&m, g_ctx_id);
    m_u32(&m, g_im_serial);
    m_str(&m, s);
    m_send(&m, 1); // zwp_input_method_context_v1.commit_string
}

static void send_keysym_tap(uint32_t sym) {
    if (!g_ctx_id) return;
    uint32_t t = now_ms();
    struct msg_buf m;
    // press (state = 1)
    m_init(&m, g_ctx_id);
    m_u32(&m, g_im_serial);
    m_u32(&m, t);
    m_u32(&m, sym);
    m_u32(&m, 1);
    m_u32(&m, 0);
    m_send(&m, 8); // zwp_input_method_context_v1.keysym
    // release (state = 0)
    m_init(&m, g_ctx_id);
    m_u32(&m, g_im_serial);
    m_u32(&m, t + 5);
    m_u32(&m, sym);
    m_u32(&m, 0);
    m_u32(&m, 0);
    m_send(&m, 8);
}

static int hit_key(int x, int y) {
    for (int i = 0; i < g_nkeys; i++) {
        const struct key_def *k = &g_keys[i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h)
            return i;
    }
    return -1;
}

static void handle_tap_down(int x, int y) {
    int idx = hit_key(x, y);
    if (idx < 0) return;
    g_pressed_idx = idx;
    commit_panel_frame();
}

static void handle_tap_up(int x, int y) {
    int idx = hit_key(x, y);
    if (idx < 0) idx = g_pressed_idx;
    g_pressed_idx = -1;
    if (idx < 0) { commit_panel_frame(); return; }

    const struct key_def *k = &g_keys[idx];
    switch (k->act) {
    case ACT_CHAR:
        send_commit_str(key_label(k));
        break;
    case ACT_SPACE:
        send_commit_str(" ");
        break;
    case ACT_SHIFT:
        g_shift = !g_shift;
        break;
    case ACT_SYM:
        g_sym = !g_sym;
        break;
    case ACT_BKSP:
        send_keysym_tap(XKB_KEY_BackSpace);
        break;
    case ACT_ENTER:
        send_keysym_tap(XKB_KEY_Return);
        break;
    case ACT_HIDE:
        commit_panel_frame();
        request_kwin_hide();
        return;
    }
    commit_panel_frame();
}

static void handle_event(uint32_t obj, uint16_t op, const uint32_t *a, int nw) {
    if (obj == g_reg_id && op == 0 && nw >= 2) { // wl_registry.global(name, iface, ver)
        uint32_t name = a[0];
        uint32_t slen = a[1];
        const char *iface = (const char *)&a[2];
        uint32_t ver_idx = 2 + (slen + 3) / 4;
        if (ver_idx >= (uint32_t)nw) return;
        uint32_t ver = a[ver_idx];

        if (strcmp(iface, "wl_compositor") == 0 && !g_comp_id) {
            g_comp_id = g_next_id++;
            reg_bind(name, iface, ver < 4 ? ver : 4, g_comp_id);
        } else if (strcmp(iface, "wl_shm") == 0 && !g_shm_id) {
            g_shm_id = g_next_id++;
            reg_bind(name, iface, 1, g_shm_id);
            ensure_shm_buffer();
        } else if (strcmp(iface, "wl_output") == 0 && !g_out_id) {
            g_out_id = g_next_id++;
            g_out_name = name;
            g_out_ver = ver < 3 ? ver : 3;
            reg_bind(name, iface, g_out_ver, g_out_id);
        } else if (strcmp(iface, "wl_seat") == 0 && !g_seat_id) {
            g_seat_id = g_next_id++;
            reg_bind(name, iface, ver < 5 ? ver : 5, g_seat_id);
        } else if (strcmp(iface, "zwp_input_panel_v1") == 0 && !g_ipanel_id) {
            g_ipanel_id = g_next_id++;
            reg_bind(name, iface, 1, g_ipanel_id);
        } else if (strcmp(iface, "zwp_input_method_v1") == 0 && !g_im_id) {
            g_im_id = g_next_id++;
            reg_bind(name, iface, 1, g_im_id);
        }

        // Only map the panel during registry discovery if KWin already activated a context!
        if (g_ctx_id && !g_panel_mapped && g_comp_id && g_shm_id && g_out_id && g_ipanel_id) {
            recreate_and_show_panel();
            request_kwin_show();
        }
    } else if (obj == g_reg_id && op == 1 && nw >= 1) { // wl_registry.global_remove(name)
        if (g_out_id && a[0] == g_out_name) {
            if (g_out_ver >= 3) {
                struct msg_buf m;
                m_init(&m, g_out_id);
                m_send(&m, 0); // wl_output.release
            }
            g_out_id = 0;
            g_out_name = 0;
            g_out_ver = 0;
        }
    } else if (obj == g_out_id && g_out_id != 0) {
        if (op == 1 && nw >= 4) { // wl_output.mode(flags, width, height, refresh)
            uint32_t flags = a[0];
            int32_t mw = (int32_t)a[1];
            int32_t mh = (int32_t)a[2];
            if ((flags & 0x1u) && mw > 0 && mh > 0) {
                update_output_dimensions(mw, mh);
            }
        }
    } else if (obj == g_seat_id && op == 0 && nw >= 1) { // wl_seat.capabilities
        uint32_t caps = a[0];
        if ((caps & 1) && !g_ptr_id) {
            g_ptr_id = g_next_id++;
            struct msg_buf m;
            m_init(&m, g_seat_id);
            m_u32(&m, g_ptr_id);
            m_send(&m, 0); // wl_seat.get_pointer
        }
        if ((caps & 4) && !g_touch_id) {
            g_touch_id = g_next_id++;
            struct msg_buf m;
            m_init(&m, g_seat_id);
            m_u32(&m, g_touch_id);
            m_send(&m, 2); // wl_seat.get_touch
        }
    } else if (obj == g_im_id) {
        if (op == 0 && nw >= 1) { // zwp_input_method_v1.activate(new_id ctx)
            if (g_ctx_id && g_ctx_id != a[0]) {
                struct msg_buf m;
                m_init(&m, g_ctx_id);
                m_send(&m, 0); // destroy previous context
            }
            g_ctx_id = a[0];
            g_im_serial = 0;
            recreate_and_show_panel();
            request_kwin_show();
        } else if (op == 1 && nw >= 1) { // zwp_input_method_v1.deactivate(ctx)
            uint32_t old_ctx = a[0];
            struct msg_buf m;
            m_init(&m, old_ctx);
            m_send(&m, 0); // zwp_input_method_context_v1.destroy
            if (g_ctx_id == old_ctx) {
                g_ctx_id = 0;
                destroy_panel_surface();
            }
        }
    } else if (obj == g_ctx_id && g_ctx_id != 0) {
        if (op == 4 && nw >= 1) { // commit_state(serial)
            g_im_serial = a[0];
        }
    } else if (obj == g_ptr_id && g_ptr_id != 0) {
        if (op == 0 && nw >= 4) { // enter(serial, surface, sx, sy)
            g_ptr_on_panel = (a[1] == g_surf_id && g_panel_mapped);
            g_ptr_x = ((int32_t)a[2]) >> 8;
            g_ptr_y = ((int32_t)a[3]) >> 8;
        } else if (op == 1) { // leave
            g_ptr_on_panel = 0;
        } else if (op == 2 && nw >= 3) { // motion(time, sx, sy)
            g_ptr_x = ((int32_t)a[1]) >> 8;
            g_ptr_y = ((int32_t)a[2]) >> 8;
        } else if (op == 3 && nw >= 4) { // button(serial, time, button, state)
            if (g_ptr_on_panel && g_panel_mapped) {
                if (a[3] == 1) handle_tap_down(g_ptr_x, g_ptr_y);
                else if (a[3] == 0) handle_tap_up(g_ptr_x, g_ptr_y);
            }
        }
    } else if (obj == g_touch_id && g_touch_id != 0) {
        if (op == 0 && nw >= 6) { // down(serial, time, surface, id, x, y)
            if (a[2] == g_surf_id && g_panel_mapped) {
                int tx = ((int32_t)a[4]) >> 8;
                int ty = ((int32_t)a[5]) >> 8;
                g_ptr_x = tx;
                g_ptr_y = ty;
                g_touch_on_panel = 1;
                handle_tap_down(tx, ty);
            } else {
                g_touch_on_panel = 0;
            }
        } else if (op == 1 && nw >= 3) { // up(serial, time, id)
            if (g_touch_on_panel && g_panel_mapped) {
                g_touch_on_panel = 0;
                handle_tap_up(g_ptr_x, g_ptr_y);
            }
        } else if (op == 2 && nw >= 4) { // motion(time, id, x, y)
            if (g_touch_on_panel) {
                g_ptr_x = ((int32_t)a[2]) >> 8;
                g_ptr_y = ((int32_t)a[3]) >> 8;
            }
        } else if (op == 4) { // cancel()
            g_touch_on_panel = 0;
            g_pressed_idx = -1;
            if (g_panel_mapped) commit_panel_frame();
        }
    }
}

#ifndef NX769J_VKBD_TEST
static int connect_wayland(void) {
    const char *ws = getenv("WAYLAND_SOCKET");
    if (ws && *ws) {
        int fd = atoi(ws);
        unsetenv("WAYLAND_SOCKET");
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        return fd;
    }
    const char *wd = getenv("WAYLAND_DISPLAY");
    const char *xr = getenv("XDG_RUNTIME_DIR");
    if (!wd) wd = "wayland-0";
    if (!xr) xr = "/run/user/1000";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (wd[0] == '/') snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", wd);
    else snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", xr, wd);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus", 0);
    build_layout();

    g_wl_fd = connect_wayland();
    if (g_wl_fd < 0) return 1;

    // wl_display.get_registry(new_id) = op 1 on obj 1
    g_reg_id = g_next_id++;
    struct msg_buf m;
    m_init(&m, 1);
    m_u32(&m, g_reg_id);
    m_send(&m, 1);

    uint8_t rbuf[65536];
    size_t rlen = 0;

    for (;;) {
        struct pollfd pfd = { .fd = g_wl_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        char cbuf[256];
        struct iovec iov = { .iov_base = rbuf + rlen, .iov_len = sizeof(rbuf) - rlen };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);

        ssize_t n = recvmsg(g_wl_fd, &msg, MSG_CMSG_CLOEXEC);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        // Close any unexpected ancillary fds (e.g. keymap) so we never leak fds
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int nf = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                int *fds = (int *)CMSG_DATA(c);
                for (int i = 0; i < nf; i++) close(fds[i]);
            }
        }
        rlen += (size_t)n;

        size_t off = 0;
        while (rlen - off >= 8) {
            uint32_t obj, hdr;
            memcpy(&obj, rbuf + off, 4);
            memcpy(&hdr, rbuf + off + 4, 4);
            uint32_t sz = hdr >> 16;
            uint16_t op = (uint16_t)(hdr & 0xffff);
            if (sz < 8 || sz > sizeof(rbuf)) { off = rlen; break; }
            if (rlen - off < sz) break;
            handle_event(obj, op, (const uint32_t *)(rbuf + off + 8), (int)((sz - 8) / 4));
            off += sz;
        }
        if (off > 0) {
            if (off < rlen) memmove(rbuf, rbuf + off, rlen - off);
            rlen -= off;
        }
    }
    return 0;
}
#endif
