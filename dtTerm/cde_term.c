/*
 * cde-term (dtTerm): Multi-Tab Native OSF/Motif 2.3 & CDE libDtTerm Terminal Emulator
 *
 * - Uses genuine CDE DtTerm (DtCreateTerm from libDtTerm) terminal widgets inside
 *   switchable 3D-beveled Motif tabs, preserving authentic CDE terminal emulation,
 *   scrollbars, fonts, and palette resources.
 * - Built-in Smart Clipboard (no LD_PRELOAD required):
 *     * Ctrl+C copies selection to CLIPBOARD and clears highlight when text is selected,
 *       or sends normal ^C (SIGINT) to the pty when nothing is selected.
 *     * Ctrl+V pastes from CLIPBOARD (falling back to PRIMARY selection).
 * - Full Multi-Tab Management:
 *     * [+ New Tab] button & Ctrl+Shift+T (inherits active tab's current working directory)
 *     * Per-tab [x] close button, middle-click close, right-click tab context menu,
 *       Rename Tab dialog, Duplicate Tab, Ctrl+Shift+W, Ctrl+Tab / Ctrl+PgUp / Ctrl+PgDn,
 *       and Alt+1..Alt+9 direct tab switching.
 *     * Live tab titles tracking working directory & foreground command via /proc.
 *     * [A-] / [A+] & Ctrl+- / Ctrl++ / Ctrl+0 font scaling and CDE/Dark/Retro themes.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <execinfo.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include <Xm/XmAll.h>
#include <Dt/Dt.h>
#include <Dt/TermPrim.h>
#include <Dt/Term.h>

#include "cde_term_icon.h"

/* Private libDtTerm helpers exported by /usr/dt/lib/libDtTerm.so */
extern Boolean _DtTermPrimSelectCopyClipboard(Widget w, Time t);
extern Boolean _DtTermPrimSelectPasteClipboard(Widget w, Time t);
extern void    _DtTermPrimSelectDisown(Widget w);
extern void    _DtTermPrimSelectLoseSelection(Widget w, Atom *selection);
extern void    _DtTermPrimSelectAll(Widget w, XEvent *event, String *params, Cardinal *num_params);
extern void    _DtTermFuncClearBuffer(Widget w, int count, int from);
extern void    _DtTermFuncSoftReset(Widget w, int count, int from);
extern void    _DtTermFuncHardReset(Widget w, int count, int from);
extern void    _DtTermPrimCursorOn(Widget w);
extern void    _DtTermPrimPtyCleanup(void);
extern void    _DtTermPrimLogFileCleanup(void);
extern int     _DtEnvControl(int mode);

#define MAX_TABS 32

typedef enum {
    THEME_CDE_PALETTE = 0,
    THEME_DARK        = 1,
    THEME_PHOSPHOR_GR = 2,
    THEME_PHOSPHOR_AM = 3
} TermTheme;

typedef struct {
    Pixel bg_chrome;
    Pixel bg_tabbar;
    Pixel bg_tab_inactive;
    Pixel bg_tab_active;
    Pixel fg_chrome;
    Pixel bg_select;
    Pixel bg_well;
    Pixel fg_well;
    Pixel term_bg;
    Pixel term_fg;
} CdeTermPalette;

typedef struct {
    int    id;
    Widget tab_frame;
    Widget tab_form;
    Widget tab_btn;
    Widget close_btn;
    Widget page_form;
    Widget sw;
    Widget vsb;
    Widget term;
    pid_t  shell_pid;
    char   custom_title[128];
    char   auto_title[128];
    char   last_cwd[PATH_MAX];
    char   last_proc[64];
    int    closing;
} TermTab;

/* ---------------- Global State ---------------- */
static XtAppContext   g_app;
static Widget         g_toplevel      = NULL;
static Widget         g_main_form     = NULL;
static Widget         g_menubar       = NULL;
static Widget         g_tabbar_frame  = NULL;
static Widget         g_tabbar_form   = NULL;
static Widget         g_tabs_rc       = NULL;
static Widget         g_new_tab_btn   = NULL;
static Widget         g_zoom_out_btn  = NULL;
static Widget         g_zoom_in_btn   = NULL;
static Widget         g_stack_frame   = NULL;
static Widget         g_stack_form    = NULL;
static Widget         g_status_frame  = NULL;
static Widget         g_st_left       = NULL;
static Widget         g_st_mid        = NULL;
static Widget         g_st_right      = NULL;
static Widget         g_tab_popup     = NULL;
static Widget         g_term_popup    = NULL;
static Widget         g_view_mb_tb    = NULL;
static Widget         g_view_sb_tb    = NULL;

static TermTab        g_tabs[MAX_TABS];
static int            g_num_tabs      = 0;
static int            g_active_idx    = -1;
static int            g_popup_tab_idx = -1;
static int            g_next_tab_id   = 1;

static CdeTermPalette g_pal;
static TermTheme      g_theme         = THEME_CDE_PALETTE;
static int            g_show_menubar  = 1;
static int            g_show_vscroll  = 1;
static int            g_login_shell   = 1;
static int            g_console_mode  = 0;
static int            g_self_test     = 0;
static int            g_init_cols     = 84;
static int            g_init_rows     = 24;
static char           g_geom_pos_buf[64] = "";
static int            g_font_idx      = -1; /* -1 = CDE default userFont; 0..6 = explicit zoom */
static XmFontList     g_font_cache[7] = {NULL};
static XmFontList     g_ui_bold_fl    = NULL;
static XmFontList     g_ui_norm_fl    = NULL;
static Pixmap         g_icon_pix      = None;
static Pixmap         g_icon_mask     = None;

static char           g_self_exe[PATH_MAX]    = "/home/adam/.local/bin/dtterm";
static char           g_init_cwd[PATH_MAX]    = "";
static char           g_fixed_win_title[256]  = "";
static char          *g_init_cmd_prog         = NULL;
static char         **g_init_cmd_argv         = NULL;

static const char *k_font_specs[7][2] = {
    {"-dt-interface user-medium-r-normal-xxs*-*-*-*-*-m-*", "6x10"},
    {"-dt-interface user-medium-r-normal-xs*-*-*-*-*-m-*",  "6x13"},
    {"-dt-interface user-medium-r-normal-s*-*-*-*-*-m-*",   "7x13"},
    {"-dt-interface user-medium-r-normal-m*-*-*-*-*-m-*",   "7x14"},
    {"-dt-interface user-medium-r-normal-l*-*-*-*-*-m-*",   "9x15"},
    {"-dt-interface user-medium-r-normal-xl*-*-*-*-*-m-*",  "10x20"},
    {"-dt-interface user-medium-r-normal-xxl*-*-*-*-*-m-*", "12x24"},
};

static const char *k_font_names[7] = {
    "XX-Small", "X-Small", "Small", "Medium", "Large", "X-Large", "XX-Large"
};

/* Forward declarations */
static int  create_new_tab(const char *cwd_override, int use_init_cmd, int console_mode);
static void activate_tab(int idx);
static void close_tab_at(int idx);
static void refresh_tab_bar_styles(void);
static void update_window_and_status(void);
static void apply_theme_all(TermTheme theme, int save_pref);
static void step_font_zoom(int delta, int reset_default);
static void prompt_rename_tab(int idx);

/* ---------------- Preferences (~/.config/dtterm/config) ---------------- */
static void get_config_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(buf, cap, "%s/.config/dtterm/config", home);
}

static void load_prefs(void) {
    char path[PATH_MAX];
    get_config_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        char *nl = strpbrk(eq, "\r\n");
        if (nl) *nl = '\0';
        if (strcmp(line, "theme") == 0) {
            int v = atoi(eq);
            if (v >= THEME_CDE_PALETTE && v <= THEME_PHOSPHOR_AM) g_theme = (TermTheme)v;
        } else if (strcmp(line, "font_idx") == 0) {
            int v = atoi(eq);
            if (v >= -1 && v <= 6) g_font_idx = v;
        } else if (strcmp(line, "scrollbar") == 0) {
            g_show_vscroll = atoi(eq) ? 1 : 0;
        }
    }
    fclose(fp);
}

static void save_prefs(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char dir1[PATH_MAX], dir2[PATH_MAX], path[PATH_MAX];
    snprintf(dir1, sizeof(dir1), "%s/.config", home);
    snprintf(dir2, sizeof(dir2), "%s/.config/dtterm", home);
    mkdir(dir1, 0755);
    mkdir(dir2, 0755);
    get_config_path(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "theme=%d\nfont_idx=%d\nscrollbar=%d\n",
            (int)g_theme, g_font_idx, g_show_vscroll);
    fclose(fp);
}

/* ---------------- Signal & Exit Cleanup ---------------- */
static void cleanup_on_signal(int sig) {
    (void)sig;
    _DtTermPrimPtyCleanup();
    _DtTermPrimLogFileCleanup();
    _exit(0);
}

static void crash_bt_handler(int sig) {
    void *bt[48];
    int n = backtrace(bt, 48);
    fprintf(stderr, "\ndtTerm caught signal %d — backtrace:\n", sig);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(128 + sig);
}

static void cleanup_and_exit(int code) {
    _DtTermPrimPtyCleanup();
    _DtTermPrimLogFileCleanup();
    exit(code);
}

/* ---------------- Color & CDE Palette Helpers ---------------- */
static Pixel alloc_color_spec(Display *dpy, Colormap cmap, const char *spec, Pixel fallback) {
    if (!spec || !spec[0]) return fallback;
    XColor col, exact;
    if (XAllocNamedColor(dpy, cmap, spec, &col, &exact)) return col.pixel;
    if (XParseColor(dpy, cmap, spec, &col) && XAllocColor(dpy, cmap, &col)) return col.pixel;
    return fallback;
}

static Pixel shade_pixel(Display *dpy, Colormap cmap, Pixel base, int permille) {
    XColor c;
    memset(&c, 0, sizeof(c));
    c.pixel = base;
    XQueryColor(dpy, cmap, &c);
    long r = ((long)c.red   * permille) / 1000L;
    long g = ((long)c.green * permille) / 1000L;
    long b = ((long)c.blue  * permille) / 1000L;
    c.red   = (unsigned short)(r < 0 ? 0 : (r > 65535 ? 65535 : r));
    c.green = (unsigned short)(g < 0 ? 0 : (g > 65535 ? 65535 : g));
    c.blue  = (unsigned short)(b < 0 ? 0 : (b > 65535 ? 65535 : b));
    return XAllocColor(dpy, cmap, &c) ? c.pixel : base;
}

