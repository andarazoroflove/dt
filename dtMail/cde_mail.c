/*
 * cde-mail (dtMail): Native Common Desktop Environment Mail Client (OSF/Motif 2.3)
 *
 * - Automatically binds to the user's active CDE Color Palette (e.g. Crimson.dp:
 *   #B24D7A active dtwm frame, #AEB2C3 3D beveled Motif chrome, #718BA5 selection,
 *   #FFF7E9 warm ivory sunken wells + subtle greenbar alternating message rows),
 *   with an optional Dark Motif toggle.
 * - Classic 3-pane layout tailored to user specs:
 *     * Menu Bar + Icon-over-Label Coolbar + corner brand tile
 *     * Full-height Left Folders frame
 *     * Clean 3-cell sunken Status Bar at bottom
 *     * Top-Right Message Header List with subtle greenbar alternating row shading
 *     * Full-width draggable Motif Splitter Bar for smooth Reading Pane resizing
 *     * Bottom-Right Reading Pane with compact header strip + Attachment/HTML actions
 */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include <Xm/XmAll.h>
#include <Dt/Dt.h>

#include "cde_oe_icons.h"

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
    if (v->type == J_STR) {
        free(v->u.str);
    } else if (v->type == J_ARR) {
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
    if (cp < 0x80) {
        (*buf)[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
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
                        if (u1 >= 0xD800 && u1 <= 0xDBFF && (*p)[0] == '\\' && (*p)[1] == 'u') {
                            int l1 = hex_val((*p)[2]), l2 = (l1 >= 0) ? hex_val((*p)[3]) : -1;
                            int l3 = (l2 >= 0) ? hex_val((*p)[4]) : -1, l4 = (l3 >= 0) ? hex_val((*p)[5]) : -1;
                            if (l4 >= 0) {
                                unsigned int u2 = (unsigned)((l1 << 12) | (l2 << 8) | (l3 << 4) | l4);
                                if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                                    *p += 6;
                                    u1 = 0x10000U + (((u1 - 0xD800U) << 10) | (u2 - 0xDC00U));
                                }
                            }
                        }
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

static JVal *j_parse_val(const char **p);

static JVal *j_parse_val(const char **p) {
    skip_ws(p);
    if (!**p) return NULL;
    if (**p == '"') {
        char *s = parse_raw_str(p);
        if (!s) return NULL;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_STR;
        v->u.str = s;
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
    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_BOOL; v->u.b = 1;
        return v;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_BOOL; v->u.b = 0;
        return v;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        JVal *v = (JVal *)calloc(1, sizeof(JVal));
        v->type = J_NULL;
        return v;
    }
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
                case '\b': fputs("\\b", fp); break;
                case '\f': fputs("\\f", fp); break;
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

/* ---------------- Data Structures & App State ---------------- */
#define MAX_FOLDERS  64
#define MAX_MESSAGES 256
#define MAX_ATTS     16

typedef struct {
    char id[256];
    char name[128];
    char well_known[64];
    int unread;
    int total;
} FolderItem;

typedef struct {
    char id[320];
    char from_name[128];
    char from_addr[160];
    char to[256];
    char cc[256];
    char subject[256];
    char date[64];
    int unread;
    int att;
    int high_pri;
} MsgSummary;

typedef struct {
    Pixel bg_chrome;
    Pixel bg_well;
    Pixel bg_well_alt;
    Pixel bg_select;
    Pixel fg_chrome;
    Pixel fg_well;
    Pixel top_sh_chrome;
    Pixel bot_sh_chrome;
    int is_dark;
} OePalette;

typedef struct ComposeWin {
    Widget shell;
    Widget to_tf;
    Widget cc_tf;
    Widget bcc_tf;
    Widget subj_tf;
    Widget att_lbl;
    Widget body_txt;
    Widget pri_btn;
    int high_priority;
    char att_paths[MAX_ATTS][512];
    int num_atts;
    int req_id;
    struct ComposeWin *next;
} ComposeWin;

static XtAppContext g_app;
static Widget g_toplevel;
static Widget g_main_win;
static Widget g_folder_hdr = NULL;
static Widget g_folder_list;
static Widget g_right_form = NULL;
static Widget g_splitter = NULL;
static int    g_split_promille = 380;
static int    g_splitter_dragging = 0;
static Widget g_col_lbl = NULL;
static Widget g_msg_list = NULL;      /* Double-buffered greenbar canvas */
static Widget g_msg_popup = NULL;     /* Right-click message context menu */
static Widget g_msg_vscroll = NULL;
static XFontStruct *g_fn_norm = NULL;
static XFontStruct *g_fn_bold = NULL;
static Pixmap g_msg_backbuf = None;
static Dimension g_msg_bb_w = 0, g_msg_bb_h = 0;
static GC     g_msg_gc = NULL;
static int    g_msg_top_row = 0;
static long   g_last_click_ms = 0;
static int    g_last_click_row = -1;
typedef struct {
    char id[256];
    char name[256];
    long size;
} AttMeta;

static Widget g_pv_from_lbl;
static Widget g_pv_to_lbl;
static Widget g_pv_subj_lbl;
static Widget g_pv_hdr_fr = NULL;
static Widget g_pv_invite_fr = NULL;
static Widget g_pv_inv_lbl = NULL;
static Widget g_pv_inv_acc_btn = NULL;
static Widget g_pv_inv_tent_btn = NULL;
static Widget g_pv_inv_dec_btn = NULL;
static Widget g_pv_inv_cal_btn = NULL;
static Widget g_pv_html_fr = NULL;
static char   g_cur_inv_event_id[320] = "";
static char   g_cur_inv_date[32] = "";
static char   g_cur_inv_ics_path[512] = "";
static Widget g_pv_act_rc = NULL;
static Widget g_pv_save_att_btn;
static Widget g_pv_att_btns[MAX_ATTS];
static AttMeta g_cur_atts[MAX_ATTS];
static int    g_num_cur_atts = 0;
static int    g_last_att_idx = -1;
static long   g_last_att_ms = 0;
static Widget g_pv_html_host = NULL;
static Widget g_st_left;
static Widget g_st_mid;
static Widget g_st_right;
typedef struct {
    Widget w;
    char **xpm_data;
    Pixmap norm_pix;
    Pixmap arm_pix;
} TbIconSlot;

#define MAX_TB_SLOTS 16
static TbIconSlot g_tb_slots[MAX_TB_SLOTS];
static int g_num_tb_slots = 0;

/* Dialogs */
static Widget g_acct_dlg = NULL;
static Widget g_acct_who_lbl = NULL;
static Widget g_acct_status_lbl = NULL;

static Widget g_sig_dlg = NULL;
static Widget g_sig_enable_tb, g_sig_skip_reply_tb, g_sig_txt;

static OePalette g_pal;
static XmFontList g_msg_fontlist = NULL;
static int g_font_pt = 11;
static Pixmap g_oe_icon_32 = None, g_oe_mask_32 = None;

/* Backend Sync IPC */
static FILE *g_to_backend = NULL;
static int g_from_backend_fd = -1;
static pid_t g_backend_pid = -1;
static char *g_rx_buf = NULL;
static size_t g_rx_len = 0, g_rx_cap = 0;

/* Embedded WebKit2 HTML Reading Pane IPC */
static FILE *g_to_htmlview = NULL;
static int g_from_htmlview_fd = -1;
static pid_t g_htmlview_pid = -1;
static Window g_html_child_xid = None;
static char *g_hv_rx_buf = NULL;
static size_t g_hv_rx_len = 0, g_hv_rx_cap = 0;
static char g_cur_html_path[512] = "";

/* Current Mail State */
static FolderItem g_folders[MAX_FOLDERS];
static int g_num_folders = 0;
static char g_cur_folder_id[256] = "";

static MsgSummary g_msgs[MAX_MESSAGES];
static int g_num_msgs = 0;
static int g_selected_msg_idx = -1;

static char g_cur_msg_id[320] = "";
static char *g_cur_msg_body = NULL;
static int g_cur_msg_has_att = 0;

/* Config State */
static char g_cfg_name[128] = "";
static char g_cfg_email[160] = "";
static char g_cfg_tenant[128] = "organizations";
static char g_cfg_client_id[128] = "";
static char g_cfg_transport[32] = "graph";
static int  g_cfg_sig_enabled = 1;
static int  g_cfg_sig_skip_replies = 0;
static char g_cfg_signature[4096] = "Best regards,";
static int  g_signed_in = 0;
static int  g_next_req_id = 100;
static ComposeWin *g_compose_list = NULL;

/* ---------------- CDE Live Palette Discovery & Motif Coloring ---------------- */
static Pixel alloc_named_or_rgb(Display *dpy, Colormap cmap, const char *spec, Pixel fallback) {
    if (!spec || !spec[0]) return fallback;
    XColor col, exact;
    if (XAllocNamedColor(dpy, cmap, spec, &col, &exact)) return col.pixel;
    if (XParseColor(dpy, cmap, spec, &col) && XAllocColor(dpy, cmap, &col)) return col.pixel;
    return fallback;
}

static void load_active_cde_palette(Widget top, int force_dark, OePalette *pal) {
    Display *dpy = XtDisplay(top);
    int scr = DefaultScreen(dpy);
    Colormap cmap = DefaultColormap(dpy, scr);
    Pixel blk = BlackPixel(dpy, scr);
    Pixel wht = WhitePixel(dpy, scr);

    pal->is_dark = force_dark;
    if (force_dark) {
        pal->bg_chrome = alloc_named_or_rgb(dpy, cmap, "#21252D", blk);
        pal->bg_well   = alloc_named_or_rgb(dpy, cmap, "#15181E", blk);
        pal->bg_well_alt = alloc_named_or_rgb(dpy, cmap, "#1A231E", pal->bg_well);
        pal->bg_select = alloc_named_or_rgb(dpy, cmap, "#2B6CB0", wht);
        pal->fg_chrome = alloc_named_or_rgb(dpy, cmap, "#EDF2F7", wht);
        pal->fg_well   = alloc_named_or_rgb(dpy, cmap, "#EDF2F7", wht);
        XmGetColors(XtScreen(top), cmap, pal->bg_chrome, NULL,
                    &pal->top_sh_chrome, &pal->bot_sh_chrome, NULL);
        return;
    }

    /* Query active CDE ColorPalette name from Xrm database (e.g. Crimson.dp) */
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
    char p1[512], p2[512];
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

    /*
     * CDE MEDIUM_COLOR mapping (matches Crimson.dp):
     *   cset[0] = Active window titlebar (#b2004d007a00)
     *   cset[1] = Primary Motif chrome (#ae00b200c300 -> #AEB2C3)
     *   cset[2] = Workspace / selection accent (#71008b00a500 -> #718BA5)
     *   cset[3] = Text & List sunken wells (#ff00f700e900 -> #FFF7E9)
     */
    const char *chrome_hex = (nlines >= 2) ? cset[1] : "#AEB2C3";
    const char *sel_hex    = (nlines >= 3) ? cset[2] : "#718BA5";
    const char *well_hex   = (nlines >= 4) ? cset[3] : "#FFF7E9";

    pal->bg_chrome = alloc_named_or_rgb(dpy, cmap, chrome_hex, wht);
    pal->bg_select = alloc_named_or_rgb(dpy, cmap, sel_hex, blk);
    pal->bg_well   = alloc_named_or_rgb(dpy, cmap, well_hex, wht);
    pal->fg_chrome = blk;
    pal->fg_well   = blk;

    /* Compute subtle vintage greenbar paper alternating row shade from bg_well */
    XColor wc;
    memset(&wc, 0, sizeof(wc));
    wc.pixel = pal->bg_well;
    XQueryColor(dpy, cmap, &wc);
    XColor ac = wc;
    ac.red   = (wc.red   > 4400) ? (unsigned short)(wc.red   - 4400) : 0;
    ac.green = (wc.green >  900) ? (unsigned short)(wc.green -  900) : wc.green;
    ac.blue  = (wc.blue  > 3600) ? (unsigned short)(wc.blue  - 3600) : 0;
    pal->bg_well_alt = XAllocColor(dpy, cmap, &ac) ? ac.pixel : pal->bg_well;

    XmGetColors(XtScreen(top), cmap, pal->bg_chrome, NULL,
                &pal->top_sh_chrome, &pal->bot_sh_chrome, NULL);
}

static void apply_palette_recursive(Widget w, const OePalette *pal) {
    if (!w) return;

    if (XmIsList(w) || XmIsText(w) || XmIsTextField(w) || w == g_pv_html_host || w == g_msg_list) {
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

    if (XmIsScrollBar(w)) {
        XmChangeColor(w, pal->bg_chrome);
        return;
    }

    XmChangeColor(w, pal->bg_chrome);
    if (XmIsLabel(w) || XmIsLabelGadget(w) || XmIsPushButton(w) ||
        XmIsPushButtonGadget(w) || XmIsCascadeButton(w) || XmIsToggleButton(w)) {
        if (XtIsSubclass(w, xmPrimitiveWidgetClass)) {
            XtVaSetValues(w, XmNforeground, pal->fg_chrome, NULL);
        }
    }

    if (XtIsComposite(w)) {
        WidgetList children = NULL;
        Cardinal n = 0;
        XtVaGetValues(w, XmNchildren, &children, XmNnumChildren, &n, NULL);
        for (Cardinal i = 0; i < n; i++) {
            apply_palette_recursive(children[i], pal);
        }
    }
    if (XmIsCascadeButton(w) || XmIsCascadeButtonGadget(w)) {
        Widget sub = NULL;
        XtVaGetValues(w, XmNsubMenuId, &sub, NULL);
        if (sub) apply_palette_recursive(sub, pal);
    }
}

/* ---------------- UI Helpers ---------------- */
static void set_label(Widget w, const char *txt) {
    if (!w) return;
    XmString s = XmStringCreateLocalized((char *)(txt ? txt : ""));
    XtVaSetValues(w, XmNlabelString, s, NULL);
    XmStringFree(s);
}

static Pixmap create_xpm_on_bg(Widget ref, char **xpm_data, Pixel bg_pix, Pixmap *out_mask) {
    Display *dpy = XtDisplay(ref);
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    unsigned int depth = (unsigned int)DefaultDepth(dpy, scr);

    XpmAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.valuemask = XpmCloseness | XpmDepth;
    attr.closeness = 40000;
    attr.depth = depth;

    Pixmap raw_pix = None, mask = None;
    if (XpmCreatePixmapFromData(dpy, root, xpm_data, &raw_pix, &mask, &attr) != XpmSuccess || raw_pix == None) {
        if (out_mask) *out_mask = None;
        return None;
    }

    if (out_mask) {
        *out_mask = mask;
        return raw_pix;
    }

    /* Composite icon cleanly onto exact bg_pix using the XPM 1-bit transparency mask */
    Pixmap comp = XCreatePixmap(dpy, root, attr.width, attr.height, depth);
    GC gc = XCreateGC(dpy, comp, 0, NULL);
    XSetForeground(dpy, gc, bg_pix);
    XFillRectangle(dpy, comp, gc, 0, 0, attr.width, attr.height);
    if (mask != None) {
        XSetClipMask(dpy, gc, mask);
        XSetClipOrigin(dpy, gc, 0, 0);
    }
    XCopyArea(dpy, raw_pix, comp, gc, 0, 0, attr.width, attr.height, 0, 0);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, raw_pix);
    if (mask != None) XFreePixmap(dpy, mask);
    return comp;
}

static void apply_widget_xpm_matched(Widget w, char **xpm_data, Pixmap *io_norm, Pixmap *io_arm) {
    if (!w || !xpm_data) return;
    Display *dpy = XtDisplay(g_toplevel);
    Pixel bg = g_pal.bg_chrome;
    XtVaGetValues(w, XmNbackground, &bg, NULL);
    Pixmap norm_pix = create_xpm_on_bg(g_toplevel, xpm_data, bg, NULL);
    if (XmIsPushButton(w)) {
        Pixel arm_bg = bg;
        Boolean fill_on_arm = True;
        XtVaGetValues(w, XmNarmColor, &arm_bg, XmNfillOnArm, &fill_on_arm, NULL);
        Pixmap arm_pix = create_xpm_on_bg(g_toplevel, xpm_data, fill_on_arm ? arm_bg : bg, NULL);
        XtVaSetValues(w,
                      XmNlabelPixmap, norm_pix,
                      XmNarmPixmap, arm_pix,
                      NULL);
        if (io_arm) {
            if (*io_arm != None) XFreePixmap(dpy, *io_arm);
            *io_arm = arm_pix;
        }
    } else {
        XtVaSetValues(w, XmNlabelPixmap, norm_pix, NULL);
    }
    if (io_norm) {
        if (*io_norm != None) XFreePixmap(dpy, *io_norm);
        *io_norm = norm_pix;
    }
}

static void register_tb_slot(Widget w, char **xpm_data, Pixmap init_norm) {
    if (g_num_tb_slots >= MAX_TB_SLOTS) return;
    g_tb_slots[g_num_tb_slots++] = (TbIconSlot){w, xpm_data, init_norm, None};
}

static void refresh_toolbar_icons(void) {
    for (int i = 0; i < g_num_tb_slots; i++) {
        apply_widget_xpm_matched(g_tb_slots[i].w, g_tb_slots[i].xpm_data,
                                 &g_tb_slots[i].norm_pix, &g_tb_slots[i].arm_pix);
    }
}

static Widget create_coolbar_btn(Widget parent, char **xpm_data, const char *label,
                                 XtCallbackProc cb, XtPointer client_data, int track_main) {
    Pixmap pix = create_xpm_on_bg(g_toplevel, xpm_data, g_pal.bg_chrome, NULL);
    XmString ls = XmStringCreateLocalized((char *)label);
    Widget btn = XtVaCreateManagedWidget(
        "tbBtn", xmPushButtonWidgetClass, parent,
        XmNlabelType, (pix != None) ? XmPIXMAP_AND_STRING : XmSTRING,
        XmNlabelPixmap, pix,
        XmNpixmapPlacement, XmPIXMAP_LEFT,
        XmNpixmapTextPadding, 5,
        XmNlabelString, ls,
        XmNmarginWidth, 6,
        XmNmarginHeight, 2,
        XmNmarginTop, 1,
        XmNmarginBottom, 1,
        XmNshadowThickness, 1,
        XmNhighlightThickness, 0,
        NULL
    );
    XmStringFree(ls);
    if (cb) XtAddCallback(btn, XmNactivateCallback, cb, client_data);
    if (track_main) register_tb_slot(btn, xpm_data, pix);
    return btn;
}

static void create_coolbar_sep(Widget parent) {
    XtVaCreateManagedWidget(
        "tbSep", xmSeparatorWidgetClass, parent,
        XmNorientation, XmVERTICAL,
        XmNseparatorType, XmSHADOW_ETCHED_IN,
        XmNmargin, 2,
        NULL
    );
}

static void show_info_dialog(Widget parent, const char *title, const char *message, int is_error) {
    XmString ms = XmStringCreateLtoR((char *)message, XmFONTLIST_DEFAULT_TAG);
    Arg args[4];
    Cardinal n = 0;
    XtSetArg(args[n], XmNmessageString, ms); n++;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_MODELESS); n++;
    Widget dlg = is_error ? XmCreateErrorDialog(parent, (char *)"msgDlg", args, n)
                          : XmCreateInformationDialog(parent, (char *)"msgDlg", args, n);
    XmStringFree(ms);
    XtVaSetValues(XtParent(dlg), XmNtitle, title, NULL);
    Widget cancel = XmMessageBoxGetChild(dlg, XmDIALOG_CANCEL_BUTTON);
    Widget help   = XmMessageBoxGetChild(dlg, XmDIALOG_HELP_BUTTON);
    if (cancel) XtUnmanageChild(cancel);
    if (help)   XtUnmanageChild(help);
    apply_palette_recursive(dlg, &g_pal);
    XtManageChild(dlg);
}

/* ---------------- UTF-8 Safe Fixed-Width Column Formatter for Message List ---------------- */
static void format_utf8_col(const char *src, int width_chars, char *dst, size_t dst_sz) {
    size_t di = 0;
    int cols = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && cols < width_chars && di + 6 < dst_sz) {
        unsigned char c = *p;
        if (c == '\r' || c == '\n' || c == '\t') {
            dst[di++] = ' ';
            cols++;
            p++;
            continue;
        }
        size_t clen = 1;
        if ((c & 0xE0) == 0xC0) clen = 2;
        else if ((c & 0xF0) == 0xE0) clen = 3;
        else if ((c & 0xF8) == 0xF0) clen = 4;
        if (di + clen + 2 >= dst_sz) break;
        for (size_t k = 0; k < clen && *p; k++) dst[di++] = (char)*p++;
        cols++;
    }
    if (*p && width_chars >= 4 && di >= 2) {
        /* Truncate cleanly with '..' */
        dst[di - 2] = '.';
        dst[di - 1] = '.';
    }
    while (cols < width_chars && di + 2 < dst_sz) {
        dst[di++] = ' ';
        cols++;
    }
    dst[di] = '\0';
}

/* ---------------- HTML Reading Pane Zoom (Ctrl+ / Ctrl- / Ctrl+0) ---------------- */
static void htmlview_load_current(void) {
    if (!g_to_htmlview) return;
    fputs("{\"op\":\"load_file\",\"path\":", g_to_htmlview);
    json_write_escaped(g_to_htmlview, g_cur_html_path);
    fprintf(g_to_htmlview, ",\"font_pt\":%d,\"dark\":%s}\n",
            g_font_pt, g_pal.is_dark ? "true" : "false");
    fflush(g_to_htmlview);
}

static void apply_unified_font_size(int new_pt, int persist_and_announce) {
    if (new_pt < 8)  new_pt = 8;
    if (new_pt > 28) new_pt = 28;
    g_font_pt = new_pt;

    if (g_to_htmlview) {
        fprintf(g_to_htmlview, "{\"op\":\"set_font_pt\",\"font_pt\":%d}\n", g_font_pt);
        fflush(g_to_htmlview);
    }

    if (persist_and_announce) {
        int pct = (g_font_pt * 100 + 5) / 11;
        char msg[128];
        snprintf(msg, sizeof(msg), "Reading Pane zoom: %d%% (Ctrl+ / Ctrl- to adjust, Ctrl+0 to reset)", pct);
        set_label(g_st_mid, msg);
        if (g_to_backend) {
            fprintf(g_to_backend, "{\"op\":\"save_config\",\"font_pt\":%d}\n", g_font_pt);
            fflush(g_to_backend);
        }
    }
}

static void adjust_font_size(int delta) {
    int target = (delta == 0) ? 11 : (g_font_pt + delta);
    apply_unified_font_size(target, 1);
}

/* ---------------- Backend Command Dispatch & Refresh ---------------- */
static void refresh_status_counts(void) {
    int unread = 0;
    for (int i = 0; i < g_num_msgs; i++) {
        if (g_msgs[i].unread) unread++;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%d message(s), %d unread", g_num_msgs, unread);
    set_label(g_st_left, buf);

    if (g_signed_in) {
        set_label(g_st_right, "Online (Office 365)");
    } else {
        set_label(g_st_right, "Not Signed In");
    }

    /* Update window title with active folder name */
    const char *fname = "Inbox";
    for (int i = 0; i < g_num_folders; i++) {
        if (strcmp(g_folders[i].id, g_cur_folder_id) == 0) {
            fname = g_folders[i].name;
            break;
        }
    }
    char wtitle[256];
    if (g_cfg_email[0]) {
        snprintf(wtitle, sizeof(wtitle), "%s - dtMail (%s)", fname, g_cfg_email);
    } else {
        snprintf(wtitle, sizeof(wtitle), "%s - dtMail", fname);
    }
    XtVaSetValues(g_toplevel, XmNtitle, wtitle, NULL);
}

static void rebuild_folder_widget(void) {
    if (!g_folder_list) return;
    XmString *items = (XmString *)calloc((size_t)(g_num_folders + 1), sizeof(XmString));
    int sel_pos = 1;
    for (int i = 0; i < g_num_folders; i++) {
        char line[200];
        if (g_folders[i].unread > 0) {
            snprintf(line, sizeof(line), " %s (%d)", g_folders[i].name, g_folders[i].unread);
            items[i] = XmStringCreate(line, (char *)"bold");
        } else {
            snprintf(line, sizeof(line), " %s", g_folders[i].name);
            items[i] = XmStringCreate(line, (char *)"normal");
        }
        if (strcmp(g_folders[i].id, g_cur_folder_id) == 0) {
            sel_pos = i + 1;
        }
    }
    XtVaSetValues(g_folder_list,
                  XmNitems, items,
                  XmNitemCount, g_num_folders,
                  NULL);
    if (g_num_folders > 0) {
        XmListSelectPos(g_folder_list, sel_pos, False);
    }
    for (int i = 0; i < g_num_folders; i++) XmStringFree(items[i]);
    free(items);
}

/* -------- Subtle Greenbar Alternating-Row Message List Canvas -------- */
static void on_reply_cb(Widget w, XtPointer c, XtPointer d);
static void on_delete_msg_cb(Widget w, XtPointer c, XtPointer d);
static void sync_htmlview_geometry(void);
static void draw_splitter_bar(void);

static int msg_row_height(void) {
    int fh = g_fn_norm ? (g_fn_norm->ascent + g_fn_norm->descent) : 13;
    return (fh < 12 ? 12 : fh) + 6;
}

static int msg_char_width(void) {
    int cw = g_fn_norm ? g_fn_norm->max_bounds.width : 8;
    return (cw < 6) ? 8 : cw;
}

static int msg_subj_cols(int canvas_w) {
    int cw = msg_char_width();
    int avail_chars = (canvas_w - 14) / cw;
    /* 4 ("! @ ") + 22 (From) + 2 + Subj + 2 + 14 (Received) = 44 + Subj */
    int sc = avail_chars - 44;
    if (sc < 26) sc = 26;
    if (sc > 130) sc = 130;
    return sc;
}

static int utf8_to_latin1(const char *src, char *dst, size_t dst_sz) {
    size_t di = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && di + 2 < dst_sz) {
        unsigned int cp = *p;
        if (cp < 0x80) {
            dst[di++] = (char)cp;
            p++;
        } else if ((cp & 0xE0) == 0xC0 && p[1]) {
            cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
            dst[di++] = (cp <= 0xFF) ? (char)cp : '?';
        } else if ((cp & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
            if (cp == 0x2013 || cp == 0x2014) dst[di++] = '-';
            else if (cp == 0x2018 || cp == 0x2019) dst[di++] = '\'';
            else if (cp == 0x201C || cp == 0x201D) dst[di++] = '"';
            else if (cp == 0x2022) dst[di++] = '*';
            else if (cp == 0x2026) {
                if (di + 4 < dst_sz) { dst[di++] = '.'; dst[di++] = '.'; dst[di++] = '.'; }
            } else dst[di++] = '?';
        } else if ((cp & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4;
            dst[di++] = '?';
        } else {
            p++;
        }
    }
    dst[di] = '\0';
    return (int)di;
}

static void update_col_header_for_width(int canvas_w) {
    if (!g_col_lbl) return;
    int sc = msg_subj_cols(canvas_w);
    char hdr[320];
    int pos = snprintf(hdr, sizeof(hdr), " ! @ %-22s  %-*s  %s", "From", sc, "Subject", "Received");
    if (pos > 0) set_label(g_col_lbl, hdr);
}

static void sync_msg_scrollbar(int canvas_h) {
    if (!g_msg_vscroll) return;
    int rh = msg_row_height();
    int vis_rows = (canvas_h > rh) ? (canvas_h / rh) : 1;
    int total = (g_num_msgs > 0) ? g_num_msgs : 1;

    if (g_msg_top_row > total - vis_rows) g_msg_top_row = total - vis_rows;
    if (g_msg_top_row < 0) g_msg_top_row = 0;

    int slider = (vis_rows > total) ? total : vis_rows;
    if (slider < 1) slider = 1;
    int max_val = (total > slider) ? total : slider;

    XtVaSetValues(g_msg_vscroll,
                  XmNminimum, 0,
                  XmNmaximum, max_val,
                  XmNsliderSize, slider,
                  XmNvalue, g_msg_top_row,
                  XmNincrement, 1,
                  XmNpageIncrement, (vis_rows > 1 ? vis_rows - 1 : 1),
                  NULL);
}

static void draw_msg_canvas(void) {
    if (!g_msg_list || !XtIsRealized(g_msg_list)) return;
    Display *dpy = XtDisplay(g_msg_list);
    Window win = XtWindow(g_msg_list);
    int scr = DefaultScreen(dpy);

    Dimension w = 0, h = 0;
    XtVaGetValues(g_msg_list, XmNwidth, &w, XmNheight, &h, NULL);
    if (w < 16 || h < 16) return;

    update_col_header_for_width((int)w);
    sync_msg_scrollbar((int)h);

    if (g_msg_backbuf == None || g_msg_bb_w != w || g_msg_bb_h != h) {
        if (g_msg_backbuf != None) XFreePixmap(dpy, g_msg_backbuf);
        g_msg_backbuf = XCreatePixmap(dpy, win, w, h, (unsigned)DefaultDepth(dpy, scr));
        g_msg_bb_w = w;
        g_msg_bb_h = h;
    }
    if (!g_msg_gc) {
        g_msg_gc = XCreateGC(dpy, g_msg_backbuf, 0, NULL);
    }

    int rh = msg_row_height();
    int ascent = g_fn_norm ? g_fn_norm->ascent : 10;
    int sc = msg_subj_cols((int)w);
    int n_vis = ((int)h / rh) + 2;
    Pixel sel_fg = WhitePixel(dpy, scr);

    for (int v = 0; v < n_vis; v++) {
        int ry = v * rh;
        if (ry >= (int)h) break;
        int idx = g_msg_top_row + v;
        int is_sel = (idx >= 0 && idx < g_num_msgs && idx == g_selected_msg_idx);

        Pixel row_bg = is_sel ? g_pal.bg_select
                              : (((idx & 1) == 1) ? g_pal.bg_well_alt : g_pal.bg_well);
        XSetForeground(dpy, g_msg_gc, row_bg);
        XFillRectangle(dpy, g_msg_backbuf, g_msg_gc, 0, ry, w, (unsigned)rh);

        if (g_num_msgs <= 0 && v == 0) {
            if (g_fn_norm) XSetFont(dpy, g_msg_gc, g_fn_norm->fid);
            XSetForeground(dpy, g_msg_gc, g_pal.fg_well);
            const char *empty_s = "   (There are no messages in this folder)";
            XDrawString(dpy, g_msg_backbuf, g_msg_gc, 6, ry + ascent + 3, empty_s, (int)strlen(empty_s));
            continue;
        }

        if (idx >= 0 && idx < g_num_msgs) {
            const MsgSummary *m = &g_msgs[idx];
            char c_from[128], c_subj[300], c_date[64], utf8_row[600], lat_row[600];
            const char *who = m->from_name[0] ? m->from_name : m->from_addr;
            format_utf8_col(who, 22, c_from, sizeof(c_from));
            format_utf8_col(m->subject, sc, c_subj, sizeof(c_subj));
            format_utf8_col(m->date, 14, c_date, sizeof(c_date));
            snprintf(utf8_row, sizeof(utf8_row), " %c %c %s  %s  %s",
                     m->high_pri ? '!' : ' ',
                     m->att ? '@' : (m->unread ? '*' : ' '),
                     c_from, c_subj, c_date);
            int llen = utf8_to_latin1(utf8_row, lat_row, sizeof(lat_row));

            XFontStruct *use_fn = (m->unread && g_fn_bold) ? g_fn_bold : g_fn_norm;
            if (use_fn) XSetFont(dpy, g_msg_gc, use_fn->fid);
            XSetForeground(dpy, g_msg_gc, is_sel ? sel_fg : g_pal.fg_well);
            XDrawString(dpy, g_msg_backbuf, g_msg_gc, 2, ry + ascent + 3, lat_row, llen);
        }
    }

    XCopyArea(dpy, g_msg_backbuf, win, g_msg_gc, 0, 0, w, h, 0, 0);
}

static void ensure_msg_row_visible(int idx) {
    if (!g_msg_list || idx < 0) return;
    Dimension h = 0;
    XtVaGetValues(g_msg_list, XmNheight, &h, NULL);
    int rh = msg_row_height();
    int vis = (h > (Dimension)rh) ? ((int)h / rh) : 1;
    if (idx < g_msg_top_row) {
        g_msg_top_row = idx;
    } else if (idx >= g_msg_top_row + vis) {
        g_msg_top_row = idx - vis + 1;
    }
    if (g_msg_top_row < 0) g_msg_top_row = 0;
}

static void select_msg_index(int idx, int notify_open) {
    if (idx < 0 || idx >= g_num_msgs) return;
    g_selected_msg_idx = idx;
    ensure_msg_row_visible(idx);
    draw_msg_canvas();
    if (notify_open && g_to_backend) {
        fputs("{\"op\":\"open_message\",\"id\":", g_to_backend);
        json_write_escaped(g_to_backend, g_msgs[idx].id);
        fputs("}\n", g_to_backend);
        fflush(g_to_backend);
    }
}

static void set_invite_bar_state(int visible, int can_rsvp, const char *summary_txt) {
    if (!g_pv_invite_fr || !g_pv_html_fr || !g_pv_hdr_fr) return;
    if (visible) {
        set_label(g_pv_inv_lbl, summary_txt ? summary_txt : "Meeting Invitation");
        if (can_rsvp) {
            XtManageChild(g_pv_inv_acc_btn);
            XtManageChild(g_pv_inv_tent_btn);
            XtManageChild(g_pv_inv_dec_btn);
        } else {
            XtUnmanageChild(g_pv_inv_acc_btn);
            XtUnmanageChild(g_pv_inv_tent_btn);
            XtUnmanageChild(g_pv_inv_dec_btn);
        }
        if (!XtIsManaged(g_pv_invite_fr)) XtManageChild(g_pv_invite_fr);
        XtVaSetValues(g_pv_html_fr, XmNtopWidget, g_pv_invite_fr, NULL);
    } else {
        XtVaSetValues(g_pv_html_fr, XmNtopWidget, g_pv_hdr_fr, NULL);
        if (XtIsManaged(g_pv_invite_fr)) XtUnmanageChild(g_pv_invite_fr);
    }
    sync_htmlview_geometry();
}

static void rebuild_msg_widget(int keep_selection, int notify_open) {
    if (!g_msg_list) return;
    if (g_num_msgs <= 0) {
        g_selected_msg_idx = -1;
        g_msg_top_row = 0;
        g_cur_html_path[0] = '\0';
        g_cur_inv_event_id[0] = '\0';
        g_cur_inv_date[0] = '\0';
        g_cur_inv_ics_path[0] = '\0';
        set_label(g_pv_from_lbl, "From: —");
        set_label(g_pv_to_lbl,   "To:   —");
        set_label(g_pv_subj_lbl, "Subject: —");
        set_invite_bar_state(0, 0, NULL);
        htmlview_load_current();
        g_num_cur_atts = 0;
        for (int a = 0; a < MAX_ATTS; a++) {
            if (g_pv_att_btns[a]) XtUnmanageChild(g_pv_att_btns[a]);
        }
        XtUnmanageChild(g_pv_save_att_btn);
        draw_msg_canvas();
        refresh_status_counts();
        return;
    }

    int new_sel = (keep_selection && g_selected_msg_idx >= 0 && g_selected_msg_idx < g_num_msgs)
                  ? g_selected_msg_idx : 0;
    for (int i = 0; i < g_num_msgs; i++) {
        if (keep_selection && g_cur_msg_id[0] && strcmp(g_msgs[i].id, g_cur_msg_id) == 0) {
            new_sel = i;
            break;
        }
    }

    if (!keep_selection) g_msg_top_row = 0;
    g_selected_msg_idx = new_sel;
    ensure_msg_row_visible(new_sel);
    draw_msg_canvas();
    if (notify_open && g_to_backend) {
        fputs("{\"op\":\"open_message\",\"id\":", g_to_backend);
        json_write_escaped(g_to_backend, g_msgs[new_sel].id);
        fputs("}\n", g_to_backend);
        fflush(g_to_backend);
    }
    refresh_status_counts();
}

static void parse_folders_from_json(const JVal *arr) {
    if (!arr || arr->type != J_ARR) return;
    g_num_folders = 0;
    for (size_t i = 0; i < arr->u.arr.len && g_num_folders < MAX_FOLDERS; i++) {
        const JVal *f = arr->u.arr.items[i];
        FolderItem *dst = &g_folders[g_num_folders++];
        snprintf(dst->id, sizeof(dst->id), "%s", j_str(f, "id", ""));
        snprintf(dst->name, sizeof(dst->name), "%s", j_str(f, "name", "Folder"));
        snprintf(dst->well_known, sizeof(dst->well_known), "%s", j_str(f, "well_known", ""));
        dst->unread = j_int(f, "unread", 0);
        dst->total  = j_int(f, "total", 0);
    }
}

static void parse_msgs_from_json(const JVal *arr) {
    if (!arr || arr->type != J_ARR) return;
    g_num_msgs = 0;
    for (size_t i = 0; i < arr->u.arr.len && g_num_msgs < MAX_MESSAGES; i++) {
        const JVal *m = arr->u.arr.items[i];
        MsgSummary *dst = &g_msgs[g_num_msgs++];
        snprintf(dst->id, sizeof(dst->id), "%s", j_str(m, "id", ""));
        snprintf(dst->from_name, sizeof(dst->from_name), "%s", j_str(m, "from_name", ""));
        snprintf(dst->from_addr, sizeof(dst->from_addr), "%s", j_str(m, "from_addr", ""));
        snprintf(dst->to, sizeof(dst->to), "%s", j_str(m, "to", ""));
        snprintf(dst->cc, sizeof(dst->cc), "%s", j_str(m, "cc", ""));
        snprintf(dst->subject, sizeof(dst->subject), "%s", j_str(m, "subject", "(No Subject)"));
        snprintf(dst->date, sizeof(dst->date), "%s", j_str(m, "date", ""));
        dst->unread   = j_bool(m, "unread", 0);
        dst->att      = j_bool(m, "att", 0);
        dst->high_pri = (strcmp(j_str(m, "importance", "normal"), "high") == 0);
    }
}

static void parse_config_from_json(const JVal *c) {
    if (!c || c->type != J_OBJ) return;
    snprintf(g_cfg_name, sizeof(g_cfg_name), "%s", j_str(c, "display_name", g_cfg_name));
    snprintf(g_cfg_email, sizeof(g_cfg_email), "%s", j_str(c, "email", g_cfg_email));
    snprintf(g_cfg_tenant, sizeof(g_cfg_tenant), "%s", j_str(c, "tenant", g_cfg_tenant));
    snprintf(g_cfg_client_id, sizeof(g_cfg_client_id), "%s", j_str(c, "client_id", g_cfg_client_id));
    snprintf(g_cfg_transport, sizeof(g_cfg_transport), "%s", j_str(c, "transport", g_cfg_transport));
    g_cfg_sig_enabled      = j_bool(c, "sig_enabled", g_cfg_sig_enabled);
    g_cfg_sig_skip_replies = j_bool(c, "sig_skip_replies", g_cfg_sig_skip_replies);
    int cfg_pt = j_int(c, "font_pt", g_font_pt);
    if (cfg_pt >= 8 && cfg_pt <= 24 && cfg_pt != g_font_pt) {
        apply_unified_font_size(cfg_pt, 0);
    }
    int cfg_split = j_int(c, "split_promille", g_split_promille);
    if (cfg_split >= 120 && cfg_split <= 880 && cfg_split != g_split_promille) {
        g_split_promille = cfg_split;
        if (g_splitter) {
            XtVaSetValues(g_splitter, XmNtopPosition, g_split_promille, NULL);
            sync_htmlview_geometry();
        }
    }
    const char *sig = j_str(c, "signature", NULL);
    if (sig) snprintf(g_cfg_signature, sizeof(g_cfg_signature), "%s", sig);
}

static void handle_backend_json(const char *line) {
    const char *p = line;
    JVal *root = j_parse_val(&p);
    if (!root || root->type != J_OBJ) { j_free(root); return; }

    const char *ev = j_str(root, "event", "");
    if (strcmp(ev, "init") == 0) {
        parse_config_from_json(j_get(root, "config"));
        g_signed_in = j_bool(root, "signed_in", 0);
        parse_folders_from_json(j_get(root, "folders"));
        snprintf(g_cur_folder_id, sizeof(g_cur_folder_id), "%s", j_str(root, "initial_folder", ""));
        parse_msgs_from_json(j_get(root, "messages"));
        rebuild_folder_widget();
        rebuild_msg_widget(0, 1);
        set_label(g_st_mid, g_signed_in ? "Connected to Office 365 — checking for new mail..."
                                        : "Ready — Use Tools -> Account... (or Sign In via Browser) to log in");
    } else if (strcmp(ev, "sync_progress") == 0 || strcmp(ev, "status") == 0) {
        const char *st = j_str(root, "status", "");
        if (st[0]) set_label(g_st_mid, st);
        if (j_bool(root, "alert", 0)) {
            show_info_dialog(g_toplevel, "dtMail", st, 0);
        }
    } else if (strcmp(ev, "sync_done") == 0) {
        parse_config_from_json(j_get(root, "config"));
        if (j_get(root, "folders")) {
            parse_folders_from_json(j_get(root, "folders"));
        }
        const char *fid = j_str(root, "folder_id", "");
        if (fid[0]) {
            snprintf(g_cur_folder_id, sizeof(g_cur_folder_id), "%s", fid);
            parse_msgs_from_json(j_get(root, "messages"));
        }
        if (j_bool(root, "ok", 0)) g_signed_in = 1;
        rebuild_folder_widget();
        rebuild_msg_widget(1, g_cur_msg_id[0] ? 0 : 1);
        set_label(g_st_mid, j_str(root, "status", "Synchronized."));
    } else if (strcmp(ev, "read_state_changed") == 0) {
        const char *mid = j_str(root, "id", "");
        int unread = j_bool(root, "unread", 1);
        for (int i = 0; i < g_num_msgs; i++) {
            if (strcmp(g_msgs[i].id, mid) == 0) {
                g_msgs[i].unread = unread;
                break;
            }
        }
        if (j_get(root, "folders")) {
            parse_folders_from_json(j_get(root, "folders"));
            rebuild_folder_widget();
        }
        draw_msg_canvas();
        refresh_status_counts();
        const char *st = j_str(root, "status", "");
        if (st[0]) set_label(g_st_mid, st);
    } else if (strcmp(ev, "folder_messages") == 0) {
        const char *fid = j_str(root, "folder_id", "");
        if (strcmp(fid, g_cur_folder_id) == 0) {
            parse_msgs_from_json(j_get(root, "messages"));
            if (j_get(root, "folders")) parse_folders_from_json(j_get(root, "folders"));
            rebuild_folder_widget();
            rebuild_msg_widget(0, 1);
        }
        const char *st = j_str(root, "status", "");
        if (st[0]) set_label(g_st_mid, st);
    } else if (strcmp(ev, "message_detail") == 0) {
        JVal *m = j_get(root, "message");
        if (m && m->type == J_OBJ) {
            const char *mid = j_str(m, "id", "");
            snprintf(g_cur_msg_id, sizeof(g_cur_msg_id), "%s", mid);

            /* Mark read in local summary list too */
            int marked_any = 0;
            for (int i = 0; i < g_num_msgs; i++) {
                if (strcmp(g_msgs[i].id, mid) == 0 && g_msgs[i].unread) {
                    g_msgs[i].unread = 0;
                    marked_any = 1;
                }
            }
            if (marked_any) draw_msg_canvas();
            if (j_get(root, "folders")) {
                parse_folders_from_json(j_get(root, "folders"));
                rebuild_folder_widget();
            }

            char lfrom[360], lto[420], lsubj[320];
            const char *fn = j_str(m, "from_name", "");
            const char *fa = j_str(m, "from_addr", "");
            if (fn[0] && fa[0] && strcmp(fn, fa) != 0)
                snprintf(lfrom, sizeof(lfrom), "From: %s <%s>   (%s)", fn, fa, j_str(m, "date", ""));
            else
                snprintf(lfrom, sizeof(lfrom), "From: %s   (%s)", fn[0] ? fn : fa, j_str(m, "date", ""));

            const char *to = j_str(m, "to", "");
            const char *cc = j_str(m, "cc", "");
            if (cc[0]) snprintf(lto, sizeof(lto), "To: %s   |   Cc: %s", to, cc);
            else       snprintf(lto, sizeof(lto), "To: %s", to[0] ? to : "—");

            snprintf(lsubj, sizeof(lsubj), "Subject: %s", j_str(m, "subject", ""));
            set_label(g_pv_from_lbl, lfrom);
            set_label(g_pv_to_lbl, lto);
            set_label(g_pv_subj_lbl, lsubj);

            const char *btxt = j_str(m, "body", "");
            free(g_cur_msg_body);
            g_cur_msg_body = strdup(btxt);

            const char *hp = j_str(m, "html_path", "");
            snprintf(g_cur_html_path, sizeof(g_cur_html_path), "%s", hp);
            htmlview_load_current();

            g_cur_msg_has_att = j_bool(m, "att", 0);
            g_num_cur_atts = 0;
            JVal *atts_arr = j_get(m, "attachments");
            if (atts_arr && atts_arr->type == J_ARR) {
                for (size_t ai = 0; ai < atts_arr->u.arr.len && g_num_cur_atts < MAX_ATTS; ai++) {
                    JVal *ao = atts_arr->u.arr.items[ai];
                    if (!ao || ao->type != J_OBJ) continue;
                    const char *aname = j_str(ao, "name", "");
                    if (!aname[0]) continue;
                    AttMeta *am = &g_cur_atts[g_num_cur_atts++];
                    snprintf(am->id, sizeof(am->id), "%s", j_str(ao, "id", ""));
                    snprintf(am->name, sizeof(am->name), "%s", aname);
                    am->size = (long)j_int(ao, "size", 0);
                }
            }

            for (int a = 0; a < MAX_ATTS; a++) {
                if (!g_pv_att_btns[a]) continue;
                if (a < g_num_cur_atts) {
                    char chip_lbl[320];
                    long sz = g_cur_atts[a].size;
                    if (sz >= 1024 * 1024) {
                        snprintf(chip_lbl, sizeof(chip_lbl), "[@] %s (%.1f MB)", g_cur_atts[a].name, (double)sz / (1024.0 * 1024.0));
                    } else if (sz >= 1024) {
                        snprintf(chip_lbl, sizeof(chip_lbl), "[@] %s (%ld KB)", g_cur_atts[a].name, (sz + 512) / 1024);
                    } else if (sz > 0) {
                        snprintf(chip_lbl, sizeof(chip_lbl), "[@] %s (%ld B)", g_cur_atts[a].name, sz);
                    } else {
                        snprintf(chip_lbl, sizeof(chip_lbl), "[@] %s", g_cur_atts[a].name);
                    }
                    set_label(g_pv_att_btns[a], chip_lbl);
                    XmChangeColor(g_pv_att_btns[a], g_pal.bg_well);
                    XtVaSetValues(g_pv_att_btns[a], XmNforeground, g_pal.fg_well, NULL);
                    XtManageChild(g_pv_att_btns[a]);
                } else {
                    XtUnmanageChild(g_pv_att_btns[a]);
                }
            }

            if (g_cur_msg_has_att && g_num_cur_atts == 0) {
                set_label(g_pv_save_att_btn, "Loading attachment(s)...");
                XtManageChild(g_pv_save_att_btn);
            } else if (g_num_cur_atts > 1) {
                set_label(g_pv_save_att_btn, "Save All");
                XtManageChild(g_pv_save_att_btn);
            } else {
                XtUnmanageChild(g_pv_save_att_btn);
            }

            /* Meeting Invitation RSVP & Calendar Companion Bar */
            g_cur_inv_event_id[0] = '\0';
            g_cur_inv_date[0] = '\0';
            g_cur_inv_ics_path[0] = '\0';
            JVal *mtg = j_get(m, "meeting");
            if (mtg && mtg->type == J_OBJ && j_bool(mtg, "is_meeting", 0)) {
                snprintf(g_cur_inv_event_id, sizeof(g_cur_inv_event_id), "%s", j_str(mtg, "event_id", ""));
                snprintf(g_cur_inv_date, sizeof(g_cur_inv_date), "%s", j_str(mtg, "date", ""));
                snprintf(g_cur_inv_ics_path, sizeof(g_cur_inv_ics_path), "%s", j_str(mtg, "ics_path", ""));
                int can_rsvp = j_bool(mtg, "can_rsvp", 0);
                const char *when_s = j_str(mtg, "when_disp", "");
                const char *loc_s  = j_str(mtg, "location", "");
                const char *st_s   = j_str(mtg, "status_label", "Invitation");
                char inv_txt[460];
                if (loc_s[0]) {
                    snprintf(inv_txt, sizeof(inv_txt), "Calendar [%s]: %s  (%s)", st_s, when_s[0] ? when_s : "Scheduled Event", loc_s);
                } else {
                    snprintf(inv_txt, sizeof(inv_txt), "Calendar [%s]: %s", st_s, when_s[0] ? when_s : "Meeting Invitation");
                }
                set_invite_bar_state(1, can_rsvp, inv_txt);
            } else {
                set_invite_bar_state(0, 0, NULL);
            }

            refresh_status_counts();
        }
    } else if (strcmp(ev, "rsvp_done") == 0) {
        const char *st = j_str(root, "status", "Invitation reply sent.");
        set_label(g_st_mid, st);
        const char *new_lbl = j_str(root, "bar_label", "");
        if (new_lbl[0] && g_pv_invite_fr && XtIsManaged(g_pv_invite_fr)) {
            set_label(g_pv_inv_lbl, new_lbl);
        }
    } else if (strcmp(ev, "auth_status") == 0 || strcmp(ev, "device_code") == 0) {
        const char *msg = j_str(root, "message", j_str(root, "status", "Authenticating..."));
        if (g_acct_dlg && g_acct_status_lbl) set_label(g_acct_status_lbl, msg);
        set_label(g_st_mid, msg);
    } else if (strcmp(ev, "auth_error") == 0) {
        const char *msg = j_str(root, "message", "Authentication error");
        if (g_acct_dlg && g_acct_status_lbl) set_label(g_acct_status_lbl, msg);
        set_label(g_st_mid, msg);
        show_info_dialog(g_acct_dlg ? g_acct_dlg : g_toplevel, "Office 365 Sign-In", msg, 1);
    } else if (strcmp(ev, "auth_ok") == 0) {
        g_signed_in = 1;
        const char *em = j_str(root, "email", "");
        const char *dn = j_str(root, "display_name", "");
        if (em[0]) snprintf(g_cfg_email, sizeof(g_cfg_email), "%s", em);
        if (dn[0]) snprintf(g_cfg_name, sizeof(g_cfg_name), "%s", dn);
        const char *msg = j_str(root, "message", "Signed in to Office 365!");
        if (g_acct_dlg && g_acct_who_lbl) {
            char who[360];
            snprintf(who, sizeof(who), "Connected Account: %s <%s>",
                     g_cfg_name[0] ? g_cfg_name : "Office 365",
                     g_cfg_email[0] ? g_cfg_email : "Active");
            set_label(g_acct_who_lbl, who);
        }
        if (g_acct_dlg && g_acct_status_lbl) set_label(g_acct_status_lbl, msg);
        if (g_acct_dlg && XtIsManaged(g_acct_dlg)) XtUnmanageChild(g_acct_dlg);
        set_label(g_st_mid, msg);
        refresh_status_counts();
    } else if (strcmp(ev, "signed_out") == 0) {
        g_signed_in = 0;
        g_cfg_email[0] = '\0';
        if (g_acct_dlg && g_acct_who_lbl) set_label(g_acct_who_lbl, "Connected Account: (Not signed in)");
        if (g_acct_dlg && g_acct_status_lbl) set_label(g_acct_status_lbl, "Signed out.");
        set_label(g_st_mid, "Signed out of Office 365.");
        refresh_status_counts();
    } else if (strcmp(ev, "config_saved") == 0) {
        parse_config_from_json(j_get(root, "config"));
        set_label(g_st_mid, j_str(root, "status", "Settings saved."));
    } else if (strcmp(ev, "send_result") == 0) {
        int ok = j_bool(root, "ok", 0);
        int req_id = j_int(root, "req_id", 0);
        const char *st = j_str(root, "status", "");
        set_label(g_st_mid, st);
        if (ok) {
            ComposeWin **pp = &g_compose_list;
            while (*pp) {
                if ((*pp)->req_id == req_id) {
                    ComposeWin *victim = *pp;
                    *pp = victim->next;
                    XtDestroyWidget(victim->shell);
                    free(victim);
                    break;
                }
                pp = &((*pp)->next);
            }
        } else {
            show_info_dialog(g_toplevel, "Send Message Error", st, 1);
        }
    }

    j_free(root);
}

static void on_backend_readable(XtPointer closure, int *source, XtInputId *id) {
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

    char *start = g_rx_buf;
    char *nl = NULL;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (*start) handle_backend_json(start);
        start = nl + 1;
    }
    size_t rem = g_rx_len - (size_t)(start - g_rx_buf);
    if (rem > 0 && start != g_rx_buf) memmove(g_rx_buf, start, rem);
    g_rx_len = rem;
    if (g_rx_buf) g_rx_buf[g_rx_len] = '\0';
}

/* ---------------- Compose / Reply / Forward Window ---------------- */
typedef enum { CMP_NEW = 0, CMP_REPLY = 1, CMP_REPLY_ALL = 2, CMP_FORWARD = 3 } ComposeMode;

static void update_compose_att_label(ComposeWin *cw) {
    if (cw->num_atts == 0) {
        set_label(cw->att_lbl, "Attachments: (none)");
        return;
    }
    char buf[512] = "Attachments: ";
    for (int i = 0; i < cw->num_atts; i++) {
        const char *base = strrchr(cw->att_paths[i], '/');
        base = base ? (base + 1) : cw->att_paths[i];
        size_t cur = strlen(buf);
        snprintf(buf + cur, sizeof(buf) - cur, "%s%s", (i > 0 ? ", " : ""), base);
    }
    set_label(cw->att_lbl, buf);
}

static void on_compose_file_ok(Widget fsb, XtPointer client_data, XtPointer call_data) {
    ComposeWin *cw = (ComposeWin *)client_data;
    XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call_data;
    char *fname = NULL;
    if (XmStringGetLtoR(cbs->value, XmFONTLIST_DEFAULT_TAG, &fname) && fname) {
        if (fname[0] && access(fname, R_OK) == 0 && cw->num_atts < MAX_ATTS) {
            snprintf(cw->att_paths[cw->num_atts++], sizeof(cw->att_paths[0]), "%s", fname);
            update_compose_att_label(cw);
        }
        XtFree(fname);
    }
    XtDestroyWidget(XtParent(fsb));
}

static void on_compose_file_cancel(Widget fsb, XtPointer client_data, XtPointer call_data) {
    (void)client_data; (void)call_data;
    XtDestroyWidget(XtParent(fsb));
}

static void on_compose_attach_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    ComposeWin *cw = (ComposeWin *)client_data;
    Widget fsb = XmCreateFileSelectionDialog(cw->shell, (char *)"attachDlg", NULL, 0);
    XtVaSetValues(XtParent(fsb), XmNtitle, "Insert Attachment", NULL);
    Widget help = XmFileSelectionBoxGetChild(fsb, XmDIALOG_HELP_BUTTON);
    if (help) XtUnmanageChild(help);
    XtAddCallback(fsb, XmNokCallback, on_compose_file_ok, cw);
    XtAddCallback(fsb, XmNcancelCallback, on_compose_file_cancel, cw);
    apply_palette_recursive(fsb, &g_pal);
    XtManageChild(fsb);
}

static void on_compose_toggle_pri(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    ComposeWin *cw = (ComposeWin *)client_data;
    cw->high_priority = !cw->high_priority;
    set_label(cw->pri_btn, cw->high_priority ? "! Priority: HIGH" : "Priority: Normal");
}

static void on_compose_insert_sig(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    ComposeWin *cw = (ComposeWin *)client_data;
    if (!g_cfg_signature[0]) return;
    char *cur = XmTextGetString(cw->body_txt);
    size_t nlen = (cur ? strlen(cur) : 0) + strlen(g_cfg_signature) + 16;
    char *nbuf = (char *)malloc(nlen);
    snprintf(nbuf, nlen, "%s\n\n-- \n%s", cur ? cur : "", g_cfg_signature);
    XmTextSetString(cw->body_txt, nbuf);
    free(nbuf);
    if (cur) XtFree(cur);
}

static void on_compose_send_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    ComposeWin *cw = (ComposeWin *)client_data;
    if (!g_to_backend) return;

    char *to   = XmTextFieldGetString(cw->to_tf);
    char *cc   = XmTextFieldGetString(cw->cc_tf);
    char *bcc  = XmTextFieldGetString(cw->bcc_tf);
    char *subj = XmTextFieldGetString(cw->subj_tf);
    char *body = XmTextGetString(cw->body_txt);

    cw->req_id = ++g_next_req_id;
    fputs("{\"op\":\"send_mail\",\"req_id\":", g_to_backend);
    fprintf(g_to_backend, "%d,\"to\":", cw->req_id);
    json_write_escaped(g_to_backend, to ? to : "");
    fputs(",\"cc\":", g_to_backend);
    json_write_escaped(g_to_backend, cc ? cc : "");
    fputs(",\"bcc\":", g_to_backend);
    json_write_escaped(g_to_backend, bcc ? bcc : "");
    fputs(",\"subject\":", g_to_backend);
    json_write_escaped(g_to_backend, subj ? subj : "");
    fputs(",\"priority\":", g_to_backend);
    json_write_escaped(g_to_backend, cw->high_priority ? "high" : "normal");
    fputs(",\"body\":", g_to_backend);
    json_write_escaped(g_to_backend, body ? body : "");
    fputs(",\"attachments\":[", g_to_backend);
    for (int i = 0; i < cw->num_atts; i++) {
        if (i > 0) fputc(',', g_to_backend);
        json_write_escaped(g_to_backend, cw->att_paths[i]);
    }
    fputs("]}\n", g_to_backend);
    fflush(g_to_backend);

    if (to) XtFree(to);
    if (cc) XtFree(cc);
    if (bcc) XtFree(bcc);
    if (subj) XtFree(subj);
    if (body) XtFree(body);
}

