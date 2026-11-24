#include "ui.h"
#include "app.h"
#include "board.h"
#include "config.h"
#include "images.h"
#include "kp_util.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(kp_font_18);
LV_FONT_DECLARE(kp_font_24);
LV_FONT_DECLARE(kp_font_32);

#define COL_BG lv_color_hex(0x000000)
#define COL_CARD lv_color_hex(0x262626)
#define COL_TEXT lv_color_hex(0xFFFFFF)
#define COL_MUTED lv_color_hex(s_kids ? 0xE4E4F0 : 0xB3B3B3)  // lighter on the kids' coloured scenes
#define COL_ACCENT lv_color_hex(0x1ED760)

static bool s_kids;  // kids mode: a playful scene behind the screens (see "kids backdrop")

#if CONFIG_KP_BOARD_CUBE
#define IS_CUBE 1
#define F_TITLE (&kp_font_32)
#define F_BODY (&kp_font_24)
#define F_SMALL (&kp_font_24)
#define COVER_BOX 300
#define CARD_PITCH KP_SCREEN_W
#define WINDOW_BEFORE 2
#define WINDOW_AFTER 2
#else
#define IS_CUBE 0
#define F_TITLE (&kp_font_24)
#define F_BODY (&kp_font_18)
#define F_SMALL (&kp_font_18)
#define COVER_BOX 140
#define CTRL_W 64                  // control column on the right
#define CARD_W COVER_BOX
#define CARD_GAP 12
#define CARD_PITCH (CARD_W + CARD_GAP)
#define WINDOW_BEFORE 2
#define WINDOW_AFTER 5
#endif

typedef enum { MODE_NONE, MODE_HOME, MODE_TRACKS, MODE_RESULTS, MODE_SEARCH } mode_t_;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *img;
    lv_obj_t *placeholder;
    lv_obj_t *badge;
    lv_obj_t *badge_label;
    char url[SP_IMG_LEN];
    char id[SP_ID_LEN];
    const lv_image_dsc_t *dsc;
} slot_t;

static lv_obj_t *s_root;     // the screen
static lv_obj_t *s_pager;    // scrolling container in list modes
static slot_t *s_slots;
static int s_nslots;
static mode_t_ s_mode;
static int s_list_token;
static char s_np_id[SP_ID_LEN];
static bool s_np_playing;
static char s_followed_id[SP_ID_LEN];
static lv_obj_t *s_vol_box, *s_vol_bar, *s_toast, *s_battery, *s_wake_catcher, *s_play_btn_label;
static lv_obj_t *s_update_overlay;
static lv_timer_t *s_vol_timer, *s_toast_timer;
static lv_obj_t *s_search_ta;  // search text field while the keyboard is shown
static int s_desired_brightness = 100;
static int s_applied_brightness = 100;
#define TICK_MS 120
// Cards' scroll position at the last tick and at the last frame; -1: no cards, or they were just rebuilt.
static int32_t s_last_scroll_x = -1, s_render_x = -1;

int ui_desired_brightness(void) { return s_desired_brightness; }

// ---------------------------------------------------------------- helpers (LVGL lock held)

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static void release_slots(void)
{
    s_last_scroll_x = s_render_x = -1;  // see measure() and render_ready_cb()
    for (int i = 0; i < s_nslots; i++) {
        if (s_slots[i].dsc) {
            lv_image_set_src(s_slots[i].img, NULL);
            images_release(s_slots[i].dsc);
            s_slots[i].dsc = NULL;
        }
    }
    s_nslots = 0;
    s_mode = MODE_NONE;
    s_pager = NULL;
    s_play_btn_label = NULL;
}

static void clear_screen(void)
{
    release_slots();
    s_search_ta = NULL;
    lv_obj_clean(s_root);
    lv_obj_scroll_to(s_root, 0, 0, LV_ANIM_OFF);
}

static int current_index(void)
{
    if (!s_pager || s_nslots == 0) return 0;
    int x = lv_obj_get_scroll_x(s_pager);
    int i = (x + CARD_PITCH / 2) / CARD_PITCH;
    if (i < 0) i = 0;
    if (i >= s_nslots) i = s_nslots - 1;
    return i;
}

