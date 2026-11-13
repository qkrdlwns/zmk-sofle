/*
 * Custom nice!view status screen for the Eyelash Sofle (ZMK v0.3.x, LVGL 8.3).
 *
 * Everything is drawn on a portrait 68x160 canvas (how the nice!view is read on
 * the keyboard), then copied rotated 90 degrees into the 160x68 landscape canvas
 * the display actually shows. Same idea as the stock nice!view screen, but with
 * one full-height canvas so layouts are not cut into 68px blocks.
 *
 * Left half (central): style A / B / C, chosen in Kconfig.
 * Right half (peripheral): battery-focused screen.
 *
 * Needs CONFIG_NICE_VIEW_WIDGET_STATUS=n so the stock screen steps aside.
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_CENTRAL
#include <zmk/keymap.h>
#include <zmk/endpoints.h>
#include <zmk/wpm.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/wpm_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
#else
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#endif

/* portrait drawing area */
#define PW 68
#define PH 160

#define PROFILES 5
#define HIST 16
#define MAX_ACTIVE 6

#define BG (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_black() : lv_color_white())
#define FG (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? lv_color_white() : lv_color_black())

/* ================================================================ state */

static struct {
    uint8_t battery;
    bool charging;
#if IS_CENTRAL
    bool usb_selected;
    int profile;
    bool profile_connected;
    bool profile_bonded;
    bool prof_conn[PROFILES];
    bool prof_bond[PROFILES];
    uint8_t layer_index;
    const char *layer_name;
    uint8_t active_cnt;
    uint8_t active_idx[MAX_ACTIVE];      /* highest first */
    const char *active_name[MAX_ACTIVE];
    uint8_t wpm[HIST];                   /* oldest ... newest */
#else
    bool connected;
#endif
} st;

/* ========================================================= canvases */

static lv_obj_t *port_cv; /* hidden, portrait: what we draw on */
static lv_obj_t *land_cv; /* visible, landscape: what the display shows */
static lv_color_t port_buf[PW * PH];
static lv_color_t land_buf[PH * PW];

static void blit(void) {
    /* portrait (px,py) -> landscape (159-py, px): portrait top = landscape right,
     * the same 90 degree rotation the stock nice!view screen uses */
    for (int py = 0; py < PH; py++) {
        for (int px = 0; px < PW; px++) {
            land_buf[px * PH + (PH - 1 - py)] = port_buf[py * PW + px];
        }
    }
    lv_obj_invalidate(land_cv);
}

/* ========================================================= drawing */

static void d_rect(int x, int y, int w, int h, lv_color_t c) {
    if (w <= 0 || h <= 0) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    lv_canvas_draw_rect(port_cv, x, y, w, h, &d);
}

static void d_frame(int x, int y, int w, int h) {
    d_rect(x, y, w, h, FG);
    d_rect(x + 1, y + 1, w - 2, h - 2, BG);
}

static void d_text(int x, int y, int w, const lv_font_t *font, lv_text_align_t align, lv_color_t c,
                   const char *txt) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = c;
    d.font = font;
    d.align = align;
    lv_canvas_draw_text(port_cv, x, y, w, &d, txt);
}