static void load_cde_palette(Widget top, CdeTermPalette *pal) {
    Display *dpy = XtDisplay(top);
    int scr = DefaultScreen(dpy);
    Colormap cmap = DefaultColormap(dpy, scr);
    Pixel blk = BlackPixel(dpy, scr);
    Pixel wht = WhitePixel(dpy, scr);

    char pal_name[128] = "Crimson.dp";
    XrmDatabase db = XtDatabase(dpy);
    if (db) {
        char *str_type = NULL;
        XrmValue val = {0, NULL};
        if ((XrmGetResource(db, "0.ColorPalette", "0.ColorPalette", &str_type, &val) ||
             XrmGetResource(db, "*0*ColorPalette", "*0*ColorPalette", &str_type, &val) ||
             XrmGetResource(db, "*ColorPalette", "*ColorPalette", &str_type, &val)) &&
            val.addr && ((char *)val.addr)[0]) {
            snprintf(pal_name, sizeof(pal_name), "%s", (char *)val.addr);
        }
    }

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char p1[PATH_MAX], p2[PATH_MAX];
    snprintf(p1, sizeof(p1), "%s/.dt/palettes/%s", home, pal_name);
    snprintf(p2, sizeof(p2), "/usr/dt/palettes/%s", pal_name);

    FILE *fp = fopen(p1, "r");
    if (!fp) fp = fopen(p2, "r");

    char cset[8][64];
    memset(cset, 0, sizeof(cset));
    int nlines = 0;
    if (fp) {
        char ln[128];
        while (nlines < 8 && fgets(ln, sizeof(ln), fp)) {
            char *nl = strpbrk(ln, "\r\n");
            if (nl) *nl = '\0';
            if (ln[0] == '#') {
                snprintf(cset[nlines], sizeof(cset[nlines]), "%.63s", ln);
                nlines++;
            }
        }
        fclose(fp);
    }

    const char *chrome_hex = (nlines >= 2) ? cset[1] : "#AEB2C3";
    const char *sel_hex    = (nlines >= 3) ? cset[2] : "#718BA5";
    const char *well_hex   = (nlines >= 4) ? cset[3] : "#FFF7E9";

    pal->bg_chrome       = alloc_color_spec(dpy, cmap, chrome_hex, wht);
    pal->bg_select       = alloc_color_spec(dpy, cmap, sel_hex, blk);
    pal->bg_well         = alloc_color_spec(dpy, cmap, well_hex, wht);
    pal->fg_chrome       = blk;
    pal->fg_well         = alloc_color_spec(dpy, cmap, "#101014", blk);
    pal->bg_tabbar       = shade_pixel(dpy, cmap, pal->bg_chrome, 860);
    pal->bg_tab_inactive = shade_pixel(dpy, cmap, pal->bg_chrome, 945);
    pal->bg_tab_active   = pal->bg_well;

    switch (g_theme) {
        case THEME_DARK:
            pal->term_bg = alloc_color_spec(dpy, cmap, "#15181E", blk);
            pal->term_fg = alloc_color_spec(dpy, cmap, "#E6EDF3", wht);
            break;
        case THEME_PHOSPHOR_GR:
            pal->term_bg = alloc_color_spec(dpy, cmap, "#0C1810", blk);
            pal->term_fg = alloc_color_spec(dpy, cmap, "#5AF78E", wht);
            break;
        case THEME_PHOSPHOR_AM:
            pal->term_bg = alloc_color_spec(dpy, cmap, "#191208", blk);
            pal->term_fg = alloc_color_spec(dpy, cmap, "#FFB84D", wht);
            break;
        case THEME_CDE_PALETTE:
        default:
            pal->term_bg = pal->bg_well;
            pal->term_fg = pal->fg_well;
            break;
    }
}

static void set_xm_label(Widget w, const char *txt) {
    if (!w) return;
    XmString s = XmStringCreateLocalized((char *)(txt ? txt : ""));
    XtVaSetValues(w, XmNlabelString, s, NULL);
    XmStringFree(s);
}

static void init_ui_fontlists(Display *dpy) {
    char **missing = NULL;
    int nmissing = 0;
    char *def_str = NULL;

    XFontSet fs_norm = XCreateFontSet(
        dpy,
        "-dt-interface system-medium-r-normal-s*-*-*-*-*-*-*-*-*,-misc-fixed-medium-r-normal--13-*-*-*-*-*-*-*,fixed",
        &missing, &nmissing, &def_str);
    if (missing) XFreeStringList(missing);
    if (fs_norm) {
        XmFontListEntry e = XmFontListEntryCreate(XmFONTLIST_DEFAULT_TAG, XmFONT_IS_FONTSET, (XtPointer)fs_norm);
        g_ui_norm_fl = XmFontListAppendEntry(NULL, e);
        XmFontListEntryFree(&e);
    }

    missing = NULL;
    nmissing = 0;
    XFontSet fs_bold = XCreateFontSet(
        dpy,
        "-dt-interface system-bold-r-normal-s*-*-*-*-*-*-*-*-*,-misc-fixed-bold-r-normal--13-*-*-*-*-*-*-*,fixed",
        &missing, &nmissing, &def_str);
    if (missing) XFreeStringList(missing);
    if (fs_bold) {
        XmFontListEntry e = XmFontListEntryCreate(XmFONTLIST_DEFAULT_TAG, XmFONT_IS_FONTSET, (XtPointer)fs_bold);
        g_ui_bold_fl = XmFontListAppendEntry(NULL, e);
        XmFontListEntryFree(&e);
    }
}

static XmFontList get_term_fontlist(int idx) {
    if (idx < 0 || idx > 6) return NULL;
    if (g_font_cache[idx]) return g_font_cache[idx];

    Display *dpy = XtDisplay(g_toplevel);
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s,%s,fixed", k_font_specs[idx][0], k_font_specs[idx][1]);

    char **missing = NULL;
    int nmissing = 0;
    char *def_str = NULL;
    XFontSet fs = XCreateFontSet(dpy, pattern, &missing, &nmissing, &def_str);
    if (missing) XFreeStringList(missing);
    if (fs) {
        XmFontListEntry entry = XmFontListEntryCreate(XmFONTLIST_DEFAULT_TAG, XmFONT_IS_FONTSET, (XtPointer)fs);
        g_font_cache[idx] = XmFontListAppendEntry(NULL, entry);
        XmFontListEntryFree(&entry);
        return g_font_cache[idx];
    }

    XFontStruct *xf = XLoadQueryFont(dpy, k_font_specs[idx][1]);
    if (!xf) xf = XLoadQueryFont(dpy, "fixed");
    if (xf) {
        XmFontListEntry entry = XmFontListEntryCreate(XmFONTLIST_DEFAULT_TAG, XmFONT_IS_FONT, (XtPointer)xf);
        g_font_cache[idx] = XmFontListAppendEntry(NULL, entry);
        XmFontListEntryFree(&entry);
    }
    return g_font_cache[idx];
}

/* ---------------- /proc Working Directory & Foreground Process Inspection ---------------- */
static void abbreviate_home(const char *path, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        size_t hlen = strlen(home);
        if (strncmp(path, home, hlen) == 0 && (path[hlen] == '/' || path[hlen] == '\0')) {
            if (path[hlen] == '\0') {
                snprintf(out, cap, "~");
            } else {
                snprintf(out, cap, "~%s", path + hlen);
            }
            return;
        }
    }
    snprintf(out, cap, "%s", path);
}