static void scroll_to_index(int i, bool anim)
{
    if (!s_pager || i < 0 || i >= s_nslots) return;
    lv_obj_update_layout(s_pager);
    lv_obj_scroll_to_x(s_pager, i * CARD_PITCH, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void update_badges(void)
{
    for (int i = 0; i < s_nslots; i++) {
        slot_t *s = &s_slots[i];
        if (!s->badge) continue;
        bool on = (s_mode == MODE_TRACKS || s_mode == MODE_RESULTS) && s_np_id[0] && strcmp(s->id, s_np_id) == 0;
        if (on) {
            lv_obj_set_hidden(s->badge, false);
            lv_label_set_text(s->badge_label, s_np_playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        } else {
            lv_obj_set_hidden(s->badge, true);
        }
#if !IS_CUBE
        lv_obj_set_style_border_width(s->img, on ? 4 : 0, 0);
#endif
    }
    if (s_play_btn_label) lv_label_set_text(s_play_btn_label, s_np_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// ---------------------------------------------------------------- kids backdrop
// In kids mode the screens sit on a playful scene instead of black. It is drawn on the display's bottom layer,
// under every screen, so clear_screen() never touches it; the screen turns transparent while it is shown. A new
// scene is picked at random each time the screen has gone dark, so waking the player shows a surprise.
//
// Scenes are laid out in 480x480 design space with the shapes in the margins around the cover and below the
// text, so nothing cuts through a title. A wide screen stretches the positions and keeps shapes round. Shapes
// that overlap are opaque: translucent ones would show a seam where they overlap.

typedef struct {
    uint32_t top, bottom;  // sky: vertical gradient
    void (*draw)(lv_layer_t *L);
} scene_t;

static int s_scene = -1;  // index into s_scenes; -1: plain black
static bool s_scene_due;  // pick a new scene once the screen is dark

#define BD_SX (KP_SCREEN_W * 256 / 480)  // design space -> screen, in 1/256
#define BD_SY (KP_SCREEN_H * 256 / 480)
#define BD_SS (KP_SCREEN_W == KP_SCREEN_H ? BD_SY : (KP_SCREEN_W < KP_SCREEN_H ? BD_SX : BD_SY) * 3 / 2)

static int32_t bx(int32_t x) { return x * BD_SX / 256; }
static int32_t by(int32_t y) { return y * BD_SY / 256; }
static int32_t bs(int32_t v) { int32_t r = v * BD_SS / 256; return r > 0 ? r : 1; }

static uint32_t mix(uint32_t a, uint32_t b, int32_t t)  // t/256 of a, the rest b
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8)
        r |= ((((a >> sh) & 0xFF) * t + ((b >> sh) & 0xFF) * (256 - t)) >> 8) << sh;
    return r;
}

// Screen-space primitives.

static void px_rect(lv_layer_t *L, int32_t cx, int32_t cy, int32_t w, int32_t h, int32_t radius, uint32_t col,
                    lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col);
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = {cx - w / 2, cy - h / 2, cx - w / 2 + w - 1, cy - h / 2 + h - 1};
    lv_draw_rect(L, &d, &a);
}

static void px_circle(lv_layer_t *L, int32_t cx, int32_t cy, int32_t r, uint32_t col)
{
    px_rect(L, cx, cy, 2 * r, 2 * r, LV_RADIUS_CIRCLE, col, LV_OPA_COVER);
}

static void px_tri(lv_layer_t *L, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                   uint32_t col)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = x1;
    d.p[0].y = y1;
    d.p[1].x = x2;
    d.p[1].y = y2;
    d.p[2].x = x3;
    d.p[2].y = y3;
    d.color = lv_color_hex(col);
    d.opa = LV_OPA_COVER;
    lv_draw_triangle(L, &d);
}

// Shapes, placed in design space.

static void dot(lv_layer_t *L, int32_t x, int32_t y, int32_t r, uint32_t col) { px_circle(L, bx(x), by(y), bs(r), col); }

// Big circle mostly below the screen, placed by its top edge: on a wide screen it then stays as low as on the
// square one instead of growing up behind the text.
static void mound(lv_layer_t *L, int32_t x, int32_t top, int32_t r, uint32_t col)
{
    px_circle(L, bx(x), by(top) + bs(r), bs(r), col);
}

static void bar(lv_layer_t *L, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col)
{
    px_rect(L, bx(x), by(y), bs(w), bs(h), LV_RADIUS_CIRCLE, col, LV_OPA_COVER);
}

static void square(lv_layer_t *L, int32_t x, int32_t y, int32_t s, uint32_t col)
{
    px_rect(L, bx(x), by(y), bs(s), bs(s), bs(3), col, LV_OPA_COVER);
}

static void triangle(lv_layer_t *L, int32_t x, int32_t y, int32_t s, uint32_t col)
{
    int32_t cx = bx(x), cy = by(y), S = bs(s);
    px_tri(L, cx - S / 2, cy + S * 2 / 5, cx + S / 2, cy + S * 2 / 5, cx, cy - S / 2, col);
}

// Four-pointed twinkle: a dot with four tapered arms that start inside it, so no two edges meet exactly.
static void twinkle(lv_layer_t *L, int32_t x, int32_t y, int32_t r, uint32_t col)
{
    int32_t cx = bx(x), cy = by(y), R = bs(r), w = bs(r) / 4 + 1;
    px_circle(L, cx, cy, w, col);
    px_tri(L, cx - w, cy, cx + w, cy, cx, cy - R, col);
    px_tri(L, cx - w, cy, cx + w, cy, cx, cy + R, col);
    px_tri(L, cx, cy - w, cx, cy + w, cx - R, cy, col);
    px_tri(L, cx, cy - w, cx, cy + w, cx + R, cy, col);
}

// Chubby five-pointed star: a round middle with five tips.
static void star(lv_layer_t *L, int32_t x, int32_t y, int32_t r, uint32_t col)
{
    int32_t cx = bx(x), cy = by(y), R = bs(r), ri = bs(r) * 45 / 100;
    px_circle(L, cx, cy, ri, col);
    for (int k = 0; k < 5; k++) {
        int32_t a = 270 + k * 72;
        px_tri(L, cx + ri * lv_trigo_cos(a - 40) / 32767, cy + ri * lv_trigo_sin(a - 40) / 32767,
               cx + ri * lv_trigo_cos(a + 40) / 32767, cy + ri * lv_trigo_sin(a + 40) / 32767,
               cx + R * lv_trigo_cos(a) / 32767, cy + R * lv_trigo_sin(a) / 32767, col);
    }
}

static void cloud(lv_layer_t *L, int32_t x, int32_t y, int32_t s, uint32_t col)
{
    int32_t cx = bx(x), cy = by(y), S = bs(s);
    px_rect(L, cx, cy + S / 4, S * 2, S * 3 / 4, LV_RADIUS_CIRCLE, col, LV_OPA_COVER);
    px_circle(L, cx - S / 3, cy, S / 2, col);
    px_circle(L, cx + S / 4, cy - S / 8, S * 2 / 3, col);
}

static void bubble(lv_layer_t *L, int32_t x, int32_t y, int32_t r)
{
    int32_t cx = bx(x), cy = by(y), R = bs(r);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;  // a ring only: a translucent fill costs as much again to draw
    d.border_color = lv_color_white();
    d.border_opa = LV_OPA_70;
    d.border_width = R > 10 ? 3 : 2;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {cx - R, cy - R, cx + R, cy + R};
    lv_draw_rect(L, &d, &a);
    px_rect(L, cx - R / 3, cy - R / 3, R / 2 + 2, R / 2 + 2, LV_RADIUS_CIRCLE, 0xFFFFFF, LV_OPA_80);
}

static void fish(lv_layer_t *L, int32_t x, int32_t y, int32_t s, uint32_t col, bool facing_left)
{
    int32_t cx = bx(x), cy = by(y), S = bs(s), d = facing_left ? 1 : -1;
    int32_t tail = cx + d * S * 9 / 10;
    px_tri(L, cx + d * S / 2, cy, tail, cy - S * 2 / 5, tail, cy + S * 2 / 5, col);
    px_rect(L, cx, cy, S * 3 / 2, S, LV_RADIUS_CIRCLE, col, LV_OPA_COVER);
    px_circle(L, cx - d * S * 2 / 5, cy - S / 8, S / 8 + 1, 0x0C2A4A);
}

static void flower(lv_layer_t *L, int32_t x, int32_t y, uint32_t col, uint32_t middle)
{
    dot(L, x, y, 7, col);
    dot(L, x, y, 3, middle);
}

static uint32_t sky_at(const scene_t *sc, int32_t screen_y)
{
    return mix(sc->bottom, sc->top, screen_y * 256 / KP_SCREEN_H);
}

// Scenes.

enum { YEL = 0xFFD23F, PNK = 0xFF5D8F, MNT = 0x3DDC97, SKY = 0x4CC9F0, ORG = 0xFF9F1C, LIL = 0xC77DFF,
       WHT = 0xFFFFFF, MOON = 0xFFE27A };

static void draw_confetti(lv_layer_t *L)
{
    enum { DOT, SQUARE, TRI, BAR, UPRIGHT, TWINKLE };
    static const uint32_t c[][5] = {  // x, y, shape, size, colour
        {30, 118, DOT, 9, YEL},       {64, 152, SQUARE, 14, MNT},  {24, 196, TRI, 18, PNK},
        {62, 232, DOT, 6, SKY},       {34, 270, BAR, 26, ORG},     {66, 304, TWINKLE, 12, YEL},
        {26, 338, DOT, 10, LIL},      {58, 92, UPRIGHT, 22, SKY},  {118, 22, SQUARE, 12, PNK},
        {166, 14, DOT, 6, YEL},       {206, 28, BAR, 24, SKY},     {262, 16, DOT, 8, MNT},
        {302, 30, TRI, 15, ORG},      {350, 18, TWINKLE, 11, LIL}, {440, 80, DOT, 8, MNT},
        {414, 112, UPRIGHT, 24, PNK}, {452, 150, TRI, 18, YEL},    {420, 192, DOT, 11, SKY},
        {456, 230, SQUARE, 14, ORG},  {418, 266, TWINKLE, 13, MNT}, {450, 304, DOT, 7, PNK},
        {424, 338, BAR, 22, YEL},     {36, 462, DOT, 8, PNK},      {92, 470, BAR, 24, MNT},
        {150, 458, SQUARE, 10, YEL},  {330, 466, DOT, 7, SKY},     {384, 458, TRI, 15, PNK},
        {440, 466, DOT, 10, ORG},
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        int32_t x = (int32_t)c[i][0], y = (int32_t)c[i][1], s = (int32_t)c[i][3];
        uint32_t col = c[i][4];
        switch (c[i][2]) {
        case DOT: dot(L, x, y, s, col); break;
        case SQUARE: square(L, x, y, s, col); break;
        case TRI: triangle(L, x, y, s, col); break;
        case BAR: bar(L, x, y, s, 9, col); break;
        case UPRIGHT: bar(L, x, y, 9, s, col); break;
        default: twinkle(L, x, y, s, col); break;
        }
    }
}

static void draw_night(lv_layer_t *L);
static void draw_sea(lv_layer_t *L);
static void draw_rainbow(lv_layer_t *L);

static const scene_t s_scenes[] = {
    {0x6A2C91, 0x2B1A66, draw_confetti},
    {0x12185A, 0x3A1F6E, draw_night},
    {0x1A8FB5, 0x0C3B6B, draw_sea},
    {0x2A63C9, 0x4A8FE0, draw_rainbow},
};
#define N_SCENES ((int)(sizeof(s_scenes) / sizeof(s_scenes[0])))

static void draw_night(lv_layer_t *L)
{
    const scene_t *sc = &s_scenes[1];
    int32_t mx = bx(434), my = by(120);
    px_circle(L, mx, my, bs(30), MOON);
    px_circle(L, mx + bs(13), my - bs(11), bs(26), sky_at(sc, my - bs(11)));  // the bite that makes it a crescent
    static const uint32_t tw[][4] = {
        {40, 130, 14, MOON}, {62, 252, 9, WHT},   {420, 222, 12, MOON}, {450, 300, 8, WHT},
        {150, 24, 10, WHT},  {330, 22, 13, MOON}, {26, 330, 8, MOON},   {244, 12, 6, WHT},
    };
    for (size_t i = 0; i < sizeof(tw) / sizeof(tw[0]); i++)
        twinkle(L, (int32_t)tw[i][0], (int32_t)tw[i][1], (int32_t)tw[i][2], tw[i][3]);
    static const int16_t dots[][2] = {
        {20, 70},  {70, 196},  {30, 288},  {58, 330},  {108, 36}, {196, 30}, {286, 36},  {380, 30},
        {412, 70}, {460, 180}, {402, 276}, {466, 336}, {74, 100}, {20, 228}, {458, 250}, {410, 164},
    };
    for (size_t i = 0; i < sizeof(dots) / sizeof(dots[0]); i++)
        px_rect(L, bx(dots[i][0]), by(dots[i][1]), bs(i % 3 ? 4 : 6), bs(i % 3 ? 4 : 6), LV_RADIUS_CIRCLE, WHT,
                i % 2 ? LV_OPA_70 : LV_OPA_COVER);
    uint32_t cl = mix(WHT, sc->bottom, 56);
    cloud(L, 40, 474, 40, cl);
    cloud(L, 120, 486, 34, cl);
    cloud(L, 370, 488, 36, cl);
    cloud(L, 446, 472, 44, cl);
}

static void draw_sea(lv_layer_t *L)
{
    const uint32_t weed = 0x1FA36B, sand = 0xE8C27A;
    for (int i = 0; i < 6; i++) {  // seaweed: wavy columns of blobs
        dot(L, 18 + ((i & 1) ? 7 : -3), 476 - i * 22, 12, weed);
        dot(L, 462 + ((i & 1) ? -7 : 3), 480 - i * 20, 11, weed);
    }
    for (int i = 0; i < 3; i++) dot(L, 52 + ((i & 1) ? 5 : -3), 480 - i * 20, 10, 0x2BC27E);
    mound(L, 110, 456, 110, sand);
    mound(L, 300, 454, 132, mix(sand, 0xC99A52, 200));
    star(L, 150, 470, 13, 0xFF7A45);
    star(L, 330, 472, 11, PNK);
    fish(L, 238, 22, 26, 0xFFB347, true);
    fish(L, 44, 300, 22, PNK, false);
    fish(L, 440, 230, 20, YEL, true);
    static const int16_t b[][3] = {
        {40, 120, 16}, {66, 170, 9},   {30, 214, 12}, {436, 96, 10}, {452, 150, 18},
        {448, 280, 13}, {420, 322, 6}, {130, 24, 9},  {360, 20, 12},
    };
    for (size_t i = 0; i < sizeof(b) / sizeof(b[0]); i++) bubble(L, b[i][0], b[i][1], b[i][2]);
}

static void draw_rainbow(lv_layer_t *L)
{
    // A quarter rainbow sweeping out of the top-right corner, landing on a cloud. Arc cost grows with the square of
    // the radius (every pixel of its bounding box is masked), and the cube redraws the whole frame while swiping:
    // a half-circle rainbow around the cover cost three times a black frame on its own.
    static const uint32_t bands[] = {0xFF595E, 0xFF924C, 0xFFCA3A, 0x8AC926, 0x52D1DC, 0x9B5DE5};
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = bx(492);
    a.center.y = by(-12);
    a.start_angle = 90;
    a.end_angle = 180;
    for (int i = 0; i < 6; i++) {  // each band overlaps the next by a pixel, so no gap shows between them
        a.radius = bs(122 - i * 9);
        a.width = bs(10);
        a.color = lv_color_hex(bands[i]);
        lv_draw_arc(L, &a);
    }
    dot(L, 24, 24, 56, YEL);  // sun
    cloud(L, 196, 22, 28, WHT);
    cloud(L, 446, 112, 26, WHT);
    dot(L, 420, 760, 340, 0x3DAA5C);  // hills
    dot(L, 50, 742, 330, 0x2A8C4A);
    flower(L, 30, 448, PNK, YEL);
    flower(L, 80, 466, YEL, ORG);
    flower(L, 130, 466, WHT, YEL);
    flower(L, 372, 470, LIL, YEL);
    flower(L, 416, 466, PNK, YEL);
    flower(L, 458, 456, YEL, ORG);
}

static void scene_draw_cb(lv_event_t *e)
{
    if (s_scene >= 0) s_scenes[s_scene].draw(lv_event_get_layer(e));
}

static void show_scene(int i)
{
    lv_obj_t *bl = lv_layer_bottom();
    s_scene = i;
    if (i >= 0) {
        lv_obj_set_style_bg_color(bl, lv_color_hex(s_scenes[i].top), 0);
        lv_obj_set_style_bg_grad_color(bl, lv_color_hex(s_scenes[i].bottom), 0);
        lv_obj_set_style_bg_grad_dir(bl, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(bl, LV_OPA_COVER, 0);
    }
    // An opaque screen hides the bottom layer, and LVGL then skips drawing it.
    lv_obj_set_style_bg_opa(s_root, i >= 0 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_invalidate(bl);
}

static int random_scene(void)
{
    if (s_scene < 0) return (int)(esp_random() % N_SCENES);
    return (s_scene + 1 + (int)(esp_random() % (N_SCENES - 1))) % N_SCENES;  // never the same twice in a row
}

void ui_set_kids_mode(bool kids)
{
    board_lock(-1);
    if (kids != s_kids) {
        s_kids = kids;
        show_scene(kids ? random_scene() : -1);
    }
    board_unlock();
}

// ---------------------------------------------------------------- periodic work (runs in the LVGL task)

static void manage_images(void)
{
    if (s_mode == MODE_NONE || !s_pager || s_nslots == 0) return;
    int cur = current_index();
    int lo = cur - WINDOW_BEFORE, hi = cur + WINDOW_AFTER;
    // Drop images far away, so the cache can reuse them.
    for (int i = 0; i < s_nslots; i++) {
        slot_t *s = &s_slots[i];
        if ((i < lo - 2 || i > hi + 2) && s->dsc) {
            lv_image_set_src(s->img, NULL);
            images_release(s->dsc);
            s->dsc = NULL;
            lv_obj_set_hidden(s->placeholder, false);
        }
    }
    // Load what is close; request the current page last so it is fetched first.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = hi; i >= lo; i--) {
            if (i < 0 || i >= s_nslots) continue;
            if ((pass == 0) == (i == cur)) continue;
            slot_t *s = &s_slots[i];
            if (s->dsc || !s->url[0]) continue;
            bool failed = false;
            const lv_image_dsc_t *d = images_acquire(s->url, &failed);
            if (d) {
                s->dsc = d;
                lv_image_set_src(s->img, d);
                lv_obj_set_hidden(s->placeholder, true);
            } else if (!failed) {
                images_request(s->url);
            }
        }
    }
}

static void follow_now_playing(void)
{
    if (s_mode != MODE_TRACKS || !s_np_id[0] || strcmp(s_np_id, s_followed_id) == 0) return;
    if (lv_display_get_inactive_time(NULL) < 15000) return;  // don't fight a child who is browsing
    for (int i = 0; i < s_nslots; i++) {
        if (strcmp(s_slots[i].id, s_np_id) == 0) {
            if (current_index() != i) scroll_to_index(i, true);
            break;
        }
    }
    kp_strlcpy(s_followed_id, s_np_id, sizeof(s_followed_id));
}

static void wake_catcher_cb(lv_event_t *e)
{
    // Swallows the touch that wakes the screen.
}

static void dim_screen(void)
{
    uint32_t idle = lv_display_get_inactive_time(NULL);
    int want = idle < 60000 ? 100 : (idle < 300000 ? 25 : 0);
    s_desired_brightness = want;
    // A new scene only while the screen is off, so the next wake-up shows it and it never changes in view.
    if (want > 0) {
        s_scene_due = true;
    } else if (s_kids && s_scene_due && s_applied_brightness == 0) {
        show_scene(random_scene());
        s_scene_due = false;
    }
    if (want == 0 && !s_wake_catcher) {
        s_wake_catcher = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(s_wake_catcher);
        lv_obj_set_size(s_wake_catcher, LV_PCT(100), LV_PCT(100));
        lv_obj_set_clickable(s_wake_catcher, true);
        lv_obj_add_event_cb(s_wake_catcher, wake_catcher_cb, LV_EVENT_CLICKED, NULL);
    } else if (want > 0 && s_wake_catcher && s_applied_brightness > 0) {
        lv_obj_delete(s_wake_catcher);
        s_wake_catcher = NULL;
    }
}

// ---------------------------------------------------------------- smoothness counters (settings page)

static uint32_t s_last_tick, s_last_render;
static uint32_t s_scroll_frames, s_scroll_ms;
static int s_longest_pause_ms;

// Frame rate while the cards move: a frame counts the time since the previous frame when both moved the cards.
// The first frame of a swipe would count the idle time before it; gaps of 1 s or more show up as a pause.
static void render_ready_cb(lv_event_t *e)
{
    static bool prev_moved;
    uint32_t now = lv_tick_get();
    int32_t x = s_pager ? lv_obj_get_scroll_x(s_pager) : -1;
    bool moved = x >= 0 && s_render_x >= 0 && x != s_render_x;
    if (moved && prev_moved && now - s_last_render < 1000) {
        s_scroll_frames++;
        s_scroll_ms += now - s_last_render;
    }
    prev_moved = moved;
    s_render_x = x;
    s_last_render = now;
}

// A late tick means the LVGL task was blocked. Only between two ticks on the same cards: building a screen
// also blocks the task, but that is not browsing.
static void measure(void)
{
    uint32_t now = lv_tick_get();
    int32_t x = s_pager ? lv_obj_get_scroll_x(s_pager) : -1;
    if (s_last_tick && s_last_scroll_x >= 0 && x >= 0) {
        int late = (int)(now - s_last_tick) - TICK_MS;
        if (late > s_longest_pause_ms) s_longest_pause_ms = late;
    }
    s_last_scroll_x = x;
    s_last_tick = now;
}

void ui_get_diag(ui_diag_t *d)
{
    d->scroll_fps = s_scroll_ms ? (int)(s_scroll_frames * 1000ULL / s_scroll_ms) : 0;
    d->longest_pause_ms = s_longest_pause_ms;
}

static void tick_cb(lv_timer_t *t)
{
    measure();
    if (!s_update_overlay) {
        manage_images();
        follow_now_playing();
    }
    dim_screen();
}

// Called by the app's button task (outside the LVGL lock).
void ui_applied_brightness(int b) { s_applied_brightness = b; }

// ---------------------------------------------------------------- overlays

static void hide_cb(lv_timer_t *t)
{
    lv_obj_t *o = lv_timer_get_user_data(t);
    lv_obj_set_hidden(o, true);
    lv_timer_pause(t);
}

static void make_overlays(void)
{
    lv_obj_t *top = lv_layer_top();
    s_vol_box = lv_obj_create(top);
    lv_obj_set_size(s_vol_box, IS_CUBE ? 340 : 360, IS_CUBE ? 70 : 56);
    lv_obj_align(s_vol_box, LV_ALIGN_BOTTOM_MID, 0, IS_CUBE ? -24 : -8);
    lv_obj_set_style_bg_color(s_vol_box, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(s_vol_box, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_vol_box, 35, 0);
    lv_obj_set_style_border_width(s_vol_box, 0, 0);
    lv_obj_set_scrollable(s_vol_box, false);
    lv_obj_t *icon = label(s_vol_box, &lv_font_montserrat_28, COL_TEXT, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
    s_vol_bar = lv_bar_create(s_vol_box);
    lv_obj_set_size(s_vol_bar, IS_CUBE ? 240 : 270, 14);
    lv_obj_align(s_vol_bar, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0x505050), 0);
    lv_obj_set_style_bg_color(s_vol_bar, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_hidden(s_vol_box, true);
    s_vol_timer = lv_timer_create(hide_cb, 1500, s_vol_box);
    lv_timer_pause(s_vol_timer);

    s_toast = lv_label_create(top);
    lv_obj_set_style_text_font(s_toast, F_BODY, 0);
    lv_obj_set_style_text_color(s_toast, COL_TEXT, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(s_toast, 12, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_width(s_toast, IS_CUBE ? 400 : 420);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, IS_CUBE ? 24 : 8);
    lv_obj_set_hidden(s_toast, true);
    s_toast_timer = lv_timer_create(hide_cb, 3000, s_toast);
    lv_timer_pause(s_toast_timer);

    s_battery = label(top, &lv_font_montserrat_20, COL_MUTED, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -28, 22);
    lv_obj_set_hidden(s_battery, true);
}

// ---------------------------------------------------------------- init

void ui_init(lv_display_t *disp)
{
    s_slots = heap_caps_calloc(KP_MAX_TRACKS > KP_MAX_HOME ? KP_MAX_TRACKS : KP_MAX_HOME, sizeof(slot_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    board_lock(-1);
    s_root = lv_screen_active();
    lv_obj_set_style_bg_color(s_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_add_event_cb(lv_display_get_layer_bottom(disp), scene_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    make_overlays();
    lv_timer_create(tick_cb, TICK_MS, NULL);
    lv_display_add_event_cb(disp, render_ready_cb, LV_EVENT_RENDER_READY, NULL);
    board_unlock();
}

// ---------------------------------------------------------------- simple screens

static void build_message(lv_obj_t *parent, const char *icon, const char *title, const char *text, bool spinner)
{
#if IS_CUBE
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 420, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 16, 0);
    if (spinner) {
        lv_obj_t *sp = lv_spinner_create(col);
        lv_obj_set_size(sp, 72, 72);
        lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
    } else if (icon) {
        label(col, &lv_font_montserrat_40, COL_ACCENT, icon);
    }
    lv_obj_t *t = label(col, F_TITLE, COL_TEXT, title);
    lv_obj_set_width(t, 420);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    if (text && text[0]) {
        lv_obj_t *b = label(col, F_BODY, COL_MUTED, text);
        lv_obj_set_width(b, 420);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    }
#else
    if (spinner) {
        lv_obj_t *sp = lv_spinner_create(parent);
        lv_obj_set_size(sp, 64, 64);
        lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_align(sp, LV_ALIGN_LEFT_MID, 24, 0);
    } else if (icon) {
        lv_obj_t *i = label(parent, &lv_font_montserrat_40, COL_ACCENT, icon);
        lv_obj_align(i, LV_ALIGN_LEFT_MID, 32, 0);
    }
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 500, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, 116, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *t = label(col, F_TITLE, COL_TEXT, title);
    lv_obj_set_width(t, 500);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    if (text && text[0]) {
        lv_obj_t *b = label(col, F_BODY, COL_MUTED, text);
        lv_obj_set_width(b, 500);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    }
#endif
}

void ui_show_message(const char *icon, const char *title, const char *text, bool spinner)
{
    board_lock(-1);
    clear_screen();
    build_message(s_root, icon, title, text, spinner);
    board_unlock();
}

void ui_show_update(const char *text, bool complete)
{
    board_lock(-1);
    if (!s_update_overlay) {
        s_update_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_update_overlay);
        lv_obj_set_size(s_update_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_update_overlay, COL_BG, 0);
        lv_obj_set_style_bg_opa(s_update_overlay, LV_OPA_COVER, 0);
        lv_obj_set_scrollable(s_update_overlay, false);
        lv_obj_set_clickable(s_update_overlay, true);
    } else {
        lv_obj_clean(s_update_overlay);
    }
    build_message(s_update_overlay, complete ? LV_SYMBOL_OK : NULL, complete ? "Updated" : "Updating…", text,
                  !complete);
    board_unlock();
}

void ui_hide_update(void)
{
    board_lock(-1);
    if (s_update_overlay) {
        lv_obj_delete(s_update_overlay);
        s_update_overlay = NULL;
    }
    board_unlock();
}

void ui_show_qr(const char *title, const char *qr_text, const char *line1, const char *line2)
{
    board_lock(-1);
    clear_screen();
#if IS_CUBE
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, title);
    lv_obj_set_width(t, 420);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_t *qr = lv_qrcode_create(s_root);
    lv_qrcode_set_size(qr, 240);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_text, strlen(qr_text));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 10, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *l1 = label(s_root, &kp_font_18, COL_TEXT, line1);
    lv_obj_set_width(l1, 420);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 340);
    if (line2 && line2[0]) {
        lv_obj_t *l2 = label(s_root, &kp_font_18, COL_MUTED, line2);
        lv_obj_set_width(l2, 400);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(l2, l1, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    }
#else
    lv_obj_t *qr = lv_qrcode_create(s_root);
    lv_qrcode_set_size(qr, 148);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_text, strlen(qr_text));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 450, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, 178, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *t = label(col, F_TITLE, COL_TEXT, title);
    lv_obj_set_width(t, 450);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *l1 = label(col, &kp_font_18, COL_TEXT, line1);
    lv_obj_set_width(l1, 450);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_MODE_WRAP);
    if (line2 && line2[0]) {
        lv_obj_t *l2 = label(col, &kp_font_18, COL_MUTED, line2);
        lv_obj_set_width(l2, 450);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_WRAP);
    }
#endif
    board_unlock();
}

static void room_cb(lv_event_t *e)
{
    app_post(CMD_SELECT_ROOM, (int)(intptr_t)lv_event_get_user_data(e), 0);
}

static lv_obj_t *big_button(lv_obj_t *parent, const char *text, int w, int h, bool selected)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, selected ? COL_ACCENT : COL_CARD, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, F_BODY, selected ? lv_color_black() : COL_TEXT, text);
    lv_obj_set_width(l, w - 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

void ui_show_rooms(const sonos_room_t *rooms, int n, const char *current_uid)
{
    board_lock(-1);
    clear_screen();
    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
#if IS_CUBE
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Which speaker should the music play on?");
    lv_obj_set_width(t, 420);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_size(list, 420, 360);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = big_button(list, rooms[i].name, 400, 72, current_uid && strcmp(current_uid, rooms[i].uid) == 0);
        lv_obj_add_event_cb(b, room_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
#else
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Speaker:");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_size(list, 540, 150);
    lv_obj_align(list, LV_ALIGN_LEFT_MID, 96, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_HOR);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = big_button(list, rooms[i].name, 180, 120, current_uid && strcmp(current_uid, rooms[i].uid) == 0);
        lv_obj_add_event_cb(b, room_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
#endif
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    board_unlock();
}

static void parent_btn_cb(lv_event_t *e)
{
    app_post((app_cmd_type_t)(intptr_t)lv_event_get_user_data(e), 0, 0);
}

void ui_show_parent(const char *settings_url, const char *room_name)
{
    char room_txt[96];
    snprintf(room_txt, sizeof(room_txt), LV_SYMBOL_AUDIO "  %s", room_name && room_name[0] ? room_name : "Choose speaker");
    char address[72];
    const char *key = strchr(settings_url, '=');
    if (key) {
        snprintf(address, sizeof(address), "%.*s\n%s", (int)(key + 1 - settings_url), settings_url, key + 1);
    } else {
        kp_strlcpy(address, settings_url, sizeof(address));
    }
    board_lock(-1);
    clear_screen();
    lv_obj_t *qr = lv_qrcode_create(s_root);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
#if IS_CUBE
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Settings: scan with your phone");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lv_qrcode_set_size(qr, 180);
    lv_qrcode_update(qr, settings_url, strlen(settings_url));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *u = label(s_root, &kp_font_18, COL_MUTED, address);
    lv_obj_set_width(u, 420);
    lv_label_set_long_mode(u, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(u, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_t *rb = big_button(s_root, room_txt, 400, 64, false);
    lv_obj_align(rb, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_t *db = big_button(s_root, "Done", 400, 64, true);
    lv_obj_align(db, LV_ALIGN_TOP_MID, 0, 394);
#else
    lv_qrcode_set_size(qr, 148);
    lv_qrcode_update(qr, settings_url, strlen(settings_url));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_LEFT_MID, 6, 0);
    // Title, the two-line address, then both buttons in one row: three rows fit 172 px without crowding.
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Settings: scan with your phone");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 178, 8);
    lv_obj_t *u = label(s_root, &kp_font_18, COL_MUTED, address);
    lv_obj_set_width(u, 456);
    lv_label_set_long_mode(u, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(u, LV_ALIGN_TOP_LEFT, 178, 38);
    lv_obj_t *rb = big_button(s_root, room_txt, 280, 56, false);
    lv_obj_align(rb, LV_ALIGN_TOP_LEFT, 178, 106);
    lv_obj_t *db = big_button(s_root, "Done", 164, 56, true);
    lv_obj_align(db, LV_ALIGN_TOP_LEFT, 470, 106);
#endif
    lv_obj_add_event_cb(rb, parent_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)CMD_PICK_ROOM);
    lv_obj_add_event_cb(db, parent_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)CMD_CLOSE_PARENT);
    board_unlock();
}

// ---------------------------------------------------------------- browsing screens

static void card_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nslots) return;
    bool is_playing_song = s_np_id[0] && strcmp(s_slots[i].id, s_np_id) == 0;
    if (s_mode == MODE_HOME) {
        app_post(CMD_OPEN_HOME, i, 0);
    } else if ((s_mode == MODE_TRACKS || s_mode == MODE_RESULTS) && is_playing_song) {
        // Tapping the song that is on: pause / resume instead of starting it over.
        app_post(CMD_TOGGLE, 0, 0);
        s_np_playing = !s_np_playing;
        update_badges();
    } else if (s_mode == MODE_TRACKS) {
        app_post(CMD_PLAY, s_list_token, i);
        ui_mark_playing_request(i);
    } else if (s_mode == MODE_RESULTS) {
        app_post(CMD_OPEN_RESULT, i, 0);
    }
}

static void ctrl_cb(lv_event_t *e)
{
    app_post((app_cmd_type_t)(intptr_t)lv_event_get_user_data(e), 0, 0);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int size, app_cmd_type_t cmd)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COL_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, &lv_font_montserrat_28, COL_TEXT, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, ctrl_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    return b;
}

static const char *placeholder_icon(uint8_t kind)
{
    switch (kind) {
    case SP_KIND_SEARCH: return LV_SYMBOL_KEYBOARD;
    case SP_KIND_LIKED: return LV_SYMBOL_OK;
    case SP_KIND_ALBUM: return LV_SYMBOL_IMAGE;
    case SP_KIND_PLAYLIST: return LV_SYMBOL_LIST;
    default: return LV_SYMBOL_AUDIO;
    }
}

#define CHIP_ALBUM 1
#define CHIP_PLAYLIST 2

// One line of text, cut with "…" when too long (DOTS needs a fixed height).
static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text, int w)
{
    lv_obj_t *l = label(parent, font, color, text);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// Build a pager/carousel of cards. subs may be NULL (second line, cube only). chips: on the strip, mark
// albums and/or playlists with a small tag on the cover (the cube shows this in the second line).
static void build_cards(const sp_entry_t *e, int n, const char *const *subs, bool can_go_back, unsigned chips)
{
    s_pager = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pager);
#if IS_CUBE
    lv_obj_set_size(s_pager, KP_SCREEN_W, KP_SCREEN_H);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_style_pad_column(s_pager, 0, 0);
#else
    lv_obj_set_size(s_pager, KP_SCREEN_W - CTRL_W, KP_SCREEN_H);
    lv_obj_set_style_pad_left(s_pager, CARD_GAP, 0);
    lv_obj_set_style_pad_right(s_pager, CARD_GAP, 0);
    lv_obj_set_style_pad_column(s_pager, CARD_GAP, 0);
#endif
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
#if IS_CUBE
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scroll_one(s_pager, true);
#endif

    for (int i = 0; i < n; i++) {
        slot_t *s = &s_slots[i];
        memset(s, 0, sizeof(*s));
        kp_strlcpy(s->url, e[i].img, sizeof(s->url));
        kp_strlcpy(s->id, e[i].id, sizeof(s->id));
        bool special = e[i].kind == SP_KIND_SEARCH || e[i].kind == SP_KIND_LIKED;
        lv_obj_t *c = lv_obj_create(s_pager);
        s->card = c;
        lv_obj_remove_style_all(c);
        lv_obj_set_clickable(c, true);
        lv_obj_set_scrollable(c, false);
        lv_obj_set_event_bubble(c, true);
        lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
#if IS_CUBE
        lv_obj_set_size(c, KP_SCREEN_W, KP_SCREEN_H);
        int cx = (KP_SCREEN_W - COVER_BOX) / 2, cy = 46;
#else
        lv_obj_set_size(c, CARD_W, KP_SCREEN_H);
        int cx = 0, cy = 4;
#endif
        s->placeholder = lv_obj_create(c);
        lv_obj_remove_style_all(s->placeholder);
        lv_obj_set_size(s->placeholder, COVER_BOX, COVER_BOX);
        lv_obj_set_pos(s->placeholder, cx, cy);
        lv_obj_set_style_bg_color(s->placeholder, special ? COL_ACCENT : COL_CARD, 0);
        lv_obj_set_style_bg_opa(s->placeholder, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s->placeholder, 16, 0);
        lv_obj_set_clickable(s->placeholder, false);
        lv_obj_t *note = label(s->placeholder, &lv_font_montserrat_40, special ? lv_color_black() : COL_MUTED,
                               placeholder_icon(e[i].kind));
        lv_obj_center(note);

        s->img = lv_image_create(c);
        lv_obj_set_size(s->img, COVER_BOX, COVER_BOX);
        lv_obj_set_pos(s->img, cx, cy);
        lv_image_set_inner_align(s->img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_clickable(s->img, false);
#if !IS_CUBE
        lv_obj_set_style_border_color(s->img, COL_ACCENT, 0);  // shown on the song that is playing
        lv_obj_set_style_border_width(s->img, 0, 0);
        lv_obj_set_style_border_post(s->img, true, 0);  // draw it over the cover, not under it
        const char *chip = (e[i].kind == SP_KIND_ALBUM && (chips & CHIP_ALBUM))         ? "Album"
                           : (e[i].kind == SP_KIND_PLAYLIST && (chips & CHIP_PLAYLIST)) ? "Playlist"
                                                                                       : NULL;
        if (chip) {
            lv_obj_t *ch = label(c, &lv_font_montserrat_14, COL_TEXT, chip);
            lv_obj_set_style_bg_color(ch, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(ch, LV_OPA_70, 0);
            lv_obj_set_style_pad_hor(ch, 6, 0);
            lv_obj_set_style_pad_ver(ch, 2, 0);
            lv_obj_set_style_radius(ch, 6, 0);
            lv_obj_set_pos(ch, cx + 6, cy + 6);
        }
#endif

        s->badge = lv_obj_create(c);
        lv_obj_remove_style_all(s->badge);
        int bs = IS_CUBE ? 72 : 44;
        lv_obj_set_size(s->badge, bs, bs);
        lv_obj_set_pos(s->badge, cx + COVER_BOX - bs - 8, cy + COVER_BOX - bs - 8);
        lv_obj_set_style_radius(s->badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s->badge, COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(s->badge, LV_OPA_COVER, 0);
        lv_obj_set_clickable(s->badge, false);
        s->badge_label = label(s->badge, IS_CUBE ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                               lv_color_black(), LV_SYMBOL_PLAY);
        lv_obj_center(s->badge_label);
        lv_obj_set_hidden(s->badge, true);

#if IS_CUBE
        lv_obj_t *t = one_line(c, F_TITLE, COL_TEXT, e[i].name, 440);
        lv_obj_set_pos(t, 20, cy + COVER_BOX + 14);
        if (subs && subs[i] && subs[i][0]) {
            lv_obj_t *a = one_line(c, F_BODY, COL_MUTED, subs[i], 440);
            lv_obj_set_pos(a, 20, cy + COVER_BOX + 62);
        }
#else
        lv_obj_t *t = one_line(c, F_SMALL, COL_TEXT, e[i].name, CARD_W);
        lv_obj_set_pos(t, 0, cy + COVER_BOX + 2);
#endif
    }
    s_nslots = n;

#if IS_CUBE
    if (can_go_back) {
        lv_obj_t *b = round_button(s_root, LV_SYMBOL_LEFT, 60, CMD_BACK);
        lv_obj_set_pos(b, 22, 22);
    }
#else
    lv_obj_t *ctrl = lv_obj_create(s_root);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, CTRL_W, KP_SCREEN_H);
    lv_obj_align(ctrl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(ctrl, false);
    if (can_go_back) round_button(ctrl, LV_SYMBOL_LEFT, 38, CMD_BACK);
    round_button(ctrl, LV_SYMBOL_PLUS, 38, CMD_VOL_UP);
    lv_obj_t *pb = round_button(ctrl, LV_SYMBOL_PLAY, 38, CMD_TOGGLE);
    s_play_btn_label = lv_obj_get_child(pb, 0);
    round_button(ctrl, LV_SYMBOL_MINUS, 38, CMD_VOL_DOWN);
#endif
}

void ui_show_home(const sp_entry_t *e, int n, int focus)
{
    if (n > KP_MAX_HOME) n = KP_MAX_HOME;
    // Second line on the cube: the artist of an album.
    const char **subs = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!subs) return;
    for (int i = 0; i < n; i++) subs[i] = e[i].kind == SP_KIND_ALBUM ? e[i].artist : "";
    board_lock(-1);
    clear_screen();
    build_cards(e, n, subs, false, CHIP_ALBUM);
    s_mode = MODE_HOME;
    scroll_to_index(focus, false);
    board_unlock();
    heap_caps_free(subs);
}

void ui_show_tracks(int list_token, const char *title, const sp_entry_t *t, int n, bool can_go_back, int focus)
{
    if (n > KP_MAX_TRACKS) n = KP_MAX_TRACKS;
    const char **subs = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!subs) return;
    for (int i = 0; i < n; i++) subs[i] = t[i].artist;
    board_lock(-1);
    clear_screen();
    build_cards(t, n, subs, can_go_back, 0);
    s_mode = MODE_TRACKS;
    s_list_token = list_token;
    s_followed_id[0] = 0;
    update_badges();
    int f = focus;
    if (f < 0) {  // start at the song that is playing, if it is in this list
        f = 0;
        for (int i = 0; i < n; i++) if (s_np_id[0] && strcmp(t[i].id, s_np_id) == 0) f = i;
    }
    scroll_to_index(f, false);
    board_unlock();
    heap_caps_free(subs);
}

void ui_show_results(const sp_entry_t *e, int n)
{
    if (n > KP_MAX_RESULTS) n = KP_MAX_RESULTS;
    char *buf = heap_caps_calloc(n ? n : 1, 96, MALLOC_CAP_SPIRAM);
    const char **subs = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!buf || !subs) {
        heap_caps_free(buf);
        heap_caps_free(subs);
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *what = e[i].kind == SP_KIND_ALBUM ? "Album" : e[i].kind == SP_KIND_PLAYLIST ? "Playlist" : "Song";
        snprintf(buf + i * 96, 96, e[i].artist[0] ? "%s · %s" : "%s", what, e[i].artist);
        subs[i] = buf + i * 96;
    }
    board_lock(-1);
    clear_screen();
    build_cards(e, n, subs, true, CHIP_ALBUM | CHIP_PLAYLIST);
    s_mode = MODE_RESULTS;
    update_badges();
    scroll_to_index(0, false);
    board_unlock();
    heap_caps_free(subs);
    heap_caps_free(buf);
}

