/* SPDX-License-Identifier: Apache-2.0 */
/* apps/paint.c: prosty Paint. Rysujesz palcem po planszy 14 x 16 "pikseli" (po 18 px); pod plansza paleta: gumka i 7 kolorow.
 * "1x1 / 2x2" zmienia pedzel, "Czysc" czysci plansze po drugim dotknieciu ("Tak?"). Rysunek zapisuje /user/paint.sav (116 bajtow, 4 bity na piksel; 3 s po ostatniej zmianie i przy zamknieciu).
 * Gest wstecz tuz po rysowaniu (przeciagniecie w prawo po planszy) nie zamyka aplikacji; zamyka ja przesuniecie w prawo poza plansza (paleta, dol ekranu). */
#include "ui.h"

#ifndef RGB
#define RGB(r, g, b) ((u16)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#endif
typedef struct { i32 x, y; } Pt;
#define lv_indev_get_point(i, p)    F(0x2c279310, void, (lv_indev_t *, Pt *))(i, p)             /* lv_point_t: two 32-bit coordinates (docs/details.md) */
#define set_border_color(o, c)      F(0x2c27d584, void, (lv_obj_t *, u16, u32))(o, c, 0)
#define EV_PRESSING 2
#define EV_PRESS_LOST 3
#define EV_RELEASED 8

#define PT_PATH "/user/paint.sav"
#define PT_MAGIC 0x31544e50u                                    /* "PNT1" */
#define PT_COLS 14
#define PT_ROWS 16
#define PT_CELL 18
#define PT_N (PT_COLS * PT_ROWS)
#define PT_X0 2
#define PT_Y0 4
#define PT_NCOL 8                                               /* 0 = eraser, 1..7 colours */

typedef struct { u32 magic; u8 d[PT_N / 2]; } PtFile;
typedef struct {
    u8 col[PT_N], cur, brush, dirty, armed;
    i32 lx, ly; u32 dirty_t, armed_t, paint_t;
    lv_obj_t *canvas, *cell[PT_N], *sw[PT_NCOL], *b_brush, *l_brush, *b_clear, *l_clear;
    lv_timer_t *timer;
    char t_brush[TXT_MAX], t_clear[TXT_MAX];
} Paint;
static const u16 PT_COL[PT_NCOL] = { C_CARD, RGB(255, 255, 255), RGB(240, 60, 60), RGB(250, 150, 40), RGB(250, 230, 70), RGB(70, 200, 90), RGB(70, 160, 250), RGB(170, 100, 230) };