static int read_pid_cwd(pid_t pid, char *out, size_t cap) {
    if (pid <= 0) return 0;
    char link_path[64];
    snprintf(link_path, sizeof(link_path), "/proc/%d/cwd", (int)pid);
    ssize_t n = readlink(link_path, out, cap - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

static void read_fg_comm(pid_t shell_pid, char *out, size_t cap) {
    out[0] = '\0';
    if (shell_pid <= 0) return;

    char stat_path[64], buf[512];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", (int)shell_pid);
    FILE *fp = fopen(stat_path, "r");
    if (!fp) return;
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return;
    }
    fclose(fp);

    /* Parse comm inside (...) and tpgid (5th field after ')') */
    char *lp = strchr(buf, '(');
    char *rp = strrchr(buf, ')');
    if (!lp || !rp || rp <= lp) return;

    char shell_comm[64];
    size_t clen = (size_t)(rp - lp - 1);
    if (clen >= sizeof(shell_comm)) clen = sizeof(shell_comm) - 1;
    memcpy(shell_comm, lp + 1, clen);
    shell_comm[clen] = '\0';

    char state = 0;
    int ppid = 0, pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
    if (sscanf(rp + 2, "%c %d %d %d %d %d", &state, &ppid, &pgrp, &session, &tty_nr, &tpgid) == 6 &&
        tpgid > 0 && tpgid != (int)shell_pid) {
        char fg_path[64], fg_buf[128];
        snprintf(fg_path, sizeof(fg_path), "/proc/%d/comm", tpgid);
        FILE *ff = fopen(fg_path, "r");
        if (ff) {
            if (fgets(fg_buf, sizeof(fg_buf), ff)) {
                char *nl = strpbrk(fg_buf, "\r\n");
                if (nl) *nl = '\0';
                if (fg_buf[0]) {
                    snprintf(out, cap, "%.*s", (int)(cap - 1), fg_buf);
                    fclose(ff);
                    return;
                }
            }
            fclose(ff);
        }
    }
    snprintf(out, cap, "%.*s", (int)(cap - 1), shell_comm);
}

static void refresh_tab_metadata(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    TermTab *t = &g_tabs[idx];
    if (t->closing || !t->term) return;

    if (t->shell_pid <= 0) {
        int pid_val = 0;
        XtVaGetValues(t->term, DtNsubprocessPid, &pid_val, NULL);
        if (pid_val > 0) t->shell_pid = (pid_t)pid_val;
    }

    char cwd[PATH_MAX] = "";
    char proc[64] = "";
    if (read_pid_cwd(t->shell_pid, cwd, sizeof(cwd))) {
        snprintf(t->last_cwd, sizeof(t->last_cwd), "%s", cwd);
    }
    read_fg_comm(t->shell_pid, proc, sizeof(proc));
    if (proc[0]) {
        snprintf(t->last_proc, sizeof(t->last_proc), "%s", proc);
    } else {
        snprintf(proc, sizeof(proc), "%s", t->last_proc);
    }

    char short_cwd[80] = "~";
    if (t->last_cwd[0]) {
        abbreviate_home(t->last_cwd, short_cwd, sizeof(short_cwd));
        /* Keep tab pill compact: if path is long, show basename */
        if (strlen(short_cwd) > 18) {
            const char *slash = strrchr(short_cwd, '/');
            if (slash && slash[1]) {
                char tmp[80];
                snprintf(tmp, sizeof(tmp), ".../%.60s", slash + 1);
                snprintf(short_cwd, sizeof(short_cwd), "%s", tmp);
            }
        }
    }

    if (proc[0] &&
        strcmp(proc, "bash") != 0 &&
        strcmp(proc, "sh") != 0 &&
        strcmp(proc, "zsh") != 0 &&
        strcmp(proc, "ksh") != 0 &&
        strcmp(proc, "dash") != 0) {
        snprintf(t->auto_title, sizeof(t->auto_title), "%.60s — %.48s", short_cwd, proc);
    } else {
        snprintf(t->auto_title, sizeof(t->auto_title), "%.80s", short_cwd);
    }

    char btn_label[180];
    const char *core = t->custom_title[0] ? t->custom_title : t->auto_title;
    snprintf(btn_label, sizeof(btn_label), "%d: %s", idx + 1, core[0] ? core : "dtTerm");
    set_xm_label(t->tab_btn, btn_label);
}

/* ---------------- Xt Actions (Smart Copy/Paste + Tab Hotkeys) ---------------- */
static Time extract_event_time(XEvent *ev) {
    if (!ev) return CurrentTime;
    if (ev->type == KeyPress || ev->type == KeyRelease) return ev->xkey.time;
    if (ev->type == ButtonPress || ev->type == ButtonRelease) return ev->xbutton.time;
    return CurrentTime;
}

static void act_smart_copy(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    int force_copy = (num_params && *num_params > 0 && params && params[0] &&
                      strcmp(params[0], "force") == 0);
    Time t = extract_event_time(ev);

    if (_DtTermPrimSelectCopyClipboard(w, t)) {
        Atom primary = XA_PRIMARY;
        _DtTermPrimSelectLoseSelection(w, &primary);
        _DtTermPrimSelectDisown(w);
        return;
    }
    if (!force_copy) {
        XtCallActionProc(w, "self-insert", ev, params, num_params ? *num_params : 0);
    }
}

static void act_smart_paste(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    Time t = extract_event_time(ev);
    if (_DtTermPrimSelectPasteClipboard(w, t)) {
        return;
    }
    XtCallActionProc(w, "copy-to", ev, params, num_params ? *num_params : 0);
}

static void spawn_new_window(const char *cwd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) { /* ignore */ }
        }
        execl(g_self_exe, "dtTerm", (char *)NULL);
        _exit(127);
    }
}

static const char *active_tab_cwd(void) {
    if (g_active_idx >= 0 && g_active_idx < g_num_tabs) {
        refresh_tab_metadata(g_active_idx);
        if (g_tabs[g_active_idx].last_cwd[0]) {
            return g_tabs[g_active_idx].last_cwd;
        }
    }
    return NULL;
}

static void act_new_tab(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev; (void)params; (void)num_params;
    int idx = create_new_tab(active_tab_cwd(), 0, 0);
    if (idx >= 0) activate_tab(idx);
}

static void act_close_tab(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev; (void)params; (void)num_params;
    if (g_active_idx >= 0) close_tab_at(g_active_idx);
}

static void act_next_tab(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev; (void)params; (void)num_params;
    if (g_num_tabs > 1) {
        activate_tab((g_active_idx + 1) % g_num_tabs);
    }
}

static void act_prev_tab(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev; (void)params; (void)num_params;
    if (g_num_tabs > 1) {
        activate_tab((g_active_idx - 1 + g_num_tabs) % g_num_tabs);
    }
}

static void act_goto_tab(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev;
    if (!num_params || *num_params == 0 || !params || !params[0]) return;
    int n = atoi(params[0]) - 1;
    if (n >= 0 && n < g_num_tabs) {
        activate_tab(n);
    }
}

static void act_new_window(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev; (void)params; (void)num_params;
    spawn_new_window(active_tab_cwd());
}

static void act_zoom(Widget w, XEvent *ev, String *params, Cardinal *num_params) {
    (void)w; (void)ev;
    if (!num_params || *num_params == 0 || !params || !params[0]) return;
    if (strcmp(params[0], "in") == 0) step_font_zoom(+1, 0);
    else if (strcmp(params[0], "out") == 0) step_font_zoom(-1, 0);
    else if (strcmp(params[0], "reset") == 0) step_font_zoom(0, 1);
}

static XtActionsRec g_term_actions[] = {
    {"dtterm-smart-copy",  act_smart_copy},
    {"dtterm-smart-paste", act_smart_paste},
    {"dtterm-new-tab",     act_new_tab},
    {"dtterm-close-tab",   act_close_tab},
    {"dtterm-next-tab",    act_next_tab},
    {"dtterm-prev-tab",    act_prev_tab},
    {"dtterm-goto-tab",    act_goto_tab},
    {"dtterm-new-window",  act_new_window},
    {"dtterm-zoom",        act_zoom},
};

static const char *k_term_translations =
    "Ctrl Shift <Key>t: dtterm-new-tab()\n"
    "Ctrl Shift <Key>T: dtterm-new-tab()\n"
    "Ctrl Shift <Key>w: dtterm-close-tab()\n"
    "Ctrl Shift <Key>W: dtterm-close-tab()\n"
    "Ctrl Shift <Key>n: dtterm-new-window()\n"
    "Ctrl Shift <Key>N: dtterm-new-window()\n"
    "Ctrl <Key>Next: dtterm-next-tab()\n"
    "Ctrl <Key>Prior: dtterm-prev-tab()\n"
    "Ctrl Shift <Key>ISO_Left_Tab: dtterm-prev-tab()\n"
    "Ctrl Shift <Key>Tab: dtterm-prev-tab()\n"
    "~Shift Ctrl <Key>Tab: dtterm-next-tab()\n"
    "Mod1 <Key>1: dtterm-goto-tab(1)\n"
    "Mod1 <Key>2: dtterm-goto-tab(2)\n"
    "Mod1 <Key>3: dtterm-goto-tab(3)\n"
    "Mod1 <Key>4: dtterm-goto-tab(4)\n"
    "Mod1 <Key>5: dtterm-goto-tab(5)\n"
    "Mod1 <Key>6: dtterm-goto-tab(6)\n"
    "Mod1 <Key>7: dtterm-goto-tab(7)\n"
    "Mod1 <Key>8: dtterm-goto-tab(8)\n"
    "Mod1 <Key>9: dtterm-goto-tab(9)\n"
    "Ctrl <Key>plus: dtterm-zoom(in)\n"
    "Ctrl Shift <Key>plus: dtterm-zoom(in)\n"
    "Ctrl <Key>equal: dtterm-zoom(in)\n"
    "Ctrl <Key>KP_Add: dtterm-zoom(in)\n"
    "Ctrl <Key>minus: dtterm-zoom(out)\n"
    "Ctrl <Key>KP_Subtract: dtterm-zoom(out)\n"
    "Ctrl <Key>0: dtterm-zoom(reset)\n"
    "Ctrl <Key>KP_0: dtterm-zoom(reset)\n"
    "Ctrl Shift <Key>c: dtterm-smart-copy(force)\n"
    "Ctrl Shift <Key>C: dtterm-smart-copy(force)\n"
    "Ctrl Shift <Key>v: dtterm-smart-paste()\n"
    "Ctrl Shift <Key>V: dtterm-smart-paste()\n"
    "~Shift ~Meta ~Alt Ctrl <Key>c: dtterm-smart-copy()\n"
    "~Shift ~Meta ~Alt Ctrl <Key>v: dtterm-smart-paste()";

/* ---------------- Tab Styling & Switching ---------------- */
static void focus_active_term_timeout(XtPointer client_data, XtIntervalId *id) {
    (void)client_data; (void)id;
    if (g_active_idx >= 0 && g_active_idx < g_num_tabs) {
        Widget term = g_tabs[g_active_idx].term;
        if (term && XtIsRealized(term)) {
            XmProcessTraversal(term, XmTRAVERSE_CURRENT);
        }
    }
}

static void update_window_and_status(void) {
    if (g_active_idx < 0 || g_active_idx >= g_num_tabs) return;
    TermTab *t = &g_tabs[g_active_idx];
    refresh_tab_metadata(g_active_idx);

    const char *tab_name = t->custom_title[0] ? t->custom_title :
                           (t->auto_title[0] ? t->auto_title : "~");

    char win_title[320];
    if (g_fixed_win_title[0]) {
        if (g_num_tabs > 1) {
            snprintf(win_title, sizeof(win_title), "%s [%d/%d] — dtTerm",
                     g_fixed_win_title, g_active_idx + 1, g_num_tabs);
        } else {
            snprintf(win_title, sizeof(win_title), "%s", g_fixed_win_title);
        }
    } else {
        if (g_num_tabs > 1) {
            snprintf(win_title, sizeof(win_title), "dtTerm — %s (%d/%d)",
                     tab_name, g_active_idx + 1, g_num_tabs);
        } else {
            snprintf(win_title, sizeof(win_title), "dtTerm — %s", tab_name);
        }
    }
    XtVaSetValues(g_toplevel, XmNtitle, win_title, NULL);

    char left_txt[384];
    char pretty_cwd[256] = "~";
    if (t->last_cwd[0]) abbreviate_home(t->last_cwd, pretty_cwd, sizeof(pretty_cwd));
    if (t->shell_pid > 0) {
        snprintf(left_txt, sizeof(left_txt), " %s  (%s, PID %d)",
                 pretty_cwd, t->last_proc[0] ? t->last_proc : "shell", (int)t->shell_pid);
    } else {
        snprintf(left_txt, sizeof(left_txt), " %s", pretty_cwd);
    }
    set_xm_label(g_st_left, left_txt);

    char right_txt[128];
    const char *sz = (g_font_idx >= 0 && g_font_idx <= 6) ? k_font_names[g_font_idx] : "Default";
    snprintf(right_txt, sizeof(right_txt), "Tab %d of %d  |  Font: %s ",
             g_active_idx + 1, g_num_tabs, sz);
    set_xm_label(g_st_right, right_txt);
}

