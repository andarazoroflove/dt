/*
 * cde-calendar (installed as ~/.local/bin/dtcm & cde-calendar):
 * Native OSF/Motif 2.3 Clone of CDE Calendar Manager (dtcm) for Office 365.
 *
 * - Uses genuine CDE DtCM icons (/usr/dt/appconfig/icons/C/DtCM*.pm) composited
 *   cleanly onto Motif chrome and binds to the active CDE desktop palette.
 * - Preserves CDE's live Front Panel dynamic Date icon (CONTROL Date, TYPE date,
 *   CLIENT_NAME dtcm, PUSH_RECALL True) while launching this Office 365 Calendar.
 * - Companion app to dtMail (cde-mail), sharing ~/.config/cde-mail/ OAuth 2.0 tokens
 *   and live IPC signals (calendar_dirty.stamp & calendar_goto.json).
 * - Supports Day, Week, Month, and Year views, full Event Creation/Editing with
 *   invited Attendees, Pending Invitations view, and Microsoft Graph RSVP replies
 *   (Accept / Tentative / Decline + optional comment to organizer).
 */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include <Xm/XmAll.h>
#include <Dt/Dt.h>

/* ---------------- Minimal Embedded JSON DOM Parser ---------------- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
typedef struct { char *key; JVal *val; } JKV;

struct JVal {
    JType type;
    union {
        int b;
        double num;
        char *str;
        struct { JVal **items; size_t len, cap; } arr;
        struct { JKV *pairs; size_t len, cap; } obj;
    } u;
};

static void j_free(JVal *v) {
    if (!v) return;
    if (v->type == J_STR) free(v->u.str);
    else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->u.arr.len; i++) j_free(v->u.arr.items[i]);
        free(v->u.arr.items);
    } else if (v->type == J_OBJ) {
        for (size_t i = 0; i < v->u.obj.len; i++) {
            free(v->u.obj.pairs[i].key);
            j_free(v->u.obj.pairs[i].val);
        }
        free(v->u.obj.pairs);
    }
    free(v);
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void encode_utf8(unsigned int cp, char **buf, size_t *len, size_t *cap) {
    if (*len + 8 >= *cap) {
        *cap = (*cap == 0) ? 64 : (*cap * 2);
        *buf = (char *)realloc(*buf, *cap);
    }
    if (cp < 0x80) (*buf)[(*len)++] = (char)cp;
    else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 10000U || cp < 0x10000U) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static char *parse_raw_str(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    size_t cap = 64, len = 0;
    char *out = (char *)malloc(cap);
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (c == '\\' && **p) {
            char esc = *(*p)++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    int h1 = hex_val((*p)[0]), h2 = (h1 >= 0) ? hex_val((*p)[1]) : -1;
                    int h3 = (h2 >= 0) ? hex_val((*p)[2]) : -1, h4 = (h3 >= 0) ? hex_val((*p)[3]) : -1;
                    if (h4 >= 0) {
                        unsigned int u1 = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                        *p += 4;
                        encode_utf8(u1, &out, &len, &cap);
                        continue;
                    }
                    c = '?';
                    break;
                }
                default: c = (unsigned char)esc; break;
            }
        }
        if (len + 2 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[len++] = (char)c;
    }
    if (**p == '"') (*p)++;
    out[len] = '\0';
    return out;
}

static JVal *j_parse_val(const char **p) {
    skip_ws(p);
    if (!**p) return NULL;
    if (**p == '"') {
        char *s = parse_raw_str(p);
        if (!s) return NULL;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_STR; v->u.str = s;
        return v;
    }
    if (**p == '{') {
        (*p)++;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_OBJ;
        skip_ws(p);
        if (**p == '}') { (*p)++; return v; }
        while (**p) {
            skip_ws(p);
            char *k = parse_raw_str(p);
            if (!k) { j_free(v); return NULL; }
            skip_ws(p);
            if (**p != ':') { free(k); j_free(v); return NULL; }
            (*p)++;
            JVal *sub = j_parse_val(p);
            if (!sub) { free(k); j_free(v); return NULL; }
            if (v->u.obj.len >= v->u.obj.cap) {
                v->u.obj.cap = v->u.obj.cap ? v->u.obj.cap * 2 : 8;
                v->u.obj.pairs = (JKV *)realloc(v->u.obj.pairs, v->u.obj.cap * sizeof(JKV));
            }
            v->u.obj.pairs[v->u.obj.len++] = (JKV){k, sub};
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            break;
        }
        return v;
    }
    if (**p == '[') {
        (*p)++;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_ARR;
        skip_ws(p);
        if (**p == ']') { (*p)++; return v; }
        while (**p) {
            JVal *sub = j_parse_val(p);
            if (!sub) { j_free(v); return NULL; }
            if (v->u.arr.len >= v->u.arr.cap) {
                v->u.arr.cap = v->u.arr.cap ? v->u.arr.cap * 2 : 8;
                v->u.arr.items = (JVal **)realloc(v->u.arr.items, v->u.arr.cap * sizeof(JVal *));
            }
            v->u.arr.items[v->u.arr.len++] = sub;
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == ']') { (*p)++; break; }
            break;
        }
        return v;
    }
    if (strncmp(*p, "true", 4) == 0)  { *p += 4; JVal *v = (JVal *)calloc(1, sizeof(JVal)); v->type = J_BOOL; v->u.b = 1; return v; }
    if (strncmp(*p, "false", 5) == 0) { *p += 5; JVal *v = (JVal *)calloc(1, sizeof(JVal)); v->type = J_BOOL; v->u.b = 0; return v; }
    if (strncmp(*p, "null", 4) == 0)  { *p += 4; JVal *v = (JVal *)calloc(1, sizeof(JVal)); v->type = J_NULL; return v; }
    if (**p == '-' || isdigit((unsigned char)**p)) {
        char *endp = NULL;
        double d = strtod(*p, &endp);
        if (endp == *p) return NULL;
        *p = endp;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_NUM; v->u.num = d;
        return v;
    }
    return NULL;
}

static JVal *j_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->u.obj.len; i++) {
        if (strcmp(obj->u.obj.pairs[i].key, key) == 0) return obj->u.obj.pairs[i].val;
    }
    return NULL;
}

static const char *j_str(const JVal *obj, const char *key, const char *def) {
    JVal *v = j_get(obj, key);
    return (v && v->type == J_STR && v->u.str) ? v->u.str : def;
}

static int j_bool(const JVal *obj, const char *key, int def) {
    JVal *v = j_get(obj, key);
    if (!v) return def;
    if (v->type == J_BOOL) return v->u.b;
    if (v->type == J_NUM) return v->u.num != 0.0;
    return def;
}

static int j_int(const JVal *obj, const char *key, int def) {
    JVal *v = j_get(obj, key);
    return (v && v->type == J_NUM) ? (int)v->u.num : def;
}

static void json_write_escaped(FILE *fp, const char *s) {
    fputc('"', fp);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"':  fputs("\\\"", fp); break;
                case '\\': fputs("\\\\", fp); break;
                case '\n': fputs("\\n", fp); break;
                case '\r': fputs("\\r", fp); break;
                case '\t': fputs("\\t", fp); break;
                default:
                    if (*p < 0x20) fprintf(fp, "\\u%04x", *p);
                    else fputc(*p, fp);
                    break;
            }
        }
    }
    fputc('"', fp);
}

/* ---------------- Calendar Date Math Helpers ---------------- */
static const char *MONTH_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *DOW_LONG[7]  = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *DOW_SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m) {
    static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    if (m == 2 && is_leap_year(y)) return 29;
    return d[m - 1];
}

static int day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m - 1] + d) % 7;
}

static void add_days(int *y, int *m, int *d, int delta) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = *y - 1900;
    t.tm_mon  = *m - 1;
    t.tm_mday = *d + delta;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    mktime(&t);
    *y = t.tm_year + 1900;
    *m = t.tm_mon + 1;
    *d = t.tm_mday;
}

static void fmt_ymd(int y, int m, int d, char out[16]) {
    snprintf(out, 16, "%04u-%02u-%02u",
             (unsigned)(y > 0 ? y : 1970) % 10000U,
             (unsigned)(m > 0 ? m : 1) % 100U,
             (unsigned)(d > 0 ? d : 1) % 100U);
}

/* ---------------- App State & Structures ---------------- */
#define MAX_EVENTS 512

typedef enum { VIEW_DAY = 0, VIEW_WEEK = 1, VIEW_MONTH = 2, VIEW_YEAR = 3 } CalViewMode;

typedef struct {
    char id[320];
    char subject[200];
    char location[160];
    char organizer[128];
    char organizer_email[160];
    int  is_organizer;
    int  response_requested;
    char response_status[32];
    char attendees[512];
    char notes[512];
    char start_date[16];
    char end_date[16];
    int start_min;
    int end_min;
    char start_disp[32];
    char time_range[64];
    int all_day;
} CalEvent;

typedef struct {
    Pixel bg_chrome;
    Pixel bg_well;
    Pixel bg_accent;
    Pixel bg_today;
    Pixel bg_grid_hdr;
    Pixel fg_chrome;
    Pixel fg_well;
    Pixel fg_white;
    Pixel grid_line;
} DtcmPalette;

static XtAppContext g_app;
static Widget g_toplevel;
static Widget g_view_title_lbl;
static Widget g_invites_btn = NULL;
static Widget g_canvas;
static Widget g_st_lbl;

/* Appointment Editor & RSVP Dialog */
static Widget g_ed_dlg = NULL;
static Widget g_ed_date_tf, g_ed_start_tf, g_ed_end_tf, g_ed_loc_tf, g_ed_att_tf;
static Widget g_ed_allday_tb, g_ed_what_txt, g_ed_list;
static Widget g_ed_org_lbl = NULL;
static Widget g_rsvp_note_tf = NULL;
static Widget g_rsvp_send_tb = NULL;
static Widget g_rsvp_acc_btn = NULL, g_rsvp_tent_btn = NULL, g_rsvp_dec_btn = NULL;
static int g_ed_map_idx[MAX_EVENTS];
static int g_ed_num_mapped = 0;
static char g_ed_selected_id[320] = "";

/* Month Appointment List Dialog */
static Widget g_list_dlg = NULL;
static Widget g_list_box = NULL;
static int g_list_map_idx[MAX_EVENTS];
static int g_list_num_mapped = 0;

/* Pending Invitations Dialog */
static Widget g_inv_dlg = NULL;
static Widget g_inv_list = NULL;
static int g_inv_map_idx[MAX_EVENTS];
static int g_inv_num_mapped = 0;

static DtcmPalette g_pal;
static GC g_gc = None;
static XFontStruct *g_fn_reg = NULL;
static XFontStruct *g_fn_bold = NULL;
static XFontStruct *g_fn_small = NULL;
static Pixmap g_backbuf = None;
static int g_bb_w = 0, g_bb_h = 0;

static CalViewMode g_view = VIEW_MONTH;
static int g_today_y, g_today_m, g_today_d;
static int g_sel_y, g_sel_m, g_sel_d;
static int g_sel_hour = 9;
static char g_cli_ics_path[512] = "";
static Time g_last_click_time = 0;
static int g_last_click_y = 0, g_last_click_m = 0, g_last_click_d = 0;

static CalEvent g_events[MAX_EVENTS];
static int g_num_events = 0;
static char g_account_email[160] = "";
static int g_signed_in = 0;

/* Backend IPC */
static FILE *g_to_backend = NULL;
static int g_from_backend_fd = -1;
static pid_t g_backend_pid = -1;
static char *g_rx_buf = NULL;
static size_t g_rx_len = 0, g_rx_cap = 0;

/* ---------------- CDE Palette Discovery ---------------- */
static Pixel alloc_col(Display *dpy, Colormap cmap, const char *spec, Pixel fb) {
    if (!spec || !spec[0]) return fb;
    XColor c, ex;
    if (XAllocNamedColor(dpy, cmap, spec, &c, &ex)) return c.pixel;
    if (XParseColor(dpy, cmap, spec, &c) && XAllocColor(dpy, cmap, &c)) return c.pixel;
    return fb;
}

