/* SPDX-License-Identifier: Apache-2.0 */
/* apps/rider.c: a one-touch motorbike stunt game in the spirit of Ketchapp's Rider (an original game, none of its graphics or levels).
 *
 * The bike rides on its own over an endless, hilly track. On the ground you can do nothing; in the air you HOLD the screen to spin the bike backwards (a backflip), and let go to
 * stop the spin. Land on the wheels (within 70 degrees of the slope) or crash; land after (nearly) a full turn and it is a flip: +10 each. Score = distance / 20 + flips. The bike
 * speeds up with time. Kickers (a steep ramp, then a drop) give the airtime for flips; without input the bike drifts nose-down a little, so a long drop also needs a touch to keep the nose up.
 * The best score is kept in /user/rider.sav, written when a run ends and when the app is closed (a few bytes, rarely).
 *
 * The screen is made only of boxes: 34 terrain columns of 8 px (they scroll; a column is resized only when a new part of the track reaches it), and the bike is 9 small boxes
 * (two wheels, frame, handlebar, rider) placed with integer sine tables, so there is no float and no libgcc. The track is a ring of vertices 32 px apart, filled in ahead of the bike.
 * Physics: integer, 1/16 pixel, angles in 1/8 degree, one step per 33 ms (the game catches up, at most 3 steps, if the LVGL task is late). On the ground the bike follows the track
 * at a steady speed and leaves it only when the track drops away faster than gravity pulls (a lip or a cliff); in the air it keeps its speed, spins as you hold, and lands
 * when a wheel meets the track. No globals, no libc: everything lives in the block returned by create. All names are prefixed RD_ / rd_ because every app shares one translation unit. */
#include "ui.h"
#include "icons.h"

#define RD_PATH "/user/rider.sav"
#define RD_MAGIC 0x52444531u                                    /* "RDE1" */
#define RD_BY 40                                                /* the play field: the whole screen below the header */
#define RD_BH 362
#define RD_CW 8                                                 /* terrain column width */
#define RD_NCOL (256 / RD_CW + 2)
#define RD_VS 32                                                /* distance between track vertices, px */
#define RD_NV 28                                                /* vertices kept (a ring) */
#define RD_HMIN 130                                             /* the track stays between these heights (board y, px) */
#define RD_HMAX 310
#define RD_CAMX 70                                              /* the bike's x on the screen */
#define RD_L 13                                                 /* half wheelbase, px */
#define RD_D 6                                                  /* wheel centre below the body centre */
#define RD_RW 5                                                 /* wheel radius */
#define RD_OFF 13                                               /* body centre above the track (11 on the flat, 13 is a compromise that also fits the slopes) */
#define RD_G 9                                                  /* gravity, 1/16 px per step per step */
#define RD_GAP 24                                               /* 1.5 px: the track must drop this far below the bike's path to let it fly */
#ifndef RD_LAND
#define RD_LAND 70                                              /* degrees off the slope that still count as a landing on the wheels */
#endif
#ifndef RD_DRIFT
#define RD_DRIFT 24                                             /* the nose-down drift without a touch, 1/8 degree per step: 3 degrees per step */
#endif
#define RD_TICK 33                                              /* ms per step */
#define RD_NP 9
#define RD_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define RD_SKY RD_RGB(12, 14, 40)
#define RD_TER_A RD_RGB(24, 120, 190)
#define RD_TER_B RD_RGB(34, 142, 214)
#define RD_EV_PRESSING 2                                        /* LVGL events besides ui.h's: PRESSING (repeats while a finger is down), PRESS_LOST, RELEASED */
#define RD_EV_PRESS_LOST 3
#define RD_EV_RELEASED 8
enum { RD_READY, RD_RUN, RD_OVER };

