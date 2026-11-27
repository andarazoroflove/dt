/*
 * libdtwm_subpanel_cols.so — Automatic Multi-Column CDE Subpanel Engine for dtwm
 *
 * Preloaded into /usr/dt/bin/dtwm via ~/.local/bin/dtwm-wrapper:
 *   1. Immediately strips itself from LD_PRELOAD after loading into dtwm so
 *      launched desktop applications inherit a 100% clean environment, and
 *      re-injects itself only across dtwm's own RestartWm execvp(".../dtwm").
 *   2. Inspects every CDE Front Panel SUBPANEL (XmForm with "dropzone" /
 *      "subpanel_separator" inside XmDialogShell) when posted:
 *      - Fits in 1 column above the Front Panel -> stays classic 1 column.
 *      - Too tall for 1 column -> splits into 2 (or more) balanced columns with
 *        an etched Motif vertical divider so its height always fits strictly
 *        above the main Front Panel and NEVER covers the main Front Panel.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xlib.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/SeparatoG.h>
#include <Xm/Xm.h>

extern char *__progname;

#define MAX_TRACKED_SUBPANELS 32
#define MAX_VSEPS             8
#define MAX_SUBPANEL_CTRLS    128

typedef struct {
    Widget    form;
    Widget    shell;
    Dimension base_col_w;
    Dimension row_h;
    int       last_ncols;
    int       last_count;
    Widget    vseps[MAX_VSEPS];
} SubpanelTrack;

static SubpanelTrack g_tracks[MAX_TRACKED_SUBPANELS];
static int           g_num_tracks = 0;
static bool          g_is_dtwm    = false;
static bool          g_in_reflow  = false;
static char          g_self_so[512] = "";

static void (*real_XtManageChild)(Widget) = NULL;
static void (*real_XtSetMappedWhenManaged)(Widget, Boolean) = NULL;
static void (*real_XtRealizeWidget)(Widget) = NULL;
static void (*real_DtAddToResource)(Display *, char *) = NULL;
static int  (*real_execvp)(const char *, char *const[]) = NULL;

/* Strip our .so from LD_PRELOAD so child apps launched from dtwm never inherit it */
static void strip_self_from_ld_preload(void) {
    const char *cur = getenv("LD_PRELOAD");
    if (!cur || !cur[0]) return;

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", cur);
    char out[2048] = "";
    char *save = NULL;
    for (char *tok = strtok_r(buf, ": ", &save); tok; tok = strtok_r(NULL, ": ", &save)) {
        if (strstr(tok, "libdtwm_subpanel_cols.so") != NULL) {
            if (!g_self_so[0]) snprintf(g_self_so, sizeof(g_self_so), "%s", tok);
            continue;
        }
        if (out[0]) strncat(out, ":", sizeof(out) - strlen(out) - 1);
        strncat(out, tok, sizeof(out) - strlen(out) - 1);
    }
    if (out[0]) setenv("LD_PRELOAD", out, 1);
    else        unsetenv("LD_PRELOAD");
}

__attribute__((constructor))
static void dtwm_subpanel_cols_init(void) {
    if (__progname && strcmp(__progname, "dtwm") == 0) {
        g_is_dtwm = true;
        strip_self_from_ld_preload();
        if (!g_self_so[0]) {
            const char *home = getenv("HOME");
            if (home && home[0]) {
                snprintf(g_self_so, sizeof(g_self_so),
                         "%s/.local/bin/libdtwm_subpanel_cols.so", home);
            }
        }
    }
}

/* Preserve preload across dtwm's own RestartWm -> execvp("/usr/dt/bin/dtwm", argv) */
int execvp(const char *file, char *const argv[]) {
    if (!real_execvp) {
        real_execvp = (int (*)(const char *, char *const[]))dlsym(RTLD_NEXT, "execvp");
    }
    if (g_is_dtwm && g_self_so[0]) {
        const char *base = strrchr(file, '/');
        base = base ? (base + 1) : file;
        if (strcmp(base, "dtwm") == 0 || strcmp(base, "dtwm-wrapper") == 0) {
            const char *cur = getenv("LD_PRELOAD");
            if (!cur || !strstr(cur, "libdtwm_subpanel_cols.so")) {
                char merged[2048];
                if (cur && cur[0]) snprintf(merged, sizeof(merged), "%s:%s", g_self_so, cur);
                else               snprintf(merged, sizeof(merged), "%s", g_self_so);
                setenv("LD_PRELOAD", merged, 1);
            }
        }
    }
    return real_execvp(file, argv);
}