static void refresh_tab_bar_styles(void) {
    int show_close = (g_num_tabs > 1);
    for (int i = 0; i < g_num_tabs; i++) {
        TermTab *t = &g_tabs[i];
        int is_act = (i == g_active_idx);
        Pixel bg = is_act ? g_pal.bg_tab_active : g_pal.bg_tab_inactive;

        XmChangeColor(t->tab_frame, bg);
        XmChangeColor(t->tab_form, bg);
        XmChangeColor(t->tab_btn, bg);
        XmChangeColor(t->close_btn, bg);

        XtVaSetValues(t->tab_frame,
                      XmNshadowType, is_act ? XmSHADOW_OUT : XmSHADOW_ETCHED_OUT,
                      XmNshadowThickness, 2,
                      NULL);

        XtVaSetValues(t->tab_btn,
                      XmNforeground, g_pal.fg_chrome,
                      NULL);
        if (is_act && g_ui_bold_fl) {
            XtVaSetValues(t->tab_btn, XmNfontList, g_ui_bold_fl, NULL);
        } else if (!is_act && g_ui_norm_fl) {
            XtVaSetValues(t->tab_btn, XmNfontList, g_ui_norm_fl, NULL);
        }

        if (show_close) {
            if (!XtIsManaged(t->close_btn)) XtManageChild(t->close_btn);
            XtVaSetValues(t->tab_btn,
                          XmNrightAttachment, XmATTACH_WIDGET,
                          XmNrightWidget, t->close_btn,
                          XmNrightOffset, 2,
                          NULL);
        } else {
            if (XtIsManaged(t->close_btn)) XtUnmanageChild(t->close_btn);
            XtVaSetValues(t->tab_btn,
                          XmNrightAttachment, XmATTACH_FORM,
                          XmNrightOffset, 4,
                          NULL);
        }
        refresh_tab_metadata(i);
    }
}

static void activate_tab(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;

    /* Manage target page BEFORE unmanaging previous page to keep stack geometry steady */
    if (!XtIsManaged(g_tabs[idx].page_form)) {
        XtManageChild(g_tabs[idx].page_form);
    }
    for (int i = 0; i < g_num_tabs; i++) {
        if (i != idx && XtIsManaged(g_tabs[i].page_form)) {
            XtUnmanageChild(g_tabs[i].page_form);
        }
    }

    g_active_idx = idx;
    refresh_tab_bar_styles();
    update_window_and_status();

    if (XtIsRealized(g_tabs[idx].term)) {
        XmProcessTraversal(g_tabs[idx].term, XmTRAVERSE_CURRENT);
        XtAppAddTimeOut(g_app, 20, focus_active_term_timeout, NULL);
    }
}

static int find_tab_by_id(int id) {
    for (int i = 0; i < g_num_tabs; i++) {
        if (g_tabs[i].id == id) return i;
    }
    return -1;
}

static void close_tab_at(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    if (g_num_tabs <= 1) {
        cleanup_and_exit(0);
    }

    TermTab victim = g_tabs[idx];
    victim.closing = 1;

    /* Switch away first if closing active tab */
    int next_active = g_active_idx;
    if (idx == g_active_idx) {
        int candidate = (idx + 1 < g_num_tabs) ? (idx + 1) : (idx - 1);
        if (candidate >= 0 && candidate < g_num_tabs && !XtIsManaged(g_tabs[candidate].page_form)) {
            XtManageChild(g_tabs[candidate].page_form);
        }
    }

    if (victim.shell_pid > 0) {
        kill(victim.shell_pid, SIGHUP);
    }

    if (victim.page_form) {
        XtUnmanageChild(victim.page_form);
        XtDestroyWidget(victim.page_form);
    }
    if (victim.tab_frame) {
        XtUnmanageChild(victim.tab_frame);
        XtDestroyWidget(victim.tab_frame);
    }

    for (int i = idx; i + 1 < g_num_tabs; i++) {
        g_tabs[i] = g_tabs[i + 1];
    }
    g_num_tabs--;

    if (next_active >= g_num_tabs) next_active = g_num_tabs - 1;
    else if (idx < next_active) next_active--;

    g_active_idx = -1;
    activate_tab(next_active);
}

static void deferred_subproc_close_cb(XtPointer client_data, XtIntervalId *timer_id) {
    (void)timer_id;
    int tab_id = (int)(intptr_t)client_data;
    int idx = find_tab_by_id(tab_id);
    if (idx >= 0) {
        close_tab_at(idx);
    }
}

static void on_term_subproc_exit(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int tab_id = (int)(intptr_t)client_data;
    int idx = find_tab_by_id(tab_id);
    if (idx < 0 || g_tabs[idx].closing) return;
    g_tabs[idx].closing = 1;
    g_tabs[idx].shell_pid = 0;
    XtAppAddTimeOut(g_app, 15, deferred_subproc_close_cb, (XtPointer)(intptr_t)tab_id);
}

/* ---------------- Tab & Terminal Mouse Callbacks ---------------- */
static void on_tab_btn_activate(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int tab_id = (int)(intptr_t)client_data;
    int idx = find_tab_by_id(tab_id);
    if (idx >= 0) activate_tab(idx);
}

static void on_tab_close_activate(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int tab_id = (int)(intptr_t)client_data;
    int idx = find_tab_by_id(tab_id);
    if (idx >= 0) close_tab_at(idx);
}

static void on_tab_btn_event(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w;
    if (ev->type != ButtonPress) return;
    int tab_id = (int)(intptr_t)client_data;
    int idx = find_tab_by_id(tab_id);
    if (idx < 0) return;

    if (ev->xbutton.button == Button2) {
        /* Middle-click closes tab */
        *cont = False;
        close_tab_at(idx);
    } else if (ev->xbutton.button == Button3 && g_tab_popup) {
        *cont = False;
        g_popup_tab_idx = idx;
        XmMenuPosition(g_tab_popup, &ev->xbutton);
        XtManageChild(g_tab_popup);
    }
}

static void on_term_right_click(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w; (void)client_data;
    if (ev->type == ButtonPress && ev->xbutton.button == Button3 &&
        (ev->xbutton.state & (ShiftMask | ControlMask | Mod1Mask)) == 0 && g_term_popup) {
        *cont = False;
        XmMenuPosition(g_term_popup, &ev->xbutton);
        XtManageChild(g_term_popup);
    }
}

