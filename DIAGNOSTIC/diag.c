/* SPDX-License-Identifier: Apache-2.0 */
/* apps/diag.c: "Watch Diag", diagnostics that live ONLY on the watch: nothing to run on a PC, nothing to pull over Bluetooth. Everything is read on the screen.
 *
 *   File API       six read-only steps, one per tap of Next: opendir, readdir (raw entry bytes), dir_exists, stat (which words the firmware wrote), read vs stat size, a heap probe
 *   Memory         the largest block and the number of 4 kB blocks app_malloc gives (kept small on purpose: the heap is shared with the firmware)
 *   Timer          how steadily a 50 ms LVGL timer fires over 5 s (count, average, shortest, longest interval)
 *   Touch events   an event spy: every LVGL event code that reaches the box (pressed, clicked, long press, gesture + its direction code ...), counted live
 *   Vibration      plays motor patterns 0..79 one after another
 *   Saved log      the report of the PREVIOUS session (kept in /user/diag.log) or of this one: the place to read what happened before a restart
 *
 * Crash breadcrumb: before each risky call the app writes 16 bytes to /user/diag.brk ("test, step, started"), 3 s after it finishes "done". If the watch restarts inside a call, the next start
 * shows "! crash: <test> <step>" in red at the top, adds it to the log and skips that File API step. Results are saved to /user/diag.log in 256-byte pieces 3 s apart (the SDK warns about
 * big or frequent writes). Nothing deletes or changes any other file. No globals, no libc (docs/f3x.md). Taps only set a flag; a 50 ms timer does the work. */
#include "ui.h"

#ifndef fs_opendir
#define fs_opendir(p)       F(0x2c143428, i32, (const char *))(p)
#define fs_closedir(h)      F(0x2c143530, u32, (i32))(h)
#define fs_readdir(h, pe)   F(0x2c143680, i32, (i32, void **))(h, pe)
#define fs_dir_exists(p)    F(0x2c143816, u32, (const char *))(p)
#define fs_stat(p, b)       F(0x2c1432a8, i32, (const char *, void *))(p, b)
#endif
#ifndef DG_PTR_OK
#define DG_PTR_OK(p)        ((u32)(p) >= 0x20000000u && (u32)(p) < 0x40000000u)
#endif
#define FT_TINY ROBOTO_MEDIUM(10)

#define DG_BRK "/user/diag.brk"
#define DG_LOG "/user/diag.log"
#define DG_MAGIC 0x31424744u                                     /* "DGB1" */
#define DG_REP 768
#define DG_OUT 448
#define DG_CHUNK 256
#define DG_FSTEPS 6
enum { T_NONE, T_FILES, T_MEM, T_TIMER, T_TOUCH, T_VIB, T_LOG, T_COUNT = T_LOG };
enum { ACT_NONE, ACT_OPEN, ACT_RUN, ACT_BACK };
typedef struct { u32 magic, test, step, phase; } DgBrk;          /* phase 1 = started, 2 = done, 3 = a crash was reported */
typedef struct {
    char rep[DG_REP], prev[DG_REP], out[DG_OUT], file[80], t_run[TXT_MAX];
    u32 sb[128];                                                 /* a stat buffer with room to spare */
    void *blk[8];
    u16 ev[16]; u32 gdir, gn;
    i32 cur, fstep, vib, act, arg, nrep, nsaved, nprev, have_sb, logview;
    u8 ev_dirty, done_pend, tm_on, tm_done, trunc, have_prev, pad[2];
    u32 done_t, save_t, tm_start, tm_last, tm_n, tm_min, tm_max, d_test, d_step;
    lv_obj_t *root, *title, *b_menu[T_COUNT], *pg, *ptitle, *ptxt, *b_run, *l_run, *b_back, *tbox;
    lv_timer_t *timer;
} Dg;
static const char *const DG_NAME[T_COUNT + 1] = { "", "File API", "Memory", "Timer", "Touch events", "Vibration", "Saved log" };
static const char *const DG_FSN[DG_FSTEPS] = { "opendir", "readdir", "dir_exists", "stat", "read vs size", "heap" };

