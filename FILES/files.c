/* SPDX-License-Identifier: Apache-2.0 */
/* apps/files.c: a file manager for the watch's flash (yaffs2 on NAND).
 *
 * Browse folders (a virtual root lists /user and /nand), see the size of every file, tap a file to open a sheet with:
 *   Peek   the first 60 bytes of the file as text (anything unprintable shows as ".")
 *   Delete only for files under /user/, and it asks twice ("Sure?"); folders are never deleted, /nand is read-only here
 * The back gesture goes up one folder, or closes the sheet, or leaves the app at the root.
 *
 * What is NOT verified on a real watch (docs/status.md: "we have never run readdir"): opendir/readdir/stat/unlink. The addresses and the directory entry layout (name at entry + 0xb) come from
 * docs/symbols.csv, where they are marked verified/likely. The code is defensive about it: an entry pointer outside RAM ends the listing with an error, the listing is capped, and nothing here
 * ever calls the firmware's recursive delete. Try it on a watch you can recover, with a few files you do not mind losing.
 *
 * State lives in one block from app_malloc (no globals, no libc: docs/f3x.md). Taps only set a flag; a 50 ms timer does the work, so a widget is never deleted inside its own event. */
#include "ui.h"

#ifndef fs_opendir
#define fs_opendir(p)       F(0x2c143428, i32, (const char *))(p)           /* handle, < 0 on error (5 slots) */
#define fs_closedir(h)      F(0x2c143530, u32, (i32))(h)
#define fs_readdir(h, pe)   F(0x2c143680, i32, (i32, void **))(h, pe)       /* *pe = entry, 0 at the end; the name is at entry + 0xb */
#define fs_dir_exists(p)    F(0x2c143816, u32, (const char *))(p)           /* 0 = it is a directory */
#define fs_stat(p, b)       F(0x2c1432a8, i32, (const char *, void *))(p, b) /* size: low word at +0x20, high at +0x24 */
#define fs_unlink(p)        F(0x2c1430a4, i32, (const char *))(p)
#endif
#ifndef FM_PTR_OK
#define FM_PTR_OK(p)        ((u32)(p) >= 0x20000000u && (u32)(p) < 0x40000000u)   /* a directory entry must be in RAM, else we stop instead of faulting */
#endif

#define FM_MAXE 40                                               /* entries shown per folder; the rest is counted ("N more not shown") */
#define FM_NAME 32                                               /* 31 characters + NUL; a longer name is shown but locked (cannot be opened or deleted) */
#define FM_PATH 128                                              /* the firmware's mkdir_p wants at most 127; a longer path is refused */
#define FM_ROW 78                                                /* row pitch: 72 px card + 6 px gap */
#define FM_GUARD 300                                             /* readdir calls per folder at most */
enum { ACT_NONE, ACT_OPEN, ACT_PEEK, ACT_DEL, ACT_CLOSE };

typedef struct { char name[FM_NAME]; u8 dir, lock, pad[2]; u32 size; } FmEnt;     /* size 0xffffffff = unknown */
typedef struct {
    char path[FM_PATH], fpath[FM_PATH], pv[72], t_hdr[TXT_MAX], t_del[TXT_MAX];
    i32 n, more, err, sel, act, arg;
    u8 armed, can_del, pad[2]; u32 armed_t;
    u32 sb[64];                                                  /* room for the firmware's stat structure (we read two words of it) */
    FmEnt e[FM_MAXE];
    lv_obj_t *root, *title, *list, *row[FM_MAXE], *sheet, *b_peek, *b_del, *l_del, *l_info, *l_pv;
    lv_timer_t *timer;
} Fm;

