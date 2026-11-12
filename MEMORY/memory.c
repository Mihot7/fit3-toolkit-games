/* SPDX-License-Identifier: Apache-2.0 */
/* apps/memory.c: gra Memory (pary). Odkrywasz dwie karty; pasujace (ten sam kolor i litera) zostaja, niepasujace chowaja sie po ok. 0,9 s (albo od razu po kolejnym dotknieciu).
 * Przycisk po lewej zmienia plansze: 4x3, 4x4, 4x5. Rekord (najmniej ruchow) dla kazdej planszy zapisuje /user/memory.sav (8 bajtow). */
#include "ui.h"

#ifndef RGB
#define RGB(r, g, b) ((u16)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#endif
#define MEM_PATH "/user/memory.sav"
#define MEM_MAGIC 0x314d454du                                   /* "MEM1" */
#define MEM_MAXN 20
#define MEM_NONE 0xff

typedef struct { u32 magic; u8 best[3], pad; } MemFile;
typedef struct {
    u8 face[MEM_MAXN], st[MEM_MAXN];                             /* st: 0 face down, 1 face up, 2 matched */
    u8 mode, n, sel, busy, matched, moves, won, dirty, p0, p1, best[3];
    u32 rng, busy_t, dirty_t;
    lv_obj_t *card[MEM_MAXN], *lab[MEM_MAXN], *l_title, *l_size, *b_size, *b_new;
    lv_timer_t *timer;
    char t_title[TXT_MAX], t_size[TXT_MAX], t_lab[MEM_MAXN][TXT_MAX];
} Mem;

static const u8 MEM_ROWS[3] = { 3, 4, 5 };
static const u16 MEM_COL[10] = { RGB(230, 70, 70), RGB(250, 150, 40), RGB(240, 220, 60), RGB(80, 200, 90), RGB(60, 200, 190),
                                 RGB(80, 140, 250), RGB(160, 100, 230), RGB(240, 110, 190), RGB(170, 110, 60), RGB(210, 210, 210) };
static const char *const MEM_CH[10] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J" };

static u32 mem_rand(Mem *s) { u32 x = s->rng ^ lv_tick_get(); x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x ? x : 0x9e3779b9u; return s->rng; }
static void mem_save(Mem *s) { MemFile f; fill8(&f, 0, sizeof f); f.magic = MEM_MAGIC; mcopy(f.best, s->best, 3); blob_save(MEM_PATH, &f, sizeof f); }
static void mem_flush(Mem *s) { if (s->dirty) { s->dirty = 0; mem_save(s); } }