/* ---- text helpers ---- */
static char *dg_hex(char *b, u32 v, int digits) { for (int i = digits - 1; i >= 0; i--) *b++ = "0123456789abcdef"[(v >> (4 * i)) & 15u]; *b = 0; return b; }
static char *dg_i(char *b, i32 v) { if (v < 0) { *b++ = '-'; v = -v; } return putn(b, (u32)v, 1); }
static char *dg_dump(char *b, const u8 *p, int n) { for (int i = 0; i < n; i++) { if (i && i % 4 == 0) *b++ = ' '; b = dg_hex(b, p[i], 2); } *b = 0; return b; }
static char *dg_name(char *b, const char *nm, int max) { for (int i = 0; i < max && nm[i]; i++) { u8 c = (u8)nm[i]; *b++ = (c >= 0x20 && c < 0x7f) ? (char)c : '?'; } *b = 0; return b; }
static int dg_path(char *p, const char *name) { if (!name[0]) return 0; put(put(p, "/user/"), name); return 1; }
static void dg_brk(u32 test, u32 step, u32 phase) { DgBrk b; b.magic = DG_MAGIC; b.test = test; b.step = step; b.phase = phase; blob_save(DG_BRK, &b, sizeof b); }

/* ---- the log: appended in memory, written to flash in small pieces by the timer ---- */
static void dg_rep_add(Dg *s, const char *t) {
    int n = 0; while (t[n]) n++;
    if (s->nrep + n >= DG_REP) { n = DG_REP - 1 - s->nrep; s->trunc = 1; }
    for (int i = 0; i < n; i++) s->rep[s->nrep + i] = t[i];
    s->nrep += n; s->rep[s->nrep] = 0;
}
static void dg_log(Dg *s, const char *title, const char *text) { dg_rep_add(s, "# "); dg_rep_add(s, title); dg_rep_add(s, "\n"); dg_rep_add(s, text); dg_rep_add(s, "\n"); }

