/* SPDX-License-Identifier: Apache-2.0 */
/* apps/tetris.c: Tetris for the 256 x 402 touch screen: a 10 x 20 board of 15 px cells with a side panel (score, lines, level, best, next piece).
 *
 * Under the board: "<" and ">" move the piece (hold to repeat), "Rot" turns it clockwise (with small wall kicks), "Drop" is a hard drop. Hold the board itself for a soft drop.
 * Standard rules: seven pieces from a shuffled bag, a ghost piece shows where the piece will land, 1 / 2 / 3 / 4 lines score 100 / 300 / 500 / 800 times the level, a soft drop
 * scores 1 per row and a hard drop 2 per row; a level every 10 lines, and the fall gets faster with it. The game ends when a new piece does not fit.
 * The best score is kept in /user/tetris.sav, written when a game ends and when the app is closed (a few bytes, rarely).
 *
 * Drawing: a box is created for a board cell the first time that cell is filled and then only shown, hidden and recoloured, so a game that stays low on the board uses few objects;
 * the falling piece and its ghost are 4 boxes each, the next piece 4 more. Holding "<", ">" or the board relies on the press events the finger keeps sending (each one is good for
 * 150 ms), so a missed release can never leave a piece sliding. No globals, no libc: everything lives in the block returned by create. All names are prefixed TT_ / tt_ because every
 * app shares one translation unit when bundled. */
#include "ui.h"
#include "icons.h"

#define TT_PATH "/user/tetris.sav"
#define TT_MAGIC 0x54455431u                                    /* "TET1" */
#define TT_COLS 10
#define TT_ROWS 20
#define TT_N (TT_COLS * TT_ROWS)
#define TT_CELL 15                                              /* pitch; the boxes are 14 px */
#define TT_BX 6                                                 /* board 150 x 300 at (6, 40); the panel is at x = 164; the pills start at y = 346 */
#define TT_BY 40
#define TT_TICK 25                                              /* ms per timer call */
#define TT_DAS 170                                              /* ms before a held < or > starts repeating, and the repeat period */
#define TT_REP 50
#define TT_SOFT 50                                              /* ms per row while the board is held */
#define TT_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define TT_BG TT_RGB(18, 20, 30)
#define TT_EV_PRESSING 2                                        /* LVGL events besides ui.h's: PRESSING (repeats while a finger is down), PRESS_LOST, RELEASED */
#define TT_EV_PRESS_LOST 3
#define TT_EV_RELEASED 8
enum { TT_READY, TT_RUN, TT_OVER };

/* the seven pieces I O T S Z J L in four rotations: a 4 x 4 mask, bit r * 4 + c, row 0 on top */
static const u16 TT_SHAPE[7][4] = {
    { 0x00F0, 0x4444, 0x0F00, 0x2222 },
    { 0x0660, 0x0660, 0x0660, 0x0660 },
    { 0x0072, 0x0262, 0x0270, 0x0232 },
    { 0x0036, 0x0462, 0x0360, 0x0231 },
    { 0x0063, 0x0264, 0x0630, 0x0132 },
    { 0x0071, 0x0226, 0x0470, 0x0322 },
    { 0x0074, 0x0622, 0x0170, 0x0223 },
};

typedef struct { u32 magic, best; } TtFile;
typedef struct {
    u32 rng, last_fall, over_t, held_until, hold_next, soft_until, best, score, lines;
    i32 px, py, lpx, lpy, lgy, lrot, lpiece, lnext;             /* the falling piece (top-left of its 4 x 4 mask, may be above the board) and what is drawn now */
    u8 state, level, piece, rot, next, hold, soft, dirty, bag[7], bagn, pad[2];     /* hold: the held direction, 0 none, 1 left, 2 right */
    u8 grid[TT_N], drawn[TT_N];                                 /* 0 empty, 1..7 a piece colour */
    lv_obj_t *board, *cell[TT_N], *act[4], *gho[4], *nxt[4], *ovl, *l_msg, *l_sub, *l_sub2, *l_hint;
    lv_obj_t *l_score, *l_lines, *l_level, *l_best, *b_left, *b_right, *b_rot, *b_drop;
    lv_timer_t *timer;
    char t_score[TXT_MAX], t_lines[TXT_MAX], t_level[TXT_MAX], t_best[TXT_MAX], t_msg[TXT_MAX], t_sub[TXT_MAX], t_sub2[TXT_MAX], t_hint[TXT_MAX];
} Tt;

