/* SPDX-License-Identifier: Apache-2.0 */
/* apps/mahjong.c: pasjans Mahjong (Mahjong Solitaire) na maly ekran: 44 plytki w dwoch warstwach (6x6 na dole + 4x2 na gorze), 11 rysunkow po 4 plytki.
 * Dotknij dwoch WOLNYCH plytek z tym samym znakiem, a znikna. Plytka jest wolna, gdy nic na niej nie lezy i ma puste miejsce po lewej albo po prawej (szare = zablokowana).
 * Rozdanie jest zawsze rozwiazywalne: plytki sa rozkladane "od konca" (kolejnosc usuwania par jest wybierana z gory). "Wskaz" podswietla pare; gdy nie ma ruchow, tasuje reszte plytek
 * (znowu rozwiazywalnie). Nic nie jest zapisywane. */
#include "ui.h"

#ifndef RGB
#define RGB(r, g, b) ((u16)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#endif
#define MJ_N 44
#define MJ_NF 11
#define MJ_NONE 0xff
#define C_IVORY RGB(240, 235, 215)
#define C_BLOCKED RGB(120, 118, 112)
#define C_SELECT RGB(255, 215, 60)
#define C_HINT RGB(120, 220, 130)

typedef struct {
    u8 face[MJ_N], alive[MJ_N];
    u8 sel, h0, h1, left, msg, pad[3];                           /* msg: 0 none, 1 no moves, 2 reshuffled */
    u32 rng;
    lv_obj_t *tile[MJ_N], *lab[MJ_N], *l_title, *b_hint, *b_new;
    char t_title[TXT_MAX];
} Mj;

static const char *const MJ_CH[MJ_NF] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B" };
static const u16 MJ_COL[MJ_NF] = { RGB(200, 40, 40), RGB(30, 130, 60), RGB(40, 80, 200), RGB(210, 110, 20), RGB(130, 50, 170), RGB(20, 130, 140),
                                   RGB(120, 70, 30), RGB(190, 40, 120), RGB(100, 110, 20), RGB(60, 60, 80), RGB(0, 0, 0) };