/* ---- text and paths ---- */
static int fm_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int fm_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
/* "/a/b" + "c" -> "/a/b/c"; "" + "user" -> "/user"; 0 if it would not fit */
static int fm_join(char *out, const char *dir, const char *name) {
    int a = fm_len(dir), b = fm_len(name);
    if (a + 1 + b >= FM_PATH - 1) return 0;
    mcopy(out, dir, (u32)a); out[a] = '/'; mcopy(out + a + 1, name, (u32)b + 1u);
    return 1;
}
/* "/a/b" -> "/a"; "/a" -> "" (the virtual root) */
static void fm_parent(char *p) { int n = fm_len(p); while (n > 0 && p[n - 1] != '/') n--; if (n > 0) n--; p[n] = 0; }
static char *fm_size(char *b, u32 v) {
    if (v == 0xffffffffu) return put(b, "? B");
    if (v < 1024u) { b = putn(b, v, 1); return put(b, " B"); }
    if (v < 1048576u) { b = putn(b, v >> 10, 1); b = put(b, "."); b = putn(b, ((v & 1023u) * 10u) >> 10, 1); return put(b, " kB"); }
    b = putn(b, v >> 20, 1); b = put(b, "."); b = putn(b, ((v & 0xfffffu) * 10u) >> 20, 1); return put(b, " MB");
}
/* a name for a row: at most 16 characters, longer ones cut to 14 + ".." (out needs 17 bytes) */
static void fm_shown(char *out, const char *name) {
    int n = fm_len(name), i;
    if (n <= 16) { for (i = 0; i <= n; i++) out[i] = name[i]; return; }
    for (i = 0; i < 14; i++) out[i] = name[i];
    out[14] = '.'; out[15] = '.'; out[16] = 0;
}

/* ---- the listing ---- */
/* adds a name (printable ASCII only: the watch's font has no other glyphs; a name we had to change is locked, because its real path is not what we would build) */
static FmEnt *fm_add(Fm *s, const char *nm) {
    FmEnt *e; int i = 0;
    if (s->n >= FM_MAXE) { s->more++; return 0; }
    e = &s->e[s->n++]; fill8(e, 0, sizeof *e);
    for (; nm[i] && i < FM_NAME - 1; i++) {
        u8 c = (u8)nm[i];
        if (c >= 0x20 && c < 0x7f) e->name[i] = (char)c; else { e->name[i] = '?'; e->lock = 1; }
    }
    e->name[i] = 0; if (nm[i]) e->lock = 1;
    e->size = 0xffffffffu;
    return e;
}
static int fm_less(const FmEnt *a, const FmEnt *b) {                 /* folders first, then by name */
    const char *x = a->name, *y = b->name;
    if (a->dir != b->dir) return a->dir > b->dir;
    while (*x && *x == *y) { x++; y++; }
    return (u8)*x < (u8)*y;
}
static void fm_sort(Fm *s) {
    for (int i = 1; i < s->n; i++) {
        FmEnt t; int j = i - 1;
        mcopy(&t, &s->e[i], sizeof t);
        while (j >= 0 && fm_less(&t, &s->e[j])) { mcopy(&s->e[j + 1], &s->e[j], sizeof t); j--; }
        mcopy(&s->e[j + 1], &t, sizeof t);
    }
}
static void fm_scan(Fm *s) {
    char p[FM_PATH]; i32 h;
    s->n = 0; s->more = 0; s->err = 0;
    if (!s->path[0]) {                                           /* the virtual root */
        FmEnt *a = fm_add(s, "user"), *b = fm_add(s, "nand");
        if (a) a->dir = 1;
        if (b) b->dir = 1;
        return;
    }
    h = fs_opendir(s->path);
    if (h < 0) { s->err = 1; return; }
    for (int guard = 0; guard < FM_GUARD; guard++) {
        void *ent = 0; const char *nm;
        (void)fs_readdir(h, &ent);
        if (!ent) break;
        if (!FM_PTR_OK(ent)) { s->err = 1; break; }
        nm = (const char *)ent + 0xb;
        if (!nm[0] || (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))) continue;
        fm_add(s, nm);
    }
    fs_closedir(h);                                              /* closed before the probes below: only 5 directory handles exist */
    for (int i = 0; i < s->n; i++) {
        FmEnt *e = &s->e[i];
        if (e->lock || !fm_join(p, s->path, e->name)) { e->lock = 1; continue; }
        if (fs_dir_exists(p) == 0) { e->dir = 1; continue; }
        fill8(s->sb, 0, sizeof s->sb);
        if (fs_stat(p, s->sb) == 0 && s->sb[9] == 0) e->size = s->sb[8];
    }
    fm_sort(s);
}
static void fm_header(Fm *s) {
    char b[TXT_MAX]; int n = fm_len(s->path);
    if (!n) put(b, "Files");
    else if (n <= TXT_MAX - 2) put(b, s->path);
    else put(put(b, ".."), s->path + n - (TXT_MAX - 4));
    set_txt(s->title, s->t_hdr, b);
}
static void fm_row_cb(lv_event_t *e);
static void fm_build(Fm *s) {
    lv_obj_t *l = s->list; char sh[20], inf[TXT_MAX + 8];
    lv_obj_clean(l); fill8(s->row, 0, sizeof s->row);
    if (s->err) mk_text(l, 14, 60, 228, FT_SMALL, C_MUTED, ALIGN_CENTER, "Cannot read this folder");
    else if (!s->n) mk_text(l, 14, 60, 228, FT_SMALL, C_MUTED, ALIGN_CENTER, "Empty folder");
    for (int i = 0; i < s->n; i++) {
        FmEnt *e = &s->e[i];
        lv_obj_t *r = s->row[i] = mk_box(l, 1, 4 + i * FM_ROW, 254, 72, 22, C_CARD);
        lv_obj_add_event_cb(r, fm_row_cb, 0, s);
        fm_shown(sh, e->name);
        mk_text(r, 14, 6, 226, FT_SMALL, e->dir ? C_A : C_WHITE, ALIGN_LEFT, sh);
        if (e->dir) put(inf, "Folder"); else if (e->lock) put(inf, "Name not supported"); else fm_size(inf, e->size);
        mk_text(r, 14, 38, 226, FT_SMALL, C_MUTED, ALIGN_LEFT, inf);
    }
    if (s->more) { char *p = putn(inf, (u32)s->more, 1); put(p, " more not shown"); mk_text(l, 14, 4 + s->n * FM_ROW + 6, 228, FT_SMALL, C_MUTED, ALIGN_CENTER, inf); }
    lv_obj_scroll_to_y(l, 0, 0);
}
static void fm_load(Fm *s) { fm_scan(s); fm_header(s); fm_build(s); }