/* ---- File API steps (the same checks as fsprobe.c) ---- */
static char *dg_f1(Dg *s, char *o) {
    u32 t0 = lv_tick_get(); i32 h = fs_opendir("/user"); u32 dt = lv_tick_get() - t0; (void)s;
    o = put(o, "opendir /user\nh="); o = dg_i(o, h); o = put(o, "  "); o = putn(o, dt, 1); o = put(o, " ms\n");
    if (h >= 0) { u32 r = fs_closedir(h); o = put(o, "closedir r=0x"); return dg_hex(o, r, r > 0xffu ? 8 : 2); }
    return put(o, "failed: handle < 0");
}
static char *dg_f2(Dg *s, char *o) {
    char cand[6][32], p[80]; int nc = 0, i; u32 t0 = lv_tick_get(); i32 h = fs_opendir("/user");
    s->file[0] = 0;
    if (h < 0) { o = put(o, "opendir failed: h="); return dg_i(o, h); }
    for (i = 0; i < 16; i++) {
        void *ent = 0; const char *nm;
        (void)fs_readdir(h, &ent);
        if (!ent) { o = put(o, "end after "); o = putn(o, (u32)i, 1); o = put(o, " entries\n"); break; }
        if (!DG_PTR_OK(ent)) { o = put(o, "ptr 0x"); o = dg_hex(o, (u32)ent, 8); o = put(o, " not in RAM: stopped\n"); break; }
        nm = (const char *)ent + 0xb;
        if (i < 3) {
            o = put(o, "#"); o = putn(o, (u32)i, 1); o = put(o, " @"); o = dg_hex(o, (u32)ent, 8); o = put(o, "\n");
            o = dg_dump(o, (const u8 *)ent, 24); o = put(o, "\nname@0xb=\""); o = dg_name(o, nm, 20); o = put(o, "\"\n");
        }
        if (nc < 6 && nm[0] && nm[0] != '.') dg_name(cand[nc++], nm, 31);
    }
    fs_closedir(h);
    for (i = 0; i < nc && !s->file[0]; i++) if (dg_path(p, cand[i]) && fs_dir_exists(p) != 0) put(s->file, cand[i]);
    o = put(o, "t="); o = putn(o, lv_tick_get() - t0, 1); o = put(o, " ms\nfile for 4/5: ");
    return put(o, s->file[0] ? s->file : "none");
}
static char *dg_f3(Dg *s, char *o) {
    static const char *const P[4] = { "/user", "/user/nope_xyz", "/nand/system", "/nand" };
    (void)s;
    for (int i = 0; i < 4; i++) { u32 r = fs_dir_exists(P[i]); o = put(o, P[i]); o = put(o, " r=0x"); o = dg_hex(o, r, r > 0xffu ? 8 : 2); o = put(o, "\n"); }
    return put(o, "(0 = is a directory)");
}
static char *dg_f4(Dg *s, char *o) {
    char p[80]; int i, ch = 0, shown = 0, maxi = -1; i32 r;
    if (!dg_path(p, s->file)) return put(o, "no file found in /user\n(run step 2 first)");
    for (i = 0; i < 128; i++) s->sb[i] = 0xa5a5a5a5u;
    r = fs_stat(p, s->sb); s->have_sb = 1;
    o = put(o, "stat "); o = dg_name(o, s->file, 20); o = put(o, " r="); o = dg_i(o, r); o = put(o, "\n");
    for (i = 0; i < 128; i++) if (s->sb[i] != 0xa5a5a5a5u) {
        ch++; maxi = i;
        if (shown < 12) { o = put(o, "w"); o = putn(o, (u32)i, 1); o = put(o, "="); o = dg_hex(o, s->sb[i], 8); o = put(o, shown % 2 ? "\n" : " "); shown++; }
    }
    o = put(o, "\nwritten words: "); o = putn(o, (u32)ch, 1); o = put(o, ", last w"); o = dg_i(o, maxi);
    if (maxi >= 64) o = put(o, "  BIG struct!");
    return put(o, "\nsize guess lo=w8 hi=w9");
}
static char *dg_f5(Dg *s, char *o) {
    u8 buf[128]; u32 tot = 0; char p[80]; i32 fd, r;
    if (!dg_path(p, s->file)) return put(o, "no file found in /user\n(run step 2 first)");
    fd = f_open(p, "r");
    if (fd < 0) { o = put(o, "open failed: fd="); return dg_i(o, fd); }
    r = f_read(fd, buf, sizeof buf);
    if (r > 0) { o = put(o, "first bytes:\n"); o = dg_dump(o, buf, r < 16 ? r : 16); o = put(o, "\n"); tot = (u32)r; }
    while (r > 0 && tot < 262144u) { r = f_read(fd, buf, sizeof buf); if (r > 0) tot += (u32)r; }
    f_close(fd);
    o = put(o, "read="); o = putn(o, tot, 1);
    if (!s->have_sb) return put(o, "\n(run step 4 for the stat size)");
    o = put(o, " stat="); o = putn(o, s->sb[8], 1);
    if (s->sb[9]) o = put(o, " hi!=0");
    return put(o, tot >= 262144u ? "  capped" : (tot == s->sb[8] && !s->sb[9]) ? "  MATCH" : "  DIFF");
}
static char *dg_f6(Dg *s, char *o) {
    (void)s;
    for (u32 n = 4096; n <= 32768u; n <<= 1) {
        void *m = app_malloc(n);
        o = putn(o, n >> 10, 1); o = put(o, "k ");
        if (m) { o = put(o, "ok @"); o = dg_hex(o, (u32)m, 8); app_free(m); } else o = put(o, "FAILED");
        o = put(o, "\n");
    }
    return o;
}

