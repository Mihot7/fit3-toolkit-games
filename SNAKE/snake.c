/* SPDX-License-Identifier: Apache-2.0 */
/* apps/snake.c: the classic Snake for the 256 x 402 touch screen.
 *
 * Steering is relative to the snake: tap "Left" to turn left, "Right" to turn right (two big pills under the board, so the swipe-back gesture is never in the way).
 * The snake dies on a wall or on itself; it speeds up with every apple. Reaching MAX_LEN segments wins. The best score is kept in /user/snake.sav, written
 * when a run ends and when the app is closed (a few bytes, rarely).
 *
 * Drawing: a 12 x 14 board of 20 px cells. The body is a ring of boxes: a step moves the tail box to the new head position (nothing is created or deleted while
 * playing, except one new box per apple, up to MAX_LEN). No globals, no libc: everything lives in the block returned by create. */
#include "ui.h"
#include "icons.h"

#define SNAKE_PATH "/user/snake.sav"
#define SNAKE_MAGIC 0x534e4b31u                                 /* "SNK1" */
#define GW 12                                                   /* board size in cells */
#define GH 14
#define CELL 20
#define BX 8                                                    /* board position: 8 + 12 * 20 + 8 = 256; 46 + 14 * 20 = 326, the pills start at 344 */
#define BY 46
#define START_LEN 3
#ifndef MAX_LEN
#define MAX_LEN 64                                              /* the most boxes the app ever creates; reaching it wins */
#endif
#define RING (MAX_LEN + 2)
#define RGB565(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BODY RGB565(60, 190, 90)
#define C_HEAD RGB565(150, 240, 110)
enum { ST_READY, ST_RUN, ST_OVER };

typedef struct { u32 magic, best; } SnakeFile;
typedef struct {
    u32 rng, last_t, over_t, best;
    u8 state, dir, last_dir, head, tail, len, fx, fy, dirty, npool, pad[2];
    u8 px[RING], py[RING], occ[GW * GH];
    lv_obj_t *seg[RING], *pool[RING];
    lv_obj_t *board, *food, *ovl, *l_msg, *l_sub, *l_hint, *l_score, *b_left, *b_right;
    lv_timer_t *timer;
    char t_score[TXT_MAX], t_msg[TXT_MAX], t_sub[TXT_MAX], t_hint[TXT_MAX];
} Snake;

