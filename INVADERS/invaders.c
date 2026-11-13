/* SPDX-License-Identifier: Apache-2.0 */
/* apps/invaders.c: a Space Invaders-style shooter for the 256 x 402 touch screen.
 *
 * Under the field: "<" and ">" move the cannon (hold), "Fire" shoots (hold to keep firing: one shot at a time). 5 rows of 8 invaders march sideways, drop a row at each edge and
 * speed up as they die; they shoot back (more bullets and faster ones in later waves). Three bunkers take 4 hits each, a mystery ship crosses the top now and then (50 / 100 / 150).
 * You have 3 lives; the game ends when they are gone or the invaders reach your row. Clearing a wave brings a faster, lower one. Rows score 30 / 20 / 20 / 10 / 10.
 * The best score is kept in /user/invaders.sav, written when a game ends and when the app is closed (a few bytes, rarely).
 *
 * The invaders are 40 boxes inside one container, so the whole formation moves with a single set_pos; the cannon is 2 boxes, the shots 4, the bunkers 3, the mystery ship 1.
 * Holding "<", ">" or "Fire" relies on the press events the finger keeps sending (each one is good for 150 ms), so a missed release can never leave the cannon sliding.
 * Physics: integer pixels, one step per 30 ms (the game catches up, at most 3 steps, if the LVGL task is late). No globals, no libc: everything lives in the block returned by
 * create. All names are prefixed SI_ / si_ because every app shares one translation unit when bundled. */
#include "ui.h"
#include "icons.h"

#define SI_PATH "/user/invaders.sav"
#define SI_MAGIC 0x53493031u                                    /* "SI01" */
#define SI_BY 40                                                /* the field: 256 x 300 at y = 40; the pills start at y = 344 */
#define SI_BH 300
#define SI_ROWS 5
#define SI_COLS 8
#define SI_N (SI_ROWS * SI_COLS)
#define SI_AW 20                                                /* an invader is 20 x 14 on a 28 x 22 grid */
#define SI_AH 14
#define SI_GX 28
#define SI_GY 22
#define SI_FW (7 * SI_GX + SI_AW)                               /* the whole formation: 216 x 102 */
#define SI_FH (4 * SI_GY + SI_AH)
#define SI_PW 26                                                /* the cannon: a 26 x 10 base and a turret on top, at this y */
#define SI_PLY 274
#define SI_PSPD 4                                               /* px per step */
#define SI_BUNY 232                                             /* bunkers: 3 of 40 x 20, 4 hits each */
#define SI_BUNW 40
#define SI_TICK 30
#define SI_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define SI_EV_PRESSING 2                                        /* LVGL events besides ui.h's: PRESSING (repeats while a finger is down), PRESS_LOST, RELEASED */
#define SI_EV_PRESS_LOST 3
#define SI_EV_RELEASED 8
enum { SI_READY, SI_RUN, SI_OVER };

typedef struct { u32 magic, best; } SiFile;
typedef struct {
    u32 rng, last_t, last_step, over_t, held_until, fire_until, pause_until, best, score;
    i32 ax, ay, dir, px, pbx, pby, ufox, ufodir, ufopts, lax, lay, lpx, lpbx, lpby, lufox;     /* formation corner, its direction, the cannon, the shot, the mystery ship, and what is drawn now */
    i32 abx[3], aby[3], bhp[3], lbhp[3];
    u8 state, lives, wave, nalive, anim, dirty, hold, pbul, ufo, inv, aact[3], alive[SI_N], pad[2];   /* hold: 0 none, 1 left, 2 right */
    lv_obj_t *board, *form, *al[SI_N], *pl, *tur, *pb, *ab[3], *bun[3], *uf, *ovl, *l_msg, *l_sub, *l_sub2, *l_hint, *l_score, *l_lives, *b_left, *b_right, *b_fire;
    lv_timer_t *timer;
    char t_score[TXT_MAX], t_lives[TXT_MAX], t_msg[TXT_MAX], t_sub[TXT_MAX], t_sub2[TXT_MAX], t_hint[TXT_MAX];
} Si;