static SubpanelTrack *get_or_add_track(Widget form, Widget shell) {
    for (int i = 0; i < g_num_tracks; i++) {
        if (g_tracks[i].form == form) {
            g_tracks[i].shell = shell;
            return &g_tracks[i];
        }
    }
    if (g_num_tracks >= MAX_TRACKED_SUBPANELS) return NULL;
    SubpanelTrack *t = &g_tracks[g_num_tracks++];
    memset(t, 0, sizeof(*t));
    t->form  = form;
    t->shell = shell;
    return t;
}

static SubpanelTrack *find_track_by_shell(Widget shell) {
    for (int i = 0; i < g_num_tracks; i++) {
        if (g_tracks[i].shell == shell) return &g_tracks[i];
    }
    return NULL;
}

/* Walk X11 window tree up to direct child of RootWindow to find exact outer WM frame Y */
static bool get_outer_frame_rect(Display *dpy, Window w, int *out_x, int *out_y,
                                 unsigned int *out_w, unsigned int *out_h) {
    if (!dpy || w == None) return false;
    Window root = None, parent = None, cur = w;
    Window *children = NULL;
    unsigned int nchildren = 0;

    for (int depth = 0; depth < 12; depth++) {
        if (!XQueryTree(dpy, cur, &root, &parent, &children, &nchildren)) break;
        if (children) XFree(children);
        if (parent == root || parent == None) break;
        cur = parent;
    }
    Window r_ret = None;
    int x = 0, y = 0;
    unsigned int wd = 0, ht = 0, bw = 0, dp = 0;
    if (!XGetGeometry(dpy, cur, &r_ret, &x, &y, &wd, &ht, &bw, &dp)) return false;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = wd + 2 * bw;
    if (out_h) *out_h = ht + 2 * bw;
    return true;
}

/* Returns the exact top Y coordinate of the main Front Panel on screen */
static int get_front_panel_top_y(Widget subpanel_shell) {
    Screen *scr = XtScreen(subpanel_shell);
    int scr_h = HeightOfScreen(scr);
    Widget panel_shell = XtParent(subpanel_shell);
    if (panel_shell && XtIsRealized(panel_shell)) {
        int fx = 0, fy = 0;
        unsigned int fw = 0, fh = 0;
        if (get_outer_frame_rect(XtDisplay(panel_shell), XtWindow(panel_shell), &fx, &fy, &fw, &fh)) {
            if (fy > scr_h / 3) return fy;
        }
        Position px = 0, py = 0;
        Dimension ph = 0;
        XtTranslateCoords(panel_shell, 0, 0, &px, &py);
        XtVaGetValues(panel_shell, XmNheight, &ph, NULL);
        if (py > scr_h / 3) return (int)py - 6;
        if (ph > 0 && ph < 240) return scr_h - (int)ph - 8;
    }
    return scr_h - 86;
}

static bool is_cde_subpanel_form(Widget w, Widget *out_dropzone, Widget *out_sep) {
    if (!w || !XtIsSubclass(w, xmFormWidgetClass)) return false;
    Widget parent = XtParent(w);
    if (!parent || !XtIsSubclass(parent, xmDialogShellWidgetClass)) return false;

    Widget *kids = NULL;
    Cardinal nkids = 0;
    XtVaGetValues(w, XmNchildren, &kids, XmNnumChildren, &nkids, NULL);
    Widget dz = NULL, sep = NULL;
    for (Cardinal i = 0; i < nkids; i++) {
        const char *nm = XtName(kids[i]);
        if (!nm) continue;
        if (strcmp(nm, "dropzone") == 0) dz = kids[i];
        else if (strcmp(nm, "subpanel_separator") == 0) sep = kids[i];
    }
    if (!dz && !sep) return false;
    if (out_dropzone) *out_dropzone = dz;
    if (out_sep)      *out_sep      = sep;
    return true;
}