static void init_dtcm_palette(Widget top, DtcmPalette *pal) {
    Display *dpy = XtDisplay(top);
    int scr = DefaultScreen(dpy);
    Colormap cmap = DefaultColormap(dpy, scr);
    Pixel blk = BlackPixel(dpy, scr);
    Pixel wht = WhitePixel(dpy, scr);

    char pal_name[128] = "Crimson.dp";
    XrmDatabase db = XtDatabase(dpy);
    if (db) {
        char *stype = NULL;
        XrmValue val = {0, NULL};
        if ((XrmGetResource(db, "0.ColorPalette", "0.ColorPalette", &stype, &val) ||
             XrmGetResource(db, "*0*ColorPalette", "*0*ColorPalette", &stype, &val) ||
             XrmGetResource(db, "*ColorPalette", "*ColorPalette", &stype, &val)) &&
            val.addr && ((char *)val.addr)[0]) {
            snprintf(pal_name, sizeof(pal_name), "%s", (char *)val.addr);
        }
    }

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/.dt/palettes/%s", home, pal_name);
    snprintf(p2, sizeof(p2), "/usr/dt/palettes/%s", pal_name);
    FILE *fp = fopen(p1, "r");
    if (!fp) fp = fopen(p2, "r");

    char cset[8][64];
    memset(cset, 0, sizeof(cset));
    int n = 0;
    if (fp) {
        char ln[128];
        while (n < 8 && fgets(ln, sizeof(ln), fp)) {
            char *nl = strpbrk(ln, "\r\n");
            if (nl) *nl = '\0';
            if (ln[0] == '#') snprintf(cset[n++], sizeof(cset[0]), "%.63s", ln);
        }
        fclose(fp);
    }

    const char *today_hex  = (n >= 1) ? cset[0] : "#B24D7A";
    const char *chrome_hex = (n >= 2) ? cset[1] : "#AEB2C3";
    const char *accent_hex = (n >= 3) ? cset[2] : "#718BA5";
    const char *well_hex   = (n >= 4) ? cset[3] : "#FFF7E9";

    pal->bg_today    = alloc_col(dpy, cmap, today_hex, blk);
    pal->bg_chrome   = alloc_col(dpy, cmap, chrome_hex, wht);
    pal->bg_accent   = alloc_col(dpy, cmap, accent_hex, blk);
    pal->bg_well     = alloc_col(dpy, cmap, well_hex, wht);
    pal->bg_grid_hdr = alloc_col(dpy, cmap, "#9CA2B8", pal->bg_chrome);
    pal->grid_line   = alloc_col(dpy, cmap, "#8A8F9E", blk);
    pal->fg_chrome   = blk;
    pal->fg_well     = blk;
    pal->fg_white    = wht;
}

static void theme_widgets(Widget w, const DtcmPalette *pal) {
    if (!w) return;
    if (XmIsList(w) || XmIsText(w) || XmIsTextField(w) || w == g_canvas) {
        XmChangeColor(w, pal->bg_well);
        XtVaSetValues(w, XmNforeground, pal->fg_well, NULL);
        Widget p = XtParent(w);
        if (p && XmIsScrolledWindow(p)) {
            Widget hsb = NULL, vsb = NULL;
            XtVaGetValues(p, XmNhorizontalScrollBar, &hsb, XmNverticalScrollBar, &vsb, NULL);
            if (hsb) XmChangeColor(hsb, pal->bg_chrome);
            if (vsb) XmChangeColor(vsb, pal->bg_chrome);
        }
        return;
    }
    XmChangeColor(w, pal->bg_chrome);
    if (XtIsSubclass(w, xmPrimitiveWidgetClass)) {
        XtVaSetValues(w, XmNforeground, pal->fg_chrome, NULL);
    }
    if (XtIsComposite(w)) {
        WidgetList ch = NULL;
        Cardinal nc = 0;
        XtVaGetValues(w, XmNchildren, &ch, XmNnumChildren, &nc, NULL);
        for (Cardinal i = 0; i < nc; i++) theme_widgets(ch[i], pal);
    }
    if (XmIsCascadeButton(w) || XmIsCascadeButtonGadget(w)) {
        Widget sub = NULL;
        XtVaGetValues(w, XmNsubMenuId, &sub, NULL);
        if (sub) theme_widgets(sub, pal);
    }
}

static void set_label(Widget w, const char *txt) {
    if (!w) return;
    XmString s = XmStringCreateLocalized((char *)(txt ? txt : ""));
    XtVaSetValues(w, XmNlabelString, s, NULL);
    XmStringFree(s);
}

static Pixmap load_xpm_file_on_bg(Widget ref, const char *xpm_path, Pixel bg_pix) {
    Display *dpy = XtDisplay(ref);
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    unsigned int depth = (unsigned int)DefaultDepth(dpy, scr);

    XpmAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.valuemask = XpmCloseness | XpmDepth;
    attr.closeness = 40000;
    attr.depth = depth;

    Pixmap raw = None, mask = None;
    if (XpmReadFileToPixmap(dpy, root, (char *)xpm_path, &raw, &mask, &attr) != XpmSuccess || raw == None) {
        return None;
    }

    Pixmap comp = XCreatePixmap(dpy, root, attr.width, attr.height, depth);
    GC gc = XCreateGC(dpy, comp, 0, NULL);
    XSetForeground(dpy, gc, bg_pix);
    XFillRectangle(dpy, comp, gc, 0, 0, attr.width, attr.height);
    if (mask != None) {
        XSetClipMask(dpy, gc, mask);
        XSetClipOrigin(dpy, gc, 0, 0);
    }
    XCopyArea(dpy, raw, comp, gc, 0, 0, attr.width, attr.height, 0, 0);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, raw);
    if (mask != None) XFreePixmap(dpy, mask);
    return comp;
}

static const char *event_invite_prefix(const CalEvent *ev) {
    if (!ev->is_organizer) {
        if (strcmp(ev->response_status, "notResponded") == 0) return "[?] ";
        if (strcmp(ev->response_status, "tentativelyAccepted") == 0) return "[~] ";
    }
    return "";
}

static const char *human_response_status(const CalEvent *ev) {
    if (ev->is_organizer) return "Organizer";
    if (strcmp(ev->response_status, "notResponded") == 0) return "Needs Reply (Unresponded)";
    if (strcmp(ev->response_status, "tentativelyAccepted") == 0) return "Tentative";
    if (strcmp(ev->response_status, "accepted") == 0) return "Accepted";
    if (strcmp(ev->response_status, "declined") == 0) return "Declined";
    return ev->response_status[0] ? ev->response_status : "Scheduled";
}

static int count_pending_invites(void) {
    int c = 0;
    for (int i = 0; i < g_num_events; i++) {
        if (!g_events[i].is_organizer && strcmp(g_events[i].response_status, "notResponded") == 0) {
            c++;
        }
    }
    return c;
}

static void update_invites_button_badge(void) {
    if (!g_invites_btn) return;
    int p = count_pending_invites();
    char lbl[48];
    if (p > 0) snprintf(lbl, sizeof(lbl), "Invites (%d)", p);
    else       snprintf(lbl, sizeof(lbl), "Invites");
    set_label(g_invites_btn, lbl);
}

static int event_on_date(const CalEvent *ev, const char *ymd) {
    return strcmp(ev->start_date, ymd) <= 0 && strcmp(ymd, ev->end_date) <= 0;
}

static int count_events_on_ymd(const char *ymd) {
    int c = 0;
    for (int i = 0; i < g_num_events; i++) {
        if (event_on_date(&g_events[i], ymd)) c++;
    }
    return c;
}

static void draw_clipped_str(Display *dpy, Drawable d, GC gc, XFontStruct *fn,
                             int x, int y, int max_w, const char *str) {
    if (!str || !str[0] || max_w <= 6) return;
    XSetFont(dpy, gc, fn->fid);
    int len = (int)strlen(str);
    while (len > 0 && XTextWidth(fn, str, len) > max_w) {
        len--;
    }
    if (len > 0) {
        XDrawString(dpy, d, gc, x, y, str, len);
    }
}

/* ---------------- Update View Header & Backend Window ---------------- */
static void notify_backend_window(void) {
    if (!g_to_backend) return;
    fprintf(g_to_backend, "{\"op\":\"set_window\",\"year\":%d,\"month\":%d,\"full_year\":%s}\n",
            g_sel_y, g_sel_m, (g_view == VIEW_YEAR) ? "true" : "false");
    fflush(g_to_backend);
}

static void update_titles(void) {
    char vtitle[160];
    if (g_view == VIEW_MONTH) {
        snprintf(vtitle, sizeof(vtitle), "%s %d", MONTH_NAMES[g_sel_m - 1], g_sel_y);
    } else if (g_view == VIEW_DAY) {
        int dow = day_of_week(g_sel_y, g_sel_m, g_sel_d);
        snprintf(vtitle, sizeof(vtitle), "%s, %s %d, %d",
                 DOW_LONG[dow], MONTH_NAMES[g_sel_m - 1], g_sel_d, g_sel_y);
    } else if (g_view == VIEW_WEEK) {
        int dow = day_of_week(g_sel_y, g_sel_m, g_sel_d);
        int sy = g_sel_y, sm = g_sel_m, sd = g_sel_d;
        add_days(&sy, &sm, &sd, -dow);
        int ey = sy, em = sm, ed = sd;
        add_days(&ey, &em, &ed, 6);
        snprintf(vtitle, sizeof(vtitle), "Week of %.3s %d – %.3s %d, %d",
                 MONTH_NAMES[sm - 1], sd, MONTH_NAMES[em - 1], ed, ey);
    } else {
        snprintf(vtitle, sizeof(vtitle), "%d", g_sel_y);
    }
    set_label(g_view_title_lbl, vtitle);

    char wtitle[240];
    snprintf(wtitle, sizeof(wtitle), "Calendar : %s",
             g_account_email[0] ? g_account_email : "Office 365");
    XtVaSetValues(g_toplevel, XmNtitle, wtitle, NULL);
    update_invites_button_badge();
}

/* ---------------- Double-Buffered dtcm View Renderers ---------------- */
static void draw_mini_month(Display *dpy, Drawable buf, int x0, int y0, int w, int h,
                            int year, int month, int highlight_sel) {
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s %d", MONTH_NAMES[month - 1], year);
    XSetForeground(dpy, g_gc, g_pal.bg_chrome);
    XFillRectangle(dpy, buf, g_gc, x0, y0, (unsigned)w, 20);
    XSetForeground(dpy, g_gc, g_pal.grid_line);
    XDrawRectangle(dpy, buf, g_gc, x0, y0, (unsigned)w, (unsigned)h);

    XSetForeground(dpy, g_gc, g_pal.fg_chrome);
    XSetFont(dpy, g_gc, g_fn_bold->fid);
    int tw = XTextWidth(g_fn_bold, hdr, (int)strlen(hdr));
    XDrawString(dpy, buf, g_gc, x0 + (w - tw) / 2, y0 + 15, hdr, (int)strlen(hdr));

    int cw = w / 7;
    int gy = y0 + 22;
    XSetFont(dpy, g_gc, g_fn_small->fid);
    static const char *dl[7] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int c = 0; c < 7; c++) {
        XDrawString(dpy, buf, g_gc, x0 + c * cw + cw / 3, gy + 11, dl[c], 1);
    }

    int first_dow = day_of_week(year, month, 1);
    int ndays = days_in_month(year, month);
    int rh = (h - 38) / 6;
    if (rh < 13) rh = 13;

    for (int d = 1; d <= ndays; d++) {
        int slot = first_dow + (d - 1);
        int row = slot / 7;
        int col = slot % 7;
        int cx = x0 + col * cw;
        int cy = gy + 15 + row * rh;

        char ymd[16];
        fmt_ymd(year, month, d, ymd);
        int has_ev = count_events_on_ymd(ymd) > 0;
        int is_tod = (year == g_today_y && month == g_today_m && d == g_today_d);
        int is_sel = highlight_sel && (year == g_sel_y && month == g_sel_m && d == g_sel_d);

        if (is_sel) {
            XSetForeground(dpy, g_gc, g_pal.bg_accent);
            XFillRectangle(dpy, buf, g_gc, cx + 1, cy, (unsigned)(cw - 2), (unsigned)(rh - 1));
        } else if (is_tod) {
            XSetForeground(dpy, g_gc, g_pal.bg_today);
            XDrawRectangle(dpy, buf, g_gc, cx + 1, cy, (unsigned)(cw - 3), (unsigned)(rh - 2));
        }

        char ds[16];
        snprintf(ds, sizeof(ds), "%2d", d);
        XSetFont(dpy, g_gc, has_ev ? g_fn_bold->fid : g_fn_small->fid);
        XSetForeground(dpy, g_gc, is_sel ? g_pal.fg_white : g_pal.fg_well);
        XDrawString(dpy, buf, g_gc, cx + 4, cy + rh - 3, ds, (int)strlen(ds));
        if (has_ev && !is_sel) {
            XSetForeground(dpy, g_gc, g_pal.bg_today);
            XDrawLine(dpy, buf, g_gc, cx + 4, cy + rh - 1, cx + cw - 5, cy + rh - 1);
        }
    }
}