static void open_compose_window(ComposeMode mode) {
    ComposeWin *cw = (ComposeWin *)calloc(1, sizeof(ComposeWin));
    cw->next = g_compose_list;
    g_compose_list = cw;

    cw->shell = XtVaAppCreateShell(
        "composeMail", "CdeMail", topLevelShellWidgetClass, XtDisplay(g_toplevel),
        XmNtitle, (mode == CMP_NEW) ? "New Message - dtMail" : "Reply / Forward - dtMail",
        XmNwidth, 680,
        XmNheight, 510,
        XmNdeleteResponse, XmDESTROY,
        NULL
    );
    if (g_oe_icon_32 != None) {
        XtVaSetValues(cw->shell, XmNiconPixmap, g_oe_icon_32, XmNiconMask, g_oe_mask_32, NULL);
    }

    Widget form = XtVaCreateManagedWidget("cmpForm", xmFormWidgetClass, cw->shell,
                                          XmNfractionBase, 100, NULL);

    /* Compose Toolbar */
    Widget tb_frame = XtVaCreateManagedWidget(
        "cmpTbFrame", xmFrameWidgetClass, form,
        XmNshadowType, XmSHADOW_OUT,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget tb_rc = XtVaCreateManagedWidget(
        "cmpTbRc", xmRowColumnWidgetClass, tb_frame,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 3,
        XmNmarginHeight, 2,
        XmNmarginWidth, 3,
        NULL
    );
    Pixmap cmp_p1 = None, cmp_p2 = None, cmp_p3 = None;
    Widget b_send = create_coolbar_btn(tb_rc, xpm_tb_send_now, "Send", on_compose_send_cb, cw, 0);
    XtVaGetValues(b_send, XmNlabelPixmap, &cmp_p1, NULL);
    create_coolbar_sep(tb_rc);
    Widget b_att = create_coolbar_btn(tb_rc, xpm_tb_attach, "Attach", on_compose_attach_cb, cw, 0);
    XtVaGetValues(b_att, XmNlabelPixmap, &cmp_p2, NULL);
    Widget b_sig = create_coolbar_btn(tb_rc, xpm_tb_signature, "Insert Sig", on_compose_insert_sig, cw, 0);
    XtVaGetValues(b_sig, XmNlabelPixmap, &cmp_p3, NULL);
    cw->pri_btn = XtVaCreateManagedWidget("Priority: Normal", xmPushButtonWidgetClass, tb_rc,
                                          XmNmarginWidth, 8, XmNmarginHeight, 3,
                                          XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    XtAddCallback(cw->pri_btn, XmNactivateCallback, on_compose_toggle_pri, cw);

    /* Header Grid (To, Cc, Bcc, Subject) */
    Widget hdr_form = XtVaCreateManagedWidget(
        "hdrForm", xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, tb_frame, XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
        NULL
    );

    const char *labels[4] = {"To:", "Cc:", "Bcc:", "Subject:"};
    Widget *tfs[4] = {&cw->to_tf, &cw->cc_tf, &cw->bcc_tf, &cw->subj_tf};
    Widget prev_row = NULL;
    for (int i = 0; i < 4; i++) {
        Widget row = XtVaCreateManagedWidget(
            "hdrRow", xmFormWidgetClass, hdr_form,
            XmNtopAttachment, prev_row ? XmATTACH_WIDGET : XmATTACH_FORM,
            XmNtopWidget, prev_row,
            XmNtopOffset, prev_row ? 4 : 0,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            NULL
        );
        Widget lbl = XtVaCreateManagedWidget(
            labels[i], xmLabelWidgetClass, row,
            XmNwidth, 68,
            XmNalignment, XmALIGNMENT_END,
            XmNleftAttachment, XmATTACH_FORM,
            XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 4,
            NULL
        );
        *tfs[i] = XtVaCreateManagedWidget(
            "hdrTf", xmTextFieldWidgetClass, row,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl, XmNleftOffset, 6,
            XmNrightAttachment, XmATTACH_FORM,
            XmNtopAttachment, XmATTACH_FORM,
            NULL
        );
        prev_row = row;
    }

    cw->att_lbl = XtVaCreateManagedWidget(
        "Attachments: (none)", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, hdr_form, XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 12,
        NULL
    );

    Arg targs[8];
    Cardinal tn = 0;
    XtSetArg(targs[tn], XmNeditMode, XmMULTI_LINE_EDIT); tn++;
    XtSetArg(targs[tn], XmNwordWrap, True); tn++;
    XtSetArg(targs[tn], XmNscrollHorizontal, False); tn++;
    cw->body_txt = XmCreateScrolledText(form, (char *)"cmpBody", targs, tn);
    XtVaSetValues(XtParent(cw->body_txt),
                  XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, cw->att_lbl, XmNtopOffset, 4,
                  XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8,
                  XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
                  XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 8,
                  NULL);
    XtManageChild(cw->body_txt);

    /* Populate Reply / Reply All / Forward fields + Automatic Signature */
    const MsgSummary *sel = (g_selected_msg_idx >= 0 && g_selected_msg_idx < g_num_msgs)
                            ? &g_msgs[g_selected_msg_idx] : NULL;
    if (sel && mode != CMP_NEW) {
        if (mode == CMP_REPLY || mode == CMP_REPLY_ALL) {
            XmTextFieldSetString(cw->to_tf, (char *)(sel->from_addr[0] ? sel->from_addr : sel->from_name));
            if (mode == CMP_REPLY_ALL && sel->cc[0]) {
                XmTextFieldSetString(cw->cc_tf, (char *)sel->cc);
            }
            char rsubj[300];
            if (strncasecmp(sel->subject, "Re:", 3) == 0) snprintf(rsubj, sizeof(rsubj), "%s", sel->subject);
            else snprintf(rsubj, sizeof(rsubj), "Re: %s", sel->subject);
            XmTextFieldSetString(cw->subj_tf, rsubj);
        } else if (mode == CMP_FORWARD) {
            char fsubj[300];
            if (strncasecmp(sel->subject, "Fw:", 3) == 0 || strncasecmp(sel->subject, "Fwd:", 4) == 0)
                snprintf(fsubj, sizeof(fsubj), "%s", sel->subject);
            else
                snprintf(fsubj, sizeof(fsubj), "Fw: %s", sel->subject);
            XmTextFieldSetString(cw->subj_tf, fsubj);
        }
    }

    int add_sig = g_cfg_sig_enabled && g_cfg_signature[0] &&
                  !(mode != CMP_NEW && g_cfg_sig_skip_replies);
    size_t qlen = (g_cur_msg_body ? strlen(g_cur_msg_body) : 0) + strlen(g_cfg_signature) + 512;
    char *init_body = (char *)calloc(1, qlen);
    if (add_sig) {
        snprintf(init_body, qlen, "\n\n-- \n%s", g_cfg_signature);
    }
    if (sel && mode != CMP_NEW && g_cur_msg_body) {
        size_t cur_l = strlen(init_body);
        snprintf(init_body + cur_l, qlen - cur_l,
                 "\n\n----- Original Message -----\nFrom: %s <%s>\nSent: %s\nSubject: %s\n\n%s",
                 sel->from_name, sel->from_addr, sel->date, sel->subject, g_cur_msg_body);
    }
    XmTextSetString(cw->body_txt, init_body);
    XmTextSetInsertionPosition(cw->body_txt, 0);
    free(init_body);

    apply_palette_recursive(cw->shell, &g_pal);
    apply_widget_xpm_matched(b_send, xpm_tb_send_now, &cmp_p1, NULL);
    apply_widget_xpm_matched(b_att,  xpm_tb_attach,   &cmp_p2, NULL);
    apply_widget_xpm_matched(b_sig,  xpm_tb_signature,&cmp_p3, NULL);
    XtRealizeWidget(cw->shell);
    XtPopup(cw->shell, XtGrabNone);
}

/* ---------------- Signature Editor Dialog ---------------- */
static void on_sig_save_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    g_cfg_sig_enabled = XmToggleButtonGetState(g_sig_enable_tb) ? 1 : 0;
    g_cfg_sig_skip_replies = XmToggleButtonGetState(g_sig_skip_reply_tb) ? 1 : 0;
    char *txt = XmTextGetString(g_sig_txt);
    snprintf(g_cfg_signature, sizeof(g_cfg_signature), "%s", txt ? txt : "");
    if (txt) XtFree(txt);

    if (g_to_backend) {
        fprintf(g_to_backend,
                "{\"op\":\"save_config\",\"sig_enabled\":%s,\"sig_skip_replies\":%s,\"signature\":",
                g_cfg_sig_enabled ? "true" : "false",
                g_cfg_sig_skip_replies ? "true" : "false");
        json_write_escaped(g_to_backend, g_cfg_signature);
        fputs("}\n", g_to_backend);
        fflush(g_to_backend);
    }
    XtUnmanageChild(g_sig_dlg);
}

static void on_sig_cancel_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    XtUnmanageChild(g_sig_dlg);
}