static u32 snake_rand(Snake *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static void snake_put(lv_obj_t *o, int x, int y) { lv_obj_set_pos(o, x * CELL + 1, y * CELL + 1); }
static u32 snake_score(const Snake *s) { return s->len > START_LEN ? (u32)(s->len - START_LEN) : 0; }
static u32 snake_interval(const Snake *s) { u32 d = 5u * snake_score(s); return d > 150u ? 110u : 260u - d; }   /* ms per step: 260 down to 110 */

static void snake_flush(Snake *s) {
    SnakeFile f;
    if (!s->dirty) return;
    s->dirty = 0; f.magic = SNAKE_MAGIC; f.best = s->best; blob_save(SNAKE_PATH, &f, sizeof f);
}
static void snake_render(Snake *s) {
    char b[TXT_MAX], *e = put(b, "Score "); e = putn(e, snake_score(s), 1); e = put(e, "  Best "); putn(e, s->best, 1);
    set_txt(s->l_score, s->t_score, b);
}
static void snake_overlay(Snake *s, const char *msg, const char *sub, const char *hint) {
    set_txt(s->l_msg, s->t_msg, msg); set_txt(s->l_sub, s->t_sub, sub); set_txt(s->l_hint, s->t_hint, hint);
    lv_obj_clear_flag(s->ovl, FL_HIDDEN);
}
static lv_obj_t *snake_new_seg(Snake *s) {
    if (s->npool) return s->pool[--s->npool];
    lv_obj_t *o = mk_box(s->board, 0, 0, CELL - 2, CELL - 2, 6, C_BODY);
    lv_obj_clear_flag(o, FL_CLICKABLE);
    return o;
}
/* an apple on a random free cell (random tries first, then a scan: the board is never full below MAX_LEN) */
static void snake_food(Snake *s) {
    int idx = -1;
    for (int k = 0; k < 40 && idx < 0; k++) { u32 c = snake_rand(s) % (u32)(GW * GH); if (!s->occ[c]) idx = (int)c; }
    if (idx < 0) {
        u32 c0 = snake_rand(s) % (u32)(GW * GH);
        for (u32 k = 0; k < (u32)(GW * GH) && idx < 0; k++) { u32 c = (c0 + k) % (u32)(GW * GH); if (!s->occ[c]) idx = (int)c; }
    }
    if (idx < 0) return;
    s->fx = (u8)((u32)idx % GW); s->fy = (u8)((u32)idx / GW);
    snake_put(s->food, s->fx, s->fy); lv_obj_clear_flag(s->food, FL_HIDDEN);
}
static void snake_start(Snake *s) {
    for (u32 i = 0; i < s->len; i++) {                          /* the boxes of the last run go back to the pool */
        lv_obj_t *o = s->seg[(s->tail + i) % RING];
        lv_obj_add_flag(o, FL_HIDDEN); s->pool[s->npool++] = o;
    }
    fill8(s->occ, 0, sizeof s->occ);
    for (u32 i = 0; i < START_LEN; i++) {
        lv_obj_t *o = snake_new_seg(s);
        s->seg[i] = o; s->px[i] = (u8)(3 + i); s->py[i] = GH / 2; s->occ[(GH / 2) * GW + 3 + i] = 1;
        snake_put(o, 3 + (int)i, GH / 2); set_bg_color(o, i == START_LEN - 1 ? C_HEAD : C_BODY); lv_obj_clear_flag(o, FL_HIDDEN);
    }
    s->len = START_LEN; s->head = START_LEN - 1; s->tail = 0; s->dir = 0; s->last_dir = 0;   /* heading right */
    snake_food(s);
    lv_obj_add_flag(s->ovl, FL_HIDDEN);
    s->state = ST_RUN; s->last_t = lv_tick_get();
    snake_render(s);
}
static void snake_over(Snake *s, int win) {
    char b[TXT_MAX], *e;
    s->state = ST_OVER; s->over_t = lv_tick_get(); motor_once(4, 1);
    snake_flush(s);
    e = put(b, "Score "); putn(e, snake_score(s), 1);
    snake_overlay(s, win ? "You win!" : "Game over", b, "Tap to retry");
}
static void snake_step(Snake *s) {
    if (s->dir == ((s->last_dir + 2) & 3)) s->dir = s->last_dir;       /* two quick taps must not turn the snake back into its neck */
    s->last_dir = s->dir;
    int nx = s->px[s->head] + (s->dir == 0) - (s->dir == 2), ny = s->py[s->head] + (s->dir == 1) - (s->dir == 3);
    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) { snake_over(s, 0); return; }
    int eat = nx == s->fx && ny == s->fy;
    if (!eat) s->occ[s->py[s->tail] * GW + s->px[s->tail]] = 0;        /* the tail leaves its cell first: the head may enter it */
    if (s->occ[ny * GW + nx]) { snake_over(s, 0); return; }
    lv_obj_t *o;
    if (eat) { o = snake_new_seg(s); s->len++; }
    else { o = s->seg[s->tail]; s->tail = (u8)((s->tail + 1) % RING); }
    set_bg_color(s->seg[s->head], C_BODY);
    s->head = (u8)((s->head + 1) % RING);
    s->seg[s->head] = o; s->px[s->head] = (u8)nx; s->py[s->head] = (u8)ny; s->occ[ny * GW + nx] = 1;
    snake_put(o, nx, ny); set_bg_color(o, C_HEAD); lv_obj_clear_flag(o, FL_HIDDEN);
    if (eat) {
        if (snake_score(s) > s->best) { s->best = snake_score(s); s->dirty = 1; }
        snake_render(s); motor_once(1, 1);
        if (s->len >= MAX_LEN) { snake_over(s, 1); return; }
        snake_food(s);
    }
}
static void snake_click(lv_event_t *e) {
    Snake *s = (Snake *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED) return;
    if (s->state == ST_RUN) s->dir = (u8)((s->dir + (t == s->b_right ? 1 : 3)) & 3);   /* right = clockwise, left = counter-clockwise */
    else if (s->state == ST_READY || lv_tick_get() - s->over_t > 700) snake_start(s);   /* a short pause after dying, so a late tap does not restart at once */
}
static void snake_tick(lv_timer_t *t) {
    Snake *s = (Snake *)timer_user_data(t);
    if (s->state == ST_RUN && lv_tick_get() - s->last_t >= snake_interval(s)) { s->last_t = lv_tick_get(); snake_step(s); }
}
static void *snake_create(lv_obj_t *root) {
    Snake *s = (Snake *)app_malloc(sizeof(Snake)); if (!s) return 0;
    fill8(s, 0, sizeof(Snake)); s->rng = lv_tick_get() | 1u;
    SnakeFile f; if (blob_load(SNAKE_PATH, &f, sizeof f) && f.magic == SNAKE_MAGIC && f.best < MAX_LEN) s->best = f.best;
    s->l_score = mk_text(root, 0, 8, 256, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    s->board = mk_box(root, BX, BY, GW * CELL, GH * CELL, 8, C_CARD); lv_obj_clear_flag(s->board, FL_CLICKABLE);
    s->food = mk_box(s->board, 0, 0, CELL - 2, CELL - 2, 100, C_B); lv_obj_clear_flag(s->food, FL_CLICKABLE); lv_obj_add_flag(s->food, FL_HIDDEN);
    s->ovl = mk_box(root, 12, 120, 232, 140, 24, C_BLACK); set_bg_opa(s->ovl, 215); lv_obj_clear_flag(s->ovl, FL_CLICKABLE);
    s->l_msg = mk_text(s->ovl, 0, 14, 232, FT_BTN, C_WHITE, ALIGN_CENTER, "");
    s->l_sub = mk_text(s->ovl, 0, 58, 232, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    s->l_hint = mk_text(s->ovl, 0, 92, 232, FT_SMALL, C_WHITE, ALIGN_CENTER, "");
    s->b_left = mk_pill(root, 6, 344, 118, "Left", 0, snake_click, s);
    s->b_right = mk_pill(root, 132, 344, 118, "Right", 0, snake_click, s);
    s->timer = lv_timer_create(snake_tick, 30, s);
    snake_render(s); snake_overlay(s, "Snake", "Left / Right turn", "Tap to start");
    return s;
}
static int snake_back(void *st) { (void)st; return 0; }
static void snake_destroy(void *st) { Snake *s = (Snake *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; snake_flush(s); app_free(s); }
APP_EXPORT(snake, "Snake", &ICON_SNAKE, snake_create, snake_back, snake_destroy)