/* ---- the sheet of a file: name, size, Peek, Delete (under /user/ only), Close ---- */
static void fm_sheet_cb(lv_event_t *e);
static void fm_sheet_close(Fm *s) {
    if (s->sheet) lv_obj_del(s->sheet);
    s->sheet = 0; s->b_peek = s->b_del = s->l_del = s->l_info = s->l_pv = 0; s->armed = 0;
}
static void fm_sheet_render(Fm *s) {
    if (!s->sheet || !s->can_del) return;
    set_txt(s->l_del, s->t_del, s->armed ? "Sure?" : "Delete");
    set_bg_color(s->b_del, s->armed ? C_WARN : C_BTN);
}
static void fm_sheet_open(Fm *s, int i, const char *p) {
    char b[TXT_MAX + 8]; FmEnt *e = &s->e[i]; lv_obj_t *sh;
    mcopy(s->fpath, p, (u32)fm_len(p) + 1u); s->sel = i; s->armed = 0;
    s->can_del = (u8)fm_starts(p, "/user/");
    sh = s->sheet = mk_root(s->root);
    mk_text(sh, 14, 12, 228, FT_SMALL, C_WHITE, ALIGN_CENTER, e->name);
    fm_size(b, e->size);
    s->l_info = mk_text(sh, 14, 80, 228, FT_SMALL, C_MUTED, ALIGN_CENTER, b);
    s->l_pv = mk_text(sh, 14, 120, 228, FT_SMALL, C_WHITE, ALIGN_LEFT, "");
    s->b_peek = mk_pill(sh, 6, 284, s->can_del ? 118 : 244, "Peek", 0, fm_sheet_cb, s);
    s->b_del = 0; s->l_del = 0;
    if (s->can_del) { put(s->t_del, "Delete"); s->b_del = mk_pill(sh, 132, 284, 118, "Delete", &s->l_del, fm_sheet_cb, s); }
    mk_pill(sh, 6, 344, 244, "Close", 0, fm_sheet_cb, s);
    motor_once(1, 1);
}
static void fm_peek(Fm *s) {
    u8 buf[60]; int n = 0; i32 fd = f_open(s->fpath, "r");
    if (fd < 0) { lv_label_set_text(s->l_pv, "Cannot open"); return; }
    { i32 r = f_read(fd, buf, sizeof buf); if (r > 0) n = r; }
    f_close(fd);
    if (!n) { lv_label_set_text(s->l_pv, "(empty)"); return; }
    for (int i = 0; i < n; i++) s->pv[i] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : '.';
    s->pv[n] = 0; lv_label_set_text(s->l_pv, s->pv);
}
/* the first tap arms ("Sure?", 2.5 s), the second deletes; success is checked by trying to open the file again */
static void fm_delete(Fm *s) {
    i32 fd;
    if (!s->can_del) return;
    if (!s->armed) { s->armed = 1; s->armed_t = lv_tick_get(); fm_sheet_render(s); motor_once(1, 1); return; }
    s->armed = 0;
    (void)fs_unlink(s->fpath);
    fd = f_open(s->fpath, "r");
    if (fd >= 0) { f_close(fd); fm_sheet_render(s); lv_label_set_text(s->l_info, "Delete failed"); motor_once(4, 1); return; }
    fm_sheet_close(s); fm_load(s); motor_once(4, 1);
}
static void fm_open(Fm *s, int i) {
    char p[FM_PATH]; FmEnt *e;
    if (i < 0 || i >= s->n) return;
    e = &s->e[i];
    if (e->lock || !fm_join(p, s->path, e->name)) { motor_once(4, 1); return; }          /* a buzz: this name cannot be handled */
    if (e->dir) { mcopy(s->path, p, (u32)fm_len(p) + 1u); fm_load(s); motor_once(1, 1); }
    else fm_sheet_open(s, i, p);
}