static void open_signature_dialog(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    if (!g_sig_dlg) {
        Arg args[4];
        Cardinal n = 0;
        XtSetArg(args[n], XmNautoUnmanage, False); n++;
        XtSetArg(args[n], XmNwidth, 500); n++;
        XtSetArg(args[n], XmNheight, 340); n++;
        g_sig_dlg = XmCreateFormDialog(g_toplevel, (char *)"sigDlg", args, n);
        XtVaSetValues(XtParent(g_sig_dlg), XmNtitle, "Signatures - dtMail Options", NULL);

        Widget hdr = XtVaCreateManagedWidget(
            "Email Signature Settings", xmLabelWidgetClass, g_sig_dlg,
            XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 10,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
            NULL
        );

        g_sig_enable_tb = XtVaCreateManagedWidget(
            "Add signature to all outgoing messages", xmToggleButtonWidgetClass, g_sig_dlg,
            XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, hdr, XmNtopOffset, 8,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
            NULL
        );

        g_sig_skip_reply_tb = XtVaCreateManagedWidget(
            "Don't add signature to Replies and Forwards", xmToggleButtonWidgetClass, g_sig_dlg,
            XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, g_sig_enable_tb, XmNtopOffset, 4,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
            NULL
        );

        Widget edit_lbl = XtVaCreateManagedWidget(
            "Edit Signature Text:", xmLabelWidgetClass, g_sig_dlg,
            XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, g_sig_skip_reply_tb, XmNtopOffset, 10,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
            NULL
        );

        Widget btn_row = XtVaCreateManagedWidget(
            "sigBtns", xmRowColumnWidgetClass, g_sig_dlg,
            XmNorientation, XmHORIZONTAL,
            XmNspacing, 8,
            XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 10,
            XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 12,
            NULL
        );
        Widget ok_b = XtVaCreateManagedWidget("Save Signature", xmPushButtonWidgetClass, btn_row,
                                              XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
        Widget ca_b = XtVaCreateManagedWidget("Cancel", xmPushButtonWidgetClass, btn_row,
                                              XmNmarginWidth, 12, XmNmarginHeight, 4, NULL);
        XtAddCallback(ok_b, XmNactivateCallback, on_sig_save_cb, NULL);
        XtAddCallback(ca_b, XmNactivateCallback, on_sig_cancel_cb, NULL);

        Arg targs[6];
        Cardinal tn = 0;
        XtSetArg(targs[tn], XmNeditMode, XmMULTI_LINE_EDIT); tn++;
        XtSetArg(targs[tn], XmNrows, 8); tn++;
        g_sig_txt = XmCreateScrolledText(g_sig_dlg, (char *)"sigText", targs, tn);
        XtVaSetValues(XtParent(g_sig_txt),
                      XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, edit_lbl, XmNtopOffset, 4,
                      XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 12,
                      XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 12,
                      XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, btn_row, XmNbottomOffset, 10,
                      NULL);
        XtManageChild(g_sig_txt);
    }

    XmToggleButtonSetState(g_sig_enable_tb, g_cfg_sig_enabled, False);
    XmToggleButtonSetState(g_sig_skip_reply_tb, g_cfg_sig_skip_replies, False);
    XmTextSetString(g_sig_txt, g_cfg_signature);
    apply_palette_recursive(g_sig_dlg, &g_pal);
    XtManageChild(g_sig_dlg);
}

/* ---------------- 1-Click Browser Sign-In & Compact Account Dialog ---------------- */
static void trigger_direct_browser_login_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (!g_to_backend) return;
    fputs("{\"op\":\"login_browser\"}\n", g_to_backend);
    fflush(g_to_backend);
}