/* ---------------- Tab Creation ---------------- */
static int create_new_tab(const char *cwd_override, int use_init_cmd, int console_mode) {
    if (g_num_tabs >= MAX_TABS) return -1;

    char prev_cwd[PATH_MAX] = "";
    int changed_dir = 0;
    if (cwd_override && cwd_override[0]) {
        if (getcwd(prev_cwd, sizeof(prev_cwd)) != NULL && chdir(cwd_override) == 0) {
            changed_dir = 1;
        }
    }

    int idx = g_num_tabs;
    TermTab *t = &g_tabs[idx];
    memset(t, 0, sizeof(*t));
    t->id = g_next_tab_id++;
    if (cwd_override && cwd_override[0]) {
        snprintf(t->last_cwd, sizeof(t->last_cwd), "%s", cwd_override);
    } else if (getcwd(t->last_cwd, sizeof(t->last_cwd)) == NULL) {
        snprintf(t->last_cwd, sizeof(t->last_cwd), "%s", getenv("HOME") ? getenv("HOME") : "/");
    }
    if (console_mode) {
        snprintf(t->custom_title, sizeof(t->custom_title), "Console");
    }

    /* 1. Create Tab Pill in g_tabs_rc */
    t->tab_frame = XtVaCreateManagedWidget(
        "tabFrame", xmFrameWidgetClass, g_tabs_rc,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 2,
        XmNmarginWidth, 1,
        XmNmarginHeight, 1,
        NULL);

    t->tab_form = XtVaCreateManagedWidget(
        "tabForm", xmFormWidgetClass, t->tab_frame,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        NULL);

    XmString x_lbl = XmStringCreateLocalized("x");
    t->close_btn = XtVaCreateWidget(
        "tabCloseBtn", xmPushButtonWidgetClass, t->tab_form,
        XmNlabelString, x_lbl,
        XmNshadowThickness, 1,
        XmNhighlightThickness, 0,
        XmNmarginWidth, 4,
        XmNmarginHeight, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopOffset, 2,
        XmNbottomOffset, 2,
        XmNrightOffset, 2,
        NULL);
    XmStringFree(x_lbl);

    XmString init_lbl = XmStringCreateLocalized("1: ~");
    t->tab_btn = XtVaCreateManagedWidget(
        "tabSelectBtn", xmPushButtonWidgetClass, t->tab_form,
        XmNlabelString, init_lbl,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNshadowThickness, 0,
        XmNhighlightThickness, 0,
        XmNmarginWidth, 7,
        XmNmarginHeight, 3,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(init_lbl);

    XtAddCallback(t->tab_btn, XmNactivateCallback, on_tab_btn_activate, (XtPointer)(intptr_t)t->id);
    XtAddCallback(t->close_btn, XmNactivateCallback, on_tab_close_activate, (XtPointer)(intptr_t)t->id);
    XtAddEventHandler(t->tab_btn, ButtonPressMask, False, on_tab_btn_event, (XtPointer)(intptr_t)t->id);

    /* 2. Create Terminal Page inside g_stack_form (unmanaged until activated) */
    t->page_form = XtVaCreateWidget(
        "termPage", xmFormWidgetClass, g_stack_form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        NULL);

    t->sw = XtVaCreateManagedWidget(
        "dtTermScrolledWindow", xmScrolledWindowWidgetClass, t->page_form,
        XmNscrollingPolicy, XmAPPLICATION_DEFINED,
        XmNvisualPolicy, XmVARIABLE,
        XmNscrollBarDisplayPolicy, XmSTATIC,
        XmNshadowThickness, 0,
        XmNscrolledWindowMarginWidth, 0,
        XmNscrolledWindowMarginHeight, 0,
        XmNspacing, 2,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    Arg targs[24];
    Cardinal n = 0;
    XtSetArg(targs[n], DtNrows, (short)g_init_rows); n++;
    XtSetArg(targs[n], DtNcolumns, (short)g_init_cols); n++;
    XtSetArg(targs[n], DtNsaveLines, "4096"); n++;
    XtSetArg(targs[n], DtNjumpScroll, True); n++;
    XtSetArg(targs[n], DtNshadowType, DtSHADOW_IN); n++;
    XtSetArg(targs[n], XmNshadowThickness, 1); n++;
    XtSetArg(targs[n], DtNmarginWidth, 4); n++;
    XtSetArg(targs[n], DtNmarginHeight, 3); n++;
    XtSetArg(targs[n], DtNsubprocessTerminationCatch, True); n++;
    XtSetArg(targs[n], XmNbackground, g_pal.term_bg); n++;
    XtSetArg(targs[n], XmNforeground, g_pal.term_fg); n++;

    if (console_mode) {
        XtSetArg(targs[n], DtNconsoleMode, True); n++;
    }
    if (use_init_cmd && g_init_cmd_prog) {
        XtSetArg(targs[n], DtNsubprocessLoginShell, False); n++;
        XtSetArg(targs[n], DtNsubprocessCmd, g_init_cmd_prog); n++;
        if (g_init_cmd_argv) {
            XtSetArg(targs[n], DtNsubprocessArgv, g_init_cmd_argv); n++;
        }
    } else {
        XtSetArg(targs[n], DtNsubprocessLoginShell, g_login_shell ? True : False); n++;
    }

    if (g_font_idx >= 0 && g_font_idx <= 6) {
        XmFontList fl = get_term_fontlist(g_font_idx);
        if (fl) {
            XtSetArg(targs[n], DtNuserFont, fl); n++;
        }
    }

    t->term = DtCreateTerm(t->sw, "dtTerm", targs, n);
    XtManageChild(t->term);

    t->vsb = XtVaCreateManagedWidget(
        "scrollBar", xmScrollBarWidgetClass, t->sw,
        XmNorientation, XmVERTICAL,
        XmNwidth, 16,
        NULL);

    XmScrolledWindowSetAreas(t->sw, NULL, t->vsb, t->term);
    XtVaSetValues(t->term, DtNverticalScrollBar, t->vsb, NULL);
    if (!g_show_vscroll && XtIsManaged(t->vsb)) {
        XtUnmanageChild(t->vsb);
    }

    XmChangeColor(t->sw, g_pal.bg_chrome);
    XmChangeColor(t->vsb, g_pal.bg_chrome);

    XtTranslations trans = XtParseTranslationTable(k_term_translations);
    if (trans) {
        XtOverrideTranslations(t->term, trans);
    }

    XtAddCallback(t->term, DtNsubprocessTerminationCallback,
                  on_term_subproc_exit, (XtPointer)(intptr_t)t->id);
    XtAddEventHandler(t->term, ButtonPressMask, False, on_term_right_click, NULL);

    int pid_val = 0;
    XtVaGetValues(t->term, DtNsubprocessPid, &pid_val, NULL);
    if (pid_val > 0) t->shell_pid = (pid_t)pid_val;

    if (changed_dir && prev_cwd[0]) {
        if (chdir(prev_cwd) != 0) { /* ignore */ }
    }

    g_num_tabs++;
    refresh_tab_bar_styles();
    return idx;
}

/* ---------------- Font Zoom & Theme Switching ---------------- */
static void step_font_zoom(int delta, int reset_default) {
    if (reset_default) {
        g_font_idx = 3; /* Medium */
    } else {
        int base = (g_font_idx < 0) ? 3 : g_font_idx;
        int next = base + delta;
        if (next < 0) next = 0;
        if (next > 6) next = 6;
        g_font_idx = next;
    }

    XmFontList fl = get_term_fontlist(g_font_idx);
    if (fl) {
        for (int i = 0; i < g_num_tabs; i++) {
            if (g_tabs[i].term) {
                XtVaSetValues(g_tabs[i].term, DtNuserFont, fl, NULL);
            }
        }
    }
    save_prefs();
    update_window_and_status();
}

static void apply_theme_all(TermTheme theme, int save_pref) {
    g_theme = theme;
    load_cde_palette(g_toplevel, &g_pal);

    for (int i = 0; i < g_num_tabs; i++) {
        if (g_tabs[i].term) {
            XtVaSetValues(g_tabs[i].term,
                          XmNbackground, g_pal.term_bg,
                          XmNforeground, g_pal.term_fg,
                          NULL);
            if (XtIsRealized(g_tabs[i].term)) {
                XClearArea(XtDisplay(g_tabs[i].term), XtWindow(g_tabs[i].term), 0, 0, 0, 0, True);
            }
        }
    }
    refresh_tab_bar_styles();
    if (save_pref) save_prefs();
}

/* ---------------- Rename Tab Dialog & Info Dialogs ---------------- */
typedef struct {
    int tab_id;
} RenameCtx;

static void on_rename_ok(Widget w, XtPointer client_data, XtPointer call_data) {
    int tab_id = (int)(intptr_t)client_data;
    XmSelectionBoxCallbackStruct *cbs = (XmSelectionBoxCallbackStruct *)call_data;
    char *txt = NULL;
    if (cbs && cbs->value && XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &txt) && txt) {
        int idx = find_tab_by_id(tab_id);
        if (idx >= 0) {
            /* Trim whitespace; empty string restores automatic title */
            char *p = txt;
            while (*p && isspace((unsigned char)*p)) p++;
            snprintf(g_tabs[idx].custom_title, sizeof(g_tabs[idx].custom_title), "%s", p);
            refresh_tab_bar_styles();
            update_window_and_status();
        }
        XtFree(txt);
    }
    XtDestroyWidget(XtParent(w));
    focus_active_term_timeout(NULL, NULL);
}

static void on_rename_cancel(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data; (void)call_data;
    XtDestroyWidget(XtParent(w));
    focus_active_term_timeout(NULL, NULL);
}

static void prompt_rename_tab(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    TermTab *t = &g_tabs[idx];

    XmString prompt = XmStringCreateLocalized("Tab Title (leave blank for automatic folder/process title):");
    XmString val = XmStringCreateLocalized(t->custom_title[0] ? t->custom_title : t->auto_title);
    XmString title = XmStringCreateLocalized("dtTerm — Rename Tab");

    Arg args[8];
    Cardinal n = 0;
    XtSetArg(args[n], XmNselectionLabelString, prompt); n++;
    XtSetArg(args[n], XmNtextString, val); n++;
    XtSetArg(args[n], XmNdialogTitle, title); n++;

    Widget dlg = XmCreatePromptDialog(g_toplevel, "renameTabDialog", args, n);
    XmStringFree(prompt);
    XmStringFree(val);
    XmStringFree(title);

    Widget help_btn = XmSelectionBoxGetChild(dlg, XmDIALOG_HELP_BUTTON);
    if (help_btn) XtUnmanageChild(help_btn);

    XmChangeColor(dlg, g_pal.bg_chrome);
    Widget text_w = XmSelectionBoxGetChild(dlg, XmDIALOG_TEXT);
    if (text_w) {
        XmChangeColor(text_w, g_pal.bg_well);
        XtVaSetValues(text_w, XmNforeground, g_pal.fg_well, XmNcolumns, 34, NULL);
    }

    XtAddCallback(dlg, XmNokCallback, on_rename_ok, (XtPointer)(intptr_t)t->id);
    XtAddCallback(dlg, XmNcancelCallback, on_rename_cancel, NULL);
    XtManageChild(dlg);
}

static void show_info_dialog(const char *dlg_title, const char *body) {
    XmString t = XmStringCreateLocalized((char *)dlg_title);
    XmString m = XmStringCreateLtoR((char *)body, XmFONTLIST_DEFAULT_TAG);
    Arg args[6];
    Cardinal n = 0;
    XtSetArg(args[n], XmNdialogTitle, t); n++;
    XtSetArg(args[n], XmNmessageString, m); n++;
    Widget dlg = XmCreateInformationDialog(g_toplevel, "infoDialog", args, n);
    XmStringFree(t);
    XmStringFree(m);

    Widget cancel_b = XmMessageBoxGetChild(dlg, XmDIALOG_CANCEL_BUTTON);
    Widget help_b   = XmMessageBoxGetChild(dlg, XmDIALOG_HELP_BUTTON);
    if (cancel_b) XtUnmanageChild(cancel_b);
    if (help_b)   XtUnmanageChild(help_b);
    XmChangeColor(dlg, g_pal.bg_chrome);
    XtManageChild(dlg);
}

/* ---------------- Menu & Toolbar Callbacks ---------------- */
static void cb_new_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    int idx = create_new_tab(active_tab_cwd(), 0, 0);
    if (idx >= 0) activate_tab(idx);
}

static void cb_dup_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    int src = (g_popup_tab_idx >= 0 && g_popup_tab_idx < g_num_tabs) ? g_popup_tab_idx : g_active_idx;
    g_popup_tab_idx = -1;
    const char *cwd = NULL;
    if (src >= 0 && src < g_num_tabs) {
        refresh_tab_metadata(src);
        cwd = g_tabs[src].last_cwd;
    }
    int idx = create_new_tab(cwd, 0, 0);
    if (idx >= 0) activate_tab(idx);
}

static void cb_new_window(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    spawn_new_window(active_tab_cwd());
}

static void cb_rename_active_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    prompt_rename_tab(g_active_idx);
}

static void cb_popup_rename_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    int idx = (g_popup_tab_idx >= 0 && g_popup_tab_idx < g_num_tabs) ? g_popup_tab_idx : g_active_idx;
    g_popup_tab_idx = -1;
    prompt_rename_tab(idx);
}

static void cb_close_active_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx >= 0) close_tab_at(g_active_idx);
}

static void cb_popup_close_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    int idx = (g_popup_tab_idx >= 0 && g_popup_tab_idx < g_num_tabs) ? g_popup_tab_idx : g_active_idx;
    g_popup_tab_idx = -1;
    if (idx >= 0) close_tab_at(idx);
}

static void cb_popup_close_other_tabs(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    int idx = (g_popup_tab_idx >= 0 && g_popup_tab_idx < g_num_tabs) ? g_popup_tab_idx : g_active_idx;
    g_popup_tab_idx = -1;
    if (idx < 0 || idx >= g_num_tabs) return;
    int keep_id = g_tabs[idx].id;
    activate_tab(idx);
    while (g_num_tabs > 1) {
        int kill_idx = (g_tabs[0].id == keep_id) ? 1 : 0;
        close_tab_at(kill_idx);
    }
}

