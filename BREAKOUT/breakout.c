/* SPDX-License-Identifier: Apache-2.0 */
/* apps/breakout.c: Breakout for the 256 x 402 touch screen.
 *
 * Hold "Left" or "Right" under the board to slide the paddle; tap the board to serve the ball. 6 x 5 bricks (the top rows are worth more), three lives, and every cleared
 * wall is followed by a faster one. Where the ball meets the paddle decides the angle (nine fixed angles, so the speed stays constant and no float / libgcc is needed).
 * The best score is kept in /user/breakout.sav, written when a game ends and when the app is closed (a few bytes, rarely).
 *
 * Holding: the pills report "pressed" while a finger is down. The paddle moves while presses keep arriving (each one is good for 150 ms) and stops on release, so a missed
 * release can never leave it sliding. Physics: integer, 1/16 pixel, the ball moves in steps of at most 2 px so it cannot pass through a brick or the paddle.
 * No globals, no libc: everything lives in the block returned by create. All names are prefixed BK_ / bk_ because every app shares one translation unit when bundled. */
#include "ui.h"
#include "icons.h"

#define BK_PATH "/user/breakout.sav"
#define BK_MAGIC 0x42524b31u                                    /* "BRK1" */
#define BK_W 240                                                /* board in px: 8 + 240 + 8 = 256; 46 + 280 = 326, the pills start at 344 */
#define BK_H 280
#define BK_X 8
#define BK_Y 46
#define BK_BALL 10
#define BK_PADW 56
#define BK_PADH 10
#define BK_PADY 254                                             /* top of the paddle, in board pixels */
#define BK_PADSPEED 6                                           /* px per 30 ms tick while held */
#define BK_COLS 6
#define BK_ROWS 5
#define BK_N (BK_COLS * BK_ROWS)
#define BK_BW 38                                                /* brick size and pitch */
#define BK_BH 14
#define BK_PX 40
#define BK_PY 18
#define BK_BX0 1
#define BK_BY0 26
#define BK_EV_PRESSING 2                                        /* LVGL events besides ui.h's: PRESSING (repeats while a finger is down), PRESS_LOST, RELEASED */
#define BK_EV_PRESS_LOST 3
#define BK_EV_RELEASED 8
#define BK_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define BK_ABS(v) ((v) < 0 ? -(v) : (v))
enum { BK_SERVE, BK_RUN, BK_OVER };

/* nine launch angles (from -60 to +60 degrees): unit vectors in 1/64, vx and vy of equal length; the speed multiplies them */
static const signed char BK_VX[9] = { -55, -46, -34, -18, 0, 18, 34, 46, 55 };
static const signed char BK_VY[9] = { 33, 45, 54, 61, 64, 61, 54, 45, 33 };

typedef struct { u32 magic, best; } BkFile;
typedef struct {
    u32 rng, held_until, over_t, best, score;
    i32 bx, by, vx, vy, px, lbx, lby, lpx;                      /* ball (top-left) and its velocity in 1/16 px; paddle left in px; what is drawn now */
    signed char hold;                                           /* -1 left, 0 none, 1 right */
    u8 state, level, lives, nalive, dirty, pad[3];
    u8 alive[BK_N];
    lv_obj_t *board, *paddle, *ball, *ovl, *l_msg, *l_sub, *l_sub2, *l_hint, *l_score, *l_lives, *b_left, *b_right, *brick[BK_N];
    lv_timer_t *timer;
    char t_score[TXT_MAX], t_lives[TXT_MAX], t_msg[TXT_MAX], t_sub[TXT_MAX], t_sub2[TXT_MAX], t_hint[TXT_MAX];
} Brk;