static void render_month_view(Display *dpy, Drawable buf, int w, int h) {
    int hdr_h = 26;
    int col_w = w / 7;
    int first_dow = day_of_week(g_sel_y, g_sel_m, 1);
    int ndays = days_in_month(g_sel_y, g_sel_m);
    int nrows = (first_dow + ndays + 6) / 7;
    if (nrows < 5) nrows = 5;
    int row_h = (h - hdr_h) / nrows;

    XSetForeground(dpy, g_gc, g_pal.bg_chrome);
    XFillRectangle(dpy, buf, g_gc, 0, 0, (unsigned)w, (unsigned)hdr_h);
    XSetFont(dpy, g_gc, g_fn_bold->fid);
    for (int c = 0; c < 7; c++) {
        int cx = c * col_w;
        XSetForeground(dpy, g_gc, g_pal.fg_chrome);
        int tw = XTextWidth(g_fn_bold, DOW_LONG[c], (int)strlen(DOW_LONG[c]));
        const char *lbl = (tw + 10 < col_w) ? DOW_LONG[c] : DOW_SHORT[c];
        tw = XTextWidth(g_fn_bold, lbl, (int)strlen(lbl));
        XDrawString(dpy, buf, g_gc, cx + (col_w - tw) / 2, 18, lbl, (int)strlen(lbl));
        XSetForeground(dpy, g_gc, g_pal.grid_line);
        XDrawLine(dpy, buf, g_gc, cx, 0, cx, h);
    }
    XSetForeground(dpy, g_gc, g_pal.grid_line);
    XDrawLine(dpy, buf, g_gc, 0, hdr_h, w, hdr_h);

    for (int r = 0; r < nrows; r++) {
        int ry = hdr_h + r * row_h;
        XSetForeground(dpy, g_gc, g_pal.grid_line);
        XDrawLine(dpy, buf, g_gc, 0, ry, w, ry);

        for (int c = 0; c < 7; c++) {
            int slot = r * 7 + c;
            int d = slot - first_dow + 1;
            int cx = c * col_w;
            int cw = (c == 6) ? (w - cx) : col_w;
            int ch = (r == nrows - 1) ? (h - ry) : row_h;

            if (d < 1 || d > ndays) {
                XSetForeground(dpy, g_gc, g_pal.bg_chrome);
                XFillRectangle(dpy, buf, g_gc, cx + 1, ry + 1, (unsigned)(cw - 1), (unsigned)(ch - 1));
                continue;
            }

            int is_tod = (g_sel_y == g_today_y && g_sel_m == g_today_m && d == g_today_d);
            int is_sel = (d == g_sel_d);

            if (is_tod) {
                XSetForeground(dpy, g_gc, g_pal.bg_today);
                XFillRectangle(dpy, buf, g_gc, cx + 1, ry + 1, (unsigned)(cw - 2), 19);
            } else if (is_sel) {
                XSetForeground(dpy, g_gc, g_pal.bg_accent);
                XFillRectangle(dpy, buf, g_gc, cx + 1, ry + 1, (unsigned)(cw - 2), 19);
            }

            if (is_sel) {
                XSetForeground(dpy, g_gc, g_pal.bg_accent);
                XDrawRectangle(dpy, buf, g_gc, cx + 1, ry + 1, (unsigned)(cw - 3), (unsigned)(ch - 3));
                XDrawRectangle(dpy, buf, g_gc, cx + 2, ry + 2, (unsigned)(cw - 5), (unsigned)(ch - 5));
            }

            char dnum[32];
            if (is_tod) snprintf(dnum, sizeof(dnum), "%d  (Today)", d);
            else        snprintf(dnum, sizeof(dnum), "%d", d);
            XSetFont(dpy, g_gc, g_fn_bold->fid);
            XSetForeground(dpy, g_gc, (is_tod || is_sel) ? g_pal.fg_white : g_pal.fg_well);
            XDrawString(dpy, buf, g_gc, cx + 6, ry + 15, dnum, (int)strlen(dnum));

            char ymd[16];
            fmt_ymd(g_sel_y, g_sel_m, d, ymd);
            int ey = ry + 23;
            int line_h = 16;
            int max_lines = (ch - 25) / line_h;
            if (max_lines < 1) max_lines = 1;

            int total_day = count_events_on_ymd(ymd);
            int drawn = 0;
            for (int i = 0; i < g_num_events; i++) {
                const CalEvent *ev = &g_events[i];
                if (!event_on_date(ev, ymd)) continue;
                if (drawn >= max_lines - (total_day > max_lines ? 1 : 0)) break;

                const char *pfx = event_invite_prefix(ev);
                int needs_rsvp = (!ev->is_organizer && strcmp(ev->response_status, "notResponded") == 0);

                if (ev->all_day || needs_rsvp) {
                    char line[256];
                    if (ev->all_day) snprintf(line, sizeof(line), "%s%s", pfx, ev->subject);
                    else             snprintf(line, sizeof(line), "%s%s %s", pfx, ev->start_disp, ev->subject);
                    XSetForeground(dpy, g_gc, needs_rsvp ? g_pal.bg_today : g_pal.bg_accent);
                    XFillRectangle(dpy, buf, g_gc, cx + 3, ey, (unsigned)(cw - 6), 14);
                    XSetForeground(dpy, g_gc, g_pal.fg_white);
                    draw_clipped_str(dpy, buf, g_gc, needs_rsvp ? g_fn_bold : g_fn_small, cx + 6, ey + 11, cw - 10, line);
                } else {
                    char line[256];
                    snprintf(line, sizeof(line), "%s%s %s", pfx, ev->start_disp, ev->subject);
                    XSetForeground(dpy, g_gc, g_pal.fg_well);
                    draw_clipped_str(dpy, buf, g_gc, g_fn_small, cx + 5, ey + 11, cw - 8, line);
                }
                ey += line_h;
                drawn++;
            }
            if (total_day > drawn) {
                char more[32];
                snprintf(more, sizeof(more), "+%d more...", total_day - drawn);
                XSetForeground(dpy, g_gc, g_pal.bg_today);
                draw_clipped_str(dpy, buf, g_gc, g_fn_bold, cx + 5, ey + 11, cw - 8, more);
            }
        }
    }
}

static void render_day_view(Display *dpy, Drawable buf, int w, int h) {
    int side_w = 220;
    if (w < 540) side_w = w / 3;

    int mh = (h - 16) / 3;
    int py = g_sel_y, pm = g_sel_m - 1;
    if (pm < 1) { pm = 12; py--; }
    int ny = g_sel_y, nm = g_sel_m + 1;
    if (nm > 12) { nm = 1; ny++; }

    draw_mini_month(dpy, buf, 4, 4, side_w - 8, mh - 4, py, pm, 0);
    draw_mini_month(dpy, buf, 4, 4 + mh, side_w - 8, mh - 4, g_sel_y, g_sel_m, 1);
    draw_mini_month(dpy, buf, 4, 4 + 2 * mh, side_w - 8, h - 8 - 2 * mh, ny, nm, 0);

    int rx = side_w + 4;
    int rw = w - rx - 4;
    char ymd[16];
    fmt_ymd(g_sel_y, g_sel_m, g_sel_d, ymd);

    int ban_h = 34;
    XSetForeground(dpy, g_gc, g_pal.bg_chrome);
    XFillRectangle(dpy, buf, g_gc, rx, 4, 80, (unsigned)ban_h);
    XSetForeground(dpy, g_gc, g_pal.fg_chrome);
    draw_clipped_str(dpy, buf, g_gc, g_fn_bold, rx + 8, 25, 70, "All Day");
    XSetForeground(dpy, g_gc, g_pal.grid_line);
    XDrawRectangle(dpy, buf, g_gc, rx, 4, (unsigned)rw, (unsigned)ban_h);

    int bx = rx + 86;
    for (int i = 0; i < g_num_events; i++) {
        if (g_events[i].all_day && event_on_date(&g_events[i], ymd)) {
            char blbl[240];
            snprintf(blbl, sizeof(blbl), "%s%s", event_invite_prefix(&g_events[i]), g_events[i].subject);
            XSetForeground(dpy, g_gc, g_pal.bg_accent);
            XFillRectangle(dpy, buf, g_gc, bx, 8, 180, 24);
            XSetForeground(dpy, g_gc, g_pal.fg_white);
            draw_clipped_str(dpy, buf, g_gc, g_fn_bold, bx + 6, 24, 168, blbl);
            bx += 188;
            if (bx + 80 > rx + rw) break;
        }
    }

    int start_hr = 7, num_hrs = 14;
    int top_y = 4 + ban_h + 4;
    int slot_h = (h - top_y - 6) / num_hrs;
    if (slot_h < 22) slot_h = 22;

    for (int s = 0; s < num_hrs; s++) {
        int hr = start_hr + s;
        int sy = top_y + s * slot_h;

        XSetForeground(dpy, g_gc, (hr == g_sel_hour) ? g_pal.bg_accent : g_pal.bg_chrome);
        XFillRectangle(dpy, buf, g_gc, rx, sy, 80, (unsigned)slot_h);

        char hstr[24];
        if (s == num_hrs - 1) snprintf(hstr, sizeof(hstr), "Evening");
        else                  snprintf(hstr, sizeof(hstr), "%d:00 %s", (hr % 12) ? (hr % 12) : 12, (hr < 12) ? "AM" : "PM");

        XSetForeground(dpy, g_gc, (hr == g_sel_hour) ? g_pal.fg_white : g_pal.fg_chrome);
        draw_clipped_str(dpy, buf, g_gc, g_fn_bold, rx + 8, sy + 16, 70, hstr);

        XSetForeground(dpy, g_gc, g_pal.grid_line);
        XDrawRectangle(dpy, buf, g_gc, rx, sy, (unsigned)rw, (unsigned)slot_h);

        int ex = rx + 88;
        for (int i = 0; i < g_num_events; i++) {
            const CalEvent *ev = &g_events[i];
            if (ev->all_day || !event_on_date(ev, ymd)) continue;
            int ev_hr = ev->start_min / 60;
            int match = (s == 0 && ev_hr <= start_hr) ||
                        (s == num_hrs - 1 && ev_hr >= start_hr + num_hrs - 1) ||
                        (ev_hr == hr);
            if (!match) continue;

            const char *pfx = event_invite_prefix(ev);
            char desc[460];
            if (ev->location[0])
                snprintf(desc, sizeof(desc), "%s%s  %s  [%s]", pfx, ev->time_range, ev->subject, ev->location);
            else
                snprintf(desc, sizeof(desc), "%s%s  %s", pfx, ev->time_range, ev->subject);

            XSetForeground(dpy, g_gc, g_pal.bg_today);
            XFillRectangle(dpy, buf, g_gc, ex, sy + 3, 4, (unsigned)(slot_h - 6));
            XSetForeground(dpy, g_gc, g_pal.fg_well);
            draw_clipped_str(dpy, buf, g_gc, g_fn_bold, ex + 8, sy + 17, rx + rw - ex - 14, desc);
            break;
        }
    }
}

static void render_week_view(Display *dpy, Drawable buf, int w, int h) {
    int dow = day_of_week(g_sel_y, g_sel_m, g_sel_d);
    int wy = g_sel_y, wm = g_sel_m, wd = g_sel_d;
    add_days(&wy, &wm, &wd, -dow);

    int col_w = w / 7;
    for (int c = 0; c < 7; c++) {
        int cy = wy, cm = wm, cd = wd;
        add_days(&cy, &cm, &cd, c);
        int cx = c * col_w;
        int cw = (c == 6) ? (w - cx) : col_w;

        int is_tod = (cy == g_today_y && cm == g_today_m && cd == g_today_d);
        int is_sel = (cy == g_sel_y && cm == g_sel_m && cd == g_sel_d);

        XSetForeground(dpy, g_gc, is_tod ? g_pal.bg_today : (is_sel ? g_pal.bg_accent : g_pal.bg_chrome));
        XFillRectangle(dpy, buf, g_gc, cx, 0, (unsigned)cw, 30);

        char chdr[48];
        snprintf(chdr, sizeof(chdr), "%s %d/%d", DOW_SHORT[c], cm, cd);
        XSetForeground(dpy, g_gc, (is_tod || is_sel) ? g_pal.fg_white : g_pal.fg_chrome);
        draw_clipped_str(dpy, buf, g_gc, g_fn_bold, cx + 6, 20, cw - 10, chdr);

        XSetForeground(dpy, g_gc, g_pal.grid_line);
        XDrawRectangle(dpy, buf, g_gc, cx, 0, (unsigned)cw, (unsigned)h);

        char ymd[16];
        fmt_ymd(cy, cm, cd, ymd);
        int ey = 38;
        for (int i = 0; i < g_num_events; i++) {
            const CalEvent *ev = &g_events[i];
            if (!event_on_date(ev, ymd)) continue;
            if (ey + 36 > h) break;

            const char *pfx = event_invite_prefix(ev);
            char subj_disp[240];
            snprintf(subj_disp, sizeof(subj_disp), "%s%s", pfx, ev->subject);

            if (ev->all_day) {
                XSetForeground(dpy, g_gc, g_pal.bg_accent);
                XFillRectangle(dpy, buf, g_gc, cx + 4, ey, (unsigned)(cw - 8), 20);
                XSetForeground(dpy, g_gc, g_pal.fg_white);
                draw_clipped_str(dpy, buf, g_gc, g_fn_bold, cx + 7, ey + 14, cw - 14, subj_disp);
                ey += 25;
            } else {
                XSetForeground(dpy, g_gc, g_pal.bg_today);
                draw_clipped_str(dpy, buf, g_gc, g_fn_bold, cx + 5, ey + 12, cw - 10, ev->time_range);
                XSetForeground(dpy, g_gc, g_pal.fg_well);
                draw_clipped_str(dpy, buf, g_gc, g_fn_reg, cx + 5, ey + 26, cw - 10, subj_disp);
                if (ev->location[0]) {
                    draw_clipped_str(dpy, buf, g_gc, g_fn_small, cx + 5, ey + 39, cw - 10, ev->location);
                    ey += 46;
                } else {
                    ey += 34;
                }
                XSetForeground(dpy, g_gc, g_pal.grid_line);
                XDrawLine(dpy, buf, g_gc, cx + 4, ey - 3, cx + cw - 4, ey - 3);
            }
        }
    }
}