static void reflow_subpanel_form(Widget form, Widget dz, Widget sep) {
    Widget shell = XtParent(form);
    SubpanelTrack *trk = get_or_add_track(form, shell);
    if (!trk) return;

    Widget *kids = NULL;
    Cardinal nkids = 0;
    XtVaGetValues(form, XmNchildren, &kids, XmNnumChildren, &nkids, NULL);

    Widget ctrls[MAX_SUBPANEL_CTRLS];
    int nctrls = 0;
    Dimension max_cw = 0, max_ch = 0;

    for (Cardinal i = 0; i < nkids && nctrls < MAX_SUBPANEL_CTRLS; i++) {
        Widget c = kids[i];
        if (!c || c == dz || c == sep) continue;
        const char *nm = XtName(c);
        if (nm && strncmp(nm, "subpanel_vsep", 13) == 0) continue;
        if (!XtIsManaged(c)) continue;

        XtWidgetGeometry pref;
        memset(&pref, 0, sizeof(pref));
        Dimension cw = 0, ch = 0;
        XtVaGetValues(c, XmNwidth, &cw, XmNheight, &ch, NULL);
        if (XtQueryGeometry(c, NULL, &pref) != XtGeometryNo) {
            if (pref.width > cw && pref.width < 320)  cw = pref.width;
            if (pref.height > ch && pref.height < 96) ch = pref.height;
        }
        if (trk->last_ncols <= 1 && cw > max_cw) max_cw = cw;
        if (ch > max_ch) max_ch = ch;
        ctrls[nctrls++] = c;
    }

    if (nctrls <= 0) return;

    Dimension form_w = 0;
    XtVaGetValues(form, XmNwidth, &form_w, NULL);
    if (trk->base_col_w == 0) {
        Dimension w_est = (form_w > max_cw + 10) ? form_w : (max_cw + 14);
        if (w_est < 165) w_est = 165;
        if (w_est > 260) w_est = 260;
        trk->base_col_w = w_est;
    } else if (trk->last_ncols <= 1 && max_cw + 14 > trk->base_col_w && max_cw < 260) {
        trk->base_col_w = max_cw + 14;
    }

    if (max_ch < 34) max_ch = 40;
    if (max_ch > 64) max_ch = 48;
    trk->row_h = max_ch;

    int header_h = 10;
    if (dz && XtIsManaged(dz)) {
        Dimension dh = 0;
        XtVaGetValues(dz, XmNheight, &dh, NULL);
        header_h += (dh > 16 ? (int)dh : 36) + 4;
    }
    if (sep && XtIsManaged(sep)) {
        Dimension sh = 0;
        XtVaGetValues(sep, XmNheight, &sh, NULL);
        header_h += (sh > 2 ? (int)sh : 6) + 4;
    }

    int panel_top_y = get_front_panel_top_y(shell);
    /* Reserve generous 52px for dtwm subpanel titlebar + frame bevels + breathing room */
    int avail_form_h = panel_top_y - 52;
    if (avail_form_h < 200) avail_form_h = 200;

    int row_pitch = (int)trk->row_h + 3;
    int one_col_h = header_h + nctrls * row_pitch + 10;

    int ncols = 1;
    if (one_col_h > avail_form_h && nctrls >= 2) {
        int max_rows = (avail_form_h - header_h - 10) / row_pitch;
        if (max_rows < 3) max_rows = 3;
        ncols = (nctrls + max_rows - 1) / max_rows;
        if (ncols < 2) ncols = 2;
        if (ncols > MAX_VSEPS + 1) ncols = MAX_VSEPS + 1;
    }

    if (trk->last_ncols == ncols && trk->last_count == nctrls && ncols == 1) {
        return;
    }

    g_in_reflow = true;
    XtUnmanageChildren(ctrls, (Cardinal)nctrls);

    for (int s = 0; s < MAX_VSEPS; s++) {
        if (trk->vseps[s] && XtIsManaged(trk->vseps[s])) {
            XtUnmanageChild(trk->vseps[s]);
        }
    }

    XtVaSetValues(shell, XmNallowShellResize, True, NULL);
    int fb = ncols * 100;
    XtVaSetValues(form,
                  XmNresizePolicy, XmRESIZE_ANY,
                  XmNfractionBase, fb,
                  NULL);

    Widget top_anchor = NULL;
    if (dz && XtIsManaged(dz)) {
        XtVaSetValues(dz,
                      XmNtopAttachment,    XmATTACH_FORM,
                      XmNtopOffset,        4,
                      XmNleftAttachment,   XmATTACH_FORM,
                      XmNleftOffset,       5,
                      XmNrightAttachment,  XmATTACH_FORM,
                      XmNrightOffset,      5,
                      XmNbottomAttachment, XmATTACH_NONE,
                      NULL);
        top_anchor = dz;
    }
    if (sep && XtIsManaged(sep)) {
        XtVaSetValues(sep,
                      XmNtopAttachment,    top_anchor ? XmATTACH_WIDGET : XmATTACH_FORM,
                      XmNtopWidget,        top_anchor,
                      XmNtopOffset,        3,
                      XmNleftAttachment,   XmATTACH_FORM,
                      XmNleftOffset,       1,
                      XmNrightAttachment,  XmATTACH_FORM,
                      XmNrightOffset,      1,
                      XmNbottomAttachment, XmATTACH_NONE,
                      NULL);
        top_anchor = sep;
    }

    int rows_per_col = (nctrls + ncols - 1) / ncols;
    Dimension item_w = (trk->base_col_w > 14) ? (trk->base_col_w - 10) : 165;

    for (int idx = 0; idx < nctrls; idx++) {
        int col = idx / rows_per_col;
        int row = idx % rows_per_col;
        bool is_tallest_col_bottom = (col == 0 && row == rows_per_col - 1);

        if (ncols == 1) {
            XtVaSetValues(ctrls[idx],
                          XmNtopAttachment,    (row == 0 && !top_anchor) ? XmATTACH_FORM : XmATTACH_WIDGET,
                          XmNtopWidget,        (row == 0) ? top_anchor : ctrls[idx - 1],
                          XmNtopOffset,        (row == 0) ? 3 : 1,
                          XmNbottomAttachment, (idx == nctrls - 1) ? XmATTACH_FORM : XmATTACH_NONE,
                          XmNbottomOffset,     (idx == nctrls - 1) ? 5 : 0,
                          XmNleftAttachment,   XmATTACH_FORM,
                          XmNleftOffset,       5,
                          XmNrightAttachment,  XmATTACH_FORM,
                          XmNrightOffset,      5,
                          NULL);
        } else {
            int lpos = col * 100;
            int rpos = (col + 1) * 100;
            XtVaSetValues(ctrls[idx],
                          XmNwidth,            item_w,
                          XmNtopAttachment,    (row == 0 && !top_anchor) ? XmATTACH_FORM : XmATTACH_WIDGET,
                          XmNtopWidget,        (row == 0) ? top_anchor : ctrls[idx - 1],
                          XmNtopOffset,        (row == 0) ? 3 : 1,
                          XmNbottomAttachment, is_tallest_col_bottom ? XmATTACH_FORM : XmATTACH_NONE,
                          XmNbottomOffset,     is_tallest_col_bottom ? 5 : 0,
                          XmNleftAttachment,   (col == 0) ? XmATTACH_FORM : XmATTACH_POSITION,
                          XmNleftPosition,     lpos,
                          XmNleftOffset,       5,
                          XmNrightAttachment,  (col == ncols - 1) ? XmATTACH_FORM : XmATTACH_POSITION,
                          XmNrightPosition,    rpos,
                          XmNrightOffset,      5,
                          NULL);
        }
    }

    for (int c = 1; c < ncols && (c - 1) < MAX_VSEPS; c++) {
        if (!trk->vseps[c - 1]) {
            char vname[32];
            snprintf(vname, sizeof(vname), "subpanel_vsep_%d", c);
            Arg args[8];
            int n = 0;
            XtSetArg(args[n], XmNorientation,   XmVERTICAL); n++;
            XtSetArg(args[n], XmNseparatorType, XmSHADOW_ETCHED_IN); n++;
            trk->vseps[c - 1] = XmCreateSeparatorGadget(form, vname, args, n);
        }
        XtVaSetValues(trk->vseps[c - 1],
                      XmNtopAttachment,    top_anchor ? XmATTACH_WIDGET : XmATTACH_FORM,
                      XmNtopWidget,        top_anchor,
                      XmNtopOffset,        3,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNbottomOffset,     5,
                      XmNleftAttachment,   XmATTACH_POSITION,
                      XmNleftPosition,     c * 100,
                      XmNleftOffset,       -1,
                      NULL);
        real_XtManageChild(trk->vseps[c - 1]);
    }

    XtManageChildren(ctrls, (Cardinal)nctrls);

    if (ncols >= 2) {
        Dimension target_fw = (Dimension)(ncols * trk->base_col_w + (ncols - 1) * 6);
        Dimension cur_fw = 0;
        XtVaGetValues(form, XmNwidth, &cur_fw, NULL);
        if (cur_fw < target_fw) {
            XtVaSetValues(form, XmNwidth, target_fw, NULL);
        }
    }

    trk->last_ncols = ncols;
    trk->last_count = nctrls;
    g_in_reflow = false;
}