// ---------------------------------------------------------------- search keyboard

// Danish letters on the main layouts; the special-character layout is LVGL's default.
static const char *const kb_lower[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "å", LV_SYMBOL_BACKSPACE, "\n",
                                       "a", "s", "d", "f", "g", "h", "j", "k", "l", "æ", "ø", LV_SYMBOL_OK, "\n",
                                       "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "-", "\n",
                                       LV_SYMBOL_CLOSE, "1#", " ", "'", "&", ""};
static const char *const kb_upper[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Å", LV_SYMBOL_BACKSPACE, "\n",
                                       "A", "S", "D", "F", "G", "H", "J", "K", "L", "Æ", "Ø", LV_SYMBOL_OK, "\n",
                                       "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "-", "\n",
                                       LV_SYMBOL_CLOSE, "1#", " ", "'", "&", ""};
#define KB_KEY 4
#define KB_FN (LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6)
static const lv_buttonmatrix_ctrl_t kb_ctrl[] = {
    KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_FN,
    KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_FN,
    KB_FN, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY,
    KB_FN, KB_FN, 15, KB_KEY, KB_KEY,  // widths are 4 bits (max 15)
};


static void search_ready_cb(lv_event_t *e)
{
    const char *txt = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";
    while (*txt == ' ') txt++;
    if (*txt) app_post_search(txt);
}