/* sin(0..90 degrees) * 64 and atan(0..64 / 64) in degrees */
static const u8 RD_SIN[91] = { 0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 39, 40, 41, 42, 43, 44, 44, 45, 46, 47, 48, 48, 49, 50, 50, 51, 52, 52, 53, 54, 54, 55, 55, 56, 57, 57, 58, 58, 58, 59, 59, 60, 60, 61, 61, 61, 62, 62, 62, 62, 63, 63, 63, 63, 63, 64, 64, 64, 64, 64, 64, 64, 64 };
static const u8 RD_ATAN[65] = { 0, 1, 2, 3, 4, 4, 5, 6, 7, 8, 9, 10, 11, 11, 12, 13, 14, 15, 16, 17, 17, 18, 19, 20, 21, 21, 22, 23, 24, 24, 25, 26, 27, 27, 28, 29, 29, 30, 31, 31, 32, 33, 33, 34, 35, 35, 36, 36, 37, 37, 38, 39, 39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45 };
/* the bike's boxes: offset from the body centre and size (wheels, frame, handlebar, torso, head) */
static const signed char RD_POX[RD_NP] = { -13, 13, -6, 3, 10, 12, -1, -2, -2 };
static const signed char RD_POY[RD_NP] = { 6, 6, 3, 2, 0, -5, -6, -12, -19 };
static const u8 RD_PSZ[RD_NP] = { 10, 10, 4, 4, 4, 4, 5, 5, 8 };

typedef struct { u32 magic, best; } RdFile;
typedef struct {
    u32 rng, last_t, over_t, held_until, flip_until, best, t;
    i32 bx, by, vx, vy, ang8, w8, rot8, bonus;                  /* body centre in 1/16 px (world x, board y), velocity per step, angle and spin in 1/8 degree (clockwise = nose down) */
    i32 vgen, seg, dh, mode, lcam, lscore;                       /* track generator state; what is drawn now */
    i32 vh[RD_NV], colc[RD_NCOL], coltop[RD_NCOL];
    u8 state, air, air_t, dirty;
    lv_obj_t *board, *col[RD_NCOL], *part[RD_NP], *ovl, *l_msg, *l_sub, *l_sub2, *l_hint, *l_score, *l_best, *l_flip;
    lv_timer_t *timer;
    char t_score[TXT_MAX], t_best[TXT_MAX], t_msg[TXT_MAX], t_sub[TXT_MAX], t_sub2[TXT_MAX], t_hint[TXT_MAX], t_flip[TXT_MAX];
} Rd;