static void render_year_view(Display *dpy, Drawable buf, int w, int h) {
    int cw = (w - 16) / 4;
    int rh = (h - 16) / 3;
    for (int m = 1; m <= 12; m++) {
        int r = (m - 1) / 4;
        int c = (m - 1) % 4;
        draw_mini_month(dpy, buf, 6 + c * cw, 6 + r * rh, cw - 8, rh - 8, g_sel_y, m, 1);
    }
}

static void redraw_calendar(void) {
    if (!g_canvas || !XtIsRealized(g_canvas)) return;
    Display *dpy = XtDisplay(g_canvas);
    Window win = XtWindow(g_canvas);
    Dimension dw = 0, dh = 0;
    XtVaGetValues(g_canvas, XmNwidth, &dw, XmNheight, &dh, NULL);
    int w = (dw > 40) ? (int)dw : 40;
    int h = (dh > 40) ? (int)dh : 40;

    if (g_gc == None) {
        g_gc = XCreateGC(dpy, win, 0, NULL);
    }
    if (g_backbuf == None || g_bb_w != w || g_bb_h != h) {
        if (g_backbuf != None) XFreePixmap(dpy, g_backbuf);
        g_backbuf = XCreatePixmap(dpy, win, (unsigned)w, (unsigned)h,
                                  (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
        g_bb_w = w;
        g_bb_h = h;
    }

    XSetForeground(dpy, g_gc, g_pal.bg_well);
    XFillRectangle(dpy, g_backbuf, g_gc, 0, 0, (unsigned)w, (unsigned)h);

    switch (g_view) {
        case VIEW_MONTH: render_month_view(dpy, g_backbuf, w, h); break;
        case VIEW_DAY:   render_day_view(dpy, g_backbuf, w, h);   break;
        case VIEW_WEEK:  render_week_view(dpy, g_backbuf, w, h);  break;
        case VIEW_YEAR:  render_year_view(dpy, g_backbuf, w, h);  break;
    }

    XCopyArea(dpy, g_backbuf, win, g_gc, 0, 0, (unsigned)w, (unsigned)h, 0, 0);
    XFlush(dpy);
}

/* ---------------- Classic dtcm Appointment Editor & Invite RSVP ---------------- */
static void open_appt_editor(Widget w, XtPointer c, XtPointer d);

static int parse_time_minutes(const char *s, int fallback) {
    if (!s || !s[0]) return fallback;
    int hr = 0, mn = 0;
    char ap[16] = "";
    if (sscanf(s, " %d : %d %15s", &hr, &mn, ap) >= 2) {
        if (ap[0] == 'p' || ap[0] == 'P') { if (hr < 12) hr += 12; }
        else if (ap[0] == 'a' || ap[0] == 'A') { if (hr == 12) hr = 0; }
        return (hr * 60 + mn) % 1440;
    }
    if (sscanf(s, " %d %15s", &hr, ap) >= 1) {
        if (ap[0] == 'p' || ap[0] == 'P') { if (hr < 12) hr += 12; }
        else if (ap[0] == 'a' || ap[0] == 'A') { if (hr == 12) hr = 0; }
        return (hr * 60) % 1440;
    }
    return fallback;
}

static void fmt_minutes_ampm(int mins, char out[32]) {
    int h = (mins / 60) % 24;
    int m = mins % 60;
    int h12 = (h % 12) ? (h % 12) : 12;
    snprintf(out, 32, "%d:%02d %s", h12, m, (h < 12) ? "AM" : "PM");
}

static void set_rsvp_controls_enabled(int enabled) {
    if (g_rsvp_acc_btn)  XtSetSensitive(g_rsvp_acc_btn,  enabled ? True : False);
    if (g_rsvp_tent_btn) XtSetSensitive(g_rsvp_tent_btn, enabled ? True : False);
    if (g_rsvp_dec_btn)  XtSetSensitive(g_rsvp_dec_btn,  enabled ? True : False);
    if (g_rsvp_note_tf)  XtSetSensitive(g_rsvp_note_tf,  enabled ? True : False);
    if (g_rsvp_send_tb)  XtSetSensitive(g_rsvp_send_tb,  enabled ? True : False);
}

static void load_event_into_editor_fields(const CalEvent *ev) {
    if (!g_ed_dlg || !ev) return;
    snprintf(g_ed_selected_id, sizeof(g_ed_selected_id), "%s", ev->id);
    XmTextFieldSetString(g_ed_date_tf, (char *)ev->start_date);
    char sbuf[32], ebuf[32];
    fmt_minutes_ampm(ev->start_min, sbuf);
    fmt_minutes_ampm(ev->end_min, ebuf);
    XmTextFieldSetString(g_ed_start_tf, sbuf);
    XmTextFieldSetString(g_ed_end_tf, ebuf);
    XmToggleButtonSetState(g_ed_allday_tb, ev->all_day ? True : False, False);
    XmTextFieldSetString(g_ed_loc_tf, (char *)ev->location);
    XmTextFieldSetString(g_ed_att_tf, (char *)ev->attendees);

    char what_full[800];
    if (ev->notes[0]) snprintf(what_full, sizeof(what_full), "%s\n%s", ev->subject, ev->notes);
    else              snprintf(what_full, sizeof(what_full), "%s", ev->subject);
    XmTextSetString(g_ed_what_txt, what_full);

    char org_line[380];
    if (ev->organizer[0] || ev->organizer_email[0]) {
        const char *who = ev->organizer[0] ? ev->organizer : ev->organizer_email;
        snprintf(org_line, sizeof(org_line), "Organizer: %s   |   Status: %s",
                 who, human_response_status(ev));
    } else {
        snprintf(org_line, sizeof(org_line), "Status: %s", human_response_status(ev));
    }
    set_label(g_ed_org_lbl, org_line);
    set_rsvp_controls_enabled(!ev->is_organizer);
}

static void refresh_editor_list(void) {
    if (!g_ed_dlg || !g_ed_list) return;
    char ymd[16];
    fmt_ymd(g_sel_y, g_sel_m, g_sel_d, ymd);

    g_ed_num_mapped = 0;
    XmString items[MAX_EVENTS];
    int sel_list_pos = 0;
    for (int i = 0; i < g_num_events; i++) {
        if (!event_on_date(&g_events[i], ymd)) continue;
        g_ed_map_idx[g_ed_num_mapped] = i;
        char row[320];
        snprintf(row, sizeof(row), "%-10s  %s%s",
                 g_events[i].start_disp,
                 event_invite_prefix(&g_events[i]),
                 g_events[i].subject);
        items[g_ed_num_mapped] = XmStringCreateLocalized(row);
        if (g_ed_selected_id[0] && strcmp(g_events[i].id, g_ed_selected_id) == 0) {
            sel_list_pos = g_ed_num_mapped + 1;
        }
        g_ed_num_mapped++;
    }

    if (g_ed_num_mapped == 0) {
        XmString none = XmStringCreateLocalized("(No appointments on selected date)");
        XtVaSetValues(g_ed_list, XmNitems, &none, XmNitemCount, 1, NULL);
        XmStringFree(none);
    } else {
        XtVaSetValues(g_ed_list, XmNitems, items, XmNitemCount, g_ed_num_mapped, NULL);
        if (sel_list_pos > 0) XmListSelectPos(g_ed_list, sel_list_pos, False);
        for (int i = 0; i < g_ed_num_mapped; i++) XmStringFree(items[i]);
    }
}

static void populate_editor_for_selected_day(void) {
    if (!g_ed_dlg) return;
    char ymd[16], sbuf[32], ebuf[32];
    fmt_ymd(g_sel_y, g_sel_m, g_sel_d, ymd);
    fmt_minutes_ampm(g_sel_hour * 60, sbuf);
    fmt_minutes_ampm((g_sel_hour + 1) * 60, ebuf);

    g_ed_selected_id[0] = '\0';
    XmTextFieldSetString(g_ed_date_tf, ymd);
    XmTextFieldSetString(g_ed_start_tf, sbuf);
    XmTextFieldSetString(g_ed_end_tf, ebuf);
    XmToggleButtonSetState(g_ed_allday_tb, False, False);
    XmTextFieldSetString(g_ed_loc_tf, (char *)"");
    XmTextFieldSetString(g_ed_att_tf, (char *)"");
    XmTextSetString(g_ed_what_txt, (char *)"");
    if (g_rsvp_note_tf) XmTextFieldSetString(g_rsvp_note_tf, (char *)"");
    set_label(g_ed_org_lbl, "New Event — fill fields on left & click Insert (or select an invite above to reply)");
    set_rsvp_controls_enabled(0);
    refresh_editor_list();
}

static void on_ed_list_select(Widget w, XtPointer c, XtPointer call_data) {
    (void)w; (void)c;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    int pos = cbs->item_position - 1;
    if (pos < 0 || pos >= g_ed_num_mapped) return;
    load_event_into_editor_fields(&g_events[g_ed_map_idx[pos]]);
}

static void send_editor_mutation(const char *op_name) {
    if (!g_to_backend) return;
    char *dt  = XmTextFieldGetString(g_ed_date_tf);
    char *st  = XmTextFieldGetString(g_ed_start_tf);
    char *et  = XmTextFieldGetString(g_ed_end_tf);
    char *loc = XmTextFieldGetString(g_ed_loc_tf);
    char *att = XmTextFieldGetString(g_ed_att_tf);
    char *wht = XmTextGetString(g_ed_what_txt);
    int allday = XmToggleButtonGetState(g_ed_allday_tb) ? 1 : 0;

    int s_min = parse_time_minutes(st, 9 * 60);
    int e_min = parse_time_minutes(et, s_min + 60);

    char subj[256] = "";
    const char *notes = "";
    if (wht) {
        const char *nl = strchr(wht, '\n');
        if (nl) {
            size_t sl = (size_t)(nl - wht);
            if (sl >= sizeof(subj)) sl = sizeof(subj) - 1;
            memcpy(subj, wht, sl);
            subj[sl] = '\0';
            notes = nl + 1;
        } else {
            snprintf(subj, sizeof(subj), "%s", wht);
        }
    }

    fputs("{\"op\":", g_to_backend);
    json_write_escaped(g_to_backend, op_name);
    fputs(",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_ed_selected_id);
    fputs(",\"date\":", g_to_backend);
    json_write_escaped(g_to_backend, dt ? dt : "");
    fprintf(g_to_backend, ",\"start_min\":%d,\"end_min\":%d,\"all_day\":%s",
            s_min, e_min, allday ? "true" : "false");
    fputs(",\"subject\":", g_to_backend);
    json_write_escaped(g_to_backend, subj);
    fputs(",\"location\":", g_to_backend);
    json_write_escaped(g_to_backend, loc ? loc : "");
    fputs(",\"attendees\":", g_to_backend);
    json_write_escaped(g_to_backend, att ? att : "");
    fputs(",\"notes\":", g_to_backend);
    json_write_escaped(g_to_backend, notes);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);

    if (dt)  XtFree(dt);
    if (st)  XtFree(st);
    if (et)  XtFree(et);
    if (loc) XtFree(loc);
    if (att) XtFree(att);
    if (wht) XtFree(wht);
}

static void on_rsvp_btn_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    const char *action = (const char *)client_data;
    if (!g_to_backend || !g_ed_selected_id[0] || !action) return;

    char *note = g_rsvp_note_tf ? XmTextFieldGetString(g_rsvp_note_tf) : NULL;
    int send_resp = g_rsvp_send_tb ? (XmToggleButtonGetState(g_rsvp_send_tb) ? 1 : 0) : 1;

    fputs("{\"op\":\"rsvp_event\",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_ed_selected_id);
    fputs(",\"action\":", g_to_backend);
    json_write_escaped(g_to_backend, action);
    fputs(",\"comment\":", g_to_backend);
    json_write_escaped(g_to_backend, note ? note : "");
    fprintf(g_to_backend, ",\"send_response\":%s}\n", send_resp ? "true" : "false");
    fflush(g_to_backend);

    if (note) XtFree(note);
    if (g_rsvp_note_tf) XmTextFieldSetString(g_rsvp_note_tf, (char *)"");
    if (strcmp(action, "decline") == 0) {
        populate_editor_for_selected_day();
    }
}

static void on_ed_insert(Widget w, XtPointer c, XtPointer d) { (void)w; (void)c; (void)d; send_editor_mutation("insert_event"); }
static void on_ed_change(Widget w, XtPointer c, XtPointer d) { (void)w; (void)c; (void)d; send_editor_mutation("update_event"); }
static void on_ed_delete(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (!g_to_backend || !g_ed_selected_id[0]) return;
    fputs("{\"op\":\"delete_event\",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_ed_selected_id);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
    populate_editor_for_selected_day();
}
static void on_ed_clear(Widget w, XtPointer c, XtPointer d)  { (void)w; (void)c; (void)d; populate_editor_for_selected_day(); }
static void on_ed_close(Widget w, XtPointer c, XtPointer d)  { (void)w; (void)c; (void)d; XtUnmanageChild(g_ed_dlg); }

static void ensure_appt_editor_created(void) {
    if (g_ed_dlg) return;
    Arg args[4];
    Cardinal n = 0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNwidth, 760); n++;
    XtSetArg(args[n], XmNheight, 490); n++;
    g_ed_dlg = XmCreateFormDialog(g_toplevel, (char *)"apptEditor", args, n);
    XtVaSetValues(XtParent(g_ed_dlg), XmNtitle, "Calendar : Appointment Editor & Invitations", NULL);

    Widget bot = XtVaCreateManagedWidget(
        "edBtns", xmRowColumnWidgetClass, g_ed_dlg,
        XmNorientation, XmHORIZONTAL, XmNspacing, 10,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
        NULL
    );
    Widget b_ins = XtVaCreateManagedWidget("Insert (Add Event)", xmPushButtonWidgetClass, bot, XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
    Widget b_chg = XtVaCreateManagedWidget("Change", xmPushButtonWidgetClass, bot, XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
    Widget b_del = XtVaCreateManagedWidget("Delete", xmPushButtonWidgetClass, bot, XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
    Widget b_clr = XtVaCreateManagedWidget("Clear",  xmPushButtonWidgetClass, bot, XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
    Widget b_cls = XtVaCreateManagedWidget("Close",  xmPushButtonWidgetClass, bot, XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
    XtAddCallback(b_ins, XmNactivateCallback, on_ed_insert, NULL);
    XtAddCallback(b_chg, XmNactivateCallback, on_ed_change, NULL);
    XtAddCallback(b_del, XmNactivateCallback, on_ed_delete, NULL);
    XtAddCallback(b_clr, XmNactivateCallback, on_ed_clear, NULL);
    XtAddCallback(b_cls, XmNactivateCallback, on_ed_close, NULL);

    Widget sep = XtVaCreateManagedWidget(
        "edSep", xmSeparatorWidgetClass, g_ed_dlg,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, bot, XmNbottomOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    Widget split = XtVaCreateManagedWidget(
        "edSplit", xmFormWidgetClass, g_ed_dlg,
        XmNfractionBase, 100,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 10,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, sep, XmNbottomOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 12,
        NULL
    );

    /* Left Form: Date, Start, End, All Day, Location, Invite Attendees, What */
    Widget lrc = XtVaCreateManagedWidget(
        "edLeft", xmRowColumnWidgetClass, split,
        XmNorientation, XmVERTICAL, XmNspacing, 4,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 49,
        NULL
    );

    XtVaCreateManagedWidget("Date (YYYY-MM-DD):", xmLabelWidgetClass, lrc, XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_ed_date_tf = XtVaCreateManagedWidget("edDate", xmTextFieldWidgetClass, lrc, NULL);

    Widget trow = XtVaCreateManagedWidget("trow", xmRowColumnWidgetClass, lrc,
                                          XmNorientation, XmHORIZONTAL, XmNspacing, 6, NULL);
    XtVaCreateManagedWidget("Start:", xmLabelWidgetClass, trow, NULL);
    g_ed_start_tf = XtVaCreateManagedWidget("edStart", xmTextFieldWidgetClass, trow, XmNcolumns, 9, NULL);
    XtVaCreateManagedWidget("End:", xmLabelWidgetClass, trow, NULL);
    g_ed_end_tf = XtVaCreateManagedWidget("edEnd", xmTextFieldWidgetClass, trow, XmNcolumns, 9, NULL);

    g_ed_allday_tb = XtVaCreateManagedWidget("All Day Event", xmToggleButtonWidgetClass, lrc, NULL);

    XtVaCreateManagedWidget("Location:", xmLabelWidgetClass, lrc, XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_ed_loc_tf = XtVaCreateManagedWidget("edLoc", xmTextFieldWidgetClass, lrc, NULL);

    XtVaCreateManagedWidget("Invite Attendees (emails, comma-separated):", xmLabelWidgetClass, lrc, XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_ed_att_tf = XtVaCreateManagedWidget("edAtt", xmTextFieldWidgetClass, lrc, NULL);

    XtVaCreateManagedWidget("What (Line 1 = Title, below = Notes):", xmLabelWidgetClass, lrc, XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    Arg ta[4]; Cardinal tn = 0;
    XtSetArg(ta[tn], XmNeditMode, XmMULTI_LINE_EDIT); tn++;
    XtSetArg(ta[tn], XmNrows, 4); tn++;
    g_ed_what_txt = XmCreateScrolledText(lrc, (char *)"edWhat", ta, tn);
    XtManageChild(g_ed_what_txt);

    /* Right Side: Selected Day's Appointments + Organizer Status + RSVP Reply Box */
    Widget rform = XtVaCreateManagedWidget(
        "edRight", xmFormWidgetClass, split,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 51,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    Widget rsvp_fr = XtVaCreateManagedWidget(
        "rsvpFrame", xmFrameWidgetClass, rform,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget rsvp_rc = XtVaCreateManagedWidget(
        "rsvpRc", xmRowColumnWidgetClass, rsvp_fr,
        XmNorientation, XmVERTICAL, XmNspacing, 4,
        XmNmarginWidth, 6, XmNmarginHeight, 6,
        NULL
    );
    XtVaCreateManagedWidget("Reply to Meeting Invitation (RSVP):", xmLabelWidgetClass, rsvp_rc,
                            XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_ed_org_lbl = XtVaCreateManagedWidget("Select an incoming invitation above to reply", xmLabelWidgetClass, rsvp_rc,
                                           XmNalignment, XmALIGNMENT_BEGINNING, NULL);

    XtVaCreateManagedWidget("Optional Reply Message to Organizer:", xmLabelWidgetClass, rsvp_rc,
                            XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_rsvp_note_tf = XtVaCreateManagedWidget("rsvpNote", xmTextFieldWidgetClass, rsvp_rc, NULL);

    g_rsvp_send_tb = XtVaCreateManagedWidget("Send response email to organizer", xmToggleButtonWidgetClass, rsvp_rc,
                                             XmNset, True, NULL);

    Widget rsvp_btns = XtVaCreateManagedWidget(
        "rsvpBtns", xmRowColumnWidgetClass, rsvp_rc,
        XmNorientation, XmHORIZONTAL, XmNspacing, 8,
        NULL
    );
    g_rsvp_acc_btn  = XtVaCreateManagedWidget("Accept",    xmPushButtonWidgetClass, rsvp_btns, XmNmarginWidth, 10, XmNmarginHeight, 3, NULL);
    g_rsvp_tent_btn = XtVaCreateManagedWidget("Tentative", xmPushButtonWidgetClass, rsvp_btns, XmNmarginWidth, 10, XmNmarginHeight, 3, NULL);
    g_rsvp_dec_btn  = XtVaCreateManagedWidget("Decline",   xmPushButtonWidgetClass, rsvp_btns, XmNmarginWidth, 10, XmNmarginHeight, 3, NULL);
    XtAddCallback(g_rsvp_acc_btn,  XmNactivateCallback, on_rsvp_btn_cb, (XtPointer)"accept");
    XtAddCallback(g_rsvp_tent_btn, XmNactivateCallback, on_rsvp_btn_cb, (XtPointer)"tentative");
    XtAddCallback(g_rsvp_dec_btn,  XmNactivateCallback, on_rsvp_btn_cb, (XtPointer)"decline");

    Widget rlbl = XtVaCreateManagedWidget(
        "Appointments on Selected Date ([?] = Needs Reply):", xmLabelWidgetClass, rform,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    Arg la[4]; Cardinal ln = 0;
    XtSetArg(la[ln], XmNselectionPolicy, XmBROWSE_SELECT); ln++;
    g_ed_list = XmCreateScrolledList(rform, (char *)"edList", la, ln);
    XtVaSetValues(XtParent(g_ed_list),
                  XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, rlbl, XmNtopOffset, 4,
                  XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, rsvp_fr, XmNbottomOffset, 8,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtAddCallback(g_ed_list, XmNbrowseSelectionCallback, on_ed_list_select, NULL);
    XtManageChild(g_ed_list);
}

static void open_appt_editor(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    ensure_appt_editor_created();
    populate_editor_for_selected_day();
    theme_widgets(g_ed_dlg, &g_pal);
    XtManageChild(g_ed_dlg);
}

static void select_and_edit_event_idx(int ev_idx) {
    if (ev_idx < 0 || ev_idx >= g_num_events) return;
    const CalEvent *ev = &g_events[ev_idx];
    int y = 0, m = 0, d = 0;
    if (sscanf(ev->start_date, "%d-%d-%d", &y, &m, &d) == 3) {
        g_sel_y = y; g_sel_m = m; g_sel_d = d;
        update_titles();
        redraw_calendar();
    }
    ensure_appt_editor_created();
    snprintf(g_ed_selected_id, sizeof(g_ed_selected_id), "%s", ev->id);
    refresh_editor_list();
    load_event_into_editor_fields(ev);
    theme_widgets(g_ed_dlg, &g_pal);
    XtManageChild(g_ed_dlg);
}

/* ---------------- Pending Invitations Dialog ---------------- */
static void refresh_invites_dialog(void) {
    if (!g_inv_dlg || !g_inv_list) return;
    g_inv_num_mapped = 0;
    XmString items[MAX_EVENTS];

    /* Pass 1: Unresponded invitations first, Pass 2: Tentative / Accepted invitations */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_num_events; i++) {
            const CalEvent *ev = &g_events[i];
            if (ev->is_organizer) continue;
            int is_unresp = (strcmp(ev->response_status, "notResponded") == 0);
            if ((pass == 0 && !is_unresp) || (pass == 1 && is_unresp)) continue;

            g_inv_map_idx[g_inv_num_mapped] = i;
            char row[420];
            const char *who = ev->organizer[0] ? ev->organizer : ev->organizer_email;
            snprintf(row, sizeof(row), "%-14s  %s  %-18s  %s  (From: %s)",
                     is_unresp ? "[NEEDS REPLY]" : (strcmp(ev->response_status, "tentativelyAccepted") == 0 ? "[Tentative]" : "[Accepted]"),
                     ev->start_date, ev->time_range, ev->subject, who[0] ? who : "Organizer");
            items[g_inv_num_mapped++] = XmStringCreateLocalized(row);
        }
    }

    if (g_inv_num_mapped == 0) {
        XmString none = XmStringCreateLocalized("(No incoming meeting invitations in current calendar window)");
        XtVaSetValues(g_inv_list, XmNitems, &none, XmNitemCount, 1, NULL);
        XmStringFree(none);
    } else {
        XtVaSetValues(g_inv_list, XmNitems, items, XmNitemCount, g_inv_num_mapped, NULL);
        for (int i = 0; i < g_inv_num_mapped; i++) XmStringFree(items[i]);
    }
}

static void on_inv_list_select_cb(Widget w, XtPointer c, XtPointer call_data) {
    (void)w; (void)c;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    int pos = cbs->item_position - 1;
    if (pos < 0 || pos >= g_inv_num_mapped) return;
    select_and_edit_event_idx(g_inv_map_idx[pos]);
}

static void on_inv_close_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_inv_dlg) XtUnmanageChild(g_inv_dlg);
}

static void open_invites_dialog(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (!g_inv_dlg) {
        Arg a[4]; Cardinal n = 0;
        XtSetArg(a[n], XmNautoUnmanage, False); n++;
        XtSetArg(a[n], XmNwidth, 660); n++;
        XtSetArg(a[n], XmNheight, 320); n++;
        g_inv_dlg = XmCreateFormDialog(g_toplevel, (char *)"invitesDlg", a, n);
        XtVaSetValues(XtParent(g_inv_dlg), XmNtitle, "Calendar : Incoming Meeting Invitations", NULL);

        Widget hdr = XtVaCreateManagedWidget(
            "Click any invitation below to open it and send an Accept, Tentative, or Decline reply:",
            xmLabelWidgetClass, g_inv_dlg,
            XmNalignment, XmALIGNMENT_BEGINNING,
            XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 8,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
            NULL
        );

        Widget cls_b = XtVaCreateManagedWidget(
            "Close", xmPushButtonWidgetClass, g_inv_dlg,
            XmNmarginWidth, 14, XmNmarginHeight, 4,
            XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 8,
            XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
            NULL
        );
        XtAddCallback(cls_b, XmNactivateCallback, on_inv_close_cb, NULL);

        Arg la[4]; Cardinal ln = 0;
        XtSetArg(la[ln], XmNselectionPolicy, XmBROWSE_SELECT); ln++;
        g_inv_list = XmCreateScrolledList(g_inv_dlg, (char *)"invList", la, ln);
        XtVaSetValues(XtParent(g_inv_list),
                      XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, hdr, XmNtopOffset, 6,
                      XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, cls_b, XmNbottomOffset, 8,
                      XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
                      XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
                      NULL);
        XtAddCallback(g_inv_list, XmNbrowseSelectionCallback, on_inv_list_select_cb, NULL);
        XtAddCallback(g_inv_list, XmNdefaultActionCallback, on_inv_list_select_cb, NULL);
        XtManageChild(g_inv_list);
    }
    refresh_invites_dialog();
    theme_widgets(g_inv_dlg, &g_pal);
    XtManageChild(g_inv_dlg);
}

/* ---------------- Month Appointment List Dialog ---------------- */
static void on_month_list_activate_cb(Widget w, XtPointer c, XtPointer call_data) {
    (void)w; (void)c;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    int pos = cbs->item_position - 1;
    if (pos < 0 || pos >= g_list_num_mapped) return;
    select_and_edit_event_idx(g_list_map_idx[pos]);
}

static void open_appt_list_dialog(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (!g_list_dlg) {
        Arg a[4]; Cardinal n = 0;
        XtSetArg(a[n], XmNwidth, 600); n++;
        XtSetArg(a[n], XmNheight, 350); n++;
        g_list_dlg = XmCreateFormDialog(g_toplevel, (char *)"apptListDlg", a, n);
        XtVaSetValues(XtParent(g_list_dlg), XmNtitle, "Calendar : Month Appointment List (Double-click to Edit / Reply)", NULL);

        Arg la[4]; Cardinal ln = 0;
        XtSetArg(la[ln], XmNselectionPolicy, XmBROWSE_SELECT); ln++;
        g_list_box = XmCreateScrolledList(g_list_dlg, (char *)"fullApptList", la, ln);
        XtVaSetValues(XtParent(g_list_box),
                      XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 8,
                      XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 8,
                      XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8,
                      XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
                      NULL);
        XtAddCallback(g_list_box, XmNdefaultActionCallback, on_month_list_activate_cb, NULL);
        XtManageChild(g_list_box);
    }

    char ym_pref[16];
    snprintf(ym_pref, sizeof(ym_pref), "%04d-%02d", g_sel_y, g_sel_m);
    XmString items[MAX_EVENTS];
    g_list_num_mapped = 0;
    for (int i = 0; i < g_num_events; i++) {
        if (strncmp(g_events[i].start_date, ym_pref, 7) != 0) continue;
        g_list_map_idx[g_list_num_mapped] = i;
        char row[420];
        const char *pfx = event_invite_prefix(&g_events[i]);
        if (g_events[i].location[0])
            snprintf(row, sizeof(row), "%s  %-18s  %s%s (%s)",
                     g_events[i].start_date, g_events[i].time_range, pfx, g_events[i].subject, g_events[i].location);
        else
            snprintf(row, sizeof(row), "%s  %-18s  %s%s",
                     g_events[i].start_date, g_events[i].time_range, pfx, g_events[i].subject);
        items[g_list_num_mapped++] = XmStringCreateLocalized(row);
    }
    if (g_list_num_mapped == 0) {
        XmString none = XmStringCreateLocalized("(No appointments scheduled in this month)");
        XtVaSetValues(g_list_box, XmNitems, &none, XmNitemCount, 1, NULL);
        XmStringFree(none);
    } else {
        XtVaSetValues(g_list_box, XmNitems, items, XmNitemCount, g_list_num_mapped, NULL);
        for (int i = 0; i < g_list_num_mapped; i++) XmStringFree(items[i]);
    }
    theme_widgets(g_list_dlg, &g_pal);
    XtManageChild(g_list_dlg);
}

/* ---------------- Companion Launch: Open or Raise dtMail ---------------- */
static Window find_window_by_class_recursive(Display *dpy, Window w, const char *target_class) {
    XClassHint ch = {NULL, NULL};
    if (XGetClassHint(dpy, w, &ch)) {
        int match = (ch.res_class && strcasecmp(ch.res_class, target_class) == 0) ||
                    (ch.res_name && strcasecmp(ch.res_name, target_class) == 0);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        if (match) return w;
    }
    Window root = None, parent = None, *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &children, &nchildren)) return None;
    Window found = None;
    for (unsigned int i = 0; i < nchildren && found == None; i++) {
        found = find_window_by_class_recursive(dpy, children[i], target_class);
    }
    if (children) XFree(children);
    return found;
}

static void on_open_dtmail_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    Display *dpy = XtDisplay(g_toplevel);
    Window existing = find_window_by_class_recursive(dpy, DefaultRootWindow(dpy), "CdeMail");
    if (existing != None) {
        XMapRaised(dpy, existing);
        XFlush(dpy);
        set_label(g_st_lbl, "Switched to companion app: dtMail");
        return;
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char bin[512];
    snprintf(bin, sizeof(bin), "%s/.local/bin/dtmail", home);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(bin, "dtmail", (char *)NULL);
        _exit(127);
    }
    set_label(g_st_lbl, "Launching companion app: dtMail...");
}

/* ---------------- Mouse Hit-Testing on Calendar Canvas ---------------- */
static void on_canvas_expose(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    redraw_calendar();
}

static void on_canvas_resize(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    redraw_calendar();
}

static void on_canvas_input(Widget w, XtPointer closure, XEvent *ev, Boolean *cont) {
    (void)closure; (void)cont;
    if (ev->type != ButtonPress || ev->xbutton.button != Button1) return;

    Dimension dw = 0, dh = 0;
    XtVaGetValues(w, XmNwidth, &dw, XmNheight, &dh, NULL);
    int cw = (int)dw, ch = (int)dh;
    int mx = ev->xbutton.x, my = ev->xbutton.y;

    int clicked_valid = 0;
    if (g_view == VIEW_MONTH && my > 26) {
        int first_dow = day_of_week(g_sel_y, g_sel_m, 1);
        int ndays = days_in_month(g_sel_y, g_sel_m);
        int nrows = (first_dow + ndays + 6) / 7;
        if (nrows < 5) nrows = 5;
        int col = (mx * 7) / (cw > 0 ? cw : 1);
        int row = ((my - 26) * nrows) / ((ch - 26) > 0 ? (ch - 26) : 1);
        int d = row * 7 + col - first_dow + 1;
        if (d >= 1 && d <= ndays) {
            g_sel_d = d;
            clicked_valid = 1;
        }
    } else if (g_view == VIEW_WEEK && my > 0) {
        int dow = day_of_week(g_sel_y, g_sel_m, g_sel_d);
        int wy = g_sel_y, wm = g_sel_m, wd = g_sel_d;
        add_days(&wy, &wm, &wd, -dow);
        int col = (mx * 7) / (cw > 0 ? cw : 1);
        if (col >= 0 && col < 7) {
            add_days(&wy, &wm, &wd, col);
            g_sel_y = wy; g_sel_m = wm; g_sel_d = wd;
            clicked_valid = 1;
        }
    } else if (g_view == VIEW_DAY) {
        int side_w = (cw < 540) ? (cw / 3) : 220;
        if (mx > side_w + 4 && my > 42) {
            int slot_h = (ch - 48) / 14;
            if (slot_h < 22) slot_h = 22;
            int s = (my - 42) / slot_h;
            if (s >= 0 && s < 14) {
                g_sel_hour = 7 + s;
                clicked_valid = 1;
            }
        } else if (mx < side_w) {
            int mh = (ch - 16) / 3;
            int idx = (my - 4) / (mh > 1 ? mh : 1);
            if (idx >= 0 && idx <= 2) {
                int ty = g_sel_y, tm = g_sel_m + (idx - 1);
                if (tm < 1) { tm = 12; ty--; }
                if (tm > 12) { tm = 1; ty++; }
                int gy = 4 + idx * mh + 22;
                int rh = (mh - 4 - 38) / 6;
                if (rh < 13) rh = 13;
                int col = ((mx - 4) * 7) / (side_w - 8);
                int row = (my - (gy + 15)) / rh;
                int d = row * 7 + col - day_of_week(ty, tm, 1) + 1;
                if (row >= 0 && col >= 0 && col < 7 && d >= 1 && d <= days_in_month(ty, tm)) {
                    int month_changed = (ty != g_sel_y || tm != g_sel_m);
                    g_sel_y = ty; g_sel_m = tm; g_sel_d = d;
                    if (month_changed) notify_backend_window();
                    clicked_valid = 1;
                }
            }
        }
    } else if (g_view == VIEW_YEAR) {
        int cell_w = (cw - 16) / 4;
        int cell_h = (ch - 16) / 3;
        int c = (mx - 6) / (cell_w > 1 ? cell_w : 1);
        int r = (my - 6) / (cell_h > 1 ? cell_h : 1);
        if (c >= 0 && c < 4 && r >= 0 && r < 3) {
            g_sel_m = r * 4 + c + 1;
            if (g_sel_d > days_in_month(g_sel_y, g_sel_m)) g_sel_d = days_in_month(g_sel_y, g_sel_m);
            g_view = VIEW_MONTH;
            notify_backend_window();
            update_titles();
            redraw_calendar();
            return;
        }
    }

    if (clicked_valid) {
        update_titles();
        redraw_calendar();
        if (g_ed_dlg && XtIsManaged(g_ed_dlg)) {
            populate_editor_for_selected_day();
            /* Auto-select first appointment on that date if one exists! */
            if (g_ed_num_mapped > 0) {
                XmListSelectPos(g_ed_list, 1, True);
            }
        }
        Time dt = ev->xbutton.time - g_last_click_time;
        if (dt < 400 && g_last_click_y == g_sel_y && g_last_click_m == g_sel_m && g_last_click_d == g_sel_d) {
            open_appt_editor(NULL, NULL, NULL);
            if (g_ed_num_mapped > 0) {
                XmListSelectPos(g_ed_list, 1, True);
            }
        }
        g_last_click_time = ev->xbutton.time;
        g_last_click_y = g_sel_y;
        g_last_click_m = g_sel_m;
        g_last_click_d = g_sel_d;
    }
}

/* ---------------- Navigation & Toolbar Callbacks ---------------- */
static void step_period(int dir) {
    int old_y = g_sel_y, old_m = g_sel_m;
    if (g_view == VIEW_DAY) {
        add_days(&g_sel_y, &g_sel_m, &g_sel_d, dir);
    } else if (g_view == VIEW_WEEK) {
        add_days(&g_sel_y, &g_sel_m, &g_sel_d, dir * 7);
    } else if (g_view == VIEW_MONTH) {
        g_sel_m += dir;
        if (g_sel_m < 1) { g_sel_m = 12; g_sel_y--; }
        if (g_sel_m > 12) { g_sel_m = 1; g_sel_y++; }
        int dm = days_in_month(g_sel_y, g_sel_m);
        if (g_sel_d > dm) g_sel_d = dm;
    } else {
        g_sel_y += dir;
        int dm = days_in_month(g_sel_y, g_sel_m);
        if (g_sel_d > dm) g_sel_d = dm;
    }
    update_titles();
    redraw_calendar();
    if (g_sel_y != old_y || g_sel_m != old_m) notify_backend_window();
}

static void on_prev_cb(Widget w, XtPointer c, XtPointer d)  { (void)w; (void)c; (void)d; step_period(-1); }
static void on_next_cb(Widget w, XtPointer c, XtPointer d)  { (void)w; (void)c; (void)d; step_period(1); }
static void on_today_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    g_sel_y = g_today_y; g_sel_m = g_today_m; g_sel_d = g_today_d;
    update_titles();
    redraw_calendar();
    notify_backend_window();
}

static void on_set_view_cb(Widget w, XtPointer client_data, XtPointer d) {
    (void)w; (void)d;
    CalViewMode nv = (CalViewMode)(intptr_t)client_data;
    int need_year = (nv == VIEW_YEAR && g_view != VIEW_YEAR);
    g_view = nv;
    update_titles();
    redraw_calendar();
    if (need_year) notify_backend_window();
}

static void on_sync_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_to_backend) { fputs("{\"op\":\"sync\"}\n", g_to_backend); fflush(g_to_backend); }
}

static void on_connect_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_to_backend) { fputs("{\"op\":\"connect\"}\n", g_to_backend); fflush(g_to_backend); }
}

static void on_about_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    XmString ms = XmStringCreateLtoR(
        (char *)"CDE Calendar Manager (dtcm — Office 365 Companion Edition)\n\n"
                "Connected to your shared dtMail Office 365 session.\n"
                "Supports Day, Week, Month, and Year views, Event Creation with\n"
                "Invited Attendees, and Accept / Tentative / Decline Invitation Replies.",
        XmFONTLIST_DEFAULT_TAG);
    Arg a[2];
    XtSetArg(a[0], XmNmessageString, ms);
    Widget dlg = XmCreateInformationDialog(g_toplevel, (char *)"aboutDtcm", a, 1);
    XmStringFree(ms);
    XtVaSetValues(XtParent(dlg), XmNtitle, "About Calendar", NULL);
    Widget cb = XmMessageBoxGetChild(dlg, XmDIALOG_CANCEL_BUTTON);
    Widget hb = XmMessageBoxGetChild(dlg, XmDIALOG_HELP_BUTTON);
    if (cb) XtUnmanageChild(cb);
    if (hb) XtUnmanageChild(hb);
    theme_widgets(dlg, &g_pal);
    XtManageChild(dlg);
}

static void on_exit_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_to_backend) {
        fputs("{\"op\":\"quit\"}\n", g_to_backend);
        fflush(g_to_backend);
        fclose(g_to_backend);
        g_to_backend = NULL;
    }
    if (g_backend_pid > 1) {
        kill(g_backend_pid, SIGTERM);
        waitpid(g_backend_pid, NULL, WNOHANG);
    }
    exit(0);
}

/* ---------------- Backend IPC Reader ---------------- */
static void parse_events_array(const JVal *arr) {
    if (!arr || arr->type != J_ARR) return;
    g_num_events = 0;
    for (size_t i = 0; i < arr->u.arr.len && g_num_events < MAX_EVENTS; i++) {
        const JVal *e = arr->u.arr.items[i];
        CalEvent *dst = &g_events[g_num_events++];
        snprintf(dst->id, sizeof(dst->id), "%s", j_str(e, "id", ""));
        snprintf(dst->subject, sizeof(dst->subject), "%s", j_str(e, "subject", "(No Title)"));
        snprintf(dst->location, sizeof(dst->location), "%s", j_str(e, "location", ""));
        snprintf(dst->organizer, sizeof(dst->organizer), "%s", j_str(e, "organizer", ""));
        snprintf(dst->organizer_email, sizeof(dst->organizer_email), "%s", j_str(e, "organizer_email", ""));
        dst->is_organizer = j_bool(e, "is_organizer", 1);
        dst->response_requested = j_bool(e, "response_requested", 0);
        snprintf(dst->response_status, sizeof(dst->response_status), "%s", j_str(e, "response_status", "organizer"));
        snprintf(dst->attendees, sizeof(dst->attendees), "%s", j_str(e, "attendees", ""));
        snprintf(dst->notes, sizeof(dst->notes), "%s", j_str(e, "notes", ""));
        snprintf(dst->start_date, sizeof(dst->start_date), "%s", j_str(e, "start_date", ""));
        snprintf(dst->end_date, sizeof(dst->end_date), "%s", j_str(e, "end_date", ""));
        dst->start_min = j_int(e, "start_min", 0);
        dst->end_min   = j_int(e, "end_min", 60);
        snprintf(dst->start_disp, sizeof(dst->start_disp), "%s", j_str(e, "start_disp", ""));
        snprintf(dst->time_range, sizeof(dst->time_range), "%s", j_str(e, "time_range", ""));
        dst->all_day   = j_bool(e, "all_day", 0);
    }
}

static void handle_backend_line(const char *line) {
    const char *p = line;
    JVal *root = j_parse_val(&p);
    if (!root || root->type != J_OBJ) { j_free(root); return; }

    const char *ev = j_str(root, "event", "");
    if (strcmp(ev, "init") == 0 || strcmp(ev, "events_updated") == 0) {
        const char *em = j_str(root, "email", "");
        if (em[0]) snprintf(g_account_email, sizeof(g_account_email), "%s", em);
        g_signed_in = j_bool(root, "signed_in", g_signed_in);
        parse_events_array(j_get(root, "events"));
        update_titles();
        redraw_calendar();
        if (g_ed_dlg && XtIsManaged(g_ed_dlg)) {
            refresh_editor_list();
            if (g_ed_selected_id[0]) {
                for (int i = 0; i < g_num_events; i++) {
                    if (strcmp(g_events[i].id, g_ed_selected_id) == 0) {
                        load_event_into_editor_fields(&g_events[i]);
                        break;
                    }
                }
            }
        }
        if (g_inv_dlg && XtIsManaged(g_inv_dlg)) refresh_invites_dialog();
        const char *st = j_str(root, "status", "");
        if (st[0]) set_label(g_st_lbl, st);
        else if (strcmp(ev, "init") == 0) {
            set_label(g_st_lbl, g_signed_in ? "Synchronizing Office 365 Calendar..."
                                            : "Not signed in — click Browse -> Connect Office 365");
        }
    } else if (strcmp(ev, "goto_date") == 0) {
        g_sel_y = j_int(root, "year", g_sel_y);
        g_sel_m = j_int(root, "month", g_sel_m);
        g_sel_d = j_int(root, "day", g_sel_d);
        update_titles();
        redraw_calendar();
        if (XtIsRealized(g_toplevel)) {
            XMapRaised(XtDisplay(g_toplevel), XtWindow(g_toplevel));
            XFlush(XtDisplay(g_toplevel));
        }
        set_label(g_st_lbl, "Jumped to meeting date from dtMail.");
    } else if (strcmp(ev, "prefill_editor") == 0) {
        const char *ds = j_str(root, "date", "");
        int y = 0, m = 0, d = 0;
        if (sscanf(ds, "%d-%d-%d", &y, &m, &d) == 3) {
            g_sel_y = y; g_sel_m = m; g_sel_d = d;
            update_titles();
            redraw_calendar();
            notify_backend_window();
        }
        ensure_appt_editor_created();
        populate_editor_for_selected_day();
        if (ds[0]) XmTextFieldSetString(g_ed_date_tf, (char *)ds);
        char sbuf[32], ebuf[32];
        fmt_minutes_ampm(j_int(root, "start_min", 540), sbuf);
        fmt_minutes_ampm(j_int(root, "end_min", 600), ebuf);
        XmTextFieldSetString(g_ed_start_tf, sbuf);
        XmTextFieldSetString(g_ed_end_tf, ebuf);
        XmToggleButtonSetState(g_ed_allday_tb, j_bool(root, "all_day", 0) ? True : False, False);
        XmTextFieldSetString(g_ed_loc_tf, (char *)j_str(root, "location", ""));
        XmTextFieldSetString(g_ed_att_tf, (char *)j_str(root, "attendees", ""));
        const char *subj = j_str(root, "subject", "");
        const char *nts  = j_str(root, "notes", "");
        char wfull[800];
        if (nts[0]) snprintf(wfull, sizeof(wfull), "%s\n%s", subj, nts);
        else        snprintf(wfull, sizeof(wfull), "%s", subj);
        XmTextSetString(g_ed_what_txt, wfull);
        set_label(g_ed_org_lbl, "Imported .ics invitation — click 'Insert (Add Event)' to save to Office 365");
        theme_widgets(g_ed_dlg, &g_pal);
        XtManageChild(g_ed_dlg);
        if (XtIsRealized(g_toplevel)) {
            XMapRaised(XtDisplay(g_toplevel), XtWindow(g_toplevel));
        }
    } else if (strcmp(ev, "status") == 0 || strcmp(ev, "mutation_result") == 0) {
        const char *st = j_str(root, "status", "");
        if (st[0]) set_label(g_st_lbl, st);
    }

    j_free(root);
}

static void on_backend_input(XtPointer closure, int *source, XtInputId *id) {
    (void)closure; (void)id;
    char tmp[4096];
    ssize_t n = read(*source, tmp, sizeof(tmp));
    if (n <= 0) return;

    if (g_rx_len + (size_t)n + 1 >= g_rx_cap) {
        g_rx_cap = (g_rx_cap == 0) ? 8192 : (g_rx_cap + (size_t)n + 8192);
        g_rx_buf = (char *)realloc(g_rx_buf, g_rx_cap);
    }
    memcpy(g_rx_buf + g_rx_len, tmp, (size_t)n);
    g_rx_len += (size_t)n;
    g_rx_buf[g_rx_len] = '\0';

    char *start = g_rx_buf, *nl = NULL;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (*start) handle_backend_line(start);
        start = nl + 1;
    }
    size_t rem = g_rx_len - (size_t)(start - g_rx_buf);
    if (rem > 0 && start != g_rx_buf) memmove(g_rx_buf, start, rem);
    g_rx_len = rem;
    if (g_rx_buf) g_rx_buf[g_rx_len] = '\0';
}

static int spawn_calendar_backend(void) {
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0) return -1;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/.local/bin/cde-calendar-backend", home);
    const char *script = (access(p1, X_OK) == 0)
        ? p1
        : "./cde-calendar-backend.py";

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_p[0], STDIN_FILENO);
        dup2(out_p[1], STDOUT_FILENO);
        close(in_p[0]); close(in_p[1]);
        close(out_p[0]); close(out_p[1]);
        execl("/usr/bin/python3", "python3", "-u", script, (char *)NULL);
        _exit(127);
    }

    close(in_p[0]);
    close(out_p[1]);
    fcntl(out_p[0], F_SETFL, O_NONBLOCK);
    g_to_backend = fdopen(in_p[1], "w");
    g_from_backend_fd = out_p[0];
    g_backend_pid = pid;
    return 0;
}