/* ---- the other tests (pure functions: the host tests call them directly) ---- */
static char *dg_mem(Dg *s, char *o) {                                /* small on purpose: the firmware shares this heap */
    u32 big = 0; int k = 0, i;
    for (u32 n = 1024; n <= 32768u; n <<= 1) { void *p = app_malloc(n); if (!p) break; app_free(p); big = n; }
    for (i = 0; i < 8; i++) { s->blk[i] = app_malloc(4096); if (!s->blk[i]) break; k++; }
    for (i = 0; i < k; i++) app_free(s->blk[i]);
    o = put(o, "largest block (up to 32k): "); o = putn(o, big >> 10, 1); o = put(o, "k\n4k blocks held at once (up to 8): "); o = putn(o, (u32)k, 1);
    return put(o, big < 32768u || k < 8 ? "\nLOW: the heap is tight" : "\nok");
}
static void dg_tm_start(Dg *s, u32 now) { s->tm_on = 1; s->tm_done = 0; s->tm_start = s->tm_last = now; s->tm_n = 0; s->tm_min = 0xffffffffu; s->tm_max = 0; }
static void dg_tm_sample(Dg *s, u32 now) {                           /* called by the 50 ms timer itself, so the intervals are its own */
    u32 dt = now - s->tm_last; s->tm_last = now; s->tm_n++;
    if (dt < s->tm_min) s->tm_min = dt;
    if (dt > s->tm_max) s->tm_max = dt;
    if (now - s->tm_start >= 5000u) { s->tm_on = 0; s->tm_done = 1; }
}
static char *dg_tm_text(Dg *s, char *o) {
    u32 span = s->tm_last - s->tm_start;
    o = put(o, "50 ms timer over "); o = putn(o, span, 1); o = put(o, " ms\ncalls: "); o = putn(o, s->tm_n, 1);
    o = put(o, "\naverage: "); o = putn(o, s->tm_n ? span / s->tm_n : 0, 1); o = put(o, " ms\nshortest: "); o = putn(o, s->tm_min == 0xffffffffu ? 0 : s->tm_min, 1);
    o = put(o, " ms\nlongest: "); o = putn(o, s->tm_max, 1); return put(o, " ms");
}
static char *dg_spy_text(Dg *s, char *o) {
    static const char *const N[14] = { "all", "pressed", "pressing", "lost", "short", "long", "long-rep", "clicked", "released", "scr-begin", "scr-end", "scroll", "gesture", "key" };
    int any = 0;
    for (int i = 0; i < 14; i++) if (s->ev[i]) { if (any) o = put(o, "  "); o = put(o, N[i]); o = put(o, " "); o = putn(o, s->ev[i], 1); any = 1; }
    if (!any) o = put(o, "Touch, hold, drag and swipe in this box.\nA swipe to the right is the back gesture: it leaves.");
    if (s->gn) { o = put(o, "\ngesture dir code "); o = putn(o, s->gdir, 1); o = put(o, " (2 = right), x"); o = putn(o, s->gn, 1); }
    return o;
}
/* the breadcrumb of the last session -> a message (0 = no crash) */
static int dg_crash_text(Dg *s, const DgBrk *b, char *o) {
    if (b->magic != DG_MAGIC || b->phase != 1 || b->test < 1 || b->test > T_COUNT) return 0;
    o = put(o, "crash: "); o = put(o, DG_NAME[b->test]);
    if (b->test == T_FILES && b->step >= 1 && b->step <= DG_FSTEPS) { o = put(o, " "); o = putn(o, b->step, 1); o = put(o, " "); put(o, DG_FSN[b->step - 1]); s->fstep = (i32)b->step; }
    return 1;
}