static void on_acct_signout(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_to_backend) { fputs("{\"op\":\"sign_out\"}\n", g_to_backend); fflush(g_to_backend); }
}

static void on_acct_close(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_acct_dlg) XtUnmanageChild(g_acct_dlg);
}

static void open_accounts_dialog(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    if (!g_acct_dlg) {
        Arg args[4];
        Cardinal n = 0;
        XtSetArg(args[n], XmNautoUnmanage, False); n++;
        XtSetArg(args[n], XmNwidth, 460); n++;
        g_acct_dlg = XmCreateFormDialog(g_toplevel, (char *)"acctDlg", args, n);
        XtVaSetValues(XtParent(g_acct_dlg), XmNtitle, "Microsoft Office 365 Account", NULL);

        Widget rc = XtVaCreateManagedWidget(
            "acctRc", xmRowColumnWidgetClass, g_acct_dlg,
            XmNorientation, XmVERTICAL,
            XmNspacing, 10,
            XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 12,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 14,
            XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 14,
            XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 12,
            NULL
        );

        g_acct_who_lbl = XtVaCreateManagedWidget("Connected Account: (Not signed in)", xmLabelWidgetClass, rc,
                                                 XmNalignment, XmALIGNMENT_BEGINNING, NULL);

        XtVaCreateManagedWidget(
            "Click below to sign in through Microsoft's browser login (with 2FA).\nYour folders and messages will download automatically.",
            xmLabelWidgetClass, rc,
            XmNalignment, XmALIGNMENT_BEGINNING,
            NULL
        );

        Widget b_br = XtVaCreateManagedWidget("Sign In with Browser (Microsoft 365 + 2FA)", xmPushButtonWidgetClass, rc,
                                              XmNmarginWidth, 14, XmNmarginHeight, 7, NULL);
        XtAddCallback(b_br, XmNactivateCallback, trigger_direct_browser_login_cb, NULL);

        g_acct_status_lbl = XtVaCreateManagedWidget("Status: Ready", xmLabelWidgetClass, rc,
                                                    XmNalignment, XmALIGNMENT_BEGINNING, NULL);

        XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, rc, NULL);

        Widget bot_row = XtVaCreateManagedWidget("botRow", xmRowColumnWidgetClass, rc,
                                                 XmNorientation, XmHORIZONTAL, XmNspacing, 8, NULL);
        Widget b_so = XtVaCreateManagedWidget("Sign Out", xmPushButtonWidgetClass, bot_row,
                                              XmNmarginWidth, 10, XmNmarginHeight, 4, NULL);
        Widget b_cl = XtVaCreateManagedWidget("Close", xmPushButtonWidgetClass, bot_row,
                                              XmNmarginWidth, 14, XmNmarginHeight, 4, NULL);
        XtAddCallback(b_so, XmNactivateCallback, on_acct_signout, NULL);
        XtAddCallback(b_cl, XmNactivateCallback, on_acct_close, NULL);
    }

    char who[360];
    if (g_signed_in && g_cfg_email[0]) {
        snprintf(who, sizeof(who), "Connected Account: %s <%s>",
                 g_cfg_name[0] ? g_cfg_name : "Office 365", g_cfg_email);
    } else {
        snprintf(who, sizeof(who), "Connected Account: (Not signed in)");
    }
    set_label(g_acct_who_lbl, who);
    set_label(g_acct_status_lbl, g_signed_in ? "Status: Connected — automatic sync active"
                                             : "Status: Not signed in");

    apply_palette_recursive(g_acct_dlg, &g_pal);
    XtManageChild(g_acct_dlg);
}

