/* SPDX-License-Identifier: Apache-2.0 */
/* apps/mines.c: Minesweeper for the 256 x 402 touch screen: 7 x 8 cells, 8 mines.
 *
 * Tap a cell to dig, hold it to flag (the other way round when the left pill says "Flag"). Tap a number whose flags add up to it to dig around it at once ("chord").
 * The first dig is always safe, and so are the cells around it. The header shows the mines left (mines minus flags) and the time; a win shows "Win", a mine "Boom", and
 * the idle screen shows your best time. The best time is kept in /user/mines.sav, written when a game is won (a few bytes, rarely).
 *
 * Cells are 32 px boxes (34 px apart) on the screen itself. A cell's number or flag is a label created the first time it is needed and hidden after, a mine is a small
 * round box, so the app never holds more than 2 x 56 + a few objects. A long press is followed by a click when the finger lifts: the click is swallowed.
 * No globals, no libc: everything lives in the block returned by create. All names are prefixed MN_ / mn_ because every app shares one translation unit when bundled. */
#include "ui.h"
#include "icons.h"

#define MN_PATH "/user/mines.sav"
#define MN_MAGIC 0x4d4e5331u                                    /* "MNS1" */
#define MN_W 7
#define MN_H 8
#define MN_N (MN_W * MN_H)
#define MN_MINES 8
#define MN_X0 10                                                /* 10 + 7 * 34 - 2 + 10 = 256; 44 + 8 * 34 - 2 = 314, the pills start at 344 */
#define MN_Y0 44
#define MN_PITCH 34
#define MN_SZ 32
#define MN_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define MN_GREEN MN_RGB(110, 220, 120)
enum { MN_READY, MN_RUN, MN_WON, MN_LOST };

typedef struct { u32 magic, best; } MnFile;
typedef struct {
    u32 rng, start_t, secs, best;
    u8 state, flag, lp, nrev, nflag, hit, pad[2];
    u8 mine[MN_N], adj[MN_N], st[MN_N], drawn[MN_N];                 /* st: 0 hidden, 1 dug, 2 flagged; drawn: what each cell shows now (see mn_vis) */
    lv_obj_t *cell[MN_N], *lab[MN_N], *dot[MN_N];
    lv_obj_t *l_mines, *l_time, *b_mode, *b_new, *l_mode;
    lv_timer_t *timer;
    char t_mines[TXT_MAX], t_time[TXT_MAX], t_mode[TXT_MAX];
} Mn;