static void cb_exit(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    cleanup_and_exit(0);
}

static void cb_copy(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx < 0) return;
    Widget term = g_tabs[g_active_idx].term;
    if (_DtTermPrimSelectCopyClipboard(term, CurrentTime)) {
        Atom primary = XA_PRIMARY;
        _DtTermPrimSelectLoseSelection(term, &primary);
        _DtTermPrimSelectDisown(term);
    }
    focus_active_term_timeout(NULL, NULL);
}

static void cb_paste(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx < 0) return;
    Widget term = g_tabs[g_active_idx].term;
    if (!_DtTermPrimSelectPasteClipboard(term, CurrentTime)) {
        XtCallActionProc(term, "copy-to", NULL, NULL, 0);
    }
    focus_active_term_timeout(NULL, NULL);
}

static void cb_select_all(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx < 0) return;
    _DtTermPrimSelectAll(g_tabs[g_active_idx].term, NULL, NULL, NULL);
    focus_active_term_timeout(NULL, NULL);
}

static void cb_clear_buffer(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx < 0) return;
    Widget term = g_tabs[g_active_idx].term;
    _DtTermFuncClearBuffer(term, 0, 0);
    _DtTermPrimCursorOn(term);
    if (XtIsRealized(term)) {
        XClearArea(XtDisplay(term), XtWindow(term), 0, 0, 0, 0, True);
    }
    focus_active_term_timeout(NULL, NULL);
}

static void cb_soft_reset(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx < 0) return;
    _DtTermFuncSoftReset(g_tabs[g_active_idx].term, 0, 0);
    focus_active_term_timeout(NULL, NULL);
}

static void cb_hard_reset(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    if (g_active_idx < 0) return;
    Widget term = g_tabs[g_active_idx].term;
    _DtTermFuncHardReset(term, 0, 0);
    if (XtIsRealized(term)) {
        XClearArea(XtDisplay(term), XtWindow(term), 0, 0, 0, 0, True);
    }
    focus_active_term_timeout(NULL, NULL);
}

static void cb_next_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    act_next_tab(NULL, NULL, NULL, NULL);
}

static void cb_prev_tab(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    act_prev_tab(NULL, NULL, NULL, NULL);
}

static void cb_zoom_in(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    step_font_zoom(+1, 0);
    focus_active_term_timeout(NULL, NULL);
}

static void cb_zoom_out(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    step_font_zoom(-1, 0);
    focus_active_term_timeout(NULL, NULL);
}

static void cb_zoom_reset(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    step_font_zoom(0, 1);
    focus_active_term_timeout(NULL, NULL);
}

static void cb_toggle_menubar(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    g_show_menubar = !g_show_menubar;
    if (g_view_mb_tb) {
        XmToggleButtonSetState(g_view_mb_tb, g_show_menubar ? True : False, False);
    }
    if (g_show_menubar) {
        XtManageChild(g_menubar);
        XtVaSetValues(g_tabbar_frame,
                      XmNtopAttachment, XmATTACH_WIDGET,
                      XmNtopWidget, g_menubar,
                      NULL);
    } else {
        XtVaSetValues(g_tabbar_frame,
                      XmNtopAttachment, XmATTACH_FORM,
                      NULL);
        XtUnmanageChild(g_menubar);
    }
    focus_active_term_timeout(NULL, NULL);
}

static void cb_toggle_scrollbar(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    g_show_vscroll = !g_show_vscroll;
    if (g_view_sb_tb) {
        XmToggleButtonSetState(g_view_sb_tb, g_show_vscroll ? True : False, False);
    }
    for (int i = 0; i < g_num_tabs; i++) {
        if (!g_tabs[i].vsb) continue;
        if (g_show_vscroll && !XtIsManaged(g_tabs[i].vsb)) {
            XtManageChild(g_tabs[i].vsb);
        } else if (!g_show_vscroll && XtIsManaged(g_tabs[i].vsb)) {
            XtUnmanageChild(g_tabs[i].vsb);
        }
    }
    save_prefs();
    focus_active_term_timeout(NULL, NULL);
}

static void cb_set_theme(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)call;
    TermTheme th = (TermTheme)(intptr_t)cd;
    apply_theme_all(th, 1);
    focus_active_term_timeout(NULL, NULL);
}

static void cb_shortcuts_help(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    show_info_dialog(
        "dtTerm — Keyboard Shortcuts",
        "dtTerm Multi-Tab Shortcuts:\n"
        "\n"
        "  Ctrl+Shift+T        Open New Tab (same folder)\n"
        "  Ctrl+Shift+W        Close Active Tab\n"
        "  Ctrl+Shift+N        Open New dtTerm Window\n"
        "  Ctrl+Tab / PgDn     Switch to Next Tab\n"
        "  Ctrl+Shift+Tab      Switch to Previous Tab\n"
        "  Alt+1 .. Alt+9      Jump Directly to Tab 1..9\n"
        "  Middle-Click Tab    Close Tab\n"
        "  Right-Click Tab     Rename / Duplicate / Close Other Tabs\n"
        "\n"
        "Clipboard & Display:\n"
        "  Ctrl+C              Copy if text selected; ^C (SIGINT) otherwise\n"
        "  Ctrl+V              Paste from Clipboard\n"
        "  Ctrl++ / Ctrl+-     Zoom Font In / Out\n"
        "  Ctrl+0              Reset Font Size");
}

static void cb_about(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    show_info_dialog(
        "About dtTerm",
        "dtTerm — Native Multi-Tab CDE Terminal Emulator\n"
        "\n"
        "Built on OSF/Motif 2.3 and CDE libDtTerm (DtCreateTerm)\n"
        "with Smart Ctrl+C / Ctrl+V Clipboard, Working Directory\n"
        "Inheritance, Font Zoom, and Dynamic CDE Palette Sync.");
}

/* Periodic timer to keep tab titles (cwd / foreground command) updated */
static void poll_tab_titles_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data; (void)id;
    for (int i = 0; i < g_num_tabs; i++) {
        refresh_tab_metadata(i);
    }
    update_window_and_status();
    XtAppAddTimeOut(g_app, 900, poll_tab_titles_cb, NULL);
}

/* ---------------- UI Construction ---------------- */
static Widget add_menu_item(Widget menu, const char *label, const char *accel_text,
                            XtCallbackProc cb, XtPointer cd) {
    XmString lbl = XmStringCreateLocalized((char *)label);
    Arg args[8];
    Cardinal n = 0;
    XtSetArg(args[n], XmNlabelString, lbl); n++;
    XmString acc = NULL;
    if (accel_text && accel_text[0]) {
        acc = XmStringCreateLocalized((char *)accel_text);
        XtSetArg(args[n], XmNacceleratorText, acc); n++;
    }
    Widget item = XmCreatePushButton(menu, "menuItem", args, n);
    XtManageChild(item);
    XmStringFree(lbl);
    if (acc) XmStringFree(acc);
    XmChangeColor(item, g_pal.bg_chrome);
    XtVaSetValues(item, XmNforeground, g_pal.fg_chrome, NULL);
    if (cb) XtAddCallback(item, XmNactivateCallback, cb, cd);
    return item;
}

static void add_menu_sep(Widget menu) {
    Widget sep = XmCreateSeparator(menu, "sep", NULL, 0);
    XtManageChild(sep);
    XmChangeColor(sep, g_pal.bg_chrome);
}

static Widget build_pulldown(Widget menubar, const char *title, char mnemonic) {
    Widget menu = XmCreatePulldownMenu(menubar, "pulldown", NULL, 0);
    XmChangeColor(menu, g_pal.bg_chrome);

    XmString lbl = XmStringCreateLocalized((char *)title);
    Widget casc = XtVaCreateManagedWidget(
        "cascade", xmCascadeButtonWidgetClass, menubar,
        XmNlabelString, lbl,
        XmNmnemonic, (KeySym)mnemonic,
        XmNsubMenuId, menu,
        NULL);
    XmStringFree(lbl);
    XmChangeColor(casc, g_pal.bg_chrome);
    XtVaSetValues(casc, XmNforeground, g_pal.fg_chrome, NULL);
    return menu;
}