/* ---------------- Main Window Callbacks ---------------- */
static void on_folder_select_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    int idx = cbs->item_position - 1;
    if (idx < 0 || idx >= g_num_folders || !g_to_backend) return;
    if (strcmp(g_folders[idx].id, g_cur_folder_id) == 0) return;
    snprintf(g_cur_folder_id, sizeof(g_cur_folder_id), "%s", g_folders[idx].id);

    fputs("{\"op\":\"select_folder\",\"folder_id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_folder_id);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
}

static void on_msg_vscroll_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data;
    XmScrollBarCallbackStruct *cbs = (XmScrollBarCallbackStruct *)call_data;
    g_msg_top_row = cbs->value;
    draw_msg_canvas();
}

static void on_msg_canvas_event(Widget w, XtPointer closure, XEvent *ev, Boolean *cont) {
    (void)w; (void)closure; (void)cont;
    if (ev->type == Expose) {
        if (ev->xexpose.count == 0) draw_msg_canvas();
    } else if (ev->type == ConfigureNotify) {
        draw_msg_canvas();
    } else if (ev->type == ButtonPress) {
        XmProcessTraversal(g_msg_list, XmTRAVERSE_CURRENT);
        int rh = msg_row_height();
        if (ev->xbutton.button == Button4) {
            if (g_msg_top_row > 0) {
                g_msg_top_row = (g_msg_top_row >= 3) ? (g_msg_top_row - 3) : 0;
                draw_msg_canvas();
            }
        } else if (ev->xbutton.button == Button5) {
            Dimension ch = 0;
            XtVaGetValues(g_msg_list, XmNheight, &ch, NULL);
            int vis = (ch > (Dimension)rh) ? ((int)ch / rh) : 1;
            if (g_msg_top_row + vis < g_num_msgs) {
                g_msg_top_row += 3;
                if (g_msg_top_row > g_num_msgs - vis) g_msg_top_row = g_num_msgs - vis;
                if (g_msg_top_row < 0) g_msg_top_row = 0;
                draw_msg_canvas();
            }
        } else if (ev->xbutton.button == Button3 && g_num_msgs > 0) {
            int clicked = g_msg_top_row + (ev->xbutton.y / rh);
            if (clicked >= 0 && clicked < g_num_msgs) {
                /* Highlight right-clicked row WITHOUT opening/marking it read */
                g_selected_msg_idx = clicked;
                ensure_msg_row_visible(clicked);
                draw_msg_canvas();
                if (g_msg_popup) {
                    XmMenuPosition(g_msg_popup, &ev->xbutton);
                    XtManageChild(g_msg_popup);
                }
            }
        } else if (ev->xbutton.button == Button1 && g_num_msgs > 0) {
            int clicked = g_msg_top_row + (ev->xbutton.y / rh);
            if (clicked >= 0 && clicked < g_num_msgs) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                long now_ms = (long)tv.tv_sec * 1000L + (long)(tv.tv_usec / 1000);
                int is_dbl = (clicked == g_last_click_row && (now_ms - g_last_click_ms) <= 380L);
                g_last_click_row = clicked;
                g_last_click_ms = now_ms;
                select_msg_index(clicked, 1);
                if (is_dbl) {
                    open_compose_window(CMP_REPLY);
                }
            }
        }
    } else if (ev->type == KeyPress && g_num_msgs > 0) {
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        Dimension ch = 0;
        XtVaGetValues(g_msg_list, XmNheight, &ch, NULL);
        int page = ((int)ch / msg_row_height()) - 1;
        if (page < 1) page = 1;

        if (ks == XK_Up || ks == XK_KP_Up) {
            if (g_selected_msg_idx > 0) select_msg_index(g_selected_msg_idx - 1, 1);
        } else if (ks == XK_Down || ks == XK_KP_Down) {
            if (g_selected_msg_idx + 1 < g_num_msgs) select_msg_index(g_selected_msg_idx + 1, 1);
        } else if (ks == XK_Page_Up || ks == XK_KP_Page_Up) {
            int ni = g_selected_msg_idx - page;
            select_msg_index(ni < 0 ? 0 : ni, 1);
        } else if (ks == XK_Page_Down || ks == XK_KP_Page_Down) {
            int ni = g_selected_msg_idx + page;
            select_msg_index(ni >= g_num_msgs ? g_num_msgs - 1 : ni, 1);
        } else if (ks == XK_Home || ks == XK_KP_Home) {
            select_msg_index(0, 1);
        } else if (ks == XK_End || ks == XK_KP_End) {
            select_msg_index(g_num_msgs - 1, 1);
        } else if (ks == XK_Return || ks == XK_KP_Enter) {
            open_compose_window(CMP_REPLY);
        } else if (ks == XK_Delete || ks == XK_KP_Delete) {
            on_delete_msg_cb(g_msg_list, NULL, NULL);
        }
    }
}