static void clamp_subpanel_horizontal(SubpanelTrack *trk) {
    if (!trk || !trk->form || !trk->shell) return;
    Screen *scr = XtScreen(trk->shell);
    int scr_w = WidthOfScreen(scr);

    Position fx = 0;
    Dimension fw = 0;
    XtVaGetValues(trk->form, XmNx, &fx, XmNwidth, &fw, NULL);

    if ((int)fx + (int)fw + 12 > scr_w) {
        Position nx = (Position)(scr_w - (int)fw - 12);
        if (nx < 2) nx = 2;
        XtVaSetValues(trk->form,  XmNx, nx, NULL);
        XtVaSetValues(trk->shell, XmNx, nx, NULL);
    }
}

void XtManageChild(Widget w) {
    if (!real_XtManageChild) {
        real_XtManageChild = (void (*)(Widget))dlsym(RTLD_NEXT, "XtManageChild");
    }
    real_XtManageChild(w);

    if (!g_is_dtwm || g_in_reflow || !w) return;

    Widget dz = NULL, sep = NULL;
    if (is_cde_subpanel_form(w, &dz, &sep)) {
        reflow_subpanel_form(w, dz, sep);
    }
}

void XtSetMappedWhenManaged(Widget w, Boolean map_when_managed) {
    if (!real_XtSetMappedWhenManaged) {
        real_XtSetMappedWhenManaged =
            (void (*)(Widget, Boolean))dlsym(RTLD_NEXT, "XtSetMappedWhenManaged");
    }
    if (g_is_dtwm && map_when_managed && w) {
        SubpanelTrack *trk = find_track_by_shell(w);
        if (trk) {
            clamp_subpanel_horizontal(trk);
        }
    }
    real_XtSetMappedWhenManaged(w, map_when_managed);
}

/*
 * Force FrontPanel XmNgeometry to NULL right as WmPanelistShow realizes it so
 * dtwm ALWAYS takes its native bottom-center calculation on startup & Restart WM,
 * and prevent SaveWorkspaceResources from saving stale FrontPanel*geometry strings.
 */
void XtRealizeWidget(Widget w) {
    if (!real_XtRealizeWidget) {
        real_XtRealizeWidget = (void (*)(Widget))dlsym(RTLD_NEXT, "XtRealizeWidget");
    }
    if (g_is_dtwm && w) {
        const char *nm = XtName(w);
        if (nm && strcmp(nm, "FrontPanel") == 0) {
            XtVaSetValues(w, XmNgeometry, NULL, NULL);
        }
    }
    real_XtRealizeWidget(w);
}

void _DtAddToResource(Display *dpy, char *data) {
    if (!real_DtAddToResource) {
        real_DtAddToResource = (void (*)(Display *, char *))dlsym(RTLD_NEXT, "_DtAddToResource");
    }
    if (g_is_dtwm && data && strstr(data, "FrontPanel") != NULL && strstr(data, "geometry") != NULL) {
        return;
    }
    real_DtAddToResource(dpy, data);
}
