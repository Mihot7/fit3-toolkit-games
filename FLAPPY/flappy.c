/* SPDX-License-Identifier: Apache-2.0 */
/* apps/flappy.c: a Flappy Bird-style game for the 256 x 402 touch screen.
 *
 * Touch the screen to flap; the bird falls on its own. Fly through the gaps between the pipes: one point per pipe. A pipe, the ground or ... nothing else ends the run
 * (the top of the screen only stops the bird). The pipes get faster at 15 and 30 points. The best score is kept in /user/flappy.sav, written when a run ends and when the
 * app is closed (a few bytes, rarely).
 *
 * The flap is taken on the touch-down event, not on release, so it answers at once. Physics: integer, 1/16 pixel, one step per 30 ms; if the LVGL task is late the game
 * catches up (at most 3 steps) so the speed does not depend on the load. Each pipe pair is one transparent container with four boxes, so a step moves 3 objects, not 12.
 * No globals, no libc: everything lives in the block returned by create. All names are prefixed FP_ / fp_ because every app shares one translation unit when bundled. */
#include "ui.h"
#include "icons.h"

#define FP_PATH "/user/flappy.sav"
#define FP_MAGIC 0x464c5031u                                    /* "FLP1" */
#define FP_W 240                                                /* board in px: 8 + 240 + 8 = 256; 46 + 346 = 392 (the whole screen below the header is the touch area) */
#define FP_H 346
#define FP_X 8
#define FP_Y 46
#define FP_GROUND 24
#define FP_PLAY (FP_H - FP_GROUND)                              /* height above the ground */
#define FP_BX 60                                                /* the bird's left edge: it never moves sideways */
#define FP_BW 22                                                /* body 22 x 16 inside an 26 x 18 box (the beak sticks out) */
#define FP_BH 16
#define FP_BOXH 18
#define FP_GAP 104                                              /* the opening between the pipes */
#define FP_PW 44                                                /* pipe width, with the caps */
#define FP_CAP 14
#define FP_SPACE 150                                            /* from one pipe to the next, in px */
#define FP_N 3                                                  /* pipe pairs: they are recycled */
#define FP_MARGIN 30                                            /* the opening never gets closer than this to the top or the ground */
#define FP_G 19                                                 /* gravity, 1/16 px per tick per tick */
#define FP_FLAP 184                                             /* the flap sets the speed to this (up): about 55 px of rise */
#define FP_VMAX 224                                             /* terminal fall speed */
#define FP_TICK 30                                              /* ms per step */
#define FP_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define FP_SKY FP_RGB(112, 197, 206)
#define FP_PIPE FP_RGB(115, 191, 46)
#define FP_PIPE_CAP FP_RGB(84, 150, 30)
#define FP_SAND FP_RGB(222, 216, 149)
#define FP_BIRD FP_RGB(250, 210, 40)
#define FP_WING FP_RGB(255, 236, 130)
#define FP_BEAK FP_RGB(240, 120, 40)
enum { FP_READY, FP_RUN, FP_OVER };

typedef struct { u32 magic, best; } FpFile;
typedef struct {
    u32 rng, last_t, over_t, best, score;
    i32 by, vy, lby, lwing;                                     /* bird top in 1/16 px and its speed; what is drawn now */
    i32 px[FP_N], gap[FP_N], lpx[FP_N];                         /* pipe left edge (px), top of the opening (px), what is drawn now */
    u8 state, dirty, scored[FP_N], pad;
    lv_obj_t *board, *bird, *wing, *ovl, *l_msg, *l_sub, *l_sub2, *l_hint, *l_best, *l_score;
    lv_obj_t *pipe[FP_N], *top[FP_N], *topcap[FP_N], *bot[FP_N], *botcap[FP_N];
    lv_timer_t *timer;
    char t_score[TXT_MAX], t_best[TXT_MAX], t_msg[TXT_MAX], t_sub[TXT_MAX], t_sub2[TXT_MAX], t_hint[TXT_MAX];
} Fp;