static void pt_save(Paint *s) {
    PtFile f; fill8(&f, 0, sizeof f); f.magic = PT_MAGIC;
    for (int i = 0; i < PT_N; i++) f.d[i / 2] = (u8)(f.d[i / 2] | (s->col[i] << ((i & 1) * 4)));
    blob_save(PT_PATH, &f, sizeof f);
}
static void pt_flush(Paint *s) { if (s->dirty) { s->dirty = 0; pt_save(s); } }
static void pt_changed(Paint *s) { s->dirty = 1; s->dirty_t = lv_tick_get(); }
static void pt_put(Paint *s, int x, int y, int c) {
    if (x < 0 || y < 0 || x >= PT_COLS || y >= PT_ROWS) return;
    int i = y * PT_COLS + x;
    if (s->col[i] == c) return;
    s->col[i] = (u8)c;
    if (!c) { if (s->cell[i]) { lv_obj_del(s->cell[i]); s->cell[i] = 0; } return; }
    if (!s->cell[i]) {
        lv_obj_t *o = lv_obj_create(s->canvas); if (!o) return;
        lv_obj_set_size(o, PT_CELL, PT_CELL); lv_obj_set_pos(o, x * PT_CELL, y * PT_CELL);
        lv_obj_clear_flag(o, FL_SCROLLABLE); lv_obj_clear_flag(o, FL_CLICKABLE);
        set_bg_opa(o, 255); set_border_width(o, 0); set_radius(o, 4);
        s->cell[i] = o;
    }
    set_bg_color(s->cell[i], PT_COL[c]);
}
static void pt_dab(Paint *s, int x, int y) {
    for (int dy = 0; dy < s->brush; dy++) for (int dx = 0; dx < s->brush; dx++) pt_put(s, x + dx, y + dy, s->cur);
}
static void pt_stroke(Paint *s, int x, int y) {
    if (s->lx < 0) pt_dab(s, x, y);
    else {                                                       /* fill the gap to the last touch: a fast finger skips cells */
        int dx = x - s->lx, dy = y - s->ly, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, n = ax > ay ? ax : ay;
        if (!n) pt_dab(s, x, y);
        for (int t = 1; t <= n; t++) pt_dab(s, s->lx + dx * t / n, s->ly + dy * t / n);
    }
    s->lx = x; s->ly = y; pt_changed(s);
}
static void pt_render(Paint *s) {
    for (int k = 0; k < PT_NCOL; k++) set_border_width(s->sw[k], k == s->cur ? 3 : 0);
    set_txt(s->l_brush, s->t_brush, s->brush == 2 ? "2x2" : "1x1");
    set_txt(s->l_clear, s->t_clear, s->armed ? "Sure?" : "Clear");
    set_bg_color(s->b_clear, s->armed ? C_WARN : C_BTN);
}
static void pt_canvas(lv_event_t *e) {
    Paint *s = (Paint *)lv_event_get_user_data(e); u32 code = lv_event_get_code(e);
    if (code == EV_PRESSED || code == EV_PRESSING) {
        Pt p; p.x = -1; p.y = -1; lv_indev_get_point(lv_indev_get_act(), &p);
        s->paint_t = lv_tick_get();
        if (code == EV_PRESSED) s->lx = -1;
        if (p.x < PT_X0 || p.y < PT_Y0) return;
        int x = (p.x - PT_X0) / PT_CELL, y = (p.y - PT_Y0) / PT_CELL;
        if (x >= PT_COLS || y >= PT_ROWS) return;
        pt_stroke(s, x, y);
    } else if (code == EV_RELEASED || code == EV_PRESS_LOST) s->lx = -1;
}
static void pt_click(lv_event_t *e) {
    Paint *s = (Paint *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED) return;
    if (t == s->b_brush) { s->brush = (u8)(3 - s->brush); motor_once(1, 1); }
    else if (t == s->b_clear) {
        if (s->armed) { s->armed = 0; lv_obj_clean(s->canvas); fill8(s->cell, 0, sizeof s->cell); fill8(s->col, 0, PT_N); pt_changed(s); motor_once(4, 1); }
        else { s->armed = 1; s->armed_t = lv_tick_get(); motor_once(1, 1); }
    } else for (int k = 0; k < PT_NCOL; k++) if (t == s->sw[k]) { s->cur = (u8)k; motor_once(1, 1); break; }
    pt_render(s);
}
static void pt_tick(lv_timer_t *t) {
    Paint *s = (Paint *)timer_user_data(t);
    if (s->armed && lv_tick_get() - s->armed_t > 2500) { s->armed = 0; pt_render(s); }
    if (s->dirty && lv_tick_get() - s->dirty_t > 3000) pt_flush(s);
}
static void *pt_create(lv_obj_t *root) {
    Paint *s = (Paint *)app_malloc(sizeof(Paint)); if (!s) return 0;
    fill8(s, 0, sizeof(Paint)); s->cur = 1; s->brush = 1; s->lx = -1; s->paint_t = lv_tick_get() - 1000u;
    s->canvas = mk_box(root, PT_X0, PT_Y0, PT_COLS * PT_CELL, PT_ROWS * PT_CELL, 10, C_CARD);
    lv_obj_add_event_cb(s->canvas, pt_canvas, 0, s);
    for (int k = 0; k < PT_NCOL; k++) {
        s->sw[k] = mk_box(root, 4 + k * 31, 300, 28, 28, 14, PT_COL[k]);
        set_border_color(s->sw[k], C_WHITE);
        lv_obj_add_event_cb(s->sw[k], pt_click, 0, s);
    }
    mk_text(s->sw[0], 0, 2, 28, FT_SMALL, C_MUTED, ALIGN_CENTER, "X");                         /* the eraser */
    s->b_brush = mk_pill(root, 6, 344, 118, "", &s->l_brush, pt_click, s);
    s->b_clear = mk_pill(root, 132, 344, 118, "Czysc", &s->l_clear, pt_click, s);
    PtFile f;
    if (blob_load(PT_PATH, &f, sizeof f) && f.magic == PT_MAGIC) {
        int ok = 1;
        for (int i = 0; i < PT_N; i++) if (((f.d[i / 2] >> ((i & 1) * 4)) & 15) >= PT_NCOL) ok = 0;
        if (ok) for (int i = 0; i < PT_N; i++) pt_put(s, i % PT_COLS, i / PT_COLS, (f.d[i / 2] >> ((i & 1) * 4)) & 15);
    }
    s->timer = lv_timer_create(pt_tick, 500, s);
    pt_render(s);
    return s;
}
static int pt_back(void *st) { Paint *s = (Paint *)st; return lv_tick_get() - s->paint_t < 500; }  /* a swipe that ends a stroke on the canvas is not "back" */
static void pt_destroy(void *st) { Paint *s = (Paint *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; pt_flush(s); app_free(s); }
APP_EXPORT(paint, "Paint", 0, pt_create, pt_back, pt_destroy)