/* ---- pages ---- */
static void dg_set(Dg *s, const char *title, const char *text) { if (title) lv_label_set_text(s->ptitle, title); if (text) lv_label_set_text(s->ptxt, text); }
static void dg_page_close(Dg *s) {
    if (s->pg) lv_obj_del(s->pg);
    s->pg = s->ptitle = s->ptxt = s->b_run = s->l_run = s->b_back = s->tbox = 0; s->cur = T_NONE; s->tm_on = 0; s->tm_done = 0;
}
static void dg_menu_cb(lv_event_t *e) {
    Dg *s = (Dg *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED || s->pg || s->act) return;
    for (int i = 0; i < T_COUNT; i++) if (t == s->b_menu[i]) { s->act = ACT_OPEN; s->arg = i + 1; return; }
}
static void dg_page_cb(lv_event_t *e) {
    Dg *s = (Dg *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED || !s->pg || s->act) return;
    s->act = (s->b_run && t == s->b_run) ? ACT_RUN : ACT_BACK;
}
static void dg_spy_cb(lv_event_t *e) {                               /* registered for ALL events on the touch box */
    Dg *s = (Dg *)lv_event_get_user_data(e); u32 c = lv_event_get_code(e);
    if (c < 16) { if (s->ev[c] < 65535u) s->ev[c]++; s->ev_dirty = 1; }
    if (c == EV_GESTURE) { lv_indev_t *d = lv_indev_get_act(); if (d) { s->gdir = lv_indev_get_gesture_dir(d); s->gn++; } }
}
static void dg_page_open(Dg *s, int id) {
    lv_obj_t *pg, *c; const char *runlabel = "Run";
    if (id < 1 || id > T_COUNT) return;
    s->cur = id; pg = s->pg = mk_root(s->root);
    s->ptitle = mk_text(pg, 0, 8, SCREEN_W, FT_SMALL, C_MUTED, ALIGN_CENTER, DG_NAME[id]);
    if (id == T_TOUCH) {
        fill8(s->ev, 0, sizeof s->ev); s->gn = 0; s->gdir = 0;
        s->tbox = mk_box(pg, 6, 40, 244, 290, 22, C_CARD); lv_obj_add_event_cb(s->tbox, dg_spy_cb, 0, s);
        s->ptxt = mk_text(s->tbox, 8, 8, 228, FT_TINY, C_WHITE, ALIGN_LEFT, ""); dg_spy_text(s, s->out); lv_label_set_text(s->ptxt, s->out);
    } else {
        c = lv_obj_create(pg); lv_obj_set_size(c, SCREEN_W, 296); lv_obj_set_pos(c, 0, 40);
        set_bg_opa(c, 0); set_border_width(c, 0); set_radius(c, 0); set_pad_all(c, 0); lv_obj_set_scrollbar_mode(c, 0);
        s->ptxt = mk_text(c, 8, 0, 240, FT_TINY, C_WHITE, ALIGN_LEFT, "");
        if (id == T_FILES) { lv_label_set_text(s->ptxt, "Read-only file API checks, one step per tap of Next (6 steps)."); runlabel = "Next"; }
        else if (id == T_MEM) lv_label_set_text(s->ptxt, "Run: how much app_malloc gives (kept small: the heap is shared with the firmware).");
        else if (id == T_TIMER) lv_label_set_text(s->ptxt, "Run: measures this app's 50 ms timer for 5 s.");
        else if (id == T_VIB) { lv_label_set_text(s->ptxt, "Run: plays motor pattern 0, then 1, 2 ... 79."); runlabel = "Play"; s->vib = -1; }
        else { lv_label_set_text(s->ptxt, s->nprev ? s->prev : "No log from an earlier session."); runlabel = "This run"; s->logview = 0; }
    }
    if (id != T_TOUCH) { s->b_run = mk_pill(pg, 6, 344, 118, runlabel, &s->l_run, dg_page_cb, s); s->b_back = mk_pill(pg, 132, 344, 118, "Back", 0, dg_page_cb, s); }
    else s->b_back = mk_pill(pg, 6, 344, 244, "Back", 0, dg_page_cb, s);
    put(s->t_run, runlabel);
}
/* one run of the open page's test */
static void dg_run(Dg *s) {
    char t[TXT_MAX + 8], *e; i32 k;
    switch (s->cur) {
    case T_FILES:
        if (s->fstep >= DG_FSTEPS) s->fstep = 0;
        k = ++s->fstep; dg_brk(T_FILES, (u32)k, 1); s->out[0] = 0;
        switch (k) { case 1: dg_f1(s, s->out); break; case 2: dg_f2(s, s->out); break; case 3: dg_f3(s, s->out); break;
                     case 4: dg_f4(s, s->out); break; case 5: dg_f5(s, s->out); break; default: dg_f6(s, s->out); break; }
        e = put(t, "File API "); e = putn(e, (u32)k, 1); e = put(e, "/"); e = putn(e, DG_FSTEPS, 1); e = put(e, " "); put(e, DG_FSN[k - 1]);
        dg_set(s, t, s->out); dg_log(s, t, s->out); s->d_test = T_FILES; s->d_step = (u32)k;
        lv_label_set_text(s->l_run, k >= DG_FSTEPS ? "Again" : "Next"); break;
    case T_MEM:
        dg_brk(T_MEM, 0, 1); s->out[0] = 0; dg_mem(s, s->out); dg_set(s, 0, s->out); dg_log(s, "Memory", s->out); s->d_test = T_MEM; s->d_step = 0; break;
    case T_TIMER:
        if (s->tm_on) return;
        dg_tm_start(s, lv_tick_get()); dg_set(s, 0, "Measuring for 5 s ..."); break;
    case T_VIB:
        s->vib = s->vib >= 79 ? 0 : s->vib + 1; dg_brk(T_VIB, (u32)s->vib, 1); motor_once((u32)s->vib, 0);
        e = put(t, "pattern "); e = putn(e, (u32)s->vib, 1); put(e, " of 0..79"); dg_set(s, 0, t); s->d_test = T_VIB; s->d_step = (u32)s->vib; break;
    case T_LOG:
        s->logview = !s->logview; dg_set(s, s->logview ? "This session" : "Saved log", s->logview ? (s->nrep ? s->rep : "Nothing logged yet.") : (s->nprev ? s->prev : "No log from an earlier session."));
        lv_label_set_text(s->l_run, s->logview ? "Last run" : "This run"); return;
    default: return;
    }
    s->done_pend = 1; s->done_t = lv_tick_get(); motor_once(1, 1);
}
static void dg_tick(lv_timer_t *tm) {
    Dg *s = (Dg *)timer_user_data(tm); u32 now = lv_tick_get(); int a = s->act, arg = s->arg; s->act = ACT_NONE;
    if (s->tm_on) dg_tm_sample(s, now);
    if (s->tm_done) {
        s->tm_done = 0; s->out[0] = 0; dg_tm_text(s, s->out);
        if (s->pg && s->cur == T_TIMER) dg_set(s, 0, s->out);
        dg_log(s, "Timer", s->out); s->d_test = T_TIMER; s->d_step = 0; s->done_pend = 1; s->done_t = now;
    }
    if (s->ev_dirty && s->cur == T_TOUCH && s->ptxt) { s->ev_dirty = 0; s->out[0] = 0; dg_spy_text(s, s->out); lv_label_set_text(s->ptxt, s->out); }
    if (s->done_pend && now - s->done_t > 3000) { s->done_pend = 0; dg_brk(s->d_test, s->d_step, 2); }
    else if (s->nsaved < s->nrep && now - s->save_t >= 3000) {                    /* the log goes to flash 256 bytes at a time, 3 s apart */
        int n = s->nrep - s->nsaved; i32 fd;
        if (n > DG_CHUNK) n = DG_CHUNK;
        fd = f_open(DG_LOG, s->nsaved ? "a" : "w");
        if (fd >= 0) { f_write(fd, s->rep + s->nsaved, (u32)n); f_close(fd); s->nsaved += n; }
        s->save_t = now;
    }
    if (a == ACT_OPEN && !s->pg) dg_page_open(s, arg);
    else if (a == ACT_RUN && s->pg) dg_run(s);
    else if (a == ACT_BACK && s->pg) { dg_page_close(s); motor_once(1, 1); }
}