static u32 fp_rand(Fp *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static i32 fp_speed(const Fp *s) { return s->score >= 30 ? 5 : s->score >= 15 ? 4 : 3; }          /* px per step */
static lv_obj_t *fp_part(lv_obj_t *parent, i32 x, i32 y, i32 w, i32 h, i32 r, u16 c) { lv_obj_t *o = mk_box(parent, x, y, w, h, r, c); lv_obj_clear_flag(o, FL_CLICKABLE); return o; }

static void fp_flush(Fp *s) {
    FpFile f;
    if (!s->dirty) return;
    s->dirty = 0; f.magic = FP_MAGIC; f.best = s->best; blob_save(FP_PATH, &f, sizeof f);
}
static void fp_render(Fp *s) {
    char b[TXT_MAX], *e = putn(b, s->score, 1); (void)e; set_txt(s->l_score, s->t_score, b);
    e = put(b, "Best "); putn(e, s->best, 1); set_txt(s->l_best, s->t_best, b);
}
static void fp_overlay(Fp *s, const char *msg, const char *sub, const char *sub2, const char *hint) {
    set_txt(s->l_msg, s->t_msg, msg); set_txt(s->l_sub, s->t_sub, sub); set_txt(s->l_sub2, s->t_sub2, sub2); set_txt(s->l_hint, s->t_hint, hint);
    lv_obj_clear_flag(s->ovl, FL_HIDDEN);
}
static void fp_draw(Fp *s) {
    i32 y = s->by >> 4, wy = s->vy < 0 ? 4 : 9;                                                   /* the wing is up while the bird rises */
    if (y != s->lby) { s->lby = y; lv_obj_set_pos(s->bird, FP_BX, y); }
    if (wy != s->lwing) { s->lwing = wy; lv_obj_set_pos(s->wing, 3, wy); }
    for (int i = 0; i < FP_N; i++) if (s->px[i] != s->lpx[i]) { s->lpx[i] = s->px[i]; lv_obj_set_pos(s->pipe[i], s->px[i], 0); }
}
/* the four boxes of a pair for the opening that starts at gap[i] */
static void fp_geom(Fp *s, int i) {
    i32 g = s->gap[i], by = g + FP_GAP + FP_CAP;
    lv_obj_set_size(s->top[i], FP_PW - 6, g - FP_CAP); lv_obj_set_pos(s->top[i], 3, 0);
    lv_obj_set_pos(s->topcap[i], 0, g - FP_CAP);
    lv_obj_set_pos(s->botcap[i], 0, g + FP_GAP);
    lv_obj_set_size(s->bot[i], FP_PW - 6, FP_PLAY - by); lv_obj_set_pos(s->bot[i], 3, by);
}
static void fp_new_gap(Fp *s, int i) { s->gap[i] = FP_MARGIN + (i32)(fp_rand(s) % (u32)(FP_PLAY - FP_GAP - 2 * FP_MARGIN + 1)); fp_geom(s, i); }
static void fp_reset(Fp *s) {
    s->score = 0; s->state = FP_READY; s->vy = 0; s->by = 150 << 4; s->lby = -9999; s->lwing = -9999;
    for (int i = 0; i < FP_N; i++) { s->px[i] = 250 + i * FP_SPACE; s->scored[i] = 0; s->lpx[i] = -9999; fp_new_gap(s, i); }
    fp_draw(s); fp_render(s);
    fp_overlay(s, "Flappy", "Tap the screen", "to flap", "Tap to start");
}
static void fp_die(Fp *s) {
    char b[TXT_MAX], c[TXT_MAX], *e;
    s->state = FP_OVER; s->over_t = lv_tick_get(); motor_once(4, 1);
    fp_flush(s);
    e = put(b, "Score "); putn(e, s->score, 1); e = put(c, "Best "); putn(e, s->best, 1);
    fp_overlay(s, "Game over", b, c, "Tap to retry");
}
static void fp_step(Fp *s) {
    s->vy += FP_G; if (s->vy > FP_VMAX) s->vy = FP_VMAX;
    s->by += s->vy;
    if (s->by < 0) { s->by = 0; s->vy = 0; }                                                      /* the top only stops the bird */
    i32 sp = fp_speed(s);
    for (int i = 0; i < FP_N; i++) s->px[i] -= sp;
    for (int i = 0; i < FP_N; i++) {
        if (s->px[i] + FP_PW >= 0) continue;                                                      /* left the screen: it comes back behind the last pipe */
        i32 m = s->px[0]; for (int j = 1; j < FP_N; j++) if (s->px[j] > m) m = s->px[j];
        s->px[i] = m + FP_SPACE; s->scored[i] = 0; fp_new_gap(s, i);
    }
    for (int i = 0; i < FP_N; i++) {
        if (s->scored[i] || s->px[i] + FP_PW >= FP_BX) continue;
        s->scored[i] = 1; s->score++;
        if (s->score > s->best) { s->best = s->score; s->dirty = 1; }
    }
    i32 y = s->by >> 4, l = FP_BX + 3, r = FP_BX + FP_BW - 3, t = y + 4, b = y + FP_BH - 2;     /* a hitbox a little smaller than the picture */
    if (y + FP_BOXH >= FP_PLAY) { s->by = (FP_PLAY - FP_BOXH) << 4; fp_die(s); return; }
    for (int i = 0; i < FP_N; i++)
        if (r > s->px[i] && l < s->px[i] + FP_PW && (t < s->gap[i] || b > s->gap[i] + FP_GAP)) { fp_die(s); return; }
}
static void fp_event(lv_event_t *e) {
    Fp *s = (Fp *)lv_event_get_user_data(e); u32 now = lv_tick_get();
    if (lv_event_get_code(e) != EV_PRESSED) return;                                               /* touch-down: no wait for the release */
    if (s->state == FP_READY) { s->state = FP_RUN; s->last_t = now; lv_obj_add_flag(s->ovl, FL_HIDDEN); s->vy = -FP_FLAP; }
    else if (s->state == FP_RUN) s->vy = -FP_FLAP;
    else if (now - s->over_t > 700) fp_reset(s);                                                  /* a short pause after dying, so a late touch does not restart at once */
}
static void fp_tick(lv_timer_t *t) {
    Fp *s = (Fp *)timer_user_data(t); u32 now = lv_tick_get();
    if (s->state == FP_READY) {                                                                   /* the bird bobs while it waits */
        i32 ph = (i32)((now / 40u) % 24u), bob = ph < 12 ? ph : 24 - ph;
        s->by = (144 + bob) << 4; fp_draw(s); return;
    }
    if (s->state != FP_RUN) return;
    u32 n = (now - s->last_t) / FP_TICK;
    if (!n) return;
    if (n > 3) { n = 3; s->last_t = now; } else s->last_t += n * FP_TICK;
    for (u32 k = 0; k < n && s->state == FP_RUN; k++) fp_step(s);
    fp_draw(s); fp_render(s);
}
static void *fp_create(lv_obj_t *root) {
    Fp *s = (Fp *)app_malloc(sizeof(Fp)); if (!s) return 0;
    fill8(s, 0, sizeof(Fp)); s->rng = lv_tick_get() | 1u;
    FpFile f; if (blob_load(FP_PATH, &f, sizeof f) && f.magic == FP_MAGIC && f.best < 1000000u) s->best = f.best;
    s->l_best = mk_text(root, 0, 8, 256, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    s->board = mk_box(root, FP_X, FP_Y, FP_W, FP_H, 8, FP_SKY); lv_obj_add_event_cb(s->board, fp_event, 0, s);
    for (int i = 0; i < FP_N; i++) {
        s->pipe[i] = fp_part(s->board, 0, 0, FP_PW, FP_PLAY, 0, C_BLACK); set_bg_opa(s->pipe[i], 0);
        s->top[i] = fp_part(s->pipe[i], 3, 0, FP_PW - 6, 10, 0, FP_PIPE);
        s->topcap[i] = fp_part(s->pipe[i], 0, 0, FP_PW, FP_CAP, 3, FP_PIPE_CAP);
        s->bot[i] = fp_part(s->pipe[i], 3, 0, FP_PW - 6, 10, 0, FP_PIPE);
        s->botcap[i] = fp_part(s->pipe[i], 0, 0, FP_PW, FP_CAP, 3, FP_PIPE_CAP);
    }
    fp_part(s->board, 0, FP_PLAY, FP_W, FP_GROUND, 0, FP_SAND);
    fp_part(s->board, 0, FP_PLAY, FP_W, 5, 0, FP_PIPE);
    s->bird = fp_part(s->board, FP_BX, 0, FP_BW + 4, FP_BOXH, 0, C_BLACK); set_bg_opa(s->bird, 0);
    fp_part(s->bird, 0, 1, FP_BW, FP_BH, 8, FP_BIRD);
    s->wing = fp_part(s->bird, 3, 9, 10, 6, 3, FP_WING);
    fp_part(s->bird, 13, 3, 7, 7, 100, C_WHITE);
    fp_part(s->bird, 16, 5, 3, 3, 100, C_BLACK);
    fp_part(s->bird, 18, 10, 8, 5, 2, FP_BEAK);
    s->l_score = mk_text(s->board, 0, 8, FP_W, FT_POINT, C_WHITE, ALIGN_CENTER, "0"); lv_obj_clear_flag(s->l_score, FL_CLICKABLE);
    s->ovl = mk_box(root, 12, FP_Y + 80, 232, 124, 24, C_BLACK); set_bg_opa(s->ovl, 215); lv_obj_clear_flag(s->ovl, FL_CLICKABLE);
    s->l_msg = mk_text(s->ovl, 0, 4, 232, FT_BTN, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_msg, FL_CLICKABLE);
    s->l_sub = mk_text(s->ovl, 0, 40, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub, FL_CLICKABLE);
    s->l_sub2 = mk_text(s->ovl, 0, 66, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub2, FL_CLICKABLE);
    s->l_hint = mk_text(s->ovl, 0, 92, 232, FT_SMALL, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_hint, FL_CLICKABLE);
    s->timer = lv_timer_create(fp_tick, FP_TICK, s);
    fp_reset(s);
    return s;
}
static int fp_back(void *st) { (void)st; return 0; }
static void fp_destroy(void *st) { Fp *s = (Fp *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; fp_flush(s); app_free(s); }
APP_EXPORT(flappy, "Flappy", &ICON_FLAPPY, fp_create, fp_back, fp_destroy)
