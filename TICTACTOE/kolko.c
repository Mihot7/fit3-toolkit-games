/* SPDX-License-Identifier: Apache-2.0 */
/* apps/kolko.c: kolko i krzyzyk (tic-tac-toe) dla Galaxy Fit3.
 *
 * Tryby (przycisk po lewej): Latwy (zegarek czasem sie myli), Trudny (zegarek gra idealnie: minimax, nie da sie go pokonac), 2 osoby.
 * Ty grasz X, zegarek O. Kto zaczyna, zmienia sie co partie. Wynik (X / O / remisy) i tryb sa zapisane w /user/ttt.sav (12 bajtow, 3 s po zmianie i przy zamknieciu). */
#include "ui.h"

#ifndef RGB
#define RGB(r, g, b) ((u16)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#endif
#define TTT_PATH "/user/ttt.sav"
#define TTT_MAGIC 0x31545454u                                   /* "TTT1" */
#define C_WINCELL RGB(36, 110, 70)

typedef struct { u32 magic; u16 sc[3]; u16 mode; } TttFile;
typedef struct {
    u8 b[9], mode, turn, over, first, line, dirty, wait, pad;    /* b: 0 empty, 1 X, 2 O; turn: 1 X, 2 O; over: 0 playing, 1 X won, 2 O won, 3 draw */
    u16 sc[3];                                                    /* X wins, O wins, draws */
    u32 rng, dirty_t;
    lv_obj_t *cell[9], *mark[9], *l_title, *l_status, *l_mode, *b_mode, *b_new;
    lv_timer_t *timer;
    char t_title[TXT_MAX], t_status[TXT_MAX], t_mode[TXT_MAX], t_mark[9][TXT_MAX];
} Ttt;

static const u8 TTT_LINES[8][3] = { {0,1,2}, {3,4,5}, {6,7,8}, {0,3,6}, {1,4,7}, {2,5,8}, {0,4,8}, {2,4,6} };