/* -------- Draggable Full-Width Reading Pane Splitter -------- */
static void set_split_promille(int promille, int persist) {
    if (promille < 130) promille = 130;
    if (promille > 870) promille = 870;
    if (promille == g_split_promille && !persist) return;
    g_split_promille = promille;
    if (g_splitter) {
        XtVaSetValues(g_splitter, XmNtopPosition, g_split_promille, NULL);
        draw_splitter_bar();
        draw_msg_canvas();
        sync_htmlview_geometry();
    }
    if (persist && g_to_backend) {
        fprintf(g_to_backend, "{\"op\":\"save_config\",\"split_promille\":%d}\n", g_split_promille);
        fflush(g_to_backend);
    }
}

static void on_split_preset_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int target = (int)(intptr_t)client_data;
    set_split_promille(target, 1);
}

static void draw_splitter_bar(void) {
    if (!g_splitter || !XtIsRealized(g_splitter)) return;
    Display *dpy = XtDisplay(g_splitter);
    Window win = XtWindow(g_splitter);
    Dimension w = 0, h = 0;
    XtVaGetValues(g_splitter, XmNwidth, &w, XmNheight, &h, NULL);
    if (w < 8 || h < 4) return;

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, g_pal.bg_chrome);
    XFillRectangle(dpy, win, gc, 0, 0, w, h);

    /* Subtle top/bottom bevel rule across full width */
    XSetForeground(dpy, gc, g_pal.top_sh_chrome);
    XDrawLine(dpy, win, gc, 0, 1, (int)w - 1, 1);
    XSetForeground(dpy, gc, g_pal.bot_sh_chrome);
    XDrawLine(dpy, win, gc, 0, (int)h - 2, (int)w - 1, (int)h - 2);

    /* Centered raised Motif sash grip knob (44x6) */
    int kw = 44;
    int kx = ((int)w - kw) / 2;
    if (kx > 4) {
        XSetForeground(dpy, gc, g_pal.bg_chrome);
        XFillRectangle(dpy, win, gc, kx, 1, (unsigned)kw, (unsigned)(h - 2));
        XSetForeground(dpy, gc, g_pal.top_sh_chrome);
        XDrawLine(dpy, win, gc, kx, 1, kx + kw - 1, 1);
        XDrawLine(dpy, win, gc, kx, 1, kx, (int)h - 2);
        XSetForeground(dpy, gc, g_pal.bot_sh_chrome);
        XDrawLine(dpy, win, gc, kx, (int)h - 2, kx + kw - 1, (int)h - 2);
        XDrawLine(dpy, win, gc, kx + kw - 1, 1, kx + kw - 1, (int)h - 2);
        /* Grip dots */
        int cy = (int)h / 2;
        for (int dx = -12; dx <= 12; dx += 6) {
            XSetForeground(dpy, gc, g_pal.bot_sh_chrome);
            XDrawPoint(dpy, win, gc, kx + kw / 2 + dx, cy);
            XSetForeground(dpy, gc, g_pal.top_sh_chrome);
            XDrawPoint(dpy, win, gc, kx + kw / 2 + dx - 1, cy - 1);
        }
    }
    XFreeGC(dpy, gc);
}

static void on_splitter_event(Widget w, XtPointer closure, XEvent *ev, Boolean *cont) {
    (void)w; (void)closure; (void)cont;
    if (ev->type == Expose || ev->type == ConfigureNotify) {
        draw_splitter_bar();
    } else if (ev->type == MapNotify) {
        Display *dpy = XtDisplay(g_splitter);
        Cursor cur = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
        XDefineCursor(dpy, XtWindow(g_splitter), cur);
        draw_splitter_bar();
    } else if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
        g_splitter_dragging = 1;
    } else if (ev->type == MotionNotify && g_splitter_dragging && g_right_form && XtIsRealized(g_right_form)) {
        Display *dpy = XtDisplay(g_right_form);
        Window child_ret = None;
        int rx = 0, ry = 0;
        Dimension rh = 0;
        XtVaGetValues(g_right_form, XmNheight, &rh, NULL);
        if (rh > 100 && XTranslateCoordinates(dpy, DefaultRootWindow(dpy), XtWindow(g_right_form),
                                              ev->xmotion.x_root, ev->xmotion.y_root, &rx, &ry, &child_ret)) {
            int prom = ((ry - 4) * 1000) / (int)rh;
            set_split_promille(prom, 0);
        }
    } else if (ev->type == ButtonRelease && ev->xbutton.button == Button1 && g_splitter_dragging) {
        g_splitter_dragging = 0;
        set_split_promille(g_split_promille, 1);
    }
}

static void set_selected_msg_read_state(int is_read) {
    if (g_selected_msg_idx < 0 || g_selected_msg_idx >= g_num_msgs) return;
    MsgSummary *m = &g_msgs[g_selected_msg_idx];
    m->unread = is_read ? 0 : 1;
    draw_msg_canvas();
    refresh_status_counts();

    if (g_to_backend) {
        fputs("{\"op\":\"set_read\",\"id\":", g_to_backend);
        json_write_escaped(g_to_backend, m->id);
        fprintf(g_to_backend, ",\"read\":%s}\n", is_read ? "true" : "false");
        fflush(g_to_backend);
    }
}

static void on_mark_unread_cb(Widget w, XtPointer c, XtPointer d) { (void)w; (void)c; (void)d; set_selected_msg_read_state(0); }
static void on_mark_read_cb(Widget w, XtPointer c, XtPointer d)   { (void)w; (void)c; (void)d; set_selected_msg_read_state(1); }

static void on_new_mail_cb(Widget w, XtPointer c, XtPointer d)   { (void)w; (void)c; (void)d; open_compose_window(CMP_NEW); }
static void on_reply_cb(Widget w, XtPointer c, XtPointer d)      { (void)w; (void)c; (void)d; open_compose_window(CMP_REPLY); }
static void on_reply_all_cb(Widget w, XtPointer c, XtPointer d)  { (void)w; (void)c; (void)d; open_compose_window(CMP_REPLY_ALL); }
static void on_forward_cb(Widget w, XtPointer c, XtPointer d)    { (void)w; (void)c; (void)d; open_compose_window(CMP_FORWARD); }

static void on_send_recv_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (!g_to_backend) return;
    fputs("{\"op\":\"sync\",\"folder_id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_folder_id);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
}

static void on_delete_msg_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_selected_msg_idx < 0 || g_selected_msg_idx >= g_num_msgs || !g_to_backend) return;
    fputs("{\"op\":\"delete_msg\",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_msgs[g_selected_msg_idx].id);
    fputs(",\"folder_id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_folder_id);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
}

static void request_attachment_action(int idx, int open_after_save) {
    if (idx < 0 || idx >= g_num_cur_atts || !g_cur_msg_id[0] || !g_to_backend) return;
    const AttMeta *am = &g_cur_atts[idx];
    char st[360];
    snprintf(st, sizeof(st), open_after_save
             ? "Downloading '%s' to ~/Downloads and opening..."
             : "Saving '%s' to ~/Downloads...", am->name);
    set_label(g_st_mid, st);

    fputs("{\"op\":\"open_attachment\",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_msg_id);
    fputs(",\"att_id\":", g_to_backend);
    json_write_escaped(g_to_backend, am->id);
    fputs(",\"att_name\":", g_to_backend);
    json_write_escaped(g_to_backend, am->name);
    fprintf(g_to_backend, ",\"open\":%s}\n", open_after_save ? "true" : "false");
    fflush(g_to_backend);
}

static void on_att_chip_event(Widget w, XtPointer closure, XEvent *ev, Boolean *cont) {
    (void)w; (void)cont;
    int idx = (int)(intptr_t)closure;
    if (idx < 0 || idx >= g_num_cur_atts) return;
    if (ev->type == ButtonPress) {
        if (ev->xbutton.button == Button1) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            long now_ms = (long)tv.tv_sec * 1000L + (long)(tv.tv_usec / 1000);
            if (idx == g_last_att_idx && (now_ms - g_last_att_ms) <= 500L) {
                g_last_att_idx = -1;
                g_last_att_ms = 0;
                request_attachment_action(idx, 1);
            } else {
                g_last_att_idx = idx;
                g_last_att_ms = now_ms;
                char tip[360];
                snprintf(tip, sizeof(tip),
                         "Attachment '%s' — Double-click to download to ~/Downloads & open (right-click to save only)",
                         g_cur_atts[idx].name);
                set_label(g_st_mid, tip);
            }
        } else if (ev->xbutton.button == Button3) {
            request_attachment_action(idx, 0);
        }
    }
}

static void on_save_atts_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (!g_cur_msg_id[0] || !g_to_backend) return;
    fputs("{\"op\":\"save_attachments\",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_msg_id);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
}

static void on_invite_rsvp_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    const char *action = (const char *)client_data;
    if (!g_cur_msg_id[0] || !g_to_backend || !action) return;
    fputs("{\"op\":\"rsvp_invite\",\"id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_msg_id);
    fputs(",\"event_id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_inv_event_id);
    fputs(",\"action\":", g_to_backend);
    json_write_escaped(g_to_backend, action);
    fputs(",\"folder_id\":", g_to_backend);
    json_write_escaped(g_to_backend, g_cur_folder_id);
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
}

static void on_open_calendar_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int use_invite_ctx = (int)(intptr_t)client_data;
    if (!g_to_backend) return;
    fputs("{\"op\":\"open_calendar\",\"date\":", g_to_backend);
    json_write_escaped(g_to_backend, use_invite_ctx ? g_cur_inv_date : "");
    fputs(",\"ics_path\":", g_to_backend);
    json_write_escaped(g_to_backend, use_invite_ctx ? g_cur_inv_ics_path : "");
    fputs("}\n", g_to_backend);
    fflush(g_to_backend);
}

static void on_font_inc_cb(Widget w, XtPointer c, XtPointer d)   { (void)w; (void)c; (void)d; adjust_font_size(1); }
static void on_font_dec_cb(Widget w, XtPointer c, XtPointer d)   { (void)w; (void)c; (void)d; adjust_font_size(-1); }
static void on_font_reset_cb(Widget w, XtPointer c, XtPointer d) { (void)w; (void)c; (void)d; adjust_font_size(0); }

static void on_switch_theme_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int dark = (int)(intptr_t)client_data;
    load_active_cde_palette(g_toplevel, dark, &g_pal);
    apply_palette_recursive(g_toplevel, &g_pal);
    refresh_toolbar_icons();
    if (g_msg_popup) apply_palette_recursive(g_msg_popup, &g_pal);
    if (g_msg_vscroll) XmChangeColor(g_msg_vscroll, g_pal.bg_chrome);
    draw_msg_canvas();
    draw_splitter_bar();
    if (g_acct_dlg) apply_palette_recursive(g_acct_dlg, &g_pal);
    if (g_sig_dlg)  apply_palette_recursive(g_sig_dlg, &g_pal);
    if (g_to_backend) {
        fprintf(g_to_backend, "{\"op\":\"save_config\",\"theme\":\"%s\"}\n", dark ? "dark" : "cde");
        fflush(g_to_backend);
    }
    if (g_to_htmlview) {
        fprintf(g_to_htmlview, "{\"op\":\"set_theme\",\"dark\":%s}\n", dark ? "true" : "false");
        fflush(g_to_htmlview);
    }
    /* Re-render current message HTML with new theme paper background */
    if (g_cur_msg_id[0] && g_to_backend) {
        fputs("{\"op\":\"open_message\",\"id\":", g_to_backend);
        json_write_escaped(g_to_backend, g_cur_msg_id);
        fputs("}\n", g_to_backend);
        fflush(g_to_backend);
    }
}

static void on_about_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    const char *about =
        "dtMail — CDE Office 365 Mail Client\n"
        "Common Desktop Environment (OSF/Motif 2.3) Edition\n\n"
        "Palette: Bound to active CDE desktop palette + subtle greenbar list\n"
        "Reading Pane: Adjustable splitter + embedded HTML engine (Ctrl+ / Ctrl-)\n"
        "Transport: Native Office 365 OAuth 2.0 (2FA Browser Sign-In)";
    show_info_dialog(g_toplevel, "About dtMail", about, 0);
}