/* ---------------- Build Authentic CDE dtcm UI ---------------- */
static void add_mitem(Widget menu, const char *lbl, char mnem, const char *acc,
                      const char *acc_t, XtCallbackProc cb, XtPointer cd) {
    Widget b = XtVaCreateManagedWidget(lbl, xmPushButtonWidgetClass, menu, NULL);
    if (mnem) XtVaSetValues(b, XmNmnemonic, (KeySym)mnem, NULL);
    if (acc && acc_t) {
        XmString as = XmStringCreateLocalized((char *)acc_t);
        XtVaSetValues(b, XmNaccelerator, acc, XmNacceleratorText, as, NULL);
        XmStringFree(as);
    }
    if (cb) XtAddCallback(b, XmNactivateCallback, cb, cd);
}

static Widget make_dtcm_icon_btn(Widget parent, const char *xpm_path, const char *lbl,
                                 int show_text_right, XtCallbackProc cb, XtPointer cd) {
    Pixmap pix = load_xpm_file_on_bg(g_toplevel, xpm_path, g_pal.bg_chrome);
    if (pix == None) {
        Pixmap fallback = XmGetPixmap(XtScreen(g_toplevel), (char *)xpm_path, g_pal.fg_chrome, g_pal.bg_chrome);
        if (fallback != XmUNSPECIFIED_PIXMAP) pix = fallback;
    }
    Widget b;
    if (pix != None && show_text_right && lbl) {
        XmString ls = XmStringCreateLocalized((char *)lbl);
        b = XtVaCreateManagedWidget("dtcmIconLblBtn", xmPushButtonWidgetClass, parent,
                                    XmNlabelType, XmPIXMAP_AND_STRING,
                                    XmNlabelPixmap, pix,
                                    XmNpixmapPlacement, XmPIXMAP_LEFT,
                                    XmNpixmapTextPadding, 4,
                                    XmNlabelString, ls,
                                    XmNmarginWidth, 5, XmNmarginHeight, 2,
                                    XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
        XmStringFree(ls);
    } else if (pix != None) {
        b = XtVaCreateManagedWidget("dtcmIconBtn", xmPushButtonWidgetClass, parent,
                                    XmNlabelType, XmPIXMAP,
                                    XmNlabelPixmap, pix,
                                    XmNmarginWidth, 3, XmNmarginHeight, 2,
                                    XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    } else {
        b = XtVaCreateManagedWidget(lbl ? lbl : "Btn", xmPushButtonWidgetClass, parent,
                                    XmNmarginWidth, 6, XmNmarginHeight, 3,
                                    XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    }
    if (cb) XtAddCallback(b, XmNactivateCallback, cb, cd);
    return b;
}

static void build_dtcm_ui(void) {
    Display *dpy = XtDisplay(g_toplevel);
    init_dtcm_palette(g_toplevel, &g_pal);

    g_fn_reg   = XLoadQueryFont(dpy, "8x13");
    if (!g_fn_reg) g_fn_reg = XLoadQueryFont(dpy, "fixed");
    g_fn_bold  = XLoadQueryFont(dpy, "8x13bold");
    if (!g_fn_bold) g_fn_bold = g_fn_reg;
    g_fn_small = XLoadQueryFont(dpy, "7x13");
    if (!g_fn_small) g_fn_small = g_fn_reg;

    Pixmap win_icon = XmGetPixmap(XtScreen(g_toplevel), (char *)"/usr/dt/appconfig/icons/C/DtCM.l.pm",
                                  g_pal.fg_chrome, g_pal.bg_chrome);
    if (win_icon != XmUNSPECIFIED_PIXMAP && win_icon != None) {
        XtVaSetValues(g_toplevel, XmNiconPixmap, win_icon, NULL);
    }

    Widget mw = XtVaCreateManagedWidget("mainWin", xmMainWindowWidgetClass, g_toplevel, NULL);

    /* Classic dtcm Menu Bar: File | Edit | View | Browse | Help */
    Widget mbar = XmCreateMenuBar(mw, (char *)"menuBar", NULL, 0);

    Widget m_file = XmCreatePulldownMenu(mbar, (char *)"fileMenu", NULL, 0);
    XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_file, XmNmnemonic, 'F', NULL);
    add_mitem(m_file, "New / Edit Appointment...", 'N', "Ctrl<Key>n", "Ctrl+N", open_appt_editor, NULL);
    add_mitem(m_file, "Incoming Invitations (RSVP)...", 'I', "Ctrl<Key>i", "Ctrl+I", open_invites_dialog, NULL);
    add_mitem(m_file, "Sync Office 365 Calendar", 'S', "<Key>F5", "F5", on_sync_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_file, NULL);
    add_mitem(m_file, "Open dtMail (Companion)...", 'M', NULL, NULL, on_open_dtmail_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_file, NULL);
    add_mitem(m_file, "Exit", 'x', "Alt<Key>F4", "Alt+F4", on_exit_cb, NULL);

    Widget m_edit = XmCreatePulldownMenu(mbar, (char *)"editMenu", NULL, 0);
    XtVaCreateManagedWidget("Edit", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_edit, XmNmnemonic, 'E', NULL);
    add_mitem(m_edit, "Appointment & RSVP...", 'A', "Ctrl<Key>a", "Ctrl+A", open_appt_editor, NULL);
    add_mitem(m_edit, "Pending Invitations...", 'I', NULL, NULL, open_invites_dialog, NULL);
    add_mitem(m_edit, "Appointment List...", 'L', NULL, NULL, open_appt_list_dialog, NULL);

    Widget m_view = XmCreatePulldownMenu(mbar, (char *)"viewMenu", NULL, 0);
    XtVaCreateManagedWidget("View", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_view, XmNmnemonic, 'V', NULL);
    add_mitem(m_view, "Day",   'D', "Ctrl<Key>d", "Ctrl+D", on_set_view_cb, (XtPointer)VIEW_DAY);
    add_mitem(m_view, "Week",  'W', "Ctrl<Key>w", "Ctrl+W", on_set_view_cb, (XtPointer)VIEW_WEEK);
    add_mitem(m_view, "Month", 'M', "Ctrl<Key>m", "Ctrl+M", on_set_view_cb, (XtPointer)VIEW_MONTH);
    add_mitem(m_view, "Year",  'Y', "Ctrl<Key>y", "Ctrl+Y", on_set_view_cb, (XtPointer)VIEW_YEAR);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_view, NULL);
    add_mitem(m_view, "Today", 'T', "Ctrl<Key>t", "Ctrl+T", on_today_cb, NULL);
    add_mitem(m_view, "Appointment List...", 'A', NULL, NULL, open_appt_list_dialog, NULL);

    Widget m_browse = XmCreatePulldownMenu(mbar, (char *)"browseMenu", NULL, 0);
    XtVaCreateManagedWidget("Browse", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_browse, XmNmnemonic, 'B', NULL);
    add_mitem(m_browse, "Sync Now", 'S', NULL, NULL, on_sync_cb, NULL);
    add_mitem(m_browse, "Incoming Invitations...", 'I', NULL, NULL, open_invites_dialog, NULL);
    add_mitem(m_browse, "Switch to dtMail...", 'M', NULL, NULL, on_open_dtmail_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_browse, NULL);
    add_mitem(m_browse, "Connect / Authorize Office 365...", 'C', NULL, NULL, on_connect_cb, NULL);

    Widget m_help = XmCreatePulldownMenu(mbar, (char *)"helpMenu", NULL, 0);
    Widget cb_h = XtVaCreateManagedWidget("Help", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_help, XmNmnemonic, 'H', NULL);
    XtVaSetValues(mbar, XmNmenuHelpWidget, cb_h, NULL);
    add_mitem(m_help, "About Calendar...", 'A', NULL, NULL, on_about_cb, NULL);
    XtManageChild(mbar);

    /* Work Form */
    Widget work = XtVaCreateManagedWidget("work", xmFormWidgetClass, mw, NULL);

    /* Compact Horizontal Toolbar matching dtMail's thinner button bar */
    Widget tb_fr = XtVaCreateManagedWidget(
        "tbFr", xmFrameWidgetClass, work,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget tb_form = XtVaCreateManagedWidget("tbForm", xmFormWidgetClass, tb_fr, NULL);

    Widget tb_rc = XtVaCreateManagedWidget(
        "tbRc", xmRowColumnWidgetClass, tb_form,
        XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT, XmNspacing, 3,
        XmNmarginHeight, 2, XmNmarginWidth, 3,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );

    make_dtcm_icon_btn(tb_rc, "/usr/dt/appconfig/icons/C/DtCMapt.s.pm", "New / Edit", 1, open_appt_editor, NULL);
    g_invites_btn = XtVaCreateManagedWidget("Invites", xmPushButtonWidgetClass, tb_rc,
                                            XmNmarginWidth, 7, XmNmarginHeight, 3,
                                            XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    XtAddCallback(g_invites_btn, XmNactivateCallback, open_invites_dialog, NULL);
    make_dtcm_icon_btn(tb_rc, "/usr/dt/appconfig/icons/C/DtCMtdo.s.pm", "List", 1, open_appt_list_dialog, NULL);

    XtVaCreateManagedWidget("vsep1", xmSeparatorWidgetClass, tb_rc,
                            XmNorientation, XmVERTICAL, XmNseparatorType, XmSHADOW_ETCHED_IN, XmNmargin, 2, NULL);

    Widget b_prev = XtVaCreateManagedWidget("prevBtn", xmArrowButtonWidgetClass, tb_rc,
                                            XmNarrowDirection, XmARROW_LEFT, XmNwidth, 26, XmNheight, 26,
                                            XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    Widget b_tod  = XtVaCreateManagedWidget("Today", xmPushButtonWidgetClass, tb_rc,
                                            XmNmarginWidth, 8, XmNmarginHeight, 3,
                                            XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    Widget b_next = XtVaCreateManagedWidget("nextBtn", xmArrowButtonWidgetClass, tb_rc,
                                            XmNarrowDirection, XmARROW_RIGHT, XmNwidth, 26, XmNheight, 26,
                                            XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    XtAddCallback(b_prev, XmNactivateCallback, on_prev_cb, NULL);
    XtAddCallback(b_tod,  XmNactivateCallback, on_today_cb, NULL);
    XtAddCallback(b_next, XmNactivateCallback, on_next_cb, NULL);

    XtVaCreateManagedWidget("vsep2", xmSeparatorWidgetClass, tb_rc,
                            XmNorientation, XmVERTICAL, XmNseparatorType, XmSHADOW_ETCHED_IN, XmNmargin, 2, NULL);

    make_dtcm_icon_btn(tb_rc, "/usr/dt/appconfig/icons/C/DtCMday.s.pm", "Day",   0, on_set_view_cb, (XtPointer)VIEW_DAY);
    make_dtcm_icon_btn(tb_rc, "/usr/dt/appconfig/icons/C/DtCMwk.s.pm",  "Week",  0, on_set_view_cb, (XtPointer)VIEW_WEEK);
    make_dtcm_icon_btn(tb_rc, "/usr/dt/appconfig/icons/C/DtCMmth.s.pm", "Month", 0, on_set_view_cb, (XtPointer)VIEW_MONTH);
    make_dtcm_icon_btn(tb_rc, "/usr/dt/appconfig/icons/C/DtCMyr.s.pm",  "Year",  0, on_set_view_cb, (XtPointer)VIEW_YEAR);

    XtVaCreateManagedWidget("vsep3", xmSeparatorWidgetClass, tb_rc,
                            XmNorientation, XmVERTICAL, XmNseparatorType, XmSHADOW_ETCHED_IN, XmNmargin, 2, NULL);

    const char *home = getenv("HOME");
    char oe_tiny[512];
    snprintf(oe_tiny, sizeof(oe_tiny), "%s/.dt/icons/outlook_express.s.pm", home ? home : "/tmp");
    make_dtcm_icon_btn(tb_rc, oe_tiny, "dtMail", 1, on_open_dtmail_cb, NULL);

    g_view_title_lbl = XtVaCreateManagedWidget(
        "Month Year", xmLabelWidgetClass, tb_form,
        XmNalignment, XmALIGNMENT_END,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 12,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, tb_rc, XmNleftOffset, 10,
        NULL
    );

    /* Bottom Sunken Status Bar */
    Widget st_fr = XtVaCreateManagedWidget(
        "stFr", xmFrameWidgetClass, work,
        XmNshadowType, XmSHADOW_IN,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 2,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL
    );
    g_st_lbl = XtVaCreateManagedWidget(
        "Connecting to shared Office 365 session...", xmLabelWidgetClass, st_fr,
        XmNalignment, XmALIGNMENT_BEGINNING, XmNmarginLeft, 6,
        NULL
    );

    /* Sunken Calendar Drawing Canvas */
    Widget cv_fr = XtVaCreateManagedWidget(
        "cvFr", xmFrameWidgetClass, work,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 2,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, tb_fr, XmNtopOffset, 4,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, st_fr, XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL
    );
    g_canvas = XtVaCreateManagedWidget("dtcmCanvas", xmDrawingAreaWidgetClass, cv_fr, NULL);
    XtAddCallback(g_canvas, XmNexposeCallback, on_canvas_expose, NULL);
    XtAddCallback(g_canvas, XmNresizeCallback, on_canvas_resize, NULL);
    XtAddEventHandler(g_canvas, ButtonPressMask, False, on_canvas_input, NULL);

    XmMainWindowSetAreas(mw, mbar, NULL, NULL, NULL, work);
    theme_widgets(g_toplevel, &g_pal);
    update_titles();
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    XtSetLanguageProc(NULL, NULL, NULL);

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    g_today_y = g_sel_y = lt.tm_year + 1900;
    g_today_m = g_sel_m = lt.tm_mon + 1;
    g_today_d = g_sel_d = lt.tm_mday;

    /* Extract companion CLI args (--date YYYY-MM-DD / --import-ics <file.ics>) before Xt */
    int out_argc = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            int y = 0, m = 0, d = 0;
            if (sscanf(argv[++i], "%d-%d-%d", &y, &m, &d) == 3 && y > 1970 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                g_sel_y = y; g_sel_m = m; g_sel_d = d;
            }
        } else if (strcmp(argv[i], "--import-ics") == 0 && i + 1 < argc) {
            snprintf(g_cli_ics_path, sizeof(g_cli_ics_path), "%s", argv[++i]);
        } else if (argv[i][0] != '-' && strlen(argv[i]) > 4 && strcasecmp(argv[i] + strlen(argv[i]) - 4, ".ics") == 0) {
            snprintf(g_cli_ics_path, sizeof(g_cli_ics_path), "%s", argv[i]);
        } else {
            argv[out_argc++] = argv[i];
        }
    }
    argc = out_argc;
    argv[argc] = NULL;

    char *dtcm_argv0 = (char *)"dtcm";
    if (argc > 0) argv[0] = dtcm_argv0;

    g_toplevel = XtVaAppInitialize(
        &g_app, "Dtcm", NULL, 0, &argc, argv, NULL,
        XmNtitle, "Calendar",
        XmNiconName, "Calendar",
        XmNwidth, 900,
        XmNheight, 640,
        NULL
    );

    DtAppInitialize(g_app, XtDisplay(g_toplevel), g_toplevel, (char *)"dtcm", (char *)"Dtcm");
    build_dtcm_ui();

    Atom wm_del = XInternAtom(XtDisplay(g_toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_toplevel, wm_del, on_exit_cb, NULL);

    if (spawn_calendar_backend() == 0 && g_from_backend_fd >= 0) {
        XtAppAddInput(g_app, g_from_backend_fd, (XtPointer)XtInputReadMask, on_backend_input, NULL);
        if (g_sel_y != g_today_y || g_sel_m != g_today_m) {
            notify_backend_window();
        }
        if (g_cli_ics_path[0] && g_to_backend) {
            fputs("{\"op\":\"import_ics\",\"path\":", g_to_backend);
            json_write_escaped(g_to_backend, g_cli_ics_path);
            fputs("}\n", g_to_backend);
            fflush(g_to_backend);
        }
    }

    XtRealizeWidget(g_toplevel);
    XtAppMainLoop(g_app);
    return 0;
}