static u16 tt_color(int id) {                                   /* 1..7 */
    switch (id) {
    case 1: return TT_RGB(0, 200, 230);
    case 2: return TT_RGB(240, 220, 40);
    case 3: return TT_RGB(170, 80, 220);
    case 4: return TT_RGB(80, 200, 80);
    case 5: return TT_RGB(230, 70, 70);
    case 6: return TT_RGB(70, 100, 230);
    default: return TT_RGB(240, 150, 50);
    }
}
static u32 tt_rand(Tt *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static i32 tt_interval(const Tt *s) { i32 v = 800 - 60 * ((i32)s->level - 1); return v < 100 ? 100 : v; }

static void tt_flush(Tt *s) {
    TtFile f;
    if (!s->dirty) return;
    s->dirty = 0; f.magic = TT_MAGIC; f.best = s->best; blob_save(TT_PATH, &f, sizeof f);
}
static void tt_render(Tt *s) {
    char b[TXT_MAX];
    putn(b, s->score, 1); set_txt(s->l_score, s->t_score, b);
    putn(b, s->lines, 1); set_txt(s->l_lines, s->t_lines, b);
    putn(b, s->level, 1); set_txt(s->l_level, s->t_level, b);
    putn(b, s->best, 1); set_txt(s->l_best, s->t_best, b);
}
static void tt_overlay(Tt *s, const char *msg, const char *sub, const char *sub2, const char *hint) {
    set_txt(s->l_msg, s->t_msg, msg); set_txt(s->l_sub, s->t_sub, sub); set_txt(s->l_sub2, s->t_sub2, sub2); set_txt(s->l_hint, s->t_hint, hint);
    lv_obj_clear_flag(s->ovl, FL_HIDDEN);
}
/* does the piece fit at (x, y)? cells above the board are free, the walls and the floor are not */
static int tt_fits(const Tt *s, int piece, int rot, int x, int y) {
    u16 m = TT_SHAPE[piece][rot];
    for (int i = 0; i < 16; i++) {
        int cx, cy;
        if (!(m & (1u << i))) continue;
        cx = x + i % 4; cy = y + i / 4;
        if (cx < 0 || cx >= TT_COLS || cy >= TT_ROWS) return 0;
        if (cy >= 0 && s->grid[cy * TT_COLS + cx]) return 0;
    }
    return 1;
}
static int tt_drop_dist(const Tt *s) { int d = 0; while (tt_fits(s, s->piece, s->rot, s->px, s->py + d + 1)) d++; return d; }
/* seven pieces in random order, then again */
static int tt_bag_next(Tt *s) {
    if (!s->bagn) {
        for (int i = 0; i < 7; i++) s->bag[i] = (u8)i;
        for (int i = 6; i > 0; i--) { u32 j = tt_rand(s) % (u32)(i + 1); u8 t = s->bag[i]; s->bag[i] = s->bag[j]; s->bag[j] = t; }
        s->bagn = 7;
    }
    return s->bag[--s->bagn];
}
/* the box of a board cell, made the first time it is needed */
static void tt_sync_grid(Tt *s) {
    for (int c = 0; c < TT_N; c++) {
        u8 v = s->grid[c];
        if (v == s->drawn[c]) continue;
        s->drawn[c] = v;
        if (!v) { if (s->cell[c]) lv_obj_add_flag(s->cell[c], FL_HIDDEN); continue; }
        if (!s->cell[c]) { s->cell[c] = mk_box(s->board, (c % TT_COLS) * TT_CELL, (c / TT_COLS) * TT_CELL, TT_CELL - 1, TT_CELL - 1, 3, tt_color(v)); lv_obj_clear_flag(s->cell[c], FL_CLICKABLE); }
        set_bg_color(s->cell[c], tt_color(v)); lv_obj_clear_flag(s->cell[c], FL_HIDDEN);
    }
}
/* the falling piece, its ghost and the preview: only what changed */
static void tt_draw(Tt *s) {
    int gy = s->state == TT_RUN ? s->py + tt_drop_dist(s) : 0;
    if (s->state != TT_RUN) {
        for (int k = 0; k < 4; k++) { lv_obj_add_flag(s->act[k], FL_HIDDEN); lv_obj_add_flag(s->gho[k], FL_HIDDEN); }
        s->lpiece = -1;
    } else if (s->px != s->lpx || s->py != s->lpy || s->rot != s->lrot || s->piece != s->lpiece || gy != s->lgy) {
        u16 m = TT_SHAPE[s->piece][s->rot]; int k = 0;
        for (int i = 0; i < 16 && k < 4; i++) {
            if (!(m & (1u << i))) continue;
            lv_obj_set_pos(s->act[k], (s->px + i % 4) * TT_CELL, (s->py + i / 4) * TT_CELL);
            lv_obj_set_pos(s->gho[k], (s->px + i % 4) * TT_CELL, (gy + i / 4) * TT_CELL);
            if (s->piece != s->lpiece) set_bg_color(s->act[k], tt_color(s->piece + 1));
            lv_obj_clear_flag(s->act[k], FL_HIDDEN); lv_obj_clear_flag(s->gho[k], FL_HIDDEN);
            k++;
        }
        s->lpx = s->px; s->lpy = s->py; s->lrot = s->rot; s->lpiece = s->piece; s->lgy = gy;
    }
    if (s->next != s->lnext && s->state != TT_READY) {
        u16 m = TT_SHAPE[s->next][0]; int k = 0;
        for (int i = 0; i < 16 && k < 4; i++) {
            if (!(m & (1u << i))) continue;
            lv_obj_set_pos(s->nxt[k], 172 + (i % 4) * 13, 290 + (i / 4) * 13); set_bg_color(s->nxt[k], tt_color(s->next + 1)); lv_obj_clear_flag(s->nxt[k], FL_HIDDEN);
            k++;
        }
        s->lnext = s->next;
    }
}
static void tt_over(Tt *s) {
    char b[TXT_MAX], c[TXT_MAX], *e;
    s->state = TT_OVER; s->over_t = lv_tick_get(); s->hold = 0; s->soft = 0; motor_once(4, 1);
    if (s->score > s->best) { s->best = s->score; s->dirty = 1; }
    tt_flush(s);
    e = put(b, "Score "); putn(e, s->score, 1); e = put(c, "Best "); putn(e, s->best, 1);
    tt_overlay(s, "Game over", b, c, "Tap to retry");
    tt_render(s); tt_draw(s);
}
static void tt_spawn(Tt *s) {
    s->piece = s->next; s->next = (u8)tt_bag_next(s); s->rot = 0; s->px = 3; s->py = (s->piece <= 1) ? -1 : 0;
    if (!tt_fits(s, s->piece, s->rot, s->px, s->py)) tt_over(s);
}
/* the piece stays where it is: write it into the board, clear full rows, score, next piece */
static void tt_lock(Tt *s) {
    u16 m = TT_SHAPE[s->piece][s->rot]; int top_out = 0, cleared = 0, w = TT_ROWS - 1;
    static const u16 pts[5] = { 0, 100, 300, 500, 800 };
    for (int i = 0; i < 16; i++) {
        int cy;
        if (!(m & (1u << i))) continue;
        cy = s->py + i / 4;
        if (cy < 0) top_out = 1; else s->grid[cy * TT_COLS + s->px + i % 4] = (u8)(s->piece + 1);
    }
    for (int r = TT_ROWS - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < TT_COLS; c++) if (!s->grid[r * TT_COLS + c]) full = 0;
        if (full) { cleared++; continue; }
        if (w != r) for (int c = 0; c < TT_COLS; c++) s->grid[w * TT_COLS + c] = s->grid[r * TT_COLS + c];
        w--;
    }
    for (; w >= 0; w--) for (int c = 0; c < TT_COLS; c++) s->grid[w * TT_COLS + c] = 0;
    if (cleared) {
        s->score += (u32)pts[cleared] * s->level; s->lines += (u32)cleared;
        s->level = (u8)(s->lines / 10 + 1 > 20 ? 20 : s->lines / 10 + 1);
        motor_once(1, 1);
    }
    if (s->score > s->best) { s->best = s->score; s->dirty = 1; }
    tt_sync_grid(s); tt_render(s);
    if (top_out) { tt_over(s); return; }
    tt_spawn(s);
    s->last_fall = lv_tick_get();
}
static void tt_move(Tt *s, int dx) { if (tt_fits(s, s->piece, s->rot, s->px + dx, s->py)) s->px += dx; }
static void tt_rotate(Tt *s) {
    int nr = (s->rot + 1) & 3;
    static const signed char kx[5] = { 0, -1, 1, -2, 2 };
    for (int dy = 0; dy >= -1; dy--) for (int k = 0; k < 5; k++)
        if (tt_fits(s, s->piece, nr, s->px + kx[k], s->py + dy)) { s->px += kx[k]; s->py += dy; s->rot = (u8)nr; return; }
}
static void tt_hard_drop(Tt *s) { int d = tt_drop_dist(s); s->py += d; s->score += 2u * (u32)d; tt_lock(s); }
static void tt_new(Tt *s) {
    fill8(s->grid, 0, TT_N); s->score = 0; s->lines = 0; s->level = 1; s->bagn = 0; s->hold = 0; s->soft = 0;
    s->next = (u8)tt_bag_next(s); s->lnext = -1; s->lpiece = -1;
    s->state = TT_RUN; lv_obj_add_flag(s->ovl, FL_HIDDEN);
    tt_spawn(s);
    s->last_fall = lv_tick_get();
    tt_sync_grid(s); tt_render(s); tt_draw(s);
}
static void tt_event(lv_event_t *e) {
    Tt *s = (Tt *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e); u32 code = lv_event_get_code(e), now = lv_tick_get();
    if (t == s->board) {
        if (code == EV_PRESSED) {
            if (s->state == TT_READY || (s->state == TT_OVER && now - s->over_t > 700u)) { tt_new(s); return; }    /* a short pause after the end, so a late touch does not restart at once */
            if (s->state == TT_RUN) { s->soft = 1; s->soft_until = now + 150u; }
        } else if (code == TT_EV_PRESSING) s->soft_until = now + 150u;
        else if (code == TT_EV_RELEASED || code == TT_EV_PRESS_LOST || code == EV_CLICKED) s->soft = 0;
        return;
    }
    if (s->state != TT_RUN) return;
    if (t == s->b_left || t == s->b_right) {
        u8 dir = t == s->b_left ? 1 : 2;
        if (code == EV_PRESSED) { tt_move(s, dir == 1 ? -1 : 1); s->hold = dir; s->held_until = now + 150u; s->hold_next = now + TT_DAS; }
        else if (code == TT_EV_PRESSING) s->held_until = now + 150u;
        else if ((code == TT_EV_RELEASED || code == TT_EV_PRESS_LOST || code == EV_CLICKED) && s->hold == dir) s->hold = 0;
    } else if (t == s->b_rot) { if (code == EV_PRESSED) tt_rotate(s); }
    else if (t == s->b_drop) { if (code == EV_PRESSED) tt_hard_drop(s); }
    tt_draw(s);
}
static void tt_tick(lv_timer_t *t) {
    Tt *s = (Tt *)timer_user_data(t); u32 now = lv_tick_get();
    i32 iv;
    if (s->state != TT_RUN) return;
    if (s->hold && now > s->held_until) s->hold = 0;
    if (s->soft && now > s->soft_until) s->soft = 0;
    if (s->hold) while (now >= s->hold_next) { tt_move(s, s->hold == 1 ? -1 : 1); s->hold_next += TT_REP; }
    iv = tt_interval(s); if (s->soft && iv > TT_SOFT) iv = TT_SOFT;
    for (int k = 0; k < 3 && s->state == TT_RUN && now - s->last_fall >= (u32)iv; k++) {          /* gravity, catching up at most 3 rows after a stall */
        s->last_fall += (u32)iv; if (now - s->last_fall > 3u * (u32)iv) s->last_fall = now;
        if (tt_fits(s, s->piece, s->rot, s->px, s->py + 1)) { s->py++; if (s->soft) s->score++; }
        else tt_lock(s);
    }
    if (s->state == TT_RUN) { if (s->score > s->best) { s->best = s->score; s->dirty = 1; } tt_render(s); }
    tt_draw(s);
}
static lv_obj_t *tt_cap(lv_obj_t *root, i32 y, const char *txt) { return mk_text(root, 164, y, 92, ROBOTO_MEDIUM(10), C_MUTED, ALIGN_LEFT, txt); }
static void *tt_create(lv_obj_t *root) {
    Tt *s = (Tt *)app_malloc(sizeof(Tt)); if (!s) return 0;
    fill8(s, 0, sizeof(Tt)); s->rng = lv_tick_get() | 1u; s->lpiece = -1; s->lnext = -1;
    TtFile f; if (blob_load(TT_PATH, &f, sizeof f) && f.magic == TT_MAGIC && f.best < 100000000u) s->best = f.best;
    s->board = mk_box(root, TT_BX, TT_BY, TT_COLS * TT_CELL, TT_ROWS * TT_CELL, 6, TT_BG); lv_obj_add_event_cb(s->board, tt_event, 0, s);
    for (int k = 0; k < 4; k++) {
        s->gho[k] = mk_box(s->board, 0, 0, TT_CELL - 1, TT_CELL - 1, 3, TT_RGB(50, 54, 70)); lv_obj_clear_flag(s->gho[k], FL_CLICKABLE); lv_obj_add_flag(s->gho[k], FL_HIDDEN);
        s->act[k] = mk_box(s->board, 0, 0, TT_CELL - 1, TT_CELL - 1, 3, C_WHITE); lv_obj_clear_flag(s->act[k], FL_CLICKABLE); lv_obj_add_flag(s->act[k], FL_HIDDEN);
    }
    tt_cap(root, 40, "Score"); s->l_score = mk_text(root, 164, 60, 92, FT_SMALL, C_WHITE, ALIGN_LEFT, "0");
    tt_cap(root, 96, "Lines"); s->l_lines = mk_text(root, 164, 116, 92, FT_SMALL, C_WHITE, ALIGN_LEFT, "0");
    tt_cap(root, 152, "Level"); s->l_level = mk_text(root, 164, 172, 92, FT_SMALL, C_WHITE, ALIGN_LEFT, "1");
    tt_cap(root, 208, "Best"); s->l_best = mk_text(root, 164, 228, 92, FT_SMALL, C_WHITE, ALIGN_LEFT, "0");
    tt_cap(root, 264, "Next");
    for (int k = 0; k < 4; k++) { s->nxt[k] = mk_box(root, 172, 290, 12, 12, 2, C_WHITE); lv_obj_clear_flag(s->nxt[k], FL_CLICKABLE); lv_obj_add_flag(s->nxt[k], FL_HIDDEN); }
    s->ovl = mk_box(root, 12, 120, 232, 124, 24, C_BLACK); set_bg_opa(s->ovl, 215); lv_obj_clear_flag(s->ovl, FL_CLICKABLE);
    s->l_msg = mk_text(s->ovl, 0, 4, 232, FT_BTN, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_msg, FL_CLICKABLE);
    s->l_sub = mk_text(s->ovl, 0, 40, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub, FL_CLICKABLE);
    s->l_sub2 = mk_text(s->ovl, 0, 66, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub2, FL_CLICKABLE);
    s->l_hint = mk_text(s->ovl, 0, 92, 232, FT_SMALL, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_hint, FL_CLICKABLE);
    s->b_left = mk_pill(root, 4, 346, 50, "<", 0, tt_event, s);
    s->b_right = mk_pill(root, 58, 346, 50, ">", 0, tt_event, s);
    s->b_rot = mk_pill(root, 112, 346, 68, "Rot", 0, tt_event, s);
    s->b_drop = mk_pill(root, 184, 346, 68, "Drop", 0, tt_event, s);
    s->timer = lv_timer_create(tt_tick, TT_TICK, s);
    s->next = (u8)tt_bag_next(s);
    tt_render(s);
    tt_overlay(s, "Tetris", "Drop = hard drop", "Hold board = soft", "Tap to start");
    return s;
}
static int tt_back(void *st) { (void)st; return 0; }
static void tt_destroy(void *st) { Tt *s = (Tt *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; tt_flush(s); app_free(s); }
APP_EXPORT(tetris, "Tetris", &ICON_TETRIS, tt_create, tt_back, tt_destroy)