static void on_exit_cb(Widget w, XtPointer c, XtPointer d) {
    (void)w; (void)c; (void)d;
    if (g_to_htmlview) {
        fputs("{\"op\":\"quit\"}\n", g_to_htmlview);
        fflush(g_to_htmlview);
        fclose(g_to_htmlview);
        g_to_htmlview = NULL;
    }
    if (g_htmlview_pid > 1) {
        kill(g_htmlview_pid, SIGTERM);
        waitpid(g_htmlview_pid, NULL, WNOHANG);
    }
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

/* ---------------- Embedded WebKit2 HTML Reading Pane Host ---------------- */
static void sync_htmlview_geometry(void) {
    if (!g_pv_html_host || !XtIsRealized(g_pv_html_host)) return;
    Dimension w = 0, h = 0;
    XtVaGetValues(g_pv_html_host, XmNwidth, &w, XmNheight, &h, NULL);
    int iw = (w > 16) ? (int)w : 16;
    int ih = (h > 16) ? (int)h : 16;
    if (g_html_child_xid != None) {
        XMoveResizeWindow(XtDisplay(g_pv_html_host), g_html_child_xid, 0, 0, (unsigned)iw, (unsigned)ih);
        XFlush(XtDisplay(g_pv_html_host));
    }
    if (g_to_htmlview) {
        fprintf(g_to_htmlview, "{\"op\":\"resize\",\"w\":%d,\"h\":%d}\n", iw, ih);
        fflush(g_to_htmlview);
    }
}

static void on_html_host_configure(Widget w, XtPointer closure, XEvent *ev, Boolean *cont) {
    (void)w; (void)closure; (void)cont;
    if (ev->type == ConfigureNotify || ev->type == MapNotify) {
        sync_htmlview_geometry();
    }
}

static void on_htmlview_readable(XtPointer closure, int *source, XtInputId *id) {
    (void)closure; (void)id;
    char tmp[1024];
    ssize_t n = read(*source, tmp, sizeof(tmp));
    if (n <= 0) return;

    if (g_hv_rx_len + (size_t)n + 1 >= g_hv_rx_cap) {
        g_hv_rx_cap = (g_hv_rx_cap == 0) ? 2048 : (g_hv_rx_cap + (size_t)n + 2048);
        g_hv_rx_buf = (char *)realloc(g_hv_rx_buf, g_hv_rx_cap);
    }
    memcpy(g_hv_rx_buf + g_hv_rx_len, tmp, (size_t)n);
    g_hv_rx_len += (size_t)n;
    g_hv_rx_buf[g_hv_rx_len] = '\0';

    char *start = g_hv_rx_buf;
    char *nl = NULL;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (*start) {
            const char *p = start;
            JVal *ev = j_parse_val(&p);
            if (ev && ev->type == J_OBJ) {
                const char *ename = j_str(ev, "event", "");
                if (strcmp(ename, "ready") == 0) {
                    JVal *xv = j_get(ev, "child_xid");
                    if (xv && xv->type == J_NUM) g_html_child_xid = (Window)(unsigned long)xv->u.num;
                    sync_htmlview_geometry();
                    if (g_cur_html_path[0]) htmlview_load_current();
                } else if (strcmp(ename, "font_delta") == 0) {
                    adjust_font_size(j_int(ev, "delta", 0));
                } else if (strcmp(ename, "mailto") == 0) {
                    const char *addr = j_str(ev, "to", "");
                    open_compose_window(CMP_NEW);
                    if (g_compose_list && addr[0]) {
                        XmTextFieldSetString(g_compose_list->to_tf, (char *)addr);
                    }
                }
            }
            j_free(ev);
        }
        start = nl + 1;
    }
    size_t rem = g_hv_rx_len - (size_t)(start - g_hv_rx_buf);
    if (rem > 0 && start != g_hv_rx_buf) memmove(g_hv_rx_buf, start, rem);
    g_hv_rx_len = rem;
    if (g_hv_rx_buf) g_hv_rx_buf[g_hv_rx_len] = '\0';
}

static int spawn_htmlview(void) {
    if (!g_pv_html_host || !XtIsRealized(g_pv_html_host)) return -1;
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0) return -1;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char script_p1[512];
    snprintf(script_p1, sizeof(script_p1), "%s/.local/bin/cde-mail-htmlview", home);
    const char *script = (access(script_p1, X_OK) == 0)
        ? script_p1
        : "./cde-mail-htmlview.py";

    Dimension w = 680, h = 320;
    XtVaGetValues(g_pv_html_host, XmNwidth, &w, XmNheight, &h, NULL);
    char xid_s[64], w_s[32], h_s[32];
    snprintf(xid_s, sizeof(xid_s), "0x%lx", (unsigned long)XtWindow(g_pv_html_host));
    snprintf(w_s, sizeof(w_s), "%d", (w > 20) ? (int)w : 680);
    snprintf(h_s, sizeof(h_s), "%d", (h > 20) ? (int)h : 320);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_p[0], STDIN_FILENO);
        dup2(out_p[1], STDOUT_FILENO);
        close(in_p[0]); close(in_p[1]);
        close(out_p[0]); close(out_p[1]);
        execl("/usr/bin/python3", "python3", "-u", script, xid_s, w_s, h_s, (char *)NULL);
        _exit(127);
    }

    close(in_p[0]);
    close(out_p[1]);
    fcntl(out_p[0], F_SETFL, O_NONBLOCK);
    g_to_htmlview = fdopen(in_p[1], "w");
    g_from_htmlview_fd = out_p[0];
    g_htmlview_pid = pid;
    return 0;
}

/* ---------------- Spawn Python Office 365 Sync Engine ---------------- */
static int spawn_backend(void) {
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0) return -1;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char script_p1[512];
    snprintf(script_p1, sizeof(script_p1), "%s/.local/bin/cde-mail-backend", home);
    const char *script = (access(script_p1, X_OK) == 0)
        ? script_p1
        : "./cde-mail-backend.py";

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

/* ---------------- Build Classic Outlook Express Motif UI ---------------- */
static void add_menu_item(Widget menu, const char *label, char mnemonic,
                          const char *accel, const char *accel_txt,
                          XtCallbackProc cb, XtPointer client_data) {
    Widget b = XtVaCreateManagedWidget(label, xmPushButtonWidgetClass, menu, NULL);
    if (mnemonic) XtVaSetValues(b, XmNmnemonic, (KeySym)mnemonic, NULL);
    if (accel && accel_txt) {
        XmString as = XmStringCreateLocalized((char *)accel_txt);
        XtVaSetValues(b, XmNaccelerator, accel, XmNacceleratorText, as, NULL);
        XmStringFree(as);
    }
    if (cb) XtAddCallback(b, XmNactivateCallback, cb, client_data);
}