static void mem_layout(Mem *s) {
    int rows = MEM_ROWS[s->mode], h = (286 - (rows - 1) * 4) / rows;
    s->n = (u8)(rows * 4);
    for (int i = 0; i < MEM_MAXN; i++) {
        if (i >= s->n) { lv_obj_add_flag(s->card[i], FL_HIDDEN); continue; }
        lv_obj_clear_flag(s->card[i], FL_HIDDEN);
        lv_obj_set_size(s->card[i], 58, h); lv_obj_set_pos(s->card[i], 6 + (i % 4) * 62, 38 + (i / 4) * (h + 4));
        lv_obj_set_pos(s->lab[i], 0, h / 2 - 20);
    }
}
static void mem_render(Mem *s) {
    for (int i = 0; i < s->n; i++) {
        int f = s->face[i], st = s->st[i];
        set_txt(s->lab[i], s->t_lab[i], st ? MEM_CH[f] : "?");
        set_bg_color(s->card[i], st ? MEM_COL[f] : C_BTN);
        set_bg_opa(s->card[i], st == 2 ? 110 : 255);
        set_text_color(s->lab[i], st == 1 ? C_BLACK : C_WHITE);
    }
    char b[TXT_MAX], *e;
    if (s->won) { e = put(b, "Congrats! "); e = putn(e, s->moves, 1); put(e, " moves"); }
    else {
        e = put(b, "Moves "); e = putn(e, s->moves, 1); e = put(e, "   Best ");
        if (s->best[s->mode]) putn(e, s->best[s->mode], 1); else put(e, "-");
    }
    set_txt(s->l_title, s->t_title, b);
    set_txt(s->l_size, s->t_size, s->mode == 0 ? "4x3" : s->mode == 1 ? "4x4" : "4x5");
}
static void mem_deal(Mem *s) {
    int n = s->n;
    for (int i = 0; i < n; i++) { s->face[i] = (u8)(i / 2); s->st[i] = 0; }
    for (int i = n - 1; i > 0; i--) { int j = (int)(mem_rand(s) % (u32)(i + 1)); u8 t = s->face[i]; s->face[i] = s->face[j]; s->face[j] = t; }
    s->sel = MEM_NONE; s->busy = 0; s->matched = 0; s->moves = 0; s->won = 0;
}
static void mem_unflip(Mem *s) { if (s->busy) { s->st[s->p0] = 0; s->st[s->p1] = 0; s->busy = 0; } }
static void mem_flip(Mem *s, int i) {
    if (s->won) return;
    mem_unflip(s);                                               /* a new tap turns the wrong pair back at once */
    if (s->st[i]) return;
    s->st[i] = 1; motor_once(1, 1);
    if (s->sel == MEM_NONE) { s->sel = (u8)i; return; }
    if (s->moves < 99) s->moves++;
    if (s->face[s->sel] == s->face[i]) {
        s->st[s->sel] = 2; s->st[i] = 2; s->matched = (u8)(s->matched + 2); s->sel = MEM_NONE;
        if (s->matched >= s->n) {
            s->won = 1; motor_once(4, 1);
            if (!s->best[s->mode] || s->moves < s->best[s->mode]) { s->best[s->mode] = s->moves; s->dirty = 1; s->dirty_t = lv_tick_get(); }
        }
    } else { s->p0 = s->sel; s->p1 = (u8)i; s->busy = 1; s->busy_t = lv_tick_get(); s->sel = MEM_NONE; }
}
static void mem_click(lv_event_t *e) {
    Mem *s = (Mem *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED) return;
    if (t == s->b_new) { mem_deal(s); motor_once(1, 1); }
    else if (t == s->b_size) { s->mode = (u8)((s->mode + 1) % 3); mem_layout(s); mem_deal(s); motor_once(1, 1); }
    else for (int i = 0; i < s->n; i++) if (t == s->card[i]) { mem_flip(s, i); break; }
    mem_render(s);
}
static void mem_tick(lv_timer_t *t) {
    Mem *s = (Mem *)timer_user_data(t);
    if (s->busy && lv_tick_get() - s->busy_t > 900) { mem_unflip(s); mem_render(s); }
    if (s->dirty && lv_tick_get() - s->dirty_t > 3000) mem_flush(s);
}
static void *mem_create(lv_obj_t *root) {
    Mem *s = (Mem *)app_malloc(sizeof(Mem)); if (!s) return 0;
    fill8(s, 0, sizeof(Mem)); s->rng = lv_tick_get() | 1u; s->mode = 1;
    MemFile f; if (blob_load(MEM_PATH, &f, sizeof f) && f.magic == MEM_MAGIC) mcopy(s->best, f.best, 3);
    s->l_title = mk_text(root, 0, 8, 256, FT_SMALL, C_MUTED, ALIGN_CENTER, "");
    for (int i = 0; i < MEM_MAXN; i++) {
        s->card[i] = mk_box(root, 0, 0, 58, 60, 14, C_BTN); lv_obj_add_event_cb(s->card[i], mem_click, 0, s);
        s->lab[i] = mk_text(s->card[i], 0, 0, 58, FT_POINT, C_WHITE, ALIGN_CENTER, "");
    }
    s->b_size = mk_pill(root, 6, 344, 118, "", &s->l_size, mem_click, s);
    s->b_new = mk_pill(root, 132, 344, 118, "New", 0, mem_click, s);
    mem_layout(s); mem_deal(s);
    s->timer = lv_timer_create(mem_tick, 100, s);
    mem_render(s);
    return s;
}
static int mem_back(void *st) { (void)st; return 0; }
static void mem_destroy(void *st) { Mem *s = (Mem *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; mem_flush(s); app_free(s); }
APP_EXPORT(memory, "Memory", 0, mem_create, mem_back, mem_destroy)