static u32 mj_rand(Mj *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
/* the layout: layer 0 is a 6 x 6 block (index row * 6 + col), layer 1 is 4 x 2 on top of its middle (index 36 + (row - 2) * 4 + (col - 1)) */
static int mj_idx(int l, int c, int r) {
    if (l == 0) return (c >= 0 && c < 6 && r >= 0 && r < 6) ? r * 6 + c : -1;
    return (c >= 1 && c <= 4 && r >= 2 && r <= 3) ? 36 + (r - 2) * 4 + (c - 1) : -1;
}
static int mj_live(const u8 *alive, int l, int c, int r) { int i = mj_idx(l, c, r); return i >= 0 && alive[i]; }
static void mj_cell(int i, int *l, int *c, int *r) {
    if (i >= 36) { *l = 1; *c = 1 + (i - 36) % 4; *r = 2 + (i - 36) / 4; } else { *l = 0; *c = i % 6; *r = i / 6; }
}
static int mj_free(const u8 *alive, int i) {
    int l, c, r; mj_cell(i, &l, &c, &r);
    if (l == 0 && mj_live(alive, 1, c, r)) return 0;             /* covered */
    return !mj_live(alive, l, c - 1, r) || !mj_live(alive, l, c + 1, r);
}
/* give faces to the tiles that are alive, in pairs, by taking the tiles away in a random legal order: whatever comes out can be cleared in exactly that order */
static int mj_gen(Mj *s) {
    u8 tmp[MJ_N];
    for (int att = 0; att < 100; att++) {
        mcopy(tmp, s->alive, MJ_N);
        int rem = 0, ok = 1, p = (int)(mj_rand(s) % MJ_NF);
        for (int i = 0; i < MJ_N; i++) rem += tmp[i];
        while (rem >= 2) {
            int fr[MJ_N], nf = 0;
            for (int i = 0; i < MJ_N; i++) if (tmp[i] && mj_free(tmp, i)) fr[nf++] = i;
            if (nf < 2) { ok = 0; break; }
            int a = (int)(mj_rand(s) % (u32)nf), b = (int)(mj_rand(s) % (u32)(nf - 1));
            if (b >= a) b++;
            s->face[fr[a]] = s->face[fr[b]] = (u8)(p % MJ_NF); p++;
            tmp[fr[a]] = 0; tmp[fr[b]] = 0; rem -= 2;
        }
        if (ok) return 1;
    }
    return 0;
}
static int mj_find(const Mj *s, int *a, int *b) {
    int fr[MJ_N], nf = 0;
    for (int i = 0; i < MJ_N; i++) if (s->alive[i] && mj_free(s->alive, i)) fr[nf++] = i;
    for (int i = 0; i < nf; i++) for (int j = i + 1; j < nf; j++) if (s->face[fr[i]] == s->face[fr[j]]) { *a = fr[i]; *b = fr[j]; return 1; }
    return 0;
}
static void mj_labels(Mj *s) {
    for (int i = 0; i < MJ_N; i++) if (s->alive[i]) { lv_label_set_text(s->lab[i], MJ_CH[s->face[i]]); set_text_color(s->lab[i], MJ_COL[s->face[i]]); }
}
static void mj_render(Mj *s) {
    int left = 0;
    for (int i = 0; i < MJ_N; i++) {
        if (!s->alive[i]) continue;
        left++;
        u16 c = mj_free(s->alive, i) ? C_IVORY : C_BLOCKED;
        if (i == s->sel) c = C_SELECT;
        if (i == s->h0 || i == s->h1) c = C_HINT;
        set_bg_color(s->tile[i], c);
    }
    s->left = (u8)left;
    char b[TXT_MAX], *e;
    if (!left) put(b, "You won!");
    else if (s->msg == 1) put(b, "No moves! Hint");
    else if (s->msg == 2) put(b, "Relocated");
    else { e = put(b, "Tiles "); putn(e, (u32)left, 1); }
    set_txt(s->l_title, s->t_title, b);
}
static void mj_new(Mj *s) {
    for (int i = 0; i < MJ_N; i++) { s->alive[i] = 1; lv_obj_clear_flag(s->tile[i], FL_HIDDEN); }
    mj_gen(s); mj_labels(s);
    s->sel = s->h0 = s->h1 = MJ_NONE; s->msg = 0;
}
static void mj_take(Mj *s, int i) { s->alive[i] = 0; lv_obj_add_flag(s->tile[i], FL_HIDDEN); }
static void mj_tap(Mj *s, int i) {
    if (!mj_free(s->alive, i)) return;                           /* blocked */
    s->h0 = s->h1 = MJ_NONE; s->msg = 0;
    if (s->sel == MJ_NONE) { s->sel = (u8)i; motor_once(1, 1); }
    else if (s->sel == i) s->sel = MJ_NONE;
    else if (s->face[s->sel] == s->face[i]) {
        mj_take(s, s->sel); mj_take(s, i); s->sel = MJ_NONE;
        int left = 0, a, b; for (int k = 0; k < MJ_N; k++) left += s->alive[k];
        if (!left) motor_once(4, 1); else { motor_once(1, 1); if (!mj_find(s, &a, &b)) s->msg = 1; }
    } else { s->sel = (u8)i; motor_once(1, 1); }
}
static void mj_hint(Mj *s) {
    int a, b; s->sel = MJ_NONE;
    if (mj_find(s, &a, &b)) { s->h0 = (u8)a; s->h1 = (u8)b; s->msg = 0; }
    else if (s->left) { mj_gen(s); mj_labels(s); s->msg = 2; s->h0 = s->h1 = MJ_NONE; }
    motor_once(1, 1);
}
static void mj_click(lv_event_t *e) {
    Mj *s = (Mj *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED) return;
    if (t == s->b_new) { mj_new(s); motor_once(1, 1); }
    else if (t == s->b_hint) mj_hint(s);
    else for (int i = 0; i < MJ_N; i++) if (s->alive[i] && t == s->tile[i]) { mj_tap(s, i); break; }
    mj_render(s);
}
static void *mj_create(lv_obj_t *root) {
    Mj *s = (Mj *)app_malloc(sizeof(Mj)); if (!s) return 0;
    fill8(s, 0, sizeof(Mj)); s->rng = lv_tick_get() | 1u;
    s->l_title = mk_text(root, 0, 8, 256, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    for (int i = 0; i < MJ_N; i++) {                             /* layer 0 first, so layer 1 is drawn (and tapped) on top */
        int l, c, r; mj_cell(i, &l, &c, &r);
        s->tile[i] = mk_box(root, 8 + c * 40 + l * 3, 36 + r * 46 - l * 3, 38, 44, 8, C_IVORY);
        lv_obj_add_event_cb(s->tile[i], mj_click, 0, s);
        s->lab[i] = mk_text(s->tile[i], 0, 7, 38, FT_NAME, C_BLACK, ALIGN_CENTER, "1");
    }
    s->b_hint = mk_pill(root, 6, 344, 118, "Hint", 0, mj_click, s);
    s->b_new = mk_pill(root, 132, 344, 118, "New", 0, mj_click, s);
    mj_new(s); mj_render(s);
    return s;
}
static int mj_back(void *st) { (void)st; return 0; }
static void mj_destroy(void *st) { app_free(st); }
APP_EXPORT(mahjong, "Mahjong", 0, mj_create, mj_back, mj_destroy)