static __maybe_unused int text_w(const char *txt, const lv_font_t *font) {
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* battery icon 29x12 (+3px nub) at x,y; bolt next to it when charging */
static void d_battery(int x, int y) {
    d_frame(x, y, 29, 12);
    d_rect(x + 2, y + 2, (MIN(st.battery, 100) * 25 + 50) / 100, 8, FG);
    d_rect(x + 29, y + 3, 3, 6, FG);
    if (st.charging) {
        d_text(x + 33, y - 2, 12, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT, FG,
               LV_SYMBOL_CHARGE);
    }
}

#if IS_CENTRAL

static void conn_text(char *buf, size_t n, bool spaced) {
    const char *sp = spaced ? " " : "";
    if (st.usb_selected) {
        snprintf(buf, n, LV_SYMBOL_USB);
    } else if (!st.profile_bonded) {
        snprintf(buf, n, LV_SYMBOL_SETTINGS "%s%d", sp, st.profile + 1);
    } else if (st.profile_connected) {
        snprintf(buf, n, LV_SYMBOL_BLUETOOTH "%s%d", sp, st.profile + 1);
    } else {
        snprintf(buf, n, LV_SYMBOL_CLOSE "%s%d", sp, st.profile + 1);
    }
}

#if !IS_ENABLED(CONFIG_SOFLE_SCREEN_STYLE_STACK)
/* battery on the left, connection on the right */
static void d_top_bar(void) {
    char buf[16];
    d_battery(0, 2);
    conn_text(buf, sizeof(buf), false);
    d_text(0, 0, PW, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT, FG, buf);
}
#endif

static const char *name_or_index(const char *name, uint8_t idx, char *buf, size_t n) {
    if (name && name[0]) {
        return name;
    }
    snprintf(buf, n, "LAYER %u", idx);
    return buf;
}

/* first font that fits on one line, else the last one (text then wraps) */
static __maybe_unused const lv_font_t *fit_font(const char *txt, const lv_font_t *const *fonts, int n) {
    for (int i = 0; i < n; i++) {
        if (text_w(txt, fonts[i]) <= PW) {
            return fonts[i];
        }
    }
    return fonts[n - 1];
}

#if IS_ENABLED(CONFIG_SOFLE_SCREEN_STYLE_LAYERS)
/* copy at most max_chars characters (names are ASCII) */
static const char *clip(const char *src, char *buf, size_t n, int max_chars) {
    size_t i = 0;
    while (src[i] != '\0' && i < (size_t)max_chars && i + 1 < n) {
        buf[i] = src[i];
        i++;
    }
    buf[i] = '\0';
    return buf;
}
#endif

static __maybe_unused uint8_t wpm_now(void) { return st.wpm[HIST - 1]; }

static __maybe_unused uint8_t wpm_peak(void) {
    uint8_t m = 0;
    for (int i = 0; i < HIST; i++) {
        m = MAX(m, st.wpm[i]);
    }
    return m;
}

/* ------------------------------------------------ A: info stack
 *  [bat]   +87%
 *  B 1
 *  # o o . .        profiles
 *  ------------
 *     LAYER
 *  COLEMAK_DH       (auto-sized)
 *  ------------
 *  WPM        64
 *  [######    ]
 *    PEAK 87
 */
#if IS_ENABLED(CONFIG_SOFLE_SCREEN_STYLE_STACK)
static void draw_style(void) {
    char buf[24], nb[16];

    d_battery(0, 2);
    snprintf(buf, sizeof(buf), "%u%%", st.battery);
    d_text(0, 4, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT, FG, buf);

    conn_text(buf, sizeof(buf), true);
    d_text(0, 19, PW, &lv_font_montserrat_16, LV_TEXT_ALIGN_LEFT, FG, buf);

    for (int i = 0; i < PROFILES; i++) {
        int x = 3 + i * 13;
        if (!st.usb_selected && i == st.profile) {
            d_rect(x - 1, 40, 8, 8, FG);
        } else if (st.prof_bond[i]) {
            d_frame(x, 41, 6, 6);
            if (st.prof_conn[i]) {
                d_rect(x + 2, 43, 2, 2, FG);
            }
        } else {
            d_rect(x + 2, 43, 2, 2, FG);
        }
    }

    d_rect(0, 54, PW, 1, FG);
    d_text(0, 59, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, FG, "LAYER");

    static const lv_font_t *const fonts[] = {&lv_font_montserrat_22, &lv_font_montserrat_18,
                                             &lv_font_montserrat_14};
    const char *name = name_or_index(st.layer_name, st.layer_index, nb, sizeof(nb));
    const lv_font_t *f = fit_font(name, fonts, ARRAY_SIZE(fonts));
    int lh = lv_font_get_line_height(f);
    int y = (text_w(name, f) <= PW) ? 88 - lh / 2 : 72; /* centre one line, else wrap */
    d_text(0, y, PW, f, LV_TEXT_ALIGN_CENTER, FG, name);

    d_rect(0, 110, PW, 1, FG);
    d_text(0, 120, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT, FG, "WPM");
    snprintf(buf, sizeof(buf), "%u", wpm_now());
    d_text(0, 116, PW, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT, FG, buf);

    d_frame(0, 136, PW, 10);
    d_rect(2, 138, MIN(wpm_now(), 150) * (PW - 4) / 150, 6, FG);

    snprintf(buf, sizeof(buf), "PEAK %u", wpm_peak());
    d_text(0, 150, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, FG, buf);
}
#endif

/* ------------------------------------------------ B: layer focus
 *  [bat]        B1
 *  ############
 *  #    3     #     big index, inverted
 *  #  Fn1     #
 *  ############
 *     ACTIVE
 *  ------------
 *  # Fn1
 *  o COLEMAK_
 */
#if IS_ENABLED(CONFIG_SOFLE_SCREEN_STYLE_LAYERS)
static void draw_style(void) {
    char buf[24], nb[16];

    d_top_bar();

    d_rect(0, 18, PW, 52, FG);
    snprintf(buf, sizeof(buf), "%u", st.layer_index);
    d_text(0, 19, PW, &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER, BG, buf);

    const char *name = name_or_index(st.layer_name, st.layer_index, nb, sizeof(nb));
    if (text_w(name, &lv_font_montserrat_14) <= PW - 4) {
        d_text(0, 51, PW, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER, BG, name);
    } else {
        d_text(0, 55, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, BG,
               clip(name, buf, sizeof(buf), 8));
    }

    d_text(0, 76, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, FG, "ACTIVE");
    d_rect(0, 86, PW, 1, FG);

    for (int i = 0; i < st.active_cnt && i < 5; i++) {
        int y = 91 + i * 14;
        if (i == 0) {
            d_rect(0, y, 7, 7, FG);
        } else {
            d_frame(0, y, 7, 7);
        }
        /* "5 MouseM": index first so similar names stay distinguishable */
        name = (st.active_name[i] && st.active_name[i][0]) ? st.active_name[i] : "";
        snprintf(nb, sizeof(nb), "%u %s", st.active_idx[i], name);
        d_text(10, y, PW - 10, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT, FG,
               clip(nb, buf, sizeof(buf), 7));
    }
}
#endif

/* ------------------------------------------------ C: typing meter
 *  [bat]        B1
 *       64
 *       WPM
 *  +----------+
 *  | ||  |||| |     last 16 samples
 *  +----------+
 *   COLEMAK_DH
 */
#if IS_ENABLED(CONFIG_SOFLE_SCREEN_STYLE_TYPING)
static void draw_style(void) {
    char buf[24], nb[16];

    d_top_bar();

    snprintf(buf, sizeof(buf), "%u", wpm_now());
    d_text(0, 19, PW, &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER, FG, buf);
    d_text(0, 51, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, FG, "WPM");

    d_frame(0, 64, PW, 64);
    int top = MAX(40, wpm_peak());
    for (int i = 0; i < HIST; i++) {
        int h = st.wpm[i] * 58 / top;
        d_rect(2 + i * 4, 126 - h, 3, h, FG);
    }

    static const lv_font_t *const fonts[] = {&lv_font_montserrat_16, &lv_font_montserrat_14};
    const char *name = name_or_index(st.layer_name, st.layer_index, nb, sizeof(nb));
    const lv_font_t *f = fit_font(name, fonts, ARRAY_SIZE(fonts));
    if (text_w(name, f) <= PW) {
        d_text(0, 135, PW, f, LV_TEXT_ALIGN_CENTER, FG, name);
    } else {
        /* too long even at 14px: small pixel font, up to two lines */
        d_text(0, 136, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, FG, name);
    }
}
#endif

#else /* ------------------------------------------------ right half */

static void draw_style(void) {
    char buf[8];

    d_battery(0, 2);
    d_text(0, 0, PW, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT, FG,
           st.connected ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_CLOSE);

    snprintf(buf, sizeof(buf), "%u%%", st.battery);
    d_text(0, 24, PW, &lv_font_montserrat_28, LV_TEXT_ALIGN_CENTER, FG, buf);
    d_text(0, 58, PW, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER, FG, "BATTERY");

    /* tall gauge, fills from the bottom */
    d_rect(26, 72, 16, 4, FG);
    d_frame(16, 76, 36, 80);
    int fh = MIN(st.battery, 100) * 74 / 100;
    d_rect(19, 153 - fh, 30, fh, FG);
}

#endif

static void redraw(void) {
    if (port_cv == NULL) {
        return;
    }
    d_rect(0, 0, PW, PH, BG);
    draw_style();
    blit();
}

/* ========================================================= listeners */

struct batt_state {
    uint8_t level;
    bool usb;
};

static struct batt_state batt_get(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = eh ? as_zmk_battery_state_changed(eh) : NULL;
    return (struct batt_state){
        .level = ev ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb = zmk_usb_is_powered(),
#endif
    };
}

static void batt_cb(struct batt_state s) {
    st.battery = s.level;
    st.charging = s.usb;
    redraw();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_batt, struct batt_state, batt_cb, batt_get)
ZMK_SUBSCRIPTION(sofle_batt, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(sofle_batt, zmk_usb_conn_state_changed);
#endif

#if IS_CENTRAL

struct out_state {
    bool usb;
    int profile;
    bool conn, bond;
    bool pc[PROFILES], pb[PROFILES];
};

static struct out_state out_get(const zmk_event_t *eh) {
    struct out_state s = {0};
    s.usb = zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    s.profile = zmk_ble_active_profile_index();
    s.conn = zmk_ble_active_profile_is_connected();
    s.bond = !zmk_ble_active_profile_is_open();
    for (int i = 0; i < MIN(PROFILES, ZMK_BLE_PROFILE_COUNT); i++) {
        s.pc[i] = zmk_ble_profile_is_connected(i);
        s.pb[i] = !zmk_ble_profile_is_open(i);
    }
#endif
    return s;
}

static void out_cb(struct out_state s) {
    st.usb_selected = s.usb;
    st.profile = s.profile;
    st.profile_connected = s.conn;
    st.profile_bonded = s.bond;
    memcpy(st.prof_conn, s.pc, sizeof(s.pc));
    memcpy(st.prof_bond, s.pb, sizeof(s.pb));
    redraw();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_out, struct out_state, out_cb, out_get)
ZMK_SUBSCRIPTION(sofle_out, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(sofle_out, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(sofle_out, zmk_ble_active_profile_changed);
#endif

struct lay_state {
    uint8_t index;
    const char *name;
    uint8_t n;
    uint8_t idx[MAX_ACTIVE];
    const char *names[MAX_ACTIVE];
};

static struct lay_state lay_get(const zmk_event_t *eh) {
    struct lay_state s = {0};
    s.index = zmk_keymap_highest_layer_active();
    s.name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(s.index));
    for (int i = ZMK_KEYMAP_LAYERS_LEN - 1; i >= 0 && s.n < MAX_ACTIVE; i--) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(i);
        if (id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(id)) {
            s.idx[s.n] = i;
            s.names[s.n] = zmk_keymap_layer_name(id);
            s.n++;
        }
    }
    return s;
}

static void lay_cb(struct lay_state s) {
    st.layer_index = s.index;
    st.layer_name = s.name;
    st.active_cnt = s.n;
    memcpy(st.active_idx, s.idx, sizeof(s.idx));
    memcpy(st.active_name, s.names, sizeof(s.names));
    redraw();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_lay, struct lay_state, lay_cb, lay_get)
ZMK_SUBSCRIPTION(sofle_lay, zmk_layer_state_changed);

struct wpm_state {
    uint8_t wpm;
};

static struct wpm_state wpm_get(const zmk_event_t *eh) {
    return (struct wpm_state){.wpm = MIN(zmk_wpm_get_state(), 255)};
}

static void wpm_cb(struct wpm_state s) {
    memmove(st.wpm, st.wpm + 1, HIST - 1);
    st.wpm[HIST - 1] = s.wpm;
    redraw();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_wpm, struct wpm_state, wpm_cb, wpm_get)
ZMK_SUBSCRIPTION(sofle_wpm, zmk_wpm_state_changed);

#else /* peripheral */

struct link_state {
    bool connected;
};

static struct link_state link_get(const zmk_event_t *eh) {
    return (struct link_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void link_cb(struct link_state s) {
    st.connected = s.connected;
    redraw();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_link, struct link_state, link_cb, link_get)
ZMK_SUBSCRIPTION(sofle_link, zmk_split_peripheral_status_changed);

#endif

/* ========================================================= entry */

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);

    port_cv = lv_canvas_create(scr);
    lv_canvas_set_buffer(port_cv, port_buf, PW, PH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(port_cv, LV_OBJ_FLAG_HIDDEN);

    land_cv = lv_canvas_create(scr);
    lv_canvas_set_buffer(land_cv, land_buf, PH, PW, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(land_cv, LV_ALIGN_TOP_LEFT, 0, 0);

    sofle_batt_init();
#if IS_CENTRAL
    sofle_out_init();
    sofle_lay_init();
    sofle_wpm_init();
#else
    sofle_link_init();
#endif

    return scr;
}