static void search_cancel_cb(lv_event_t *e) { app_post(CMD_BACK, 0, 0); }

void ui_show_search(const char *initial_text)
{
    board_lock(-1);
    clear_screen();
    s_mode = MODE_SEARCH;
    lv_obj_t *ta = lv_textarea_create(s_root);
    s_search_ta = ta;
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 60);
    lv_textarea_set_placeholder_text(ta, "Search Spotify");
    lv_textarea_set_text(ta, initial_text ? initial_text : "");
    lv_obj_set_style_text_font(ta, F_BODY, 0);
    lv_obj_set_style_bg_color(ta, COL_CARD, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);  // shows the cursor
    lv_obj_t *kb = lv_keyboard_create(s_root);
#if IS_CUBE
    lv_obj_set_size(ta, 400, 52);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_size(kb, KP_SCREEN_W, 300);
#else
    lv_obj_set_size(ta, KP_SCREEN_W - 16, 38);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_pad_ver(ta, 4, 0);
    lv_obj_set_size(kb, KP_SCREEN_W, KP_SCREEN_H - 44);
#endif
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, F_BODY, 0);
    lv_obj_set_style_bg_color(kb, COL_BG, 0);
    lv_obj_set_style_bg_color(kb, COL_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x3A3A3A), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, COL_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 4, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lower, kb_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_upper, kb_ctrl);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, search_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, search_cancel_cb, LV_EVENT_CANCEL, NULL);
    board_unlock();
}