static u32 si_rand(Si *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static u32 si_pts(int row) { return row == 0 ? 30u : row < 3 ? 20u : 10u; }
static u16 si_row_color(int r) { return r == 0 ? SI_RGB(255, 90, 200) : r < 3 ? SI_RGB(90, 220, 255) : SI_RGB(120, 255, 120); }
static u32 si_interval(const Si *s) { i32 v = 30 + (i32)s->nalive * 14 - ((i32)s->wave - 1) * 25; return (u32)(v < 30 ? 30 : v); }       /* ms between invader steps: 590 with all 40 alive, 44 with one */
static i32 si_bun_h(i32 hp) { return hp * 5; }
static i32 si_bun_x(int i) { return 24 + i * 84; }
static int si_overlap(i32 ax, i32 ay, i32 aw, i32 ah, i32 bx, i32 by, i32 bw, i32 bh) { return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah; }

static void si_flush(Si *s) {
    SiFile f;
    if (!s->dirty) return;
    s->dirty = 0; f.magic = SI_MAGIC; f.best = s->best; blob_save(SI_PATH, &f, sizeof f);
}
static void si_render(Si *s) {
    char b[TXT_MAX], *e = put(b, "Score "); putn(e, s->score, 1); set_txt(s->l_score, s->t_score, b);
    e = put(b, "Lives "); putn(e, s->lives, 1); set_txt(s->l_lives, s->t_lives, b);
}
static void si_overlay(Si *s, const char *msg, const char *sub, const char *sub2, const char *hint) {
    set_txt(s->l_msg, s->t_msg, msg); set_txt(s->l_sub, s->t_sub, sub); set_txt(s->l_sub2, s->t_sub2, sub2); set_txt(s->l_hint, s->t_hint, hint);
    lv_obj_clear_flag(s->ovl, FL_HIDDEN);
}
/* the invaders' width flips between 20 and 16 at every step: they "breathe" */
static void si_anim(Si *s) {
    for (int i = 0; i < SI_N; i++) {
        if (!s->alive[i]) continue;
        lv_obj_set_size(s->al[i], s->anim ? SI_AW - 4 : SI_AW, SI_AH); lv_obj_set_pos(s->al[i], (i % SI_COLS) * SI_GX + (s->anim ? 2 : 0), (i / SI_COLS) * SI_GY);
    }
}
static void si_draw(Si *s) {
    if (s->ax != s->lax || s->ay != s->lay) { s->lax = s->ax; s->lay = s->ay; lv_obj_set_pos(s->form, s->ax, s->ay); }
    if (s->px != s->lpx) { s->lpx = s->px; lv_obj_set_pos(s->pl, s->px, SI_PLY); lv_obj_set_pos(s->tur, s->px + 10, SI_PLY - 6); }
    if (s->inv && (s->inv / 4) % 2) { lv_obj_add_flag(s->pl, FL_HIDDEN); lv_obj_add_flag(s->tur, FL_HIDDEN); }                  /* blink while protected */
    else { lv_obj_clear_flag(s->pl, FL_HIDDEN); lv_obj_clear_flag(s->tur, FL_HIDDEN); }
    if (s->pbul) { if (s->pbx != s->lpbx || s->pby != s->lpby) { s->lpbx = s->pbx; s->lpby = s->pby; lv_obj_set_pos(s->pb, s->pbx, s->pby); } lv_obj_clear_flag(s->pb, FL_HIDDEN); }
    else lv_obj_add_flag(s->pb, FL_HIDDEN);
    for (int k = 0; k < 3; k++) { if (s->aact[k]) { lv_obj_set_pos(s->ab[k], s->abx[k], s->aby[k]); lv_obj_clear_flag(s->ab[k], FL_HIDDEN); } else lv_obj_add_flag(s->ab[k], FL_HIDDEN); }
    if (s->ufo) { if (s->ufox != s->lufox) { s->lufox = s->ufox; lv_obj_set_pos(s->uf, s->ufox, 8); } lv_obj_clear_flag(s->uf, FL_HIDDEN); } else lv_obj_add_flag(s->uf, FL_HIDDEN);
    for (int i = 0; i < 3; i++) {
        if (s->bhp[i] == s->lbhp[i]) continue;
        s->lbhp[i] = s->bhp[i];
        if (s->bhp[i] <= 0) { lv_obj_add_flag(s->bun[i], FL_HIDDEN); continue; }
        lv_obj_set_size(s->bun[i], SI_BUNW, si_bun_h(s->bhp[i])); lv_obj_set_pos(s->bun[i], si_bun_x(i), SI_BUNY + (4 - s->bhp[i]) * 5); lv_obj_clear_flag(s->bun[i], FL_HIDDEN);
    }
}
static void si_over(Si *s) {
    char b[TXT_MAX], c[TXT_MAX], *e;
    s->state = SI_OVER; s->over_t = lv_tick_get(); s->hold = 0; s->fire_until = 0; motor_once(4, 1);
    if (s->score > s->best) { s->best = s->score; s->dirty = 1; }
    si_flush(s);
    e = put(b, "Score "); putn(e, s->score, 1); e = put(c, "Best "); putn(e, s->best, 1);
    si_overlay(s, "Game over", b, c, "Tap to retry");
    si_render(s); si_draw(s);
}
/* a new formation, full bunkers, no shots in the air */
static void si_build(Si *s) {
    for (int i = 0; i < SI_N; i++) { s->alive[i] = 1; lv_obj_clear_flag(s->al[i], FL_HIDDEN); }
    s->nalive = SI_N; s->ax = 20; s->ay = 36 + (s->wave > 6 ? 5 : (i32)s->wave - 1) * 10; s->dir = 1; s->anim = 0;
    for (int i = 0; i < 3; i++) s->bhp[i] = 4;
    for (int k = 0; k < 3; k++) s->aact[k] = 0;
    s->pbul = 0; s->ufo = 0;
    s->last_step = s->last_t;
    si_anim(s);
}
static void si_new(Si *s) {
    s->score = 0; s->lives = 3; s->wave = 1; s->px = (256 - SI_PW) / 2; s->inv = 0; s->hold = 0; s->fire_until = 0; s->pause_until = 0;
    s->last_t = lv_tick_get();
    s->state = SI_RUN; lv_obj_add_flag(s->ovl, FL_HIDDEN);
    si_build(s); s->lbhp[0] = s->lbhp[1] = s->lbhp[2] = -1; s->lax = -9999;
    si_render(s); si_draw(s);
}
static void si_fire(Si *s) {
    if (s->pbul || s->state != SI_RUN) return;
    s->pbul = 1; s->pbx = s->px + SI_PW / 2 - 1; s->pby = SI_PLY - 10;
}
/* the invader (index) under a point of the field, or -1 */
static int si_alien_at(const Si *s, i32 x, i32 y) {
    i32 cx = x - s->ax, cy = y - s->ay;
    if (cx < 0 || cy < 0 || cx >= SI_FW || cy >= SI_FH) return -1;
    if (cx % SI_GX >= SI_AW || cy % SI_GY >= SI_AH) return -1;
    { int i = (int)(cy / SI_GY) * SI_COLS + (int)(cx / SI_GX); return s->alive[i] ? i : -1; }
}
static void si_hit_player(Si *s) {
    s->lives--; motor_once(4, 1); s->inv = 50;
    for (int k = 0; k < 3; k++) s->aact[k] = 0;
    s->pbul = 0;
    si_render(s);
    if (!s->lives) si_over(s);
}
/* the invaders take a step: sideways, or down and back at an edge; they may land */
static void si_formation(Si *s) {
    int minc = SI_COLS, maxc = -1, maxr = -1;
    i32 left, right;
    for (int i = 0; i < SI_N; i++) if (s->alive[i]) { int c = i % SI_COLS, r = i / SI_COLS; if (c < minc) minc = c; if (c > maxc) maxc = c; if (r > maxr) maxr = r; }
    if (maxc < 0) return;
    left = s->ax + minc * SI_GX; right = s->ax + maxc * SI_GX + SI_AW;
    if (s->dir > 0 ? right + 4 > 252 : left - 4 < 4) { s->ay += 10; s->dir = -s->dir; } else s->ax += s->dir * 4;
    s->anim ^= 1; si_anim(s);
    if (s->ay + maxr * SI_GY + SI_AH >= SI_BUNY) for (int i = 0; i < 3; i++) s->bhp[i] = 0;                 /* the bunkers are trampled */
    if (s->ay + maxr * SI_GY + SI_AH >= SI_PLY - 6) si_over(s);                                              /* they reached the cannon */
}
/* one step of 30 ms */
static void si_step(Si *s, u32 now) {
    u32 rn = lv_tick_get(); i32 aspd = 3 + (s->wave > 6 ? 3 : (i32)s->wave / 2);
    int maxb = 1 + (s->wave > 4 ? 2 : (int)s->wave / 2), act = 0;
    if (s->hold && rn > s->held_until) s->hold = 0;
    if (s->fire_until && rn > s->fire_until) s->fire_until = 0;
    if (s->hold) { s->px += s->hold == 1 ? -SI_PSPD : SI_PSPD; if (s->px < 2) s->px = 2; if (s->px > 256 - SI_PW - 2) s->px = 256 - SI_PW - 2; }
    if (s->fire_until) si_fire(s);
    if (s->inv) s->inv--;
    /* the cannon's shot */
    if (s->pbul) {
        int hit = 0;
        s->pby -= 8;
        for (int i = 0; i < 3 && !hit; i++) if (s->bhp[i] > 0 && si_overlap(s->pbx, s->pby, 3, 10, si_bun_x(i), SI_BUNY + (4 - s->bhp[i]) * 5, SI_BUNW, si_bun_h(s->bhp[i]))) { s->bhp[i]--; hit = 1; }
        if (!hit && s->ufo && si_overlap(s->pbx, s->pby, 3, 10, s->ufox, 8, 28, 10)) { s->score += (u32)s->ufopts; s->ufo = 0; hit = 1; motor_once(1, 1); }
        for (int p = 0; p < 2 && !hit; p++) {
            int i = si_alien_at(s, s->pbx + 1, s->pby + p * 5);
            if (i < 0) continue;
            s->alive[i] = 0; s->nalive--; lv_obj_add_flag(s->al[i], FL_HIDDEN); s->score += si_pts(i / SI_COLS); hit = 1;
        }
        if (hit || s->pby < -10) s->pbul = 0;
    }
    /* the invaders' shots */
    for (int k = 0; k < 3; k++) {
        if (!s->aact[k]) continue;
        act++; s->aby[k] += aspd;
        if (s->inv == 0 && si_overlap(s->abx[k], s->aby[k], 4, 10, s->px, SI_PLY - 6, SI_PW, 16)) { s->aact[k] = 0; si_hit_player(s); if (s->state != SI_RUN) return; continue; }
        for (int i = 0; i < 3 && s->aact[k]; i++) if (s->bhp[i] > 0 && si_overlap(s->abx[k], s->aby[k], 4, 10, si_bun_x(i), SI_BUNY + (4 - s->bhp[i]) * 5, SI_BUNW, si_bun_h(s->bhp[i]))) { s->bhp[i]--; s->aact[k] = 0; }
        if (s->aact[k] && s->aby[k] > SI_BH) s->aact[k] = 0;
    }
    if (act < maxb && s->nalive && si_rand(s) % (u32)(s->wave > 7 ? 9 : 30 - 3 * (i32)s->wave) == 0) {         /* the bottom invader of a random column shoots */
        u32 c0 = si_rand(s) % SI_COLS;
        for (u32 q = 0; q < SI_COLS; q++) {
            int c = (int)((c0 + q) % SI_COLS), r = -1;
            for (int rr = SI_ROWS - 1; rr >= 0 && r < 0; rr--) if (s->alive[rr * SI_COLS + c]) r = rr;
            if (r < 0) continue;
            for (int k = 0; k < 3; k++) if (!s->aact[k]) { s->aact[k] = 1; s->abx[k] = s->ax + c * SI_GX + SI_AW / 2 - 2; s->aby[k] = s->ay + r * SI_GY + SI_AH; break; }
            break;
        }
    }
    /* the mystery ship */
    if (s->ufo) { s->ufox += s->ufodir * 2; if (s->ufox < -32 || s->ufox > 260) s->ufo = 0; }
    else if (s->nalive > 8 && si_rand(s) % 700u == 0) { s->ufo = 1; s->ufodir = (si_rand(s) & 1u) ? 1 : -1; s->ufox = s->ufodir > 0 ? -30 : 258; s->ufopts = 50 * (1 + (i32)(si_rand(s) % 3u)); }
    /* the formation */
    if (now - s->last_step >= si_interval(s)) { s->last_step = now; si_formation(s); if (s->state != SI_RUN) return; }
    if (s->score > s->best) { s->best = s->score; s->dirty = 1; }
    if (!s->nalive) {                                                                                         /* a wave is cleared: a faster, lower one after a short pause */
        char b[TXT_MAX], *e = put(b, "Wave "); s->wave = (u8)(s->wave < 255 ? s->wave + 1 : 255); putn(e, s->wave, 1);
        si_build(s); s->pause_until = lv_tick_get() + 1200u; motor_once(4, 1);
        si_overlay(s, b, "Get ready", "", "");
    }
}
static void si_event(lv_event_t *e) {
    Si *s = (Si *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e); u32 code = lv_event_get_code(e), now = lv_tick_get();
    if (t == s->board) {
        if (code == EV_PRESSED && (s->state == SI_READY || (s->state == SI_OVER && now - s->over_t > 700u))) si_new(s);       /* a short pause after the end, so a late touch does not restart at once */
        return;
    }
    if (s->state != SI_RUN) return;
    if (t == s->b_left || t == s->b_right) {
        u8 dir = t == s->b_left ? 1 : 2;
        if (code == EV_PRESSED || code == SI_EV_PRESSING) { s->hold = dir; s->held_until = now + 150u; }
        else if ((code == SI_EV_RELEASED || code == SI_EV_PRESS_LOST || code == EV_CLICKED) && s->hold == dir) s->hold = 0;
    } else if (t == s->b_fire) {
        if (code == EV_PRESSED) { s->fire_until = now + 150u; si_fire(s); si_draw(s); }
        else if (code == SI_EV_PRESSING) s->fire_until = now + 150u;
        else if (code == SI_EV_RELEASED || code == SI_EV_PRESS_LOST || code == EV_CLICKED) s->fire_until = 0;
    }
}
static void si_tick(lv_timer_t *t) {
    Si *s = (Si *)timer_user_data(t); u32 now = lv_tick_get(), n;
    if (s->state != SI_RUN) return;
    if (s->pause_until) {
        if (now < s->pause_until) { s->last_t = now; return; }
        s->pause_until = 0; lv_obj_add_flag(s->ovl, FL_HIDDEN); s->last_t = now; s->last_step = now; si_draw(s); return;
    }
    n = (now - s->last_t) / SI_TICK;
    if (!n) return;
    if (n > 3) { n = 3; s->last_t = now; } else s->last_t += n * SI_TICK;
    for (u32 k = 0; k < n && s->state == SI_RUN && !s->pause_until; k++) si_step(s, s->last_t - (n - 1 - k) * SI_TICK);
    si_render(s); si_draw(s);
}
static void *si_create(lv_obj_t *root) {
    Si *s = (Si *)app_malloc(sizeof(Si)); if (!s) return 0;
    fill8(s, 0, sizeof(Si)); s->rng = lv_tick_get() | 1u; s->lax = -9999; s->lpx = -9999; s->lufox = -9999; s->lpbx = -9999; s->lpby = -9999; s->lbhp[0] = s->lbhp[1] = s->lbhp[2] = -1;
    SiFile f; if (blob_load(SI_PATH, &f, sizeof f) && f.magic == SI_MAGIC && f.best < 100000000u) s->best = f.best;
    s->l_score = mk_text(root, 10, 10, 130, ROBOTO_MEDIUM(10), C_MUTED, ALIGN_LEFT, "");
    s->l_lives = mk_text(root, 140, 10, 116, ROBOTO_MEDIUM(10), C_MUTED, ALIGN_LEFT, "");
    s->board = mk_box(root, 0, SI_BY, 256, SI_BH, 0, SI_RGB(8, 10, 24)); lv_obj_add_event_cb(s->board, si_event, 0, s);
    s->form = mk_box(s->board, 20, 36, SI_FW, SI_FH, 0, C_BLACK); set_bg_opa(s->form, 0); lv_obj_clear_flag(s->form, FL_CLICKABLE);
    for (int i = 0; i < SI_N; i++) { s->al[i] = mk_box(s->form, (i % SI_COLS) * SI_GX, (i / SI_COLS) * SI_GY, SI_AW, SI_AH, 4, si_row_color(i / SI_COLS)); lv_obj_clear_flag(s->al[i], FL_CLICKABLE); }
    for (int i = 0; i < 3; i++) { s->bun[i] = mk_box(s->board, si_bun_x(i), SI_BUNY, SI_BUNW, 20, 8, SI_RGB(70, 190, 90)); lv_obj_clear_flag(s->bun[i], FL_CLICKABLE); }
    s->pl = mk_box(s->board, 0, SI_PLY, SI_PW, 10, 3, SI_RGB(120, 255, 120)); lv_obj_clear_flag(s->pl, FL_CLICKABLE);
    s->tur = mk_box(s->board, 0, SI_PLY - 6, 6, 6, 2, SI_RGB(120, 255, 120)); lv_obj_clear_flag(s->tur, FL_CLICKABLE);
    s->pb = mk_box(s->board, 0, 0, 3, 10, 1, C_WHITE); lv_obj_clear_flag(s->pb, FL_CLICKABLE); lv_obj_add_flag(s->pb, FL_HIDDEN);
    for (int k = 0; k < 3; k++) { s->ab[k] = mk_box(s->board, 0, 0, 4, 10, 2, C_SERVE); lv_obj_clear_flag(s->ab[k], FL_CLICKABLE); lv_obj_add_flag(s->ab[k], FL_HIDDEN); }
    s->uf = mk_box(s->board, 0, 8, 28, 10, 5, SI_RGB(255, 60, 60)); lv_obj_clear_flag(s->uf, FL_CLICKABLE); lv_obj_add_flag(s->uf, FL_HIDDEN);
    s->ovl = mk_box(root, 12, 130, 232, 124, 24, C_BLACK); set_bg_opa(s->ovl, 215); lv_obj_clear_flag(s->ovl, FL_CLICKABLE);
    s->l_msg = mk_text(s->ovl, 0, 4, 232, FT_BTN, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_msg, FL_CLICKABLE);
    s->l_sub = mk_text(s->ovl, 0, 40, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub, FL_CLICKABLE);
    s->l_sub2 = mk_text(s->ovl, 0, 66, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub2, FL_CLICKABLE);
    s->l_hint = mk_text(s->ovl, 0, 92, 232, FT_SMALL, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_hint, FL_CLICKABLE);
    s->b_left = mk_pill(root, 6, 344, 70, "<", 0, si_event, s);
    s->b_fire = mk_pill(root, 82, 344, 92, "Fire", 0, si_event, s);
    s->b_right = mk_pill(root, 180, 344, 70, ">", 0, si_event, s);
    s->timer = lv_timer_create(si_tick, SI_TICK, s);
    s->px = (256 - SI_PW) / 2; s->lives = 3; s->wave = 1; s->last_t = lv_tick_get();
    si_build(s); si_render(s); si_draw(s);
    si_overlay(s, "Invaders", "< > move, Fire", "shoots", "Tap to start");
    return s;
}
static int si_back(void *st) { (void)st; return 0; }
static void si_destroy(void *st) { Si *s = (Si *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; si_flush(s); app_free(s); }
APP_EXPORT(invaders, "Invaders", &ICON_INVADERS, si_create, si_back, si_destroy)