static u32 rd_rand(Rd *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static i32 rd_sin(i32 d) {                                       /* sin(d degrees) * 64 */
    d %= 360; if (d < 0) d += 360;
    if (d <= 90) return RD_SIN[d];
    if (d <= 180) return RD_SIN[180 - d];
    if (d <= 270) return -(i32)RD_SIN[d - 180];
    return -(i32)RD_SIN[360 - d];
}
static i32 rd_cos(i32 d) { return rd_sin(d + 90); }
static i32 rd_atan(i32 r) {                                      /* degrees of a slope given in 1/64 */
    i32 a = r < 0 ? -r : r, d;
    if (a <= 64) d = RD_ATAN[a]; else { i32 q = 4096 / a; d = 90 - RD_ATAN[q > 64 ? 64 : q]; }
    return r < 0 ? -d : d;
}
static i32 rd_norm8(i32 a) { a %= 2880; if (a > 1440) a -= 2880; else if (a <= -1440) a += 2880; return a; }
static u32 rd_score(const Rd *s) { i32 d = (s->bx >> 4) - RD_CAMX; return (u32)((d > 0 ? d : 0) / 20 + s->bonus); }

/* the track: height (1/16 px) at world x (px) between two vertices */
static i32 rd_ground(const Rd *s, i32 x) {
    i32 v, f, h0, h1;
    if (x < 0) x = 0;
    v = x >> 5; f = x & 31;
    h0 = s->vh[v % RD_NV]; h1 = s->vh[(v + 1) % RD_NV];
    return (h0 * (32 - f) + h1 * f) / 2;
}
/* one more vertex: flat, hill up, hill down, or a kicker (steep rise, a drop behind it) */
static void rd_gen(Rd *s) {
    i32 d = 0, h, last = s->vh[(s->vgen + RD_NV - 1) % RD_NV];
    if (!s->seg) {
        u32 r = rd_rand(s) % 10u;
        if (r < 2) { s->mode = 0; s->seg = 2 + (i32)(rd_rand(s) % 3u); }
        else if (r < 5) { s->mode = 1; s->seg = 2 + (i32)(rd_rand(s) % 4u); s->dh = -(8 + (i32)(rd_rand(s) % 7u)); }
        else if (r < 8) { s->mode = 2; s->seg = 2 + (i32)(rd_rand(s) % 4u); s->dh = 8 + (i32)(rd_rand(s) % 7u); }
        else { s->mode = 3; s->seg = 6; }
    }
    if (s->mode == 1 || s->mode == 2) d = s->dh;
    else if (s->mode == 3) { static const signed char K[6] = { 0, 10, 26, 30, -36, -32 }; d = K[s->seg - 1]; }
    s->seg--;
    h = last + d; if (h < RD_HMIN) h = RD_HMIN; if (h > RD_HMAX) h = RD_HMAX;
    s->vh[s->vgen % RD_NV] = h; s->vgen++;
}
static void rd_ahead(Rd *s) { while (s->vgen * RD_VS < (s->bx >> 4) + 500) rd_gen(s); }

/* where the body centre and the angle sit when the wheels rest on the track at x (1/16 px): the angle is the slope across the wheelbase; the centre stands a fixed
 * RD_OFF above the track at its own x. (A stand-off that follows the angle would make the vertical speed wobble at every change of slope, and a wobble is what the
 * takeoff test would take for the track falling away.) */
static void rd_pose(const Rd *s, i32 x, i32 *cy, i32 *t8) {
    i32 xp = x >> 4, hr = rd_ground(s, xp - RD_L), hf = rd_ground(s, xp + RD_L);
    *cy = rd_ground(s, xp) - RD_OFF * 16;
    *t8 = rd_atan((hf - hr) * 64 / (2 * RD_L * 16)) * 8;
}
/* a point of the bike in world coordinates (1/16 px) */
static void rd_pt(const Rd *s, i32 ox, i32 oy, i32 *wx, i32 *wy) {
    i32 deg = s->ang8 / 8, c = rd_cos(deg), sn = rd_sin(deg);
    *wx = s->bx + (ox * c - oy * sn) * 16 / 64; *wy = s->by + (ox * sn + oy * c) * 16 / 64;
}

static void rd_flush(Rd *s) {
    RdFile f;
    if (!s->dirty) return;
    s->dirty = 0; f.magic = RD_MAGIC; f.best = s->best; blob_save(RD_PATH, &f, sizeof f);
}
static void rd_render(Rd *s) {
    u32 sc = rd_score(s); char b[TXT_MAX], *e;
    if ((i32)sc == s->lscore && s->t) return;
    s->lscore = (i32)sc;
    e = put(b, "Score "); putn(e, sc, 1); set_txt(s->l_score, s->t_score, b);
    e = put(b, "Best "); putn(e, s->best > sc ? s->best : sc, 1); set_txt(s->l_best, s->t_best, b);
}
static void rd_overlay(Rd *s, const char *msg, const char *sub, const char *sub2, const char *hint) {
    set_txt(s->l_msg, s->t_msg, msg); set_txt(s->l_sub, s->t_sub, sub); set_txt(s->l_sub2, s->t_sub2, sub2); set_txt(s->l_hint, s->t_hint, hint);
    lv_obj_clear_flag(s->ovl, FL_HIDDEN);
}
static void rd_draw(Rd *s) {
    i32 cam = (s->bx >> 4) - RD_CAMX, c0 = cam >> 3, deg = s->ang8 / 8, c = rd_cos(deg), sn = rd_sin(deg), moved = cam != s->lcam;
    s->lcam = cam;
    if (moved) for (i32 k = 0; k < RD_NCOL; k++) {
        i32 col = c0 + k, slot = col % RD_NCOL;
        if (s->colc[slot] != col) {                                                       /* a new stretch of track: size and colour it once */
            i32 top = rd_ground(s, col * RD_CW + RD_CW / 2) >> 4;
            s->colc[slot] = col; s->coltop[slot] = top;
            lv_obj_set_size(s->col[slot], RD_CW, RD_BH - top);
            set_bg_color(s->col[slot], ((col >> 1) & 1) ? RD_TER_B : RD_TER_A);
        }
        lv_obj_set_pos(s->col[slot], col * RD_CW - cam, s->coltop[slot]);
    }
    for (int k = 0; k < RD_NP; k++) {
        i32 ox = RD_POX[k], oy = RD_POY[k], sz = RD_PSZ[k];
        lv_obj_set_pos(s->part[k], RD_CAMX + (ox * c - oy * sn) / 64 - sz / 2, (s->by >> 4) + (ox * sn + oy * c) / 64 - sz / 2);
    }
}
static void rd_reset(Rd *s) {
    i32 cy, t8;
    for (i32 i = 0; i < 12; i++) s->vh[i] = 250;
    s->vgen = 12; s->seg = 0; s->mode = 0; s->dh = 0;
    s->bx = RD_CAMX * 16; rd_ahead(s);
    rd_pose(s, s->bx, &cy, &t8);
    s->by = cy; s->ang8 = t8; s->vx = 0; s->vy = 0; s->w8 = 0; s->rot8 = 0; s->bonus = 0; s->t = 0; s->air = 0; s->held_until = 0;
    for (int i = 0; i < RD_NCOL; i++) s->colc[i] = -1;
    s->lcam = -9999; s->lscore = -1; s->state = RD_READY;
    lv_obj_add_flag(s->l_flip, FL_HIDDEN); s->flip_until = 0;
    rd_draw(s); rd_render(s);
    rd_overlay(s, "Rider", "Hold to flip", "in the air", "Tap to start");
}
static void rd_crash(Rd *s) {
    char b[TXT_MAX], c[TXT_MAX], *e; u32 sc = rd_score(s);
    s->state = RD_OVER; s->over_t = lv_tick_get(); motor_once(4, 1);
    if (sc > s->best) { s->best = sc; s->dirty = 1; }
    rd_flush(s);
    e = put(b, "Score "); putn(e, sc, 1); e = put(c, "Best "); putn(e, s->best, 1);
    rd_overlay(s, "Crash!", b, c, "Tap to retry");
}
static void rd_flip(Rd *s, i32 n, int back) {
    char b[TXT_MAX], *e = put(b, back ? "Backflip " : "Front flip ");
    if (n > 1) { e = put(e, "x"); e = putn(e, (u32)n, 1); e = put(e, " "); }
    e = put(e, "+"); putn(e, (u32)(10 * n), 1);
    set_txt(s->l_flip, s->t_flip, b); lv_obj_clear_flag(s->l_flip, FL_HIDDEN); s->flip_until = lv_tick_get() + 1200u;
}
static void rd_step(Rd *s) {
    u32 now = lv_tick_get(); int held = s->held_until && now <= s->held_until;
    s->t++;
    if (!s->air) {
        i32 tgt = 72 + (i32)(s->t / 45u > 40u ? 40u : s->t / 45u), nbx, ncy, nt8, dang;           /* the target speed grows from 4.5 to 7 px per step over a minute */
        s->vx += (tgt - s->vx) / 16 + rd_sin(s->ang8 / 8) * 3 / 128;                               /* downhill a little faster, uphill a little slower */
        if (s->vx < 48) s->vx = 48;
        if (s->vx > 150) s->vx = 150;
        nbx = s->bx + s->vx; rd_pose(s, nbx, &ncy, &nt8);
        if (ncy - (s->by + s->vy + RD_G) > RD_GAP) {                                               /* the track fell away from under the bike: it flies on */
            s->air = 1; s->air_t = 0; s->rot8 = 0;
            if (s->w8 > 12) s->w8 = 12;                                                            /* a little of the spin the slope gave it */
            if (s->w8 < -12) s->w8 = -12;
            s->bx = nbx; s->vy += RD_G; s->by += s->vy;
            return;
        }
        dang = (nt8 - s->ang8) / 2;                                                                /* follow the slope */
        s->ang8 += dang; s->w8 = dang; s->vy = ncy - s->by; s->by = ncy; s->bx = nbx;
        return;
    }
    {
        i32 rx, ry, fx, fy, hx, hy;
        if (held) s->w8 -= 48; else s->w8 += (RD_DRIFT - s->w8) / 2;                                      /* hold: spin backwards (up to 28 degrees per step); let go: the spin dies away and a nose-down drift (3 degrees per step) remains */
        if (s->w8 < -224) s->w8 = -224;
        if (s->w8 > 32) s->w8 = 32;
        s->ang8 += s->w8; s->rot8 += s->w8;
        s->vy += RD_G; if (s->vy > 224) s->vy = 224;
        s->bx += s->vx; s->by += s->vy;
        rd_pt(s, -RD_L, RD_D, &rx, &ry); rd_pt(s, RD_L, RD_D, &fx, &fy); rd_pt(s, -2, -19, &hx, &hy);
        if (s->air_t < 3) { s->air_t++; return; }                                                  /* just left the track: the rear wheel is still on the ramp */
        if (hy + 4 * 16 >= rd_ground(s, hx >> 4)) { rd_crash(s); return; }                       /* the rider's head met the track */
        if (ry + RD_RW * 16 >= rd_ground(s, rx >> 4) || fy + RD_RW * 16 >= rd_ground(s, fx >> 4)) {
            i32 cy, t8, rel, n;
            rd_pose(s, s->bx, &cy, &t8);
            rel = rd_norm8(s->ang8 - t8) / 8;
            if (rel > RD_LAND || rel < -RD_LAND) { rd_crash(s); return; }
            {   /* landed: the bike is set on the track and slides along it from here on, with the vertical speed of the slope (anything else lets the track fall away from it and throws it up again) */
                i32 cy2, t2; rd_pose(s, s->bx + s->vx, &cy2, &t2);
                s->vy = cy2 - cy; s->by = cy;
            }
            s->ang8 = t8 + rd_norm8(s->ang8 - t8); s->air = 0;
            n = ((s->rot8 < 0 ? -s->rot8 : s->rot8) + 480) / 2880;                     /* a turn counts from 300 degrees: nobody lands one exactly */
            if (n > 0) { s->bonus += 10 * n; rd_flip(s, n, s->rot8 < 0); motor_once(1, 1); }
        }
    }
}
static void rd_event(lv_event_t *e) {
    Rd *s = (Rd *)lv_event_get_user_data(e); u32 code = lv_event_get_code(e), now = lv_tick_get();
    if (code == EV_PRESSED) {
        s->held_until = now + 150u;
        if (s->state == RD_READY) { s->state = RD_RUN; s->last_t = now; s->vx = 72; s->vy = 0; s->air = 0; lv_obj_add_flag(s->ovl, FL_HIDDEN); }
        else if (s->state == RD_OVER && now - s->over_t > 700u) rd_reset(s);                       /* a short pause after a crash, so a late touch does not restart at once */
    } else if (code == RD_EV_PRESSING) s->held_until = now + 150u;                                   /* each press event is good for 150 ms: a missed release cannot leave it spinning */
    else if (code == RD_EV_RELEASED || code == RD_EV_PRESS_LOST || code == EV_CLICKED) s->held_until = 0;
}
static void rd_tick(lv_timer_t *t) {
    Rd *s = (Rd *)timer_user_data(t); u32 now = lv_tick_get(), n;
    if (s->flip_until && now > s->flip_until) { s->flip_until = 0; lv_obj_add_flag(s->l_flip, FL_HIDDEN); }
    if (s->state != RD_RUN) return;
    n = (now - s->last_t) / RD_TICK;
    if (!n) return;
    if (n > 3) { n = 3; s->last_t = now; } else s->last_t += n * RD_TICK;
    for (u32 k = 0; k < n && s->state == RD_RUN; k++) { rd_ahead(s); rd_step(s); }
    rd_draw(s); rd_render(s);
}
static void *rd_create(lv_obj_t *root) {
    Rd *s = (Rd *)app_malloc(sizeof(Rd)); if (!s) return 0;
    fill8(s, 0, sizeof(Rd)); s->rng = lv_tick_get() | 1u;
    RdFile f; if (blob_load(RD_PATH, &f, sizeof f) && f.magic == RD_MAGIC && f.best < 10000000u) s->best = f.best;
    s->l_score = mk_text(root, 10, 10, 130, ROBOTO_MEDIUM(10), C_MUTED, ALIGN_LEFT, "");
    s->l_best = mk_text(root, 140, 10, 116, ROBOTO_MEDIUM(10), C_MUTED, ALIGN_LEFT, "");
    s->board = mk_box(root, 0, RD_BY, 256, RD_BH, 0, RD_SKY); lv_obj_add_event_cb(s->board, rd_event, 0, s);
    for (int i = 0; i < RD_NCOL; i++) { s->col[i] = mk_box(s->board, 0, 0, RD_CW, 10, 0, RD_TER_A); lv_obj_clear_flag(s->col[i], FL_CLICKABLE); }
    for (int k = 0; k < RD_NP; k++) {
        u16 color = k < 2 ? RD_RGB(235, 240, 255) : k < 6 ? RD_RGB(255, 50, 160) : k < 8 ? RD_RGB(120, 240, 255) : RD_RGB(255, 225, 80);
        s->part[k] = mk_box(s->board, 0, 0, RD_PSZ[k], RD_PSZ[k], (k < 2 || k == 8) ? 100 : 2, color); lv_obj_clear_flag(s->part[k], FL_CLICKABLE);
    }
    s->l_flip = mk_text(root, 0, 76, 256, FT_SMALL, C_SERVE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_flip, FL_CLICKABLE); lv_obj_add_flag(s->l_flip, FL_HIDDEN);
    s->ovl = mk_box(root, 24, 130, 208, 124, 24, C_BLACK); set_bg_opa(s->ovl, 215); lv_obj_clear_flag(s->ovl, FL_CLICKABLE);
    s->l_msg = mk_text(s->ovl, 0, 4, 208, FT_BTN, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_msg, FL_CLICKABLE);
    s->l_sub = mk_text(s->ovl, 0, 40, 208, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub, FL_CLICKABLE);
    s->l_sub2 = mk_text(s->ovl, 0, 66, 208, FT_SMALL, C_MUTED, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_sub2, FL_CLICKABLE);
    s->l_hint = mk_text(s->ovl, 0, 92, 208, FT_SMALL, C_WHITE, ALIGN_CENTER, ""); lv_obj_clear_flag(s->l_hint, FL_CLICKABLE);
    s->timer = lv_timer_create(rd_tick, RD_TICK, s);
    rd_reset(s);
    return s;
}
static int rd_back(void *st) { (void)st; return 0; }
static void rd_destroy(void *st) { Rd *s = (Rd *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; rd_flush(s); app_free(s); }
APP_EXPORT(rider, "Rider", &ICON_RIDER, rd_create, rd_back, rd_destroy)