static u32 mn_rand(Mn *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static u32 mn_elapsed(const Mn *s) { u32 t = (lv_tick_get() - s->start_t) / 1000u; return t > 5999u ? 5999u : t; }
/* the k-th (0..7) neighbour of cell c, or -1 off the board */
static int mn_nbr(int c, int k) {
    int d = k < 4 ? k : k + 1, x = c % MN_W + d % 3 - 1, y = c / MN_W + d / 3 - 1;
    return (x < 0 || y < 0 || x >= MN_W || y >= MN_H) ? -1 : y * MN_W + x;
}
static int mn_find(const Mn *s, const lv_obj_t *t) { for (int i = 0; i < MN_N; i++) if (s->cell[i] == t) return i; return -1; }
static u16 mn_numcol(int n) {
    switch (n) {
    case 1: return MN_RGB(110, 170, 255);
    case 2: return MN_GREEN;
    case 3: return MN_RGB(255, 110, 110);
    case 4: return MN_RGB(190, 130, 255);
    case 5: return MN_RGB(255, 170, 70);
    case 6: return MN_RGB(90, 220, 220);
    case 7: return C_WHITE;
    default: return MN_RGB(170, 170, 170);
    }
}
/* what a cell shows: 0 hidden, 1 flag, 2 dug and empty, 3..10 dug with 1..8, 11 a mine, 12 the mine that went off, 13 a flag on a cell without a mine */
static u8 mn_vis(const Mn *s, int c) {
    if (s->state == MN_LOST) {
        if (s->mine[c]) return c == s->hit ? 12 : (s->st[c] == 2 ? 1 : 11);
        if (s->st[c] == 2) return 13;
    }
    if (s->st[c] == 1) return (u8)(2 + s->adj[c]);
    return s->st[c] == 2 ? 1 : 0;
}
static void mn_paint(Mn *s, int c, u8 v) {
    lv_obj_t *b = s->cell[c]; const char *txt = 0; char t[2] = { 0, 0 }; u16 col = C_WHITE;
    set_bg_color(b, v <= 1 ? C_BTN : v == 12 ? C_WARN : C_CARD);
    if (v == 1) { txt = "F"; col = C_SERVE; }
    else if (v >= 3 && v <= 10) { t[0] = (char)('0' + v - 2); txt = t; col = mn_numcol(v - 2); }
    else if (v == 13) { txt = "X"; col = C_WARN; }
    if (txt) {
        if (!s->lab[c]) { s->lab[c] = mk_text(b, 0, 4, MN_SZ, FT_BTN, col, ALIGN_CENTER, txt); lv_obj_clear_flag(s->lab[c], FL_CLICKABLE); }
        else { lv_label_set_text(s->lab[c], txt); set_text_color(s->lab[c], col); lv_obj_clear_flag(s->lab[c], FL_HIDDEN); }
    } else if (s->lab[c]) lv_obj_add_flag(s->lab[c], FL_HIDDEN);
    if (v == 11 || v == 12) {
        if (!s->dot[c]) { s->dot[c] = mk_box(b, 9, 9, 14, 14, 100, C_BLACK); lv_obj_clear_flag(s->dot[c], FL_CLICKABLE); }
        set_bg_color(s->dot[c], v == 12 ? C_BLACK : C_MUTED); lv_obj_clear_flag(s->dot[c], FL_HIDDEN);
    } else if (s->dot[c]) lv_obj_add_flag(s->dot[c], FL_HIDDEN);
    s->drawn[c] = v;
}
static void mn_render(Mn *s) {
    char b[TXT_MAX], *e = put(b, "Mines "); putn(e, s->nflag >= MN_MINES ? 0u : (u32)(MN_MINES - s->nflag), 1); set_txt(s->l_mines, s->t_mines, b);
    if (s->state == MN_READY) { if (s->best) { e = put(b, "Best "); fmt_dur(e, s->best); } else put(b, "0:00"); }
    else if (s->state == MN_RUN) fmt_dur(b, mn_elapsed(s));
    else { e = put(b, s->state == MN_WON ? "Win " : "Boom "); fmt_dur(e, s->secs); }
    set_txt(s->l_time, s->t_time, b);
}
static void mn_sync(Mn *s) {                                    /* repaint the cells that changed, then the header */
    for (int c = 0; c < MN_N; c++) { u8 v = mn_vis(s, c); if (v != s->drawn[c]) mn_paint(s, c, v); }
    mn_render(s);
}
static void mn_mode(Mn *s) { set_txt(s->l_mode, s->t_mode, s->flag ? "Flag" : "Dig"); set_bg_color(s->b_mode, s->flag ? C_WARN : C_BTN); }
static void mn_new(Mn *s) {
    fill8(s->mine, 0, MN_N); fill8(s->adj, 0, MN_N); fill8(s->st, 0, MN_N); fill8(s->drawn, 255, MN_N);
    s->state = MN_READY; s->nrev = 0; s->nflag = 0; s->hit = 255; s->secs = 0; s->lp = 0;
    set_text_color(s->l_time, C_MUTED);
    mn_sync(s);
}
/* the mines go in at the first dig, never on that cell or next to it */
static void mn_place(Mn *s, int first) {
    int placed = 0;
    while (placed < MN_MINES) {
        int c = (int)(mn_rand(s) % MN_N), near = c == first;
        for (int k = 0; k < 8; k++) if (mn_nbr(first, k) == c) near = 1;
        if (near || s->mine[c]) continue;
        s->mine[c] = 1; placed++;
    }
    for (int c = 0; c < MN_N; c++) { int n = 0; for (int k = 0; k < 8; k++) { int m = mn_nbr(c, k); if (m >= 0 && s->mine[m]) n++; } s->adj[c] = (u8)n; }
}
/* dig a cell; an empty one spreads to its neighbours (flagged cells stay) */
static void mn_open(Mn *s, int c) {
    u8 stk[MN_N]; int sp = 0;
    if (s->st[c]) return;
    s->st[c] = 1; s->nrev++; stk[sp++] = (u8)c;
    while (sp) {
        int p = stk[--sp];
        if (s->adj[p]) continue;
        for (int k = 0; k < 8; k++) { int n = mn_nbr(p, k); if (n >= 0 && !s->st[n]) { s->st[n] = 1; s->nrev++; stk[sp++] = (u8)n; } }
    }
}
static void mn_lose(Mn *s, int hit) { s->hit = (u8)hit; s->secs = mn_elapsed(s); s->state = MN_LOST; set_text_color(s->l_time, C_WARN); motor_once(4, 1); }
static void mn_win(Mn *s) {
    s->state = MN_WON; s->secs = mn_elapsed(s); if (!s->secs) s->secs = 1;
    for (int c = 0; c < MN_N; c++) if (s->mine[c]) s->st[c] = 2;
    s->nflag = MN_MINES; set_text_color(s->l_time, MN_GREEN); motor_once(4, 1);
    if (!s->best || s->secs < s->best) { MnFile f; s->best = s->secs; f.magic = MN_MAGIC; f.best = s->best; blob_save(MN_PATH, &f, sizeof f); }
}
static void mn_flag(Mn *s, int c) {
    if (s->st[c] == 1) return;
    if (s->st[c] == 2) { s->st[c] = 0; s->nflag--; } else { s->st[c] = 2; s->nflag++; }
}
static void mn_dig(Mn *s, int c) {
    if (s->st[c] == 2) return;                                                    /* a flag protects its cell */
    if (s->state == MN_READY) { mn_place(s, c); s->state = MN_RUN; s->start_t = lv_tick_get(); }
    if (s->st[c] == 1) {                                                          /* chord: as many flags as the number says -> dig the rest around it */
        int f = 0, boom = -1;
        if (!s->adj[c]) return;
        for (int k = 0; k < 8; k++) { int n = mn_nbr(c, k); if (n >= 0 && s->st[n] == 2) f++; }
        if (f != s->adj[c]) return;
        for (int k = 0; k < 8; k++) { int n = mn_nbr(c, k); if (n >= 0 && s->st[n] == 0) { if (s->mine[n]) boom = n; else mn_open(s, n); } }
        if (boom >= 0) { mn_lose(s, boom); return; }
    } else {
        if (s->mine[c]) { mn_lose(s, c); return; }
        mn_open(s, c);
    }
    if (s->nrev == MN_N - MN_MINES) mn_win(s);
}
static void mn_act(Mn *s, int c, int flagging) {
    if (s->state == MN_WON || s->state == MN_LOST) return;
    if (flagging) mn_flag(s, c); else mn_dig(s, c);
    mn_sync(s);
}
static void mn_event(lv_event_t *e) {
    Mn *s = (Mn *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e); u32 code = lv_event_get_code(e);
    if (t == s->b_mode) { if (code == EV_CLICKED) { s->flag = (u8)!s->flag; mn_mode(s); motor_once(1, 1); } return; }
    if (t == s->b_new) { if (code == EV_CLICKED) { mn_new(s); motor_once(1, 1); } return; }
    int c = mn_find(s, t); if (c < 0) return;
    if (code == EV_PRESSED) s->lp = 0;
    else if (code == EV_LONG_PRESSED) { s->lp = 1; if (s->state < MN_WON) motor_once(1, 1); mn_act(s, c, !s->flag); }   /* hold = the other action */
    else if (code == EV_CLICKED) { if (s->lp) s->lp = 0; else mn_act(s, c, s->flag); }                               /* the click after a long press is swallowed */
}
static void mn_tick(lv_timer_t *t) { Mn *s = (Mn *)timer_user_data(t); if (s->state == MN_RUN) mn_render(s); }
static void *mn_create(lv_obj_t *root) {
    Mn *s = (Mn *)app_malloc(sizeof(Mn)); if (!s) return 0;
    fill8(s, 0, sizeof(Mn)); s->rng = lv_tick_get() | 1u;
    MnFile f; if (blob_load(MN_PATH, &f, sizeof f) && f.magic == MN_MAGIC && f.best > 0 && f.best < 6000u) s->best = f.best;
    s->l_mines = mk_text(root, 10, 8, 116, FT_SMALL, C_MUTED, ALIGN_LEFT, "");
    s->l_time = mk_text(root, 126, 8, 130, FT_SMALL, C_MUTED, ALIGN_LEFT, "");
    for (int i = 0; i < MN_N; i++) {
        s->cell[i] = mk_box(root, MN_X0 + (i % MN_W) * MN_PITCH, MN_Y0 + (i / MN_W) * MN_PITCH, MN_SZ, MN_SZ, 6, C_BTN);
        lv_obj_add_event_cb(s->cell[i], mn_event, 0, s);
    }
    s->b_mode = mk_pill(root, 6, 344, 118, "Dig", &s->l_mode, mn_event, s);
    s->b_new = mk_pill(root, 132, 344, 118, "New", 0, mn_event, s);
    s->timer = lv_timer_create(mn_tick, 250, s);
    mn_mode(s); mn_new(s);
    return s;
}
static int mn_back(void *st) { (void)st; return 0; }
static void mn_destroy(void *st) { Mn *s = (Mn *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; app_free(s); }
APP_EXPORT(mines, "Mines", &ICON_MINES, mn_create, mn_back, mn_destroy)