static u32 ttt_rand(Ttt *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static int ttt_winner(const u8 *b, int *line) {
    for (int i = 0; i < 8; i++) {
        int a = TTT_LINES[i][0];
        if (b[a] && b[a] == b[TTT_LINES[i][1]] && b[a] == b[TTT_LINES[i][2]]) { if (line) *line = i; return b[a]; }
    }
    return 0;
}
/* minimax: the value of the position for `me`, `player` to move; faster wins and slower losses score better */
static int ttt_mm(u8 *b, int player, int me, int depth) {
    int w = ttt_winner(b, 0);
    if (w) return w == me ? 10 - depth : depth - 10;
    int full = 1, best = player == me ? -100 : 100;
    for (int i = 0; i < 9; i++) if (!b[i]) {
        full = 0; b[i] = (u8)player;
        int v = ttt_mm(b, 3 - player, me, depth + 1);
        b[i] = 0;
        if (player == me ? v > best : v < best) best = v;
    }
    return full ? 0 : best;
}
static int ttt_ai(Ttt *s) {
    int cand[9], n = 0, sc[9], best = -100;
    int hard = s->mode == 1 || ttt_rand(s) % 3u == 0;             /* easy: a random move two times in three */
    for (int i = 0; i < 9; i++) if (!s->b[i]) {
        sc[i] = 0;
        if (hard) { s->b[i] = 2; sc[i] = ttt_mm(s->b, 1, 2, 1); s->b[i] = 0; }
        if (sc[i] > best) best = sc[i];
    }
    for (int i = 0; i < 9; i++) if (!s->b[i] && sc[i] == best) cand[n++] = i;
    return n ? cand[ttt_rand(s) % (u32)n] : 0;
}

static void ttt_save(Ttt *s) { TttFile f; fill8(&f, 0, sizeof f); f.magic = TTT_MAGIC; mcopy(f.sc, s->sc, 6); f.mode = s->mode; blob_save(TTT_PATH, &f, sizeof f); }
static void ttt_flush(Ttt *s) { if (s->dirty) { s->dirty = 0; ttt_save(s); } }
static void ttt_changed(Ttt *s) { s->dirty = 1; s->dirty_t = lv_tick_get(); }

static void ttt_render(Ttt *s) {
    for (int i = 0; i < 9; i++) {
        set_txt(s->mark[i], s->t_mark[i], s->b[i] == 1 ? "X" : s->b[i] == 2 ? "O" : "");
        set_text_color(s->mark[i], s->b[i] == 1 ? C_A : C_B);
        int win = s->over && s->over < 3 && (i == TTT_LINES[s->line][0] || i == TTT_LINES[s->line][1] || i == TTT_LINES[s->line][2]);
        set_bg_color(s->cell[i], win ? C_WINCELL : C_CARD);
    }
    char b[TXT_MAX], *e = put(b, "X "); e = putn(e, s->sc[0], 1); e = put(e, "   O "); e = putn(e, s->sc[1], 1); e = put(e, "   R "); putn(e, s->sc[2], 1);
    set_txt(s->l_title, s->t_title, b);
    const char *st;
    if (s->over == 3) st = "Tie";
    else if (s->over) st = s->mode == 2 ? (s->over == 1 ? "X Won!" : "O Won!") : (s->over == 1 ? "You won!" : "CPU won!");
    else if (s->mode == 2) st = s->turn == 1 ? "X's turn" : "O's turn";
    else st = s->turn == 1 ? "Your turn" : "Thinking...";
    set_txt(s->l_status, s->t_status, st);
    set_txt(s->l_mode, s->t_mode, s->mode == 0 ? "Easy" : s->mode == 1 ? "Hard" : "2 Players");
}
static void ttt_new(Ttt *s) {
    fill8(s->b, 0, 9); s->over = 0; s->line = 0;
    s->first = (u8)(3 - s->first); s->turn = s->first;
    s->wait = (s->mode < 2 && s->turn == 2) ? 4 : 0;
}
static void ttt_place(Ttt *s, int i) {
    if (s->over || s->b[i]) return;
    s->b[i] = s->turn; motor_once(1, 1);
    int ln = 0, w = ttt_winner(s->b, &ln), full = 1;
    for (int k = 0; k < 9; k++) if (!s->b[k]) full = 0;
    if (w) { s->over = (u8)w; s->line = (u8)ln; if (s->sc[w - 1] < 999) s->sc[w - 1]++; ttt_changed(s); motor_once(4, 1); }
    else if (full) { s->over = 3; if (s->sc[2] < 999) s->sc[2]++; ttt_changed(s); motor_once(4, 1); }
    else { s->turn = (u8)(3 - s->turn); if (s->mode < 2 && s->turn == 2) s->wait = 4; }
}
static void ttt_click(lv_event_t *e) {
    Ttt *s = (Ttt *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED) return;
    if (t == s->b_new) { ttt_new(s); motor_once(1, 1); }
    else if (t == s->b_mode) { s->mode = (u8)((s->mode + 1) % 3); ttt_new(s); ttt_changed(s); motor_once(1, 1); }
    else if (!s->over && !(s->mode < 2 && s->turn == 2)) {
        for (int i = 0; i < 9; i++) if (t == s->cell[i]) { ttt_place(s, i); break; }
    }
    ttt_render(s);
}
static void ttt_tick(lv_timer_t *t) {
    Ttt *s = (Ttt *)timer_user_data(t);
    if (!s->over && s->mode < 2 && s->turn == 2) {
        if (s->wait) s->wait--; else { ttt_place(s, ttt_ai(s)); ttt_render(s); }
    }
    if (s->dirty && lv_tick_get() - s->dirty_t > 3000) ttt_flush(s);
}
static void *ttt_create(lv_obj_t *root) {
    Ttt *s = (Ttt *)app_malloc(sizeof(Ttt)); if (!s) return 0;
    fill8(s, 0, sizeof(Ttt)); s->rng = lv_tick_get() | 1u; s->first = 2;
    TttFile f; if (blob_load(TTT_PATH, &f, sizeof f) && f.magic == TTT_MAGIC && f.mode < 3) { mcopy(s->sc, f.sc, 6); s->mode = (u8)f.mode; }
    s->l_title = mk_text(root, 0, 8, 256, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    for (int i = 0; i < 9; i++) {
        s->cell[i] = mk_box(root, 7 + (i % 3) * 82, 40 + (i / 3) * 82, 78, 78, 18, C_CARD);
        lv_obj_add_event_cb(s->cell[i], ttt_click, 0, s);
        s->mark[i] = mk_text(s->cell[i], 0, 19, 78, FT_POINT, C_WHITE, ALIGN_CENTER, "");
    }
    s->l_status = mk_text(root, 0, 298, 256, FT_BTN, C_WHITE, ALIGN_CENTER, "");
    s->b_mode = mk_pill(root, 6, 344, 118, "", &s->l_mode, ttt_click, s);
    s->b_new = mk_pill(root, 132, 344, 118, "New", 0, ttt_click, s);
    ttt_new(s);
    s->timer = lv_timer_create(ttt_tick, 100, s);
    ttt_render(s);
    return s;
}
static int ttt_back(void *st) { (void)st; return 0; }
static void ttt_destroy(void *st) { Ttt *s = (Ttt *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; ttt_flush(s); app_free(s); }
APP_EXPORT(kolko, "Tic tac toe", 0, ttt_create, ttt_back, ttt_destroy)