/* ---- events: taps only set a flag, the timer does the work ---- */
static void fm_row_cb(lv_event_t *e) {
    Fm *s = (Fm *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED || s->sheet || s->act) return;
    for (int i = 0; i < s->n; i++) if (t == s->row[i]) { s->act = ACT_OPEN; s->arg = i; return; }
}
static void fm_sheet_cb(lv_event_t *e) {
    Fm *s = (Fm *)lv_event_get_user_data(e); lv_obj_t *t = lv_event_get_target(e);
    if (lv_event_get_code(e) != EV_CLICKED || !s->sheet || s->act) return;
    if (t == s->b_peek) s->act = ACT_PEEK; else if (s->b_del && t == s->b_del) s->act = ACT_DEL; else s->act = ACT_CLOSE;
}
static void fm_tick(lv_timer_t *t) {
    Fm *s = (Fm *)timer_user_data(t); int a = s->act, arg = s->arg; s->act = ACT_NONE;
    if (s->armed && lv_tick_get() - s->armed_t > 2500) { s->armed = 0; fm_sheet_render(s); }
    if (a == ACT_OPEN) fm_open(s, arg);
    else if (a == ACT_PEEK && s->sheet) fm_peek(s);
    else if (a == ACT_DEL && s->sheet) fm_delete(s);
    else if (a == ACT_CLOSE && s->sheet) { fm_sheet_close(s); motor_once(1, 1); }
}

/* ---- the app ---- */
static void *fm_create(lv_obj_t *root) {
    Fm *s = (Fm *)app_malloc(sizeof(Fm)); lv_obj_t *l;
    if (!s) return 0;
    fill8(s, 0, sizeof(Fm)); s->root = root;
    s->title = mk_text(root, 0, 8, SCREEN_W, FT_SMALL, C_MUTED, ALIGN_CENTER, "Files"); put(s->t_hdr, "Files");
    l = s->list = lv_obj_create(root);
    lv_obj_set_size(l, SCREEN_W, SCREEN_H - 44); lv_obj_set_pos(l, 0, 44);
    set_bg_opa(l, 0); set_border_width(l, 0); set_radius(l, 0); set_pad_all(l, 0); lv_obj_set_scrollbar_mode(l, 0);
    fm_load(s);
    s->timer = lv_timer_create(fm_tick, 50, s);
    return s;
}
static int fm_back(void *st) {                                   /* the sheet first, then one folder up, then leave */
    Fm *s = (Fm *)st;
    if (s->sheet) { fm_sheet_close(s); return 1; }
    if (!s->path[0]) return 0;
    fm_parent(s->path); fm_load(s); return 1;
}
static void fm_destroy(void *st) { Fm *s = (Fm *)st; if (s->timer) lv_timer_del(s->timer); s->timer = 0; app_free(s); }
APP_EXPORT(files, "Files", 0, fm_create, fm_back, fm_destroy)