static u16 bk_row_color(int r) {
    switch (r) {
    case 0: return BK_RGB(235, 70, 70);
    case 1: return BK_RGB(240, 150, 60);
    case 2: return BK_RGB(240, 210, 70);
    case 3: return BK_RGB(90, 200, 100);
    default: return BK_RGB(80, 150, 240);
    }
}
static u32 bk_rand(Brk *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static i32 bk_speed(const Brk *s) { i32 v = 56 + 6 * ((i32)s->level - 1); return v > 110 ? 110 : v; }      /* 1/16 px per tick: 3.5 px up to 6.9 px */
static void bk_aim(Brk *s, i32 idx) { i32 sp = bk_speed(s); s->vx = BK_VX[idx] * sp / 64; s->vy = -(BK_VY[idx] * sp / 64); }

static void bk_flush(Brk *s) {
    BkFile f;
    if (!s->dirty) return;
    s->dirty = 0; f.magic = BK_MAGIC; f.best = s->best; blob_save(BK_PATH, &f, sizeof f);
}
static void bk_render(Brk *s) {
    char b[TXT_MAX], *e = put(b, "Score "); putn(e, s->score, 1); set_txt(s->l_score, s->t_score, b);
    e = put(b, "Lives "); putn(e, s->lives, 1); set_txt(s->l_lives, s->t_lives, b);
}
static void bk_draw(Brk *s) {
    i32 x = s->bx >> 4, y = s->by >> 4;
    if (x != s->lbx || y != s->lby) { s->lbx = x; s->lby = y; lv_obj_set_pos(s->ball, x, y); }
    if (s->px != s->lpx) { s->lpx = s->px; lv_obj_set_pos(s->paddle, s->px, BK_PADY); }
}
static void bk_overlay(Brk *s, const char *msg, const char *sub, const char *sub2, const char *hint) {
    set_txt(s->l_msg, s->t_msg, msg); set_txt(s->l_sub, s->t_sub, sub); set_txt(s->l_sub2, s->t_sub2, sub2); set_txt(s->l_hint, s->t_hint, hint);
    lv_obj_clear_flag(s->ovl, FL_HIDDEN);
}
static void bk_reset_bricks(Brk *s) {
    for (int i = 0; i < BK_N; i++) { s->alive[i] = 1; lv_obj_clear_flag(s->brick[i], FL_HIDDEN); }
    s->nalive = BK_N;
}
/* the ball waits on the paddle until the board is tapped */
static void bk_serve(Brk *s, const char *msg, const char *sub, const char *sub2) {
    s->state = BK_SERVE; s->vx = 0; s->vy = 0;
    s->bx = (s->px + BK_PADW / 2 - BK_BALL / 2) << 4; s->by = (BK_PADY - BK_BALL) << 4;
    bk_draw(s); bk_overlay(s, msg, sub, sub2, "Tap to serve");
}
static void bk_new(Brk *s) {
    s->score = 0; s->level = 1; s->lives = 3; s->px = (BK_W - BK_PADW) / 2;
    bk_reset_bricks(s);
    bk_serve(s, "Breakout", "Hold Left / Right", "to move");
    bk_render(s);
}
static void bk_over(Brk *s) {
    char b[TXT_MAX], c[TXT_MAX], *e;
    s->state = BK_OVER; s->over_t = lv_tick_get(); motor_once(4, 1);
    bk_flush(s);
    e = put(b, "Score "); putn(e, s->score, 1); e = put(c, "Best "); putn(e, s->best, 1);
    bk_overlay(s, "Game over", b, c, "Tap to retry");
}
static void bk_launch(Brk *s) {
    bk_aim(s, (bk_rand(s) & 1u) ? 3 : 5);
    lv_obj_add_flag(s->ovl, FL_HIDDEN); s->state = BK_RUN;
}
/* one tick of ball physics, in sub-steps of at most 2 px */
static void bk_move(Brk *s) {
    i32 ax = BK_ABS(s->vx), ay = BK_ABS(s->vy), n = (ax > ay ? ax : ay) / 32 + 1;
    i32 sx = s->vx / n, sy = s->vy / n;
    for (i32 k = 0; k < n && s->state == BK_RUN; k++) {
        s->bx += sx; s->by += sy;
        i32 x = s->bx >> 4, y = s->by >> 4;
        if (x < 0) { x = 0; s->bx = 0; s->vx = BK_ABS(s->vx); sx = BK_ABS(sx); }
        else if (x + BK_BALL > BK_W) { x = BK_W - BK_BALL; s->bx = x << 4; s->vx = -BK_ABS(s->vx); sx = -BK_ABS(sx); }
        if (y < 0) { y = 0; s->by = 0; s->vy = BK_ABS(s->vy); sy = BK_ABS(sy); }
        /* the paddle: the hit position picks one of the nine angles */
        if (sy > 0 && y + BK_BALL >= BK_PADY && y + BK_BALL <= BK_PADY + BK_PADH && x + BK_BALL > s->px && x < s->px + BK_PADW) {
            i32 idx = ((x + BK_BALL / 2 - s->px) * 9) / BK_PADW;
            if (idx < 0) idx = 0;
            if (idx > 8) idx = 8;
            if (idx == 4) idx = (bk_rand(s) & 1u) ? 3 : 5;                  /* never straight up: it would repeat forever */
            bk_aim(s, idx); y = BK_PADY - BK_BALL; s->by = y << 4;
            sx = s->vx / n; sy = s->vy / n;
        }
        /* bricks: the smaller overlap says which side was hit */
        for (i32 i = 0; i < BK_N; i++) {
            if (!s->alive[i]) continue;
            i32 bx0 = BK_BX0 + (i % BK_COLS) * BK_PX, by0 = BK_BY0 + (i / BK_COLS) * BK_PY;
            if (x >= bx0 + BK_BW || x + BK_BALL <= bx0 || y >= by0 + BK_BH || y + BK_BALL <= by0) continue;
            i32 ox = (x + BK_BALL < bx0 + BK_BW ? x + BK_BALL : bx0 + BK_BW) - (x > bx0 ? x : bx0);
            i32 oy = (y + BK_BALL < by0 + BK_BH ? y + BK_BALL : by0 + BK_BH) - (y > by0 ? y : by0);
            if (ox < oy) s->vx = (x + BK_BALL / 2 < bx0 + BK_BW / 2) ? -BK_ABS(s->vx) : BK_ABS(s->vx);
            else s->vy = (y + BK_BALL / 2 < by0 + BK_BH / 2) ? -BK_ABS(s->vy) : BK_ABS(s->vy);
            sx = s->vx / n; sy = s->vy / n;
            s->alive[i] = 0; s->nalive--; lv_obj_add_flag(s->brick[i], FL_HIDDEN);
            s->score += (u32)(BK_ROWS - i / BK_COLS);
            if (s->score > s->best) { s->best = s->score; s->dirty = 1; }
            break;
        }
        if (!s->nalive) {                                                    /* wall cleared: a new, faster one */
            char b[TXT_MAX], *e = put(b, "Level "); putn(e, (u32)s->level + 1u, 1);
            if (s->level < 255) s->level++;
            bk_reset_bricks(s); motor_once(4, 1); bk_serve(s, b, "Faster!", ""); return;
        }
        if (y > BK_PADY + BK_PADH + 12) {                                    /* the ball is gone */
            char b[TXT_MAX], *e = put(b, "Lives "); s->lives--; putn(e, s->lives, 1);
            motor_once(4, 1);
            if (!s->lives) bk_over(s); else bk_serve(s, "Ball lost", b, "");
            return;
        }
    }
}
static void bk_event(lv_event_t *e) {
    Brk *s = (Brk *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e); u32 code = lv_event_get_code(e), now = lv_tick_get();
    if (t == s->board) {
        if (code != EV_CLICKED) return;
        if (s->state == BK_SERVE) bk_launch(s);
        else if (s->state == BK_OVER && now - s->over_t > 700) bk_new(s);   /* a short pause after the end, so a late tap does not restart at once */
        return;
    }
    signed char dir = t == s->b_right ? 1 : -1;
    if (code == EV_PRESSED || code == BK_EV_PRESSING) { s->hold = dir; s->held_until = now + 150; }
    else if ((code == BK_EV_RELEASED || code == BK_EV_PRESS_LOST || code == EV_CLICKED) && s->hold == dir) s->hold = 0;
}
static void bk_tick(lv_timer_t *t) {
    Brk *s = (Brk *)timer_user_data(t);
    if (s->hold && lv_tick_get() > s->held_until) s->hold = 0;
    if (s->state == BK_OVER) return;
    if (s->hold) { s->px += s->hold * BK_PADSPEED; if (s->px < 0) s->px = 0; if (s->px > BK_W - BK_PADW) s->px = BK_W - BK_PADW; }
    if (s->state == BK_SERVE) { s->bx = (s->px + BK_PADW / 2 - BK_BALL / 2) << 4; s->by = (BK_PADY - BK_BALL) << 4; }
    else bk_move(s);
    bk_draw(s); bk_render(s);
}
static void *bk_create(lv_obj_t *root) {
    Brk *s = (Brk *)app_malloc(sizeof(Brk)); if (!s) return 0;
    fill8(s, 0, sizeof(Brk)); s->rng = lv_tick_get() | 1u; s->lbx = -9999; s->lby = -9999; s->lpx = -9999;
    BkFile f; if (blob_load(BK_PATH, &f, sizeof f) && f.magic == BK_MAGIC && f.best < 1000000u) s->best = f.best;
    s->l_score = mk_text(root, 12, 8, 136, FT_SMALL, C_MUTED, ALIGN_LEFT, "");
    s->l_lives = mk_text(root, 150, 8, 100, FT_SMALL, C_MUTED, ALIGN_LEFT, "");
    s->board = mk_box(root, BK_X, BK_Y, BK_W, BK_H, 8, C_CARD); lv_obj_add_event_cb(s->board, bk_event, 0, s);
    for (int i = 0; i < BK_N; i++) {
        s->brick[i] = mk_box(s->board, BK_BX0 + (i % BK_COLS) * BK_PX, BK_BY0 + (i / BK_COLS) * BK_PY, BK_BW, BK_BH, 4, bk_row_color(i / BK_COLS));
        lv_obj_clear_flag(s->brick[i], FL_CLICKABLE);
    }
    s->paddle = mk_box(s->board, 0, BK_PADY, BK_PADW, BK_PADH, 5, C_WHITE); lv_obj_clear_flag(s->paddle, FL_CLICKABLE);
    s->ball = mk_box(s->board, 0, 0, BK_BALL, BK_BALL, 100, C_SERVE); lv_obj_clear_flag(s->ball, FL_CLICKABLE);
    s->ovl = mk_box(root, 12, BK_Y + 128, 232, 124, 24, C_BLACK); set_bg_opa(s->ovl, 215); lv_obj_clear_flag(s->ovl, FL_CLICKABLE);
    s->l_msg = mk_text(s->ovl, 0, 4, 232, FT_BTN, C_WHITE, ALIGN_CENTER, "");
    s->l_sub = mk_text(s->ovl, 0, 40, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    s->l_sub2 = mk_text(s->ovl, 0, 66, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    s->l_hint = mk_text(s->ovl, 0, 92, 232, FT_SMALL, C_WHITE, ALIGN_CENTER, "");
    s->b_left = mk_pill(root, 6, 344, 118, "Left", 0, bk_event, s);
    s->b_right = mk_pill(root, 132, 344, 118, "Right", 0, bk_event, s);
    s->timer = lv_timer_create(bk_tick, 30, s);
    bk_new(s);
    return s;
}
static int bk_back(void *st) { (void)st; return 0; }
static void bk_destroy(void *st) { Brk *s = (Brk *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; bk_flush(s); app_free(s); }
APP_EXPORT(breakout, "Breakout", &ICON_BREAKOUT, bk_create, bk_back, bk_destroy)