/* ---- the app ---- */
static void *dg_create(lv_obj_t *root) {
    Dg *s = (Dg *)app_malloc(sizeof(Dg)); DgBrk b; char t[TXT_MAX + 16]; i32 fd;
    if (!s) return 0;
    fill8(s, 0, sizeof(Dg)); s->root = root; s->save_t = lv_tick_get() - 3000u;
    s->title = mk_text(root, 0, 8, SCREEN_W, FT_SMALL, C_MUTED, ALIGN_CENTER, "Watch Diag");
    for (int i = 0; i < T_COUNT; i++) s->b_menu[i] = mk_pill(root, 6, 44 + i * 58, 244, DG_NAME[i + 1], 0, dg_menu_cb, s);
    fd = f_open(DG_LOG, "r");                                    /* the log of the last session, to read in "Saved log" */
    if (fd >= 0) { i32 r; while (s->nprev < DG_REP - 1 && (r = f_read(fd, s->prev + s->nprev, (u32)(DG_REP - 1 - s->nprev))) > 0) s->nprev += r; f_close(fd); s->prev[s->nprev] = 0; }
    if (blob_load(DG_BRK, &b, sizeof b) && dg_crash_text(s, &b, t)) {
        char m[TXT_MAX + 16]; put(put(m, "! "), t);
        lv_label_set_text(s->title, m); set_text_color(s->title, C_B);
        dg_log(s, "CRASH", t); dg_brk(b.test, b.step, 3);       /* reported once */
    }
    s->timer = lv_timer_create(dg_tick, 50, s);
    return s;
}
static int dg_back(void *st) { Dg *s = (Dg *)st; if (s->pg) { dg_page_close(s); return 1; } return 0; }
static void dg_destroy(void *st) {
    Dg *s = (Dg *)st;
    if (s->timer) lv_timer_del(s->timer);
    s->timer = 0;
    if (s->done_pend) dg_brk(s->d_test, s->d_step, 2);
    app_free(s);
}
APP_EXPORT(diag, "Watch Diag", 0, dg_create, dg_back, dg_destroy)