static void build_menus_and_popups(void) {
    Arg mb_args[4];
    Cardinal mn = 0;
    XtSetArg(mb_args[mn], XmNtopAttachment, XmATTACH_FORM); mn++;
    XtSetArg(mb_args[mn], XmNleftAttachment, XmATTACH_FORM); mn++;
    XtSetArg(mb_args[mn], XmNrightAttachment, XmATTACH_FORM); mn++;
    g_menubar = XmCreateMenuBar(g_main_form, "menuBar", mb_args, mn);
    XmChangeColor(g_menubar, g_pal.bg_chrome);
    if (g_show_menubar) XtManageChild(g_menubar);

    /* File / Window Menu */
    Widget m_file = build_pulldown(g_menubar, "File", 'F');
    add_menu_item(m_file, "New Tab",       "Ctrl+Shift+T", cb_new_tab, NULL);
    add_menu_item(m_file, "Duplicate Tab", NULL,           cb_dup_tab, NULL);
    add_menu_item(m_file, "New Window",    "Ctrl+Shift+N", cb_new_window, NULL);
    add_menu_item(m_file, "Rename Tab...", NULL,           cb_rename_active_tab, NULL);
    add_menu_sep(m_file);
    add_menu_item(m_file, "Close Tab",     "Ctrl+Shift+W", cb_close_active_tab, NULL);
    add_menu_item(m_file, "Exit dtTerm",   "Alt+F4",       cb_exit, NULL);

    /* Edit Menu */
    Widget m_edit = build_pulldown(g_menubar, "Edit", 'E');
    add_menu_item(m_edit, "Copy",             "Ctrl+C / Ctrl+Shift+C", cb_copy, NULL);
    add_menu_item(m_edit, "Paste",            "Ctrl+V / Ctrl+Shift+V", cb_paste, NULL);
    add_menu_item(m_edit, "Select All",       NULL,                    cb_select_all, NULL);
    add_menu_sep(m_edit);
    add_menu_item(m_edit, "Clear Scrollback", NULL,                    cb_clear_buffer, NULL);
    add_menu_item(m_edit, "Soft Reset",       NULL,                    cb_soft_reset, NULL);
    add_menu_item(m_edit, "Hard Reset",       NULL,                    cb_hard_reset, NULL);

    /* View Menu */
    Widget m_view = build_pulldown(g_menubar, "View", 'V');
    add_menu_item(m_view, "Next Tab",          "Ctrl+Tab",  cb_next_tab, NULL);
    add_menu_item(m_view, "Previous Tab",      "Ctrl+PgUp", cb_prev_tab, NULL);
    add_menu_sep(m_view);
    add_menu_item(m_view, "Zoom In Font",      "Ctrl++",    cb_zoom_in, NULL);
    add_menu_item(m_view, "Zoom Out Font",     "Ctrl+-",    cb_zoom_out, NULL);
    add_menu_item(m_view, "Default Font Size", "Ctrl+0",    cb_zoom_reset, NULL);
    add_menu_sep(m_view);

    XmString mb_lbl = XmStringCreateLocalized("Show Menu Bar");
    g_view_mb_tb = XtVaCreateManagedWidget(
        "showMbToggle", xmToggleButtonWidgetClass, m_view,
        XmNlabelString, mb_lbl,
        XmNset, g_show_menubar ? True : False,
        XmNindicatorOn, XmINDICATOR_CHECK,
        NULL);
    XmStringFree(mb_lbl);
    XmChangeColor(g_view_mb_tb, g_pal.bg_chrome);
    XtAddCallback(g_view_mb_tb, XmNvalueChangedCallback, cb_toggle_menubar, NULL);

    XmString sb_lbl = XmStringCreateLocalized("Show Scrollbar");
    g_view_sb_tb = XtVaCreateManagedWidget(
        "showSbToggle", xmToggleButtonWidgetClass, m_view,
        XmNlabelString, sb_lbl,
        XmNset, g_show_vscroll ? True : False,
        XmNindicatorOn, XmINDICATOR_CHECK,
        NULL);
    XmStringFree(sb_lbl);
    XmChangeColor(g_view_sb_tb, g_pal.bg_chrome);
    XtAddCallback(g_view_sb_tb, XmNvalueChangedCallback, cb_toggle_scrollbar, NULL);

    add_menu_sep(m_view);
    add_menu_item(m_view, "Theme: CDE Palette (Default)", NULL, cb_set_theme, (XtPointer)THEME_CDE_PALETTE);
    add_menu_item(m_view, "Theme: Dark Workstation",      NULL, cb_set_theme, (XtPointer)THEME_DARK);
    add_menu_item(m_view, "Theme: Green Phosphor",        NULL, cb_set_theme, (XtPointer)THEME_PHOSPHOR_GR);
    add_menu_item(m_view, "Theme: Amber Phosphor",        NULL, cb_set_theme, (XtPointer)THEME_PHOSPHOR_AM);

    /* Help Menu (right-aligned per OSF/Motif style guide) */
    Widget m_help = build_pulldown(g_menubar, "Help", 'H');
    Widget help_casc = NULL;
    WidgetList children = NULL;
    Cardinal nc = 0;
    XtVaGetValues(g_menubar, XmNchildren, &children, XmNnumChildren, &nc, NULL);
    if (nc > 0) help_casc = children[nc - 1];
    if (help_casc) XtVaSetValues(g_menubar, XmNmenuHelpWidget, help_casc, NULL);

    add_menu_item(m_help, "Keyboard Shortcuts...", NULL, cb_shortcuts_help, NULL);
    add_menu_item(m_help, "About dtTerm...",       NULL, cb_about, NULL);

    /* Right-click Popup Menu on Tab Pills */
    g_tab_popup = XmCreatePopupMenu(g_main_form, "tabPopup", NULL, 0);
    XmChangeColor(g_tab_popup, g_pal.bg_chrome);
    add_menu_item(g_tab_popup, "New Tab",          "Ctrl+Shift+T", cb_new_tab, NULL);
    add_menu_item(g_tab_popup, "Duplicate Tab",    NULL,           cb_dup_tab, NULL);
    add_menu_item(g_tab_popup, "Rename Tab...",    NULL,           cb_popup_rename_tab, NULL);
    add_menu_sep(g_tab_popup);
    add_menu_item(g_tab_popup, "Close Tab",        "Ctrl+Shift+W", cb_popup_close_tab, NULL);
    add_menu_item(g_tab_popup, "Close Other Tabs", NULL,           cb_popup_close_other_tabs, NULL);

    /* Right-click Popup Menu on Terminal Canvas */
    g_term_popup = XmCreatePopupMenu(g_main_form, "termPopup", NULL, 0);
    XmChangeColor(g_term_popup, g_pal.bg_chrome);
    add_menu_item(g_term_popup, "Copy",             "Ctrl+C",       cb_copy, NULL);
    add_menu_item(g_term_popup, "Paste",            "Ctrl+V",       cb_paste, NULL);
    add_menu_sep(g_term_popup);
    add_menu_item(g_term_popup, "New Tab",          "Ctrl+Shift+T", cb_new_tab, NULL);
    add_menu_item(g_term_popup, "Rename Tab...",    NULL,           cb_rename_active_tab, NULL);
    add_menu_item(g_term_popup, "Close Tab",        "Ctrl+Shift+W", cb_close_active_tab, NULL);
    add_menu_sep(g_term_popup);
    add_menu_item(g_term_popup, "Toggle Menu Bar",  NULL,           cb_toggle_menubar, NULL);
    add_menu_item(g_term_popup, "Clear Scrollback", NULL,           cb_clear_buffer, NULL);
}