static void build_main_ui(void) {
    Display *dpy = XtDisplay(g_toplevel);
    load_active_cde_palette(g_toplevel, 0, &g_pal);

    /* Crisp fixed X11 bitmap fonts ("8x13" + "8x13bold") for aligned list rows */
    XFontStruct *fn_norm = XLoadQueryFont(dpy, "8x13");
    if (!fn_norm) fn_norm = XLoadQueryFont(dpy, "fixed");
    XFontStruct *fn_bold = XLoadQueryFont(dpy, "8x13bold");
    if (!fn_bold) fn_bold = fn_norm;
    g_fn_norm = fn_norm;
    g_fn_bold = fn_bold;

    if (fn_norm) {
        XmFontListEntry e1 = XmFontListEntryCreate((char *)"normal", XmFONT_IS_FONT, fn_norm);
        XmFontListEntry e2 = XmFontListEntryCreate((char *)"bold", XmFONT_IS_FONT, fn_bold ? fn_bold : fn_norm);
        g_msg_fontlist = XmFontListAppendEntry(NULL, e1);
        g_msg_fontlist = XmFontListAppendEntry(g_msg_fontlist, e2);
        XmFontListEntryFree(&e1);
        XmFontListEntryFree(&e2);
    }

    g_oe_icon_32 = create_xpm_on_bg(g_toplevel, xpm_oe_logo_32, g_pal.bg_chrome, &g_oe_mask_32);
    if (g_oe_icon_32 != None) {
        XtVaSetValues(g_toplevel, XmNiconPixmap, g_oe_icon_32, XmNiconMask, g_oe_mask_32, NULL);
    }

    g_main_win = XtVaCreateManagedWidget("mainWin", xmMainWindowWidgetClass, g_toplevel, NULL);

    /* 1. Menu Bar */
    Widget mbar = XmCreateMenuBar(g_main_win, (char *)"menuBar", NULL, 0);

    Widget m_file = XmCreatePulldownMenu(mbar, (char *)"fileMenu", NULL, 0);
    XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_file, XmNmnemonic, 'F', NULL);
    add_menu_item(m_file, "New Message...", 'N', "Ctrl<Key>n", "Ctrl+N", on_new_mail_cb, NULL);
    add_menu_item(m_file, "Send and Receive", 'S', "Ctrl<Key>m", "Ctrl+M", on_send_recv_cb, NULL);
    add_menu_item(m_file, "Save Attachments to ~/Downloads", 'A', NULL, NULL, on_save_atts_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_file, NULL);
    add_menu_item(m_file, "Exit", 'x', "Ctrl<Key>q", "Ctrl+Q", on_exit_cb, NULL);

    Widget m_edit = XmCreatePulldownMenu(mbar, (char *)"editMenu", NULL, 0);
    XtVaCreateManagedWidget("Edit", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_edit, XmNmnemonic, 'E', NULL);
    add_menu_item(m_edit, "Delete Message", 'D', "<Key>osfDelete", "Del", on_delete_msg_cb, NULL);

    Widget m_view = XmCreatePulldownMenu(mbar, (char *)"viewMenu", NULL, 0);
    XtVaCreateManagedWidget("View", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_view, XmNmnemonic, 'V', NULL);
    add_menu_item(m_view, "Increase Font Size", '+', NULL, "Ctrl++", on_font_inc_cb, NULL);
    add_menu_item(m_view, "Decrease Font Size", '-', NULL, "Ctrl+-", on_font_dec_cb, NULL);
    add_menu_item(m_view, "Reset Font Size (11 pt)", '0', NULL, "Ctrl+0", on_font_reset_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_view, NULL);
    add_menu_item(m_view, "Reading Pane: Larger (70%)", 'L', NULL, NULL, on_split_preset_cb, (XtPointer)300);
    add_menu_item(m_view, "Reading Pane: Balanced (50%)", 'B', NULL, NULL, on_split_preset_cb, (XtPointer)500);
    add_menu_item(m_view, "Reading Pane: Compact (30%)", 'm', NULL, NULL, on_split_preset_cb, (XtPointer)700);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_view, NULL);
    add_menu_item(m_view, "Refresh / Sync Folder", 'R', "<Key>F5", "F5", on_send_recv_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_view, NULL);
    add_menu_item(m_view, "Use Active CDE Palette (Crimson.dp)", 'C', NULL, NULL, on_switch_theme_cb, (XtPointer)0);
    add_menu_item(m_view, "Use Dark Motif Theme", 'D', NULL, NULL, on_switch_theme_cb, (XtPointer)1);

    Widget m_tools = XmCreatePulldownMenu(mbar, (char *)"toolsMenu", NULL, 0);
    XtVaCreateManagedWidget("Tools", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_tools, XmNmnemonic, 'T', NULL);
    add_menu_item(m_tools, "Send and Receive All", 'S', NULL, NULL, on_send_recv_cb, NULL);
    add_menu_item(m_tools, "Open Calendar (dtcm)...", 'C', NULL, NULL, on_open_calendar_cb, (XtPointer)0);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_tools, NULL);
    add_menu_item(m_tools, "Signature...", 'g', NULL, NULL, open_signature_dialog, NULL);
    add_menu_item(m_tools, "Account...", 'A', NULL, NULL, open_accounts_dialog, NULL);
    add_menu_item(m_tools, "Sign In via Browser (Office 365)...", 'I', NULL, NULL, trigger_direct_browser_login_cb, NULL);

    Widget m_msg = XmCreatePulldownMenu(mbar, (char *)"msgMenu", NULL, 0);
    XtVaCreateManagedWidget("Message", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_msg, XmNmnemonic, 'M', NULL);
    add_menu_item(m_msg, "New Message", 'N', NULL, NULL, on_new_mail_cb, NULL);
    add_menu_item(m_msg, "Reply to Sender", 'R', "Ctrl<Key>r", "Ctrl+R", on_reply_cb, NULL);
    add_menu_item(m_msg, "Reply to All", 'A', NULL, NULL, on_reply_all_cb, NULL);
    add_menu_item(m_msg, "Forward", 'F', "Ctrl<Key>f", "Ctrl+F", on_forward_cb, NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, m_msg, NULL);
    add_menu_item(m_msg, "Mark as Unread", 'U', "Ctrl<Key>u", "Ctrl+U", on_mark_unread_cb, NULL);
    add_menu_item(m_msg, "Mark as Read", 'd', NULL, NULL, on_mark_read_cb, NULL);

    Widget m_help = XmCreatePulldownMenu(mbar, (char *)"helpMenu", NULL, 0);
    Widget cb_help = XtVaCreateManagedWidget("Help", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, m_help, XmNmnemonic, 'H', NULL);
    XtVaSetValues(mbar, XmNmenuHelpWidget, cb_help, NULL);
    add_menu_item(m_help, "About dtMail...", 'A', NULL, NULL, on_about_cb, NULL);

    XtManageChild(mbar);

    /* 2. Work Form inside XmMainWindow */
    Widget work = XtVaCreateManagedWidget("workForm", xmFormWidgetClass, g_main_win,
                                          XmNfractionBase, 100, NULL);

    /* Compact Horizontal Icon+Label Toolbar + Right-Corner OE Brand Tile */
    g_num_tb_slots = 0;
    Widget tb_frame = XtVaCreateManagedWidget(
        "tbFrame", xmFrameWidgetClass, work,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget tb_form = XtVaCreateManagedWidget("tbForm", xmFormWidgetClass, tb_frame, NULL);

    Pixmap oe_badge_pix = create_xpm_on_bg(g_toplevel, xpm_oe_logo_20, g_pal.bg_chrome, NULL);
    Widget badge_fr = XtVaCreateManagedWidget(
        "oeBadgeFrame", xmFrameWidgetClass, tb_form,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 2,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 2,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL
    );
    Widget badge_lbl = XtVaCreateManagedWidget(
        "oeBadgeLbl", xmLabelWidgetClass, badge_fr,
        XmNlabelType, (oe_badge_pix != None) ? XmPIXMAP : XmSTRING,
        XmNlabelPixmap, oe_badge_pix,
        XmNmarginWidth, 3, XmNmarginHeight, 1,
        NULL
    );
    register_tb_slot(badge_lbl, xpm_oe_logo_20, oe_badge_pix);

    Widget tb_rc = XtVaCreateManagedWidget(
        "tbRc", xmRowColumnWidgetClass, tb_form,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 3,
        XmNmarginHeight, 2,
        XmNmarginWidth, 3,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    create_coolbar_btn(tb_rc, xpm_tb_new_mail,  "New Mail",  on_new_mail_cb,   NULL, 1);
    create_coolbar_sep(tb_rc);
    create_coolbar_btn(tb_rc, xpm_tb_reply,     "Reply",     on_reply_cb,      NULL, 1);
    create_coolbar_btn(tb_rc, xpm_tb_reply_all, "Reply All", on_reply_all_cb,  NULL, 1);
    create_coolbar_btn(tb_rc, xpm_tb_forward,   "Forward",   on_forward_cb,    NULL, 1);
    create_coolbar_sep(tb_rc);
    create_coolbar_btn(tb_rc, xpm_tb_send_recv, "Send/Recv", on_send_recv_cb,  NULL, 1);
    create_coolbar_btn(tb_rc, xpm_tb_delete,    "Delete",    on_delete_msg_cb, NULL, 1);
    create_coolbar_sep(tb_rc);
    create_coolbar_btn(tb_rc, xpm_tb_calendar,  "Calendar",  on_open_calendar_cb, (XtPointer)0, 1);

    /* 3. Bottom 3-Cell Sunken Motif Status Bar (No Inbox blue banner!) */
    Widget st_form = XtVaCreateManagedWidget(
        "statusForm", xmFormWidgetClass, work,
        XmNfractionBase, 100,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 2,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 3,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 3,
        NULL
    );

    Widget st_f1 = XtVaCreateManagedWidget("stF1", xmFrameWidgetClass, st_form,
        XmNshadowType, XmSHADOW_IN,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 24,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM, NULL);
    g_st_left = XtVaCreateManagedWidget("0 message(s)", xmLabelWidgetClass, st_f1,
        XmNalignment, XmALIGNMENT_BEGINNING, XmNmarginLeft, 6, NULL);

    Widget st_f2 = XtVaCreateManagedWidget("stF2", xmFrameWidgetClass, st_form,
        XmNshadowType, XmSHADOW_IN,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 24, XmNleftOffset, 3,
        XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 80,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM, NULL);
    g_st_mid = XtVaCreateManagedWidget("Starting dtMail...", xmLabelWidgetClass, st_f2,
        XmNalignment, XmALIGNMENT_BEGINNING, XmNmarginLeft, 6, NULL);

    Widget st_f3 = XtVaCreateManagedWidget("stF3", xmFrameWidgetClass, st_form,
        XmNshadowType, XmSHADOW_IN,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 80, XmNleftOffset, 3,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM, NULL);
    g_st_right = XtVaCreateManagedWidget("Offline", xmLabelWidgetClass, st_f3,
        XmNalignment, XmALIGNMENT_END, XmNmarginRight, 6, NULL);

    /* 4. Main Workspace Split: Full-Height Left Folders Frame + Right Vertical PanedWindow */
    Widget body_form = XtVaCreateManagedWidget(
        "bodyForm", xmFormWidgetClass, work,
        XmNfractionBase, 100,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, tb_frame, XmNtopOffset, 4,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, st_form, XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL
    );

    /* FULL-HEIGHT LEFT SIDEBAR: Folders Only (No Contacts Pane!) */
    Widget folder_frame = XtVaCreateManagedWidget(
        "folderFrame", xmFrameWidgetClass, body_form,
        XmNshadowType, XmSHADOW_OUT,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 23,
        NULL
    );
    Widget folder_inner = XtVaCreateManagedWidget("folderInner", xmFormWidgetClass, folder_frame, NULL);
    g_folder_hdr = XtVaCreateManagedWidget(
        "Folders", xmLabelWidgetClass, folder_inner,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginLeft, 6, XmNmarginTop, 3, XmNmarginBottom, 3,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    Arg fl_args[6];
    Cardinal fln = 0;
    XtSetArg(fl_args[fln], XmNselectionPolicy, XmBROWSE_SELECT); fln++;
    XtSetArg(fl_args[fln], XmNlistSizePolicy, XmCONSTANT); fln++;
    if (g_msg_fontlist) { XtSetArg(fl_args[fln], XmNfontList, g_msg_fontlist); fln++; }
    g_folder_list = XmCreateScrolledList(folder_inner, (char *)"folderList", fl_args, fln);
    XtVaSetValues(XtParent(g_folder_list),
                  XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, g_folder_hdr,
                  XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 2,
                  XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 2,
                  XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 2,
                  NULL);
    XtAddCallback(g_folder_list, XmNbrowseSelectionCallback, on_folder_select_cb, NULL);
    XtManageChild(g_folder_list);

    /* RIGHT PANE: Split Form with Greenbar Message List + Full-Width Draggable Splitter + Reading Pane */
    g_right_form = XtVaCreateManagedWidget(
        "rightSplitForm", xmFormWidgetClass, body_form,
        XmNfractionBase, 1000,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 23, XmNleftOffset, 5,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );

    /* Full-width draggable Motif splitter bar between Message List and Reading Pane */
    g_splitter = XtVaCreateManagedWidget(
        "readingSplitter", xmDrawingAreaWidgetClass, g_right_form,
        XmNheight, 8,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        XmNnavigationType, XmNONE,
        XmNtopAttachment, XmATTACH_POSITION, XmNtopPosition, g_split_promille,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtAddEventHandler(g_splitter,
                      ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask,
                      False, on_splitter_event, NULL);

    /* Top-Right: Message Header List + Beveled Column Header Strip */
    Widget msg_pane = XtVaCreateManagedWidget(
        "msgPane", xmFormWidgetClass, g_right_form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, g_splitter,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget col_hdr_fr = XtVaCreateManagedWidget(
        "colHdrFrame", xmFrameWidgetClass, msg_pane,
        XmNshadowType, XmSHADOW_OUT,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    g_col_lbl = XtVaCreateManagedWidget(
        " ! @ From                    Subject                                       Received",
        xmLabelWidgetClass, col_hdr_fr,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginTop, 2, XmNmarginBottom, 2,
        NULL
    );
    if (g_msg_fontlist) XtVaSetValues(g_col_lbl, XmNfontList, g_msg_fontlist, NULL);

    g_msg_vscroll = XtVaCreateManagedWidget(
        "msgVScroll", xmScrollBarWidgetClass, msg_pane,
        XmNorientation, XmVERTICAL,
        XmNwidth, 16,
        XmNminimum, 0,
        XmNmaximum, 10,
        XmNsliderSize, 10,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, col_hdr_fr,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtAddCallback(g_msg_vscroll, XmNvalueChangedCallback, on_msg_vscroll_cb, NULL);
    XtAddCallback(g_msg_vscroll, XmNdragCallback, on_msg_vscroll_cb, NULL);

    Widget msg_list_fr = XtVaCreateManagedWidget(
        "msgListFrame", xmFrameWidgetClass, msg_pane,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 2,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, col_hdr_fr,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, g_msg_vscroll, XmNrightOffset, 1,
        NULL
    );
    g_msg_list = XtVaCreateManagedWidget(
        "msgListCanvas", xmDrawingAreaWidgetClass, msg_list_fr,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        XmNtraversalOn, True,
        XmNnavigationType, XmTAB_GROUP,
        NULL
    );
    XtAddEventHandler(g_msg_list,
                      ExposureMask | StructureNotifyMask | ButtonPressMask | KeyPressMask,
                      False, on_msg_canvas_event, NULL);

    /* Right-click Context Popup Menu on Message List */
    Arg pop_args[2];
    XtSetArg(pop_args[0], XmNpopupEnabled, XmPOPUP_DISABLED);
    g_msg_popup = XmCreatePopupMenu(g_msg_list, (char *)"msgPopup", pop_args, 1);
    add_menu_item(g_msg_popup, "Mark as Unread", 'U', NULL, NULL, on_mark_unread_cb, NULL);
    add_menu_item(g_msg_popup, "Mark as Read",   'd', NULL, NULL, on_mark_read_cb,   NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, g_msg_popup, NULL);
    add_menu_item(g_msg_popup, "Reply to Sender", 'R', NULL, NULL, on_reply_cb,     NULL);
    add_menu_item(g_msg_popup, "Reply to All",    'A', NULL, NULL, on_reply_all_cb, NULL);
    add_menu_item(g_msg_popup, "Forward",         'F', NULL, NULL, on_forward_cb,   NULL);
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, g_msg_popup, NULL);
    add_menu_item(g_msg_popup, "Delete Message",  'D', NULL, NULL, on_delete_msg_cb, NULL);
    apply_palette_recursive(g_msg_popup, &g_pal);

    /* Bottom-Right: Preview Header Strip + Sunken Embedded HTML Reading Pane */
    Widget pv_pane = XtVaCreateManagedWidget(
        "pvPane", xmFormWidgetClass, g_right_form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, g_splitter,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    g_pv_hdr_fr = XtVaCreateManagedWidget(
        "pvHdrFrame", xmFrameWidgetClass, pv_pane,
        XmNshadowType, XmSHADOW_OUT,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget pv_hdr_form = XtVaCreateManagedWidget("pvHdrForm", xmFormWidgetClass, g_pv_hdr_fr, NULL);

    g_pv_act_rc = XtVaCreateManagedWidget(
        "pvActRc", xmRowColumnWidgetClass, pv_hdr_form,
        XmNorientation, XmVERTICAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 2,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 3,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 3,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL
    );
    for (int a = 0; a < MAX_ATTS; a++) {
        char bname[32];
        snprintf(bname, sizeof(bname), "attChip%d", a);
        g_pv_att_btns[a] = XtVaCreateWidget(
            bname, xmPushButtonWidgetClass, g_pv_act_rc,
            XmNalignment, XmALIGNMENT_BEGINNING,
            XmNmarginWidth, 6,
            XmNmarginHeight, 2,
            XmNshadowThickness, 1,
            NULL
        );
        XtAddEventHandler(g_pv_att_btns[a], ButtonPressMask, False, on_att_chip_event, (XtPointer)(intptr_t)a);
    }
    g_pv_save_att_btn = XtVaCreateWidget("Save Attachments", xmPushButtonWidgetClass, g_pv_act_rc,
                                         XmNmarginWidth, 6, XmNmarginHeight, 2, NULL);
    XtAddCallback(g_pv_save_att_btn, XmNactivateCallback, on_save_atts_cb, NULL);

    Widget pv_meta_rc = XtVaCreateManagedWidget(
        "pvMetaRc", xmRowColumnWidgetClass, pv_hdr_form,
        XmNorientation, XmVERTICAL,
        XmNspacing, 1,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 2,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 2,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 6,
        XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, g_pv_act_rc, XmNrightOffset, 6,
        NULL
    );
    g_pv_from_lbl = XtVaCreateManagedWidget("From: —", xmLabelWidgetClass, pv_meta_rc,
                                            XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_pv_to_lbl   = XtVaCreateManagedWidget("To:   —", xmLabelWidgetClass, pv_meta_rc,
                                            XmNalignment, XmALIGNMENT_BEGINNING, NULL);
    g_pv_subj_lbl = XtVaCreateManagedWidget("Subject: —", xmLabelWidgetClass, pv_meta_rc,
                                            XmNalignment, XmALIGNMENT_BEGINNING, NULL);

    /* Collapsible Meeting Invitation RSVP & Calendar Companion Strip */
    g_pv_invite_fr = XtVaCreateWidget(
        "pvInviteFrame", xmFrameWidgetClass, pv_pane,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 1,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, g_pv_hdr_fr, XmNtopOffset, 2,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    Widget inv_form = XtVaCreateManagedWidget("pvInviteForm", xmFormWidgetClass, g_pv_invite_fr, NULL);
    Widget inv_btns = XtVaCreateManagedWidget(
        "pvInviteBtns", xmRowColumnWidgetClass, inv_form,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 4,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 2,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 2,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL
    );
    g_pv_inv_acc_btn  = XtVaCreateManagedWidget("Accept",           xmPushButtonWidgetClass, inv_btns,
                                                XmNmarginWidth, 8, XmNmarginHeight, 2, XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    g_pv_inv_tent_btn = XtVaCreateManagedWidget("Tentative",        xmPushButtonWidgetClass, inv_btns,
                                                XmNmarginWidth, 8, XmNmarginHeight, 2, XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    g_pv_inv_dec_btn  = XtVaCreateManagedWidget("Decline",          xmPushButtonWidgetClass, inv_btns,
                                                XmNmarginWidth, 8, XmNmarginHeight, 2, XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    g_pv_inv_cal_btn  = XtVaCreateManagedWidget("View in Calendar", xmPushButtonWidgetClass, inv_btns,
                                                XmNmarginWidth, 8, XmNmarginHeight, 2, XmNshadowThickness, 1, XmNhighlightThickness, 0, NULL);
    XtAddCallback(g_pv_inv_acc_btn,  XmNactivateCallback, on_invite_rsvp_cb,  (XtPointer)"accept");
    XtAddCallback(g_pv_inv_tent_btn, XmNactivateCallback, on_invite_rsvp_cb,  (XtPointer)"tentative");
    XtAddCallback(g_pv_inv_dec_btn,  XmNactivateCallback, on_invite_rsvp_cb,  (XtPointer)"decline");
    XtAddCallback(g_pv_inv_cal_btn,  XmNactivateCallback, on_open_calendar_cb,(XtPointer)1);

    g_pv_inv_lbl = XtVaCreateManagedWidget(
        "Meeting Invitation", xmLabelWidgetClass, inv_form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 4,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, inv_btns, XmNrightOffset, 8,
        NULL
    );

    g_pv_html_fr = XtVaCreateManagedWidget(
        "pvHtmlFrame", xmFrameWidgetClass, pv_pane,
        XmNshadowType, XmSHADOW_IN,
        XmNshadowThickness, 2,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, g_pv_hdr_fr, XmNtopOffset, 2,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    g_pv_html_host = XtVaCreateManagedWidget(
        "pvHtmlHost", xmDrawingAreaWidgetClass, g_pv_html_fr,
        XmNmarginWidth, 0,
        XmNmarginHeight, 0,
        NULL
    );
    XtAddEventHandler(g_pv_html_host, StructureNotifyMask, False, on_html_host_configure, NULL);

    XmMainWindowSetAreas(g_main_win, mbar, NULL, NULL, NULL, work);
    apply_palette_recursive(g_toplevel, &g_pal);
    refresh_toolbar_icons();
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    XtSetLanguageProc(NULL, NULL, NULL);

    g_toplevel = XtVaAppInitialize(
        &g_app, "CdeMail", NULL, 0, &argc, argv, NULL,
        XmNtitle, "Inbox - dtMail",
        XmNiconName, "dtMail",
        XmNwidth, 980,
        XmNheight, 660,
        NULL
    );

    DtAppInitialize(g_app, XtDisplay(g_toplevel), g_toplevel, argv[0], (char *)"CdeMail");
    build_main_ui();

    Atom wm_del = XInternAtom(XtDisplay(g_toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_toplevel, wm_del, on_exit_cb, NULL);

    XtRealizeWidget(g_toplevel);

    if (spawn_htmlview() == 0 && g_from_htmlview_fd >= 0) {
        XtAppAddInput(g_app, g_from_htmlview_fd, (XtPointer)XtInputReadMask, on_htmlview_readable, NULL);
    }
    if (spawn_backend() == 0 && g_from_backend_fd >= 0) {
        XtAppAddInput(g_app, g_from_backend_fd, (XtPointer)XtInputReadMask, on_backend_readable, NULL);
    } else {
        set_label(g_st_mid, "Failed to start cde-mail-backend sync engine");
    }

    /* Event loop with global Ctrl+ / Ctrl- / Ctrl+0 font-scaling shortcut snooper */
    while (1) {
        XEvent ev;
        XtAppNextEvent(g_app, &ev);
        if (ev.type == KeyPress && (ev.xkey.state & ControlMask)) {
            KeySym ks0 = XLookupKeysym(&ev.xkey, 0);
            KeySym ks1 = XLookupKeysym(&ev.xkey, 1);
            if (ks0 == XK_plus || ks0 == XK_equal || ks0 == XK_KP_Add ||
                ks1 == XK_plus || ks1 == XK_equal) {
                adjust_font_size(1);
                continue;
            }
            if (ks0 == XK_minus || ks0 == XK_underscore || ks0 == XK_KP_Subtract ||
                ks1 == XK_minus || ks1 == XK_underscore) {
                adjust_font_size(-1);
                continue;
            }
            if (ks0 == XK_0 || ks0 == XK_KP_0) {
                adjust_font_size(0);
                continue;
            }
        }
        XtDispatchEvent(&ev);
    }
    return 0;
}