void ui_mark_playing_request(int track_index)
{
    // Called from the LVGL task (tap handler) as well as the app: no locking here.
    if (s_mode != MODE_TRACKS || track_index < 0 || track_index >= s_nslots) return;
    kp_strlcpy(s_np_id, s_slots[track_index].id, sizeof(s_np_id));
    kp_strlcpy(s_followed_id, s_np_id, sizeof(s_followed_id));
    s_np_playing = true;
    update_badges();
}

void ui_set_now_playing(const char *track_id, bool playing)
{
    board_lock(-1);
    bool changed = strcmp(track_id ? track_id : "", s_np_id) != 0 || playing != s_np_playing;
    kp_strlcpy(s_np_id, track_id ? track_id : "", sizeof(s_np_id));
    s_np_playing = playing;
    if (changed) update_badges();
    board_unlock();
}

void ui_show_volume(int vol)
{
    board_lock(-1);
    lv_bar_set_value(s_vol_bar, vol, LV_ANIM_OFF);
    lv_obj_set_hidden(s_vol_box, false);
    lv_timer_reset(s_vol_timer);
    lv_timer_resume(s_vol_timer);
    board_unlock();
}

void ui_toast(const char *text)
{
    board_lock(-1);
    lv_label_set_text(s_toast, text);
    lv_obj_set_hidden(s_toast, false);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
    board_unlock();
}

void ui_set_battery(int pct, bool charging)
{
    board_lock(-1);
    if (pct < 0) {
        lv_obj_set_hidden(s_battery, true);
    } else {
        const char *sym = pct > 85 ? LV_SYMBOL_BATTERY_FULL : pct > 60 ? LV_SYMBOL_BATTERY_3
                        : pct > 35 ? LV_SYMBOL_BATTERY_2 : pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_battery, "%s%s", charging ? LV_SYMBOL_CHARGE " " : "", sym);
        lv_obj_set_style_text_color(s_battery, pct <= 12 && !charging ? lv_color_hex(0xFF5252) : COL_MUTED, 0);
        lv_obj_set_hidden(s_battery, false);
    }
    board_unlock();
}

void ui_wake(void)
{
    board_lock(-1);
    lv_display_trigger_activity(NULL);
    s_desired_brightness = 100;
    board_unlock();
}