static void build_main_ui(void) {
    g_main_form = XtVaCreateManagedWidget(
        "mainForm", xmFormWidgetClass, g_toplevel,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        NULL);
    XmChangeColor(g_main_form, g_pal.bg_chrome);

    build_menus_and_popups();

    /* 3D Beveled Multi-Tab Deck Strip */
    g_tabbar_frame = XtVaCreateManagedWidget(
        "tabBarFrame", xmFrameWidgetClass, g_main_form,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 2,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, g_show_menubar ? XmATTACH_WIDGET : XmATTACH_FORM,
        XmNtopWidget, g_menubar,
        NULL);
    XmChangeColor(g_tabbar_frame, g_pal.bg_chrome);

    g_tabbar_form = XtVaCreateManagedWidget(
        "tabBarForm", xmFormWidgetClass, g_tabbar_frame,
        XmNmarginWidth, 3,
        XmNmarginHeight, 2,
        NULL);
    XmChangeColor(g_tabbar_form, g_pal.bg_tabbar);

    /* Right-hand controls: [+ New Tab] [A-] [A+] */
    Widget right_rc = XtVaCreateManagedWidget(
        "tabRightControls", xmRowColumnWidgetClass, g_tabbar_form,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 3,
        XmNmarginWidth, 2,
        XmNmarginHeight, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmChangeColor(right_rc, g_pal.bg_tabbar);

    XmString plus_lbl = XmStringCreateLocalized("+ New Tab");
    g_new_tab_btn = XtVaCreateManagedWidget(
        "newTabBtn", xmPushButtonWidgetClass, right_rc,
        XmNlabelString, plus_lbl,
        XmNshadowThickness, 2,
        XmNhighlightThickness, 0,
        XmNmarginWidth, 8,
        XmNmarginHeight, 3,
        NULL);
    XmStringFree(plus_lbl);
    XmChangeColor(g_new_tab_btn, g_pal.bg_chrome);
    XtVaSetValues(g_new_tab_btn, XmNforeground, g_pal.fg_chrome, NULL);
    if (g_ui_bold_fl) XtVaSetValues(g_new_tab_btn, XmNfontList, g_ui_bold_fl, NULL);
    XtAddCallback(g_new_tab_btn, XmNactivateCallback, cb_new_tab, NULL);

    XmString zm_out = XmStringCreateLocalized("A-");
    g_zoom_out_btn = XtVaCreateManagedWidget(
        "zoomOutBtn", xmPushButtonWidgetClass, right_rc,
        XmNlabelString, zm_out,
        XmNshadowThickness, 2,
        XmNhighlightThickness, 0,
        XmNmarginWidth, 5,
        XmNmarginHeight, 3,
        NULL);
    XmStringFree(zm_out);
    XmChangeColor(g_zoom_out_btn, g_pal.bg_chrome);
    XtVaSetValues(g_zoom_out_btn, XmNforeground, g_pal.fg_chrome, NULL);
    XtAddCallback(g_zoom_out_btn, XmNactivateCallback, cb_zoom_out, NULL);

    XmString zm_in = XmStringCreateLocalized("A+");
    g_zoom_in_btn = XtVaCreateManagedWidget(
        "zoomInBtn", xmPushButtonWidgetClass, right_rc,
        XmNlabelString, zm_in,
        XmNshadowThickness, 2,
        XmNhighlightThickness, 0,
        XmNmarginWidth, 5,
        XmNmarginHeight, 3,
        NULL);
    XmStringFree(zm_in);
    XmChangeColor(g_zoom_in_btn, g_pal.bg_chrome);
    XtVaSetValues(g_zoom_in_btn, XmNforeground, g_pal.fg_chrome, NULL);
    XtAddCallback(g_zoom_in_btn, XmNactivateCallback, cb_zoom_in, NULL);

    /* Left/Center horizontal tab row */
    g_tabs_rc = XtVaCreateManagedWidget(
        "tabsRow", xmRowColumnWidgetClass, g_tabbar_form,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 3,
        XmNmarginWidth, 1,
        XmNmarginHeight, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, right_rc,
        XmNrightOffset, 4,
        NULL);
    XmChangeColor(g_tabs_rc, g_pal.bg_tabbar);

    /* Bottom Sunken 3-Cell CDE Status Bar */
    g_status_frame = XtVaCreateManagedWidget(
        "statusFrame", xmFrameWidgetClass, g_main_form,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 1,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmChangeColor(g_status_frame, g_pal.bg_chrome);

    Widget st_form = XtVaCreateManagedWidget(
        "statusForm", xmFormWidgetClass, g_status_frame,
        XmNmarginWidth, 3,
        XmNmarginHeight, 2,
        NULL);
    XmChangeColor(st_form, g_pal.bg_chrome);

    Widget fr_r = XtVaCreateManagedWidget(
        "stRightFr", xmFrameWidgetClass, st_form,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmChangeColor(fr_r, g_pal.bg_chrome);
    g_st_right = XtVaCreateManagedWidget(
        "stRightLbl", xmLabelWidgetClass, fr_r,
        XmNalignment, XmALIGNMENT_END,
        XmNmarginWidth, 6,
        XmNmarginHeight, 1,
        NULL);
    XmChangeColor(g_st_right, g_pal.bg_chrome);
    if (g_ui_norm_fl) XtVaSetValues(g_st_right, XmNfontList, g_ui_norm_fl, NULL);

    Widget fr_m = XtVaCreateManagedWidget(
        "stMidFr", xmFrameWidgetClass, st_form,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, fr_r,
        XmNrightOffset, 3,
        NULL);
    XmChangeColor(fr_m, g_pal.bg_chrome);
    XmString mid_s = XmStringCreateLocalized(" Ctrl+Shift+T: New Tab  |  Ctrl+C: Copy/SIGINT  |  Ctrl+V: Paste ");
    g_st_mid = XtVaCreateManagedWidget(
        "stMidLbl", xmLabelWidgetClass, fr_m,
        XmNlabelString, mid_s,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNmarginWidth, 6,
        XmNmarginHeight, 1,
        NULL);
    XmStringFree(mid_s);
    XmChangeColor(g_st_mid, g_pal.bg_chrome);
    if (g_ui_norm_fl) XtVaSetValues(g_st_mid, XmNfontList, g_ui_norm_fl, NULL);

    Widget fr_l = XtVaCreateManagedWidget(
        "stLeftFr", xmFrameWidgetClass, st_form,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, fr_m,
        XmNrightOffset, 3,
        NULL);
    XmChangeColor(fr_l, g_pal.bg_chrome);
    g_st_left = XtVaCreateManagedWidget(
        "stLeftLbl", xmLabelWidgetClass, fr_l,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginWidth, 6,
        XmNmarginHeight, 1,
        NULL);
    XmChangeColor(g_st_left, g_pal.bg_chrome);
    if (g_ui_norm_fl) XtVaSetValues(g_st_left, XmNfontList, g_ui_norm_fl, NULL);

    /* Center Sunken Terminal Viewport Stack */
    g_stack_frame = XtVaCreateManagedWidget(
        "stackFrame", xmFrameWidgetClass, g_main_form,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 2,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_tabbar_frame,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, g_status_frame,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNleftOffset, 2,
        XmNrightOffset, 2,
        XmNtopOffset, 1,
        XmNbottomOffset, 1,
        NULL);
    XmChangeColor(g_stack_frame, g_pal.bg_chrome);

    g_stack_form = XtVaCreateManagedWidget(
        "stackForm", xmFormWidgetClass, g_stack_frame,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        NULL);
    XmChangeColor(g_stack_form, g_pal.bg_chrome);
}

static void on_wm_delete_window(Widget w, XtPointer cd, XtPointer call) {
    (void)w; (void)cd; (void)call;
    cleanup_and_exit(0);
}

/* ---------------- CLI Pre-Parsing ---------------- */
static void preparse_cli(int *argc_io, char **argv) {
    ssize_t rl = readlink("/proc/self/exe", g_self_exe, sizeof(g_self_exe) - 1);
    if (rl > 0) g_self_exe[rl] = '\0';

    int argc = *argc_io;
    int out_c = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0 && i + 1 < argc) {
            g_init_cmd_prog = argv[i + 1];
            g_init_cmd_argv = &argv[i + 1];
            break; /* -e consumes all remaining args */
        } else if (strcmp(a, "--self-test") == 0) {
            g_self_test = 1;
        } else if (strcmp(a, "-geometry") == 0 && i + 1 < argc) {
            const char *g = argv[i + 1];
            int gw = 0, gh = 0;
            char rest[64] = "";
            if (sscanf(g, "%dx%d%63s", &gw, &gh, rest) >= 2 && gw > 0 && gw <= 250 && gh > 0 && gh <= 120) {
                g_init_cols = gw;
                g_init_rows = gh;
                i++;
                if (rest[0] == '+' || rest[0] == '-') {
                    snprintf(g_geom_pos_buf, sizeof(g_geom_pos_buf), "%s", rest);
                    argv[out_c++] = "-geometry";
                    argv[out_c++] = g_geom_pos_buf;
                }
            } else {
                argv[out_c++] = argv[i];
                argv[out_c++] = argv[++i];
            }
        } else if (strcmp(a, "-C") == 0) {
            g_console_mode = 1;
        } else if (strcmp(a, "-ls") == 0) {
            g_login_shell = 1;
        } else if (strcmp(a, "+ls") == 0) {
            g_login_shell = 0;
        } else if ((strcmp(a, "-cd") == 0 || strcmp(a, "--working-directory") == 0) && i + 1 < argc) {
            snprintf(g_init_cwd, sizeof(g_init_cwd), "%s", argv[++i]);
        } else if (strncmp(a, "--working-directory=", 20) == 0) {
            snprintf(g_init_cwd, sizeof(g_init_cwd), "%s", a + 20);
        } else if ((strcmp(a, "-T") == 0 || strcmp(a, "-title") == 0) && i + 1 < argc) {
            snprintf(g_fixed_win_title, sizeof(g_fixed_win_title), "%s", argv[i + 1]);
            argv[out_c++] = argv[i];
            argv[out_c++] = argv[++i];
        } else if (a[0] != '-') {
            /* Dropped folder or file from CDE dtfile / Front Panel */
            struct stat st;
            if (stat(a, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    snprintf(g_init_cwd, sizeof(g_init_cwd), "%s", a);
                } else {
                    char tmp[PATH_MAX];
                    snprintf(tmp, sizeof(tmp), "%s", a);
                    char *sl = strrchr(tmp, '/');
                    if (sl) {
                        *sl = '\0';
                        snprintf(g_init_cwd, sizeof(g_init_cwd), "%s", tmp[0] ? tmp : "/");
                    }
                }
            } else {
                argv[out_c++] = argv[i];
            }
        } else {
            if (strcmp(a, "-xrm") == 0 && i + 1 < argc && strstr(argv[i + 1], "menuBar") &&
                (strstr(argv[i + 1], "False") || strstr(argv[i + 1], "false"))) {
                g_show_menubar = 0;
            }
            argv[out_c++] = argv[i];
        }
    }
    argv[out_c] = NULL;
    *argc_io = out_c;
}

static void self_test_step_cb(XtPointer client_data, XtIntervalId *id) {
    (void)id;
    int step = (int)(intptr_t)client_data;
    if (step == 0) {
        int idx = create_new_tab(active_tab_cwd(), 0, 0);
        if (idx >= 0) activate_tab(idx);
        XtAppAddTimeOut(g_app, 200, self_test_step_cb, (XtPointer)1);
    } else if (step == 1) {
        activate_tab(0);
        XtAppAddTimeOut(g_app, 150, self_test_step_cb, (XtPointer)2);
    } else if (step == 2) {
        if (g_num_tabs > 1) close_tab_at(1);
        XtAppAddTimeOut(g_app, 150, self_test_step_cb, (XtPointer)3);
    } else {
        printf("SELF_TEST_OK: multi-tab dtTerm verified (tabs=%d)\n", g_num_tabs);
        fflush(stdout);
        cleanup_and_exit(0);
    }
}

int main(int argc, char **argv) {
    /* Ensure child shells never inherit any wrapper LD_PRELOAD */
    unsetenv("LD_PRELOAD");

    signal(SIGHUP,  cleanup_on_signal);
    signal(SIGTERM, cleanup_on_signal);
    signal(SIGINT,  cleanup_on_signal);
    signal(SIGSEGV, crash_bt_handler);

    load_prefs();
    preparse_cli(&argc, argv);

    _DtEnvControl(0);
    XtSetLanguageProc(NULL, NULL, NULL);
    /* Must be called BEFORE toolkit initialization per libDtTerm requirements */
    DtTermInitialize();

    String fallback_res[] = {
        "*menuBar.menuAccelerator: <Key>None",
        "*tabPopup.menuAccelerator: <Key>None",
        "*termPopup.menuAccelerator: <Key>None",
        "*menu_pulldown.menuAccelerator: <Key>None",
        "*menu_popup.menuAccelerator: <Key>None",
        "*saveLines: 4096",
        "*scrollBar: True",
        NULL
    };

    g_toplevel = XtVaAppInitialize(
        &g_app, "Dtterm", NULL, 0, &argc, argv, fallback_res,
        XmNtitle, g_fixed_win_title[0] ? g_fixed_win_title : "dtTerm",
        XmNiconName, g_console_mode ? "Console" : "dtTerm",
        XmNallowShellResize, True,
        NULL);

    Display *dpy = XtDisplay(g_toplevel);

    XtAppAddActions(g_app, g_term_actions, XtNumber(g_term_actions));

    /* Set 32x32 XPM window icon for dtwm */
    XpmAttributes xattr;
    memset(&xattr, 0, sizeof(xattr));
    xattr.valuemask = XpmCloseness;
    xattr.closeness = 40000;
    if (XpmCreatePixmapFromData(dpy, DefaultRootWindow(dpy), dtterm_icon_32_xpm,
                                &g_icon_pix, &g_icon_mask, &xattr) == XpmSuccess) {
        XtVaSetValues(g_toplevel,
                      XmNiconPixmap, g_icon_pix,
                      XmNiconMask, g_icon_mask,
                      NULL);
    }

    init_ui_fontlists(dpy);
    load_cde_palette(g_toplevel, &g_pal);
    build_main_ui();

    /* Open initial tab */
    int first = create_new_tab(g_init_cwd[0] ? g_init_cwd : NULL,
                               g_init_cmd_prog != NULL,
                               g_console_mode);
    if (first >= 0) {
        activate_tab(first);
    }

    Atom wm_delete = XmInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_toplevel, wm_delete, on_wm_delete_window, NULL);
    XtVaSetValues(g_toplevel, XmNdeleteResponse, XmDO_NOTHING, NULL);

    XtRealizeWidget(g_toplevel);
    /* Prevent opening subsequent tabs from altering outer window geometry */
    XtVaSetValues(g_toplevel, XmNallowShellResize, False, NULL);

    focus_active_term_timeout(NULL, NULL);
    XtAppAddTimeOut(g_app, 500, poll_tab_titles_cb, NULL);
    if (g_self_test) {
        XtAppAddTimeOut(g_app, 250, self_test_step_cb, (XtPointer)0);
    }

    XtAppMainLoop(g_app);
    return 0;
}
