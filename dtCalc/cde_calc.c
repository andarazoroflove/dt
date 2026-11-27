/*
 * cde-calc (dtCalc): Native OSF/Motif 2.3 RPN, Algebraic, Scientific, RF &
 *                    64-Bit Programmer Workstation Calculator for CDE
 *
 * Features:
 *   - Dynamic CDE ColorPalette binding (xrdb / ~/.dt/palettes / /usr/dt/palettes)
 *   - Chiseled 3D OSF/Motif color-coded keycaps via XmGetColors shadow synthesis
 *   - Custom Xlib double-buffered Vacuum Fluorescent Display (VFD) with vector
 *     7-segment main readout, 4-level RPN stack (T/Z/Y/X) or Algebraic expression,
 *     4 display themes (Cyan VFD, HP Red LED, Amber Plasma, Olive LCD), and
 *     64-bit clickable LED binary bit-toggle switches (bits 63..0)
 *   - Full Scientific, 64/32/16/8-bit Programmer (HEX/DEC/OCT/BIN), and Amateur
 *     Radio / RF Bench functions (f<->lambda, dBm<->W, dBV, LC Resonance)
 *   - Toggleable #FFF7E9 sunken Motif Paper-Tape Roll + X11 CLIPBOARD copy/paste
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>
#include <Xm/XmAll.h>
#include <Xm/CutPaste.h>

#include "cde_calc_icon.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif

#define C_LIGHT_MPS 299.792458 /* Speed of light in MHz * meters */

typedef enum {
    DISP_VFD_CYAN = 0,
    DISP_LED_RED  = 1,
    DISP_AMBER    = 2,
    DISP_LCD_OLIVE = 3
} DisplayTheme;

typedef enum {
    BASE_DEC = 10,
    BASE_HEX = 16,
    BASE_OCT = 8,
    BASE_BIN = 2
} NumBase;

typedef enum {
    KEY_DIGIT = 0,
    KEY_HEX,
    KEY_OP,
    KEY_SCI,
    KEY_RF,
    KEY_BIT,
    KEY_SHIFT,
    KEY_ENTER,
    KEY_CLEAR
} KeyStyle;

typedef struct {
    bool         rpn_mode;       /* true = HP 4-level RPN, false = Algebraic */
    bool         deg_mode;       /* true = Degrees, false = Radians */
    bool         shift_2nd;      /* 2nd function shift latch */
    bool         signed_word;    /* Programmer signed vs unsigned interpretation */
    int          word_bits;      /* 64, 32, 16, or 8 */
    NumBase      base;           /* 10, 16, 8, 2 */
    DisplayTheme theme;

    /* RPN 4-level stack: X (0), Y (1), Z (2), T (3) + LASTx */
    double       stack[4];
    double       last_x;
    double       mem_val;
    bool         has_mem;
    bool         stack_lift;     /* lift stack on next digit entry */
    bool         entering;       /* currently typing into entry_buf */
    bool         error_flag;
    char         error_msg[64];
    char         entry_buf[96];

    /* Algebraic operator-precedence stacks */
    double       alg_vals[32];
    char         alg_ops[32];
    int          alg_vtop;
    int          alg_otop;
    char         alg_expr[256];
} CalcState;

/* ---------------- Global UI & State ---------------- */
static CalcState    g_st;
static XtAppContext g_app;
static Widget       g_toplevel   = NULL;
static Widget       g_vfd_area   = NULL;
static Widget       g_tape_frame = NULL;
static Widget       g_tape_text  = NULL;
static Widget       g_hex_btns[6];
static Widget       g_sci_btns[6]; /* Dynamic labels when 2nd is active */
static Widget       g_btn_rpn    = NULL;
static Widget       g_btn_deg    = NULL;
static Widget       g_btn_base[4];
static Widget       g_btn_bits   = NULL;
static Widget       g_btn_sign   = NULL;
static Widget       g_btn_theme  = NULL;
static Widget       g_btn_tape   = NULL;
static bool         g_tape_shown = true;

static Pixmap       g_vfd_pix    = None;
static int          g_vfd_w      = 0;
static int          g_vfd_h      = 0;
static GC           g_vfd_gc     = NULL;
static XFontStruct *g_font_sm    = NULL;
static XFontStruct *g_font_md    = NULL;

/* Bit switch hitboxes populated on draw */
typedef struct {
    int x, y, w, h;
} RectI;
static RectI g_bit_rects[64];

/* ---------------- Word Masking & Formatting Helpers ---------------- */
static uint64_t word_mask(int bits) {
    if (bits >= 64) return UINT64_MAX;
    if (bits <= 0)  return 0xFFULL;
    return (1ULL << bits) - 1ULL;
}

static uint64_t double_to_u64(double v, int bits) {
    uint64_t mask = word_mask(bits);
    if (!isfinite(v)) return 0ULL;
    if (v < 0.0) {
        int64_t sv = (int64_t)llround(v);
        return ((uint64_t)sv) & mask;
    }
    if (v >= 18446744073709551615.0) return UINT64_MAX & mask;
    uint64_t uv = (uint64_t)llround(v);
    return uv & mask;
}

static double u64_to_double(uint64_t u, int bits, bool is_signed) {
    uint64_t mask = word_mask(bits);
    u &= mask;
    if (is_signed) {
        uint64_t sign_bit = (bits >= 64) ? (1ULL << 63) : (1ULL << (bits - 1));
        if (u & sign_bit) {
            if (bits >= 64) {
                return (double)((int64_t)u);
            } else {
                int64_t ext = (int64_t)(u | (~mask));
                return (double)ext;
            }
        }
    }
    return (double)u;
}

static void format_u64_base(uint64_t u, NumBase base, int bits, bool is_signed, char *out, size_t out_sz) {
    u &= word_mask(bits);
    if (base == BASE_HEX) {
        int nyb = bits / 4;
        snprintf(out, out_sz, "%0*" PRIX64, nyb, u);
    } else if (base == BASE_OCT) {
        snprintf(out, out_sz, "%" PRIo64, u);
    } else if (base == BASE_BIN) {
        char tmp[72];
        int pos = 0;
        int show_bits = (bits > 32) ? 32 : bits; /* Main 7-seg fits up to 20-32 chars */
        bool started = false;
        for (int b = bits - 1; b >= 0; b--) {
            int bit = (int)((u >> b) & 1ULL);
            if (bit) started = true;
            if (started || b < show_bits || b == 0) {
                if (pos < (int)sizeof(tmp) - 2) tmp[pos++] = bit ? '1' : '0';
            }
        }
        tmp[pos] = '\0';
        snprintf(out, out_sz, "%s", tmp);
    } else {
        if (is_signed) {
            int64_t sv = (bits >= 64) ? (int64_t)u
                         : ((u & (1ULL << (bits - 1))) ? (int64_t)(u | (~word_mask(bits))) : (int64_t)u);
            snprintf(out, out_sz, "%" PRId64, sv);
        } else {
            snprintf(out, out_sz, "%" PRIu64, u);
        }
    }
}

static void format_value(const CalcState *st, double val, char *out, size_t out_sz) {
    if (!isfinite(val)) {
        snprintf(out, out_sz, "OVERFLOW");
        return;
    }
    if (st->base != BASE_DEC) {
        uint64_t u = double_to_u64(val, st->word_bits);
        format_u64_base(u, st->base, st->word_bits, st->signed_word, out, out_sz);
        return;
    }
    if (fabs(val) == 0.0) val = 0.0;
    double av = fabs(val);
    if (av != 0.0 && (av >= 1e12 || av < 1e-7)) {
        snprintf(out, out_sz, "%.8E", val);
    } else {
        snprintf(out, out_sz, "%.10g", val);
    }
}

static double parse_entry_buf(const CalcState *st) {
    if (st->entry_buf[0] == '\0') return 0.0;
    if (st->base == BASE_DEC) {
        return strtod(st->entry_buf, NULL);
    }
    bool neg = (st->entry_buf[0] == '-');
    const char *p = neg ? st->entry_buf + 1 : st->entry_buf;
    uint64_t u = strtoull(p, NULL, (int)st->base);
    if (neg) u = (~u + 1ULL);
    return u64_to_double(u, st->word_bits, st->signed_word);
}

/* ---------------- Paper Tape Logging ---------------- */
static void tape_append(const char *line) {
    if (!g_tape_text || !line) return;
    XmTextPosition last = XmTextGetLastPosition(g_tape_text);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\n", line);
    XmTextInsert(g_tape_text, last, buf);
    XmTextShowPosition(g_tape_text, XmTextGetLastPosition(g_tape_text));
}

/* ---------------- Core Calculator Engine ---------------- */
static void calc_init(CalcState *st) {
    memset(st, 0, sizeof(*st));
    st->rpn_mode    = true;   /* Classic HP workstation default, 1-click toggle to ALG */
    st->deg_mode    = true;
    st->signed_word = true;
    st->word_bits   = 64;
    st->base        = BASE_DEC;
    st->theme       = DISP_VFD_CYAN;
    st->stack_lift  = false;
}

static void calc_set_error(CalcState *st, const char *msg) {
    st->error_flag = true;
    snprintf(st->error_msg, sizeof(st->error_msg), "%s", msg ? msg : "MATH ERR");
    st->entering = false;
    st->entry_buf[0] = '\0';
}

static void commit_entry_if_needed(CalcState *st) {
    if (!st->entering) return;
    st->stack[0] = parse_entry_buf(st);
    st->entering = false;
    st->entry_buf[0] = '\0';
    st->stack_lift = true;
}

static void rpn_push(CalcState *st, double v) {
    st->stack[3] = st->stack[2];
    st->stack[2] = st->stack[1];
    st->stack[1] = st->stack[0];
    st->stack[0] = v;
}

static double rpn_pop_binary(CalcState *st, double result) {
    st->last_x   = st->stack[0];
    st->stack[0] = result;
    st->stack[1] = st->stack[2];
    st->stack[2] = st->stack[3];
    /* T (stack[3]) replicates on HP-48/41 RPN drop */
    st->stack_lift = true;
    return result;
}

static void calc_digit(CalcState *st, char ch) {
    if (st->error_flag) {
        st->error_flag = false;
        st->error_msg[0] = '\0';
    }
    ch = (char)toupper((unsigned char)ch);

    /* Validate character for current base */
    if (ch == '.') {
        if (st->base != BASE_DEC) return;
        if (st->entering && strchr(st->entry_buf, '.')) return;
    } else if (ch == 'E' && st->base == BASE_DEC) {
        if (!st->entering) {
            snprintf(st->entry_buf, sizeof(st->entry_buf), "1E");
            if (st->rpn_mode && st->stack_lift) rpn_push(st, 0.0);
            st->entering = true;
            st->stack_lift = false;
            st->stack[0] = parse_entry_buf(st);
            return;
        }
        if (strchr(st->entry_buf, 'E')) return;
    } else {
        int dval = -1;
        if (ch >= '0' && ch <= '9') dval = ch - '0';
        else if (ch >= 'A' && ch <= 'F') dval = 10 + (ch - 'A');
        if (dval < 0 || dval >= (int)st->base) return;
    }

    if (!st->entering) {
        if (st->rpn_mode && st->stack_lift) {
            rpn_push(st, st->stack[0]);
        }
        st->entering = true;
        st->stack_lift = false;
        if (ch == '.') {
            snprintf(st->entry_buf, sizeof(st->entry_buf), "0.");
        } else {
            snprintf(st->entry_buf, sizeof(st->entry_buf), "%c", ch);
        }
    } else {
        size_t len = strlen(st->entry_buf);
        if (len + 2 < sizeof(st->entry_buf)) {
            st->entry_buf[len]     = ch;
            st->entry_buf[len + 1] = '\0';
        }
    }
    st->stack[0] = parse_entry_buf(st);
}

static void calc_backspace(CalcState *st) {
    if (st->error_flag) {
        st->error_flag = false;
        return;
    }
    if (st->entering) {
        size_t len = strlen(st->entry_buf);
        if (len > 0) st->entry_buf[len - 1] = '\0';
        if (st->entry_buf[0] == '\0' || strcmp(st->entry_buf, "-") == 0) {
            st->entry_buf[0] = '\0';
            st->entering = false;
            st->stack[0] = 0.0;
        } else {
            st->stack[0] = parse_entry_buf(st);
        }
    } else {
        st->stack[0] = 0.0;
        st->stack_lift = false;
    }
}

static void calc_negate(CalcState *st) {
    if (st->error_flag) return;
    if (st->entering && st->base == BASE_DEC) {
        char *ep = strchr(st->entry_buf, 'E');
        if (ep) {
            /* Toggle exponent sign if right after E */
            if (ep[1] == '-') {
                memmove(ep + 1, ep + 2, strlen(ep + 2) + 1);
            } else if (ep[1] == '+') {
                ep[1] = '-';
            } else {
                char tail[64];
                snprintf(tail, sizeof(tail), "%s", ep + 1);
                snprintf(ep + 1, sizeof(st->entry_buf) - (size_t)(ep + 1 - st->entry_buf), "-%s", tail);
            }
        } else {
            if (st->entry_buf[0] == '-') {
                memmove(st->entry_buf, st->entry_buf + 1, strlen(st->entry_buf));
            } else {
                char tmp[96];
                snprintf(tmp, sizeof(tmp), "-%s", st->entry_buf);
                snprintf(st->entry_buf, sizeof(st->entry_buf), "%s", tmp);
            }
        }
        st->stack[0] = parse_entry_buf(st);
    } else {
        commit_entry_if_needed(st);
        if (st->base != BASE_DEC) {
            uint64_t u = double_to_u64(st->stack[0], st->word_bits);
            u = (~u + 1ULL) & word_mask(st->word_bits);
            st->stack[0] = u64_to_double(u, st->word_bits, st->signed_word);
        } else {
            st->stack[0] = -st->stack[0];
        }
        st->stack_lift = true;
    }
}

/* Algebraic Precedence Helpers */
static int alg_prec(char op) {
    switch (op) {
        case '|': case '^': return 1;
        case '&':           return 2;
        case '<': case '>': return 3;
        case '+': case '-': return 4;
        case '*': case '/': case '%': return 5;
        case 'P': /* power x^y */     return 6;
        default: return 0;
    }
}

static bool eval_binary(CalcState *st, double y, double x, char op, double *out) {
    if (op == '+') { *out = y + x; return true; }
    if (op == '-') { *out = y - x; return true; }
    if (op == '*') { *out = y * x; return true; }
    if (op == '/') {
        if (fabs(x) < 1e-300) { calc_set_error(st, "DIV BY ZERO"); return false; }
        *out = y / x;
        return true;
    }
    if (op == '%') {
        if (fabs(x) < 1e-300) { calc_set_error(st, "MOD BY ZERO"); return false; }
        *out = fmod(y, x);
        return true;
    }
    if (op == 'P') {
        double r = pow(y, x);
        if (!isfinite(r)) { calc_set_error(st, "DOMAIN / OVF"); return false; }
        *out = r;
        return true;
    }
    uint64_t uy = double_to_u64(y, st->word_bits);
    uint64_t ux = double_to_u64(x, st->word_bits);
    uint64_t ur = 0;
    int sh = (int)(ux & 63ULL);
    if (op == '&') ur = uy & ux;
    else if (op == '|') ur = uy | ux;
    else if (op == '^') ur = uy ^ ux;
    else if (op == '<') ur = (sh >= st->word_bits) ? 0ULL : (uy << sh);
    else if (op == '>') ur = (sh >= st->word_bits) ? 0ULL : (uy >> sh);
    *out = u64_to_double(ur, st->word_bits, st->signed_word);
    return true;
}

static bool alg_reduce_top(CalcState *st) {
    if (st->alg_otop <= 0 || st->alg_vtop < 2) return true;
    char op = st->alg_ops[--st->alg_otop];
    double x = st->alg_vals[--st->alg_vtop];
    double y = st->alg_vals[--st->alg_vtop];
    double r = 0.0;
    if (!eval_binary(st, y, x, op, &r)) return false;
    st->alg_vals[st->alg_vtop++] = r;
    return true;
}

static void calc_binary_op(CalcState *st, char op, const char *op_label) {
    if (st->error_flag) return;
    commit_entry_if_needed(st);

    if (st->rpn_mode) {
        double y = st->stack[1];
        double x = st->stack[0];
        double r = 0.0;
        if (!eval_binary(st, y, x, op, &r)) return;
        char sy[48], sx[48], sr[48], line[200];
        format_value(st, y, sy, sizeof(sy));
        format_value(st, x, sx, sizeof(sx));
        rpn_pop_binary(st, r);
        format_value(st, r, sr, sizeof(sr));
        snprintf(line, sizeof(line), "RPN: %s %s %s = %s", sy, op_label, sx, sr);
        tape_append(line);
    } else {
        double cur = st->stack[0];
        char sv[48];
        format_value(st, cur, sv, sizeof(sv));
        if (st->alg_vtop < 30) st->alg_vals[st->alg_vtop++] = cur;
        while (st->alg_otop > 0 && st->alg_ops[st->alg_otop - 1] != '(' &&
               alg_prec(st->alg_ops[st->alg_otop - 1]) >= alg_prec(op)) {
            if (!alg_reduce_top(st)) return;
        }
        if (st->alg_otop < 30) st->alg_ops[st->alg_otop++] = op;
        if (st->alg_vtop > 0) st->stack[0] = st->alg_vals[st->alg_vtop - 1];
        size_t l = strlen(st->alg_expr);
        snprintf(st->alg_expr + l, sizeof(st->alg_expr) - l, "%s %s ", sv, op_label);
        st->stack_lift = false;
    }
}

static void calc_enter_or_equals(CalcState *st) {
    if (st->error_flag) {
        st->error_flag = false;
        return;
    }
    if (st->rpn_mode) {
        if (st->entering) {
            st->stack[0] = parse_entry_buf(st);
            st->entering = false;
            st->entry_buf[0] = '\0';
        }
        rpn_push(st, st->stack[0]);
        st->stack_lift = false; /* Classic HP ENTER disables next digit lift */
        char sx[48], line[128];
        format_value(st, st->stack[0], sx, sizeof(sx));
        snprintf(line, sizeof(line), "ENTER -> %s", sx);
        tape_append(line);
    } else {
        commit_entry_if_needed(st);
        char sx[48];
        format_value(st, st->stack[0], sx, sizeof(sx));
        if (st->alg_vtop < 30) st->alg_vals[st->alg_vtop++] = st->stack[0];
        while (st->alg_otop > 0) {
            if (st->alg_ops[st->alg_otop - 1] == '(') {
                st->alg_otop--;
                continue;
            }
            if (!alg_reduce_top(st)) return;
        }
        if (st->alg_vtop > 0) {
            st->last_x   = st->stack[0];
            st->stack[0] = st->alg_vals[0];
        }
        st->alg_vtop = 0;
        st->alg_otop = 0;
        char sr[48], line[360];
        format_value(st, st->stack[0], sr, sizeof(sr));
        snprintf(line, sizeof(line), "%s%s = %s", st->alg_expr, sx, sr);
        tape_append(line);
        st->alg_expr[0] = '\0';
        st->stack_lift = true;
    }
}

static void calc_paren(CalcState *st, char p) {
    if (st->rpn_mode || st->error_flag) return;
    if (p == '(') {
        if (st->alg_otop < 30) st->alg_ops[st->alg_otop++] = '(';
        size_t l = strlen(st->alg_expr);
        snprintf(st->alg_expr + l, sizeof(st->alg_expr) - l, "( ");
    } else if (p == ')') {
        commit_entry_if_needed(st);
        char sx[48];
        format_value(st, st->stack[0], sx, sizeof(sx));
        if (st->alg_vtop < 30) st->alg_vals[st->alg_vtop++] = st->stack[0];
        while (st->alg_otop > 0 && st->alg_ops[st->alg_otop - 1] != '(') {
            if (!alg_reduce_top(st)) return;
        }
        if (st->alg_otop > 0 && st->alg_ops[st->alg_otop - 1] == '(') st->alg_otop--;
        if (st->alg_vtop > 0) {
            /* Pop back into X so subsequent binary op re-pushes cleanly */
            st->stack[0] = st->alg_vals[--st->alg_vtop];
        }
        size_t l = strlen(st->alg_expr);
        snprintf(st->alg_expr + l, sizeof(st->alg_expr) - l, "%s ) ", sx);
    }
}

/* Scientific, Bitwise Unary, and RF Engineering Operations */
static void calc_unary_or_rf(CalcState *st, const char *op) {
    if (st->error_flag) return;
    commit_entry_if_needed(st);
    double x = st->stack[0];
    double y = st->stack[1];
    double r = x;
    char desc[160] = "";
    double d2r = st->deg_mode ? (M_PI / 180.0) : 1.0;
    double r2d = st->deg_mode ? (180.0 / M_PI) : 1.0;

    if (strcmp(op, "SIN") == 0) {
        r = st->shift_2nd ? (asin(x) * r2d) : sin(x * d2r);
        snprintf(desc, sizeof(desc), "%s(%.8g)", st->shift_2nd ? "ASIN" : "SIN", x);
    } else if (strcmp(op, "COS") == 0) {
        r = st->shift_2nd ? (acos(x) * r2d) : cos(x * d2r);
        snprintf(desc, sizeof(desc), "%s(%.8g)", st->shift_2nd ? "ACOS" : "COS", x);
    } else if (strcmp(op, "TAN") == 0) {
        r = st->shift_2nd ? (atan(x) * r2d) : tan(x * d2r);
        snprintf(desc, sizeof(desc), "%s(%.8g)", st->shift_2nd ? "ATAN" : "TAN", x);
    } else if (strcmp(op, "LN") == 0) {
        if (!st->shift_2nd && x <= 0.0) { calc_set_error(st, "LN DOMAIN"); return; }
        r = st->shift_2nd ? exp(x) : log(x);
        snprintf(desc, sizeof(desc), "%s(%.8g)", st->shift_2nd ? "EXP" : "LN", x);
    } else if (strcmp(op, "LOG") == 0) {
        if (!st->shift_2nd && x <= 0.0) { calc_set_error(st, "LOG DOMAIN"); return; }
        r = st->shift_2nd ? pow(10.0, x) : log10(x);
        snprintf(desc, sizeof(desc), "%s(%.8g)", st->shift_2nd ? "10^x" : "LOG10", x);
    } else if (strcmp(op, "SQRT") == 0) {
        if (st->shift_2nd) {
            r = x * x;
            snprintf(desc, sizeof(desc), "SQR(%.8g)", x);
        } else {
            if (x < 0.0) { calc_set_error(st, "SQRT NEG"); return; }
            r = sqrt(x);
            snprintf(desc, sizeof(desc), "SQRT(%.8g)", x);
        }
    } else if (strcmp(op, "INV") == 0) {
        if (fabs(x) < 1e-300) { calc_set_error(st, "DIV BY ZERO"); return; }
        r = 1.0 / x;
        snprintf(desc, sizeof(desc), "1/(%.8g)", x);
    } else if (strcmp(op, "FACT") == 0) {
        if (x < 0.0 || x > 170.0 || floor(x) != x) { calc_set_error(st, "FACT DOMAIN"); return; }
        r = tgamma(x + 1.0);
        snprintf(desc, sizeof(desc), "%.0f!", x);
    } else if (strcmp(op, "PI") == 0) {
        if (st->rpn_mode && st->stack_lift) rpn_push(st, st->stack[0]);
        st->stack[0] = st->shift_2nd ? M_E : M_PI;
        st->stack_lift = true;
        st->shift_2nd = false;
        return;
    } else if (strcmp(op, "NOT") == 0) {
        uint64_t u = (~double_to_u64(x, st->word_bits)) & word_mask(st->word_bits);
        r = u64_to_double(u, st->word_bits, st->signed_word);
        snprintf(desc, sizeof(desc), "NOT(%s)", "X");
    } else if (strcmp(op, "ROL") == 0) {
        uint64_t u = double_to_u64(x, st->word_bits);
        int b = st->word_bits;
        u = ((u << 1) | (u >> (b - 1))) & word_mask(b);
        r = u64_to_double(u, b, st->signed_word);
        snprintf(desc, sizeof(desc), "ROL_%d", b);
    } else if (strcmp(op, "ROR") == 0) {
        uint64_t u = double_to_u64(x, st->word_bits);
        int b = st->word_bits;
        u = ((u >> 1) | (u << (b - 1))) & word_mask(b);
        r = u64_to_double(u, b, st->signed_word);
        snprintf(desc, sizeof(desc), "ROR_%d", b);
    } else if (strcmp(op, "BSWP") == 0) {
        uint64_t u = double_to_u64(x, st->word_bits);
        uint64_t sw = 0;
        int nbytes = st->word_bits / 8;
        for (int i = 0; i < nbytes; i++) {
            sw = (sw << 8) | ((u >> (i * 8)) & 0xFFULL);
        }
        r = u64_to_double(sw, st->word_bits, st->signed_word);
        snprintf(desc, sizeof(desc), "BSWAP_%d", st->word_bits);
    } else if (strcmp(op, "FLAM") == 0) {
        /* RF Frequency (MHz) <-> Wavelength (meters): c = 299.792458 */
        if (x <= 0.0) { calc_set_error(st, "FREQ/WL <= 0"); return; }
        r = C_LIGHT_MPS / x;
        snprintf(desc, sizeof(desc), "RF f<->lambda(%.6g) -> %.6g", x, r);
    } else if (strcmp(op, "DBMW") == 0) {
        /* dBm -> Watts (or 2nd: Watts -> dBm) */
        if (st->shift_2nd) {
            if (x <= 0.0) { calc_set_error(st, "WATTS <= 0"); return; }
            r = 10.0 * log10(x) + 30.0;
            snprintf(desc, sizeof(desc), "RF %.6g W -> %.4f dBm", x, r);
        } else {
            r = pow(10.0, (x - 30.0) / 10.0);
            snprintf(desc, sizeof(desc), "RF %.4g dBm -> %.6g W", x, r);
        }
    } else if (strcmp(op, "WDBM") == 0) {
        if (x <= 0.0) { calc_set_error(st, "WATTS <= 0"); return; }
        r = 10.0 * log10(x) + 30.0;
        snprintf(desc, sizeof(desc), "RF %.6g W -> %.4f dBm", x, r);
    } else if (strcmp(op, "LC") == 0) {
        /* Resonant Freq (MHz) from Y = L (uH), X = C (pF): f = 1000 / (2*pi*sqrt(L_uH * C_pF)) */
        if (x <= 0.0 || y <= 0.0) { calc_set_error(st, "NEED Y=uH X=pF"); return; }
        r = 1000.0 / (2.0 * M_PI * sqrt(y * x));
        st->last_x = x;
        if (st->rpn_mode) rpn_pop_binary(st, r);
        else st->stack[0] = r;
        snprintf(desc, sizeof(desc), "LC(%.4g uH, %.4g pF) = %.6f MHz", y, x, r);
        tape_append(desc);
        st->shift_2nd = false;
        return;
    } else if (strcmp(op, "DBV") == 0) {
        /* Voltage dB = 20 * log10(X / Y) if Y > 0 else 20 * log10(X) */
        double ref = (st->rpn_mode && y > 0.0) ? y : 1.0;
        if (x <= 0.0) { calc_set_error(st, "VOLTS <= 0"); return; }
        r = 20.0 * log10(x / ref);
        snprintf(desc, sizeof(desc), "20*log10(%.4g/%.4g) = %.3f dB", x, ref, r);
    }

    if (!isfinite(r)) {
        calc_set_error(st, "OVERFLOW/NAN");
        st->shift_2nd = false;
        return;
    }
    st->last_x = x;
    st->stack[0] = r;
    st->stack_lift = true;
    st->shift_2nd = false;
    if (desc[0]) {
        char sr[48], line[220];
        format_value(st, r, sr, sizeof(sr));
        snprintf(line, sizeof(line), "%s = %s", desc, sr);
        tape_append(line);
    }
}

/* ---------------- X11 Vector 7-Segment + VFD + 64-Bit LED Renderer ---------------- */
static Pixel alloc_hex(Display *dpy, Colormap cmap, const char *hex, Pixel fallback) {
    XColor xc;
    if (XParseColor(dpy, cmap, hex, &xc) && XAllocColor(dpy, cmap, &xc)) {
        return xc.pixel;
    }
    return fallback;
}

typedef struct {
    Pixel glass_bg;
    Pixel bezel_dark;
    Pixel seg_lit;
    Pixel seg_glow;
    Pixel seg_ghost;
    Pixel ann_amber;
    Pixel stack_txt;
    Pixel bit_on;
    Pixel bit_off;
    Pixel bit_txt;
} VfdColors;

static VfdColors get_vfd_colors(Display *dpy, Colormap cmap, DisplayTheme th) {
    VfdColors c;
    Pixel blk = BlackPixel(dpy, DefaultScreen(dpy));
    Pixel wht = WhitePixel(dpy, DefaultScreen(dpy));
    c.bezel_dark = alloc_hex(dpy, cmap, "#181C24", blk);
    if (th == DISP_LED_RED) {
        c.glass_bg  = alloc_hex(dpy, cmap, "#1B0507", blk);
        c.seg_lit   = alloc_hex(dpy, cmap, "#FF3A32", wht);
        c.seg_glow  = alloc_hex(dpy, cmap, "#7A1214", blk);
        c.seg_ghost = alloc_hex(dpy, cmap, "#330B0E", blk);
        c.ann_amber = alloc_hex(dpy, cmap, "#FF9E3D", wht);
        c.stack_txt = alloc_hex(dpy, cmap, "#FF6E68", wht);
        c.bit_on    = alloc_hex(dpy, cmap, "#FF3A32", wht);
        c.bit_off   = alloc_hex(dpy, cmap, "#3D1215", blk);
        c.bit_txt   = alloc_hex(dpy, cmap, "#C88A8E", wht);
    } else if (th == DISP_AMBER) {
        c.glass_bg  = alloc_hex(dpy, cmap, "#191005", blk);
        c.seg_lit   = alloc_hex(dpy, cmap, "#FFA42E", wht);
        c.seg_glow  = alloc_hex(dpy, cmap, "#6E4008", blk);
        c.seg_ghost = alloc_hex(dpy, cmap, "#33210A", blk);
        c.ann_amber = alloc_hex(dpy, cmap, "#FFE074", wht);
        c.stack_txt = alloc_hex(dpy, cmap, "#DF9030", wht);
        c.bit_on    = alloc_hex(dpy, cmap, "#FFAE38", wht);
        c.bit_off   = alloc_hex(dpy, cmap, "#3B2810", blk);
        c.bit_txt   = alloc_hex(dpy, cmap, "#BFA07A", wht);
    } else if (th == DISP_LCD_OLIVE) {
        c.glass_bg  = alloc_hex(dpy, cmap, "#9BA882", wht);
        c.seg_lit   = alloc_hex(dpy, cmap, "#161E12", blk);
        c.seg_glow  = alloc_hex(dpy, cmap, "#7D8B68", blk);
        c.seg_ghost = alloc_hex(dpy, cmap, "#8E9B76", blk);
        c.ann_amber = alloc_hex(dpy, cmap, "#1E2B18", blk);
        c.stack_txt = alloc_hex(dpy, cmap, "#22301B", blk);
        c.bit_on    = alloc_hex(dpy, cmap, "#182414", blk);
        c.bit_off   = alloc_hex(dpy, cmap, "#85936E", blk);
        c.bit_txt   = alloc_hex(dpy, cmap, "#2D3B24", blk);
    } else {
        /* Classic Cyan-Green Vacuum Fluorescent Display */
        c.glass_bg  = alloc_hex(dpy, cmap, "#071817", blk);
        c.seg_lit   = alloc_hex(dpy, cmap, "#4AFCD4", wht);
        c.seg_glow  = alloc_hex(dpy, cmap, "#125E4E", blk);
        c.seg_ghost = alloc_hex(dpy, cmap, "#0E2E2B", blk);
        c.ann_amber = alloc_hex(dpy, cmap, "#FFB840", wht);
        c.stack_txt = alloc_hex(dpy, cmap, "#52CDB5", wht);
        c.bit_on    = alloc_hex(dpy, cmap, "#FF483E", wht);
        c.bit_off   = alloc_hex(dpy, cmap, "#163330", blk);
        c.bit_txt   = alloc_hex(dpy, cmap, "#7DB5AD", wht);
    }
    return c;
}

/* 7-segment bitmask: bit0=A(top), 1=B(top-R), 2=C(bot-R), 3=D(bot), 4=E(bot-L), 5=F(top-L), 6=G(mid) */
static uint8_t seg7_mask_for_char(char ch) {
    switch (toupper((unsigned char)ch)) {
        case '0': return 0x3F;
        case '1': return 0x06;
        case '2': return 0x5B;
        case '3': return 0x4F;
        case '4': return 0x66;
        case '5': return 0x6D;
        case '6': return 0x7D;
        case '7': return 0x07;
        case '8': return 0x7F;
        case '9': return 0x6F;
        case 'A': return 0x77;
        case 'B': return 0x7C;
        case 'C': return 0x39;
        case 'D': return 0x5E;
        case 'E': return 0x79;
        case 'F': return 0x71;
        case '-': return 0x40;
        case 'R': return 0x50;
        case 'O': return 0x5C;
        case 'V': return 0x3E;
        case 'L': return 0x38;
        case 'N': return 0x54;
        case 'H': return 0x76;
        case 'P': return 0x73;
        case 'U': return 0x3E;
        default:  return 0x00;
    }
}

static void draw_seg7_digit(Display *dpy, Drawable drw, GC gc, int x, int y, int w, int h,
                            uint8_t mask, Pixel lit, Pixel ghost) {
    int t = (w >= 14) ? 3 : 2;
    int hh = h / 2;
    struct { int x, y, w, h; } segs[7] = {
        { x + t,         y,              w - 2 * t, t },          /* A */
        { x + w - t,     y + t,          t,         hh - t },     /* B */
        { x + w - t,     y + hh + 1,     t,         h - hh - t - 1 }, /* C */
        { x + t,         y + h - t,      w - 2 * t, t },          /* D */
        { x,             y + hh + 1,     t,         h - hh - t - 1 }, /* E */
        { x,             y + t,          t,         hh - t },     /* F */
        { x + t,         y + hh - t / 2, w - 2 * t, t }           /* G */
    };
    for (int i = 0; i < 7; i++) {
        XSetForeground(dpy, gc, (mask & (1 << i)) ? lit : ghost);
        if (segs[i].w > 0 && segs[i].h > 0) {
            XFillRectangle(dpy, drw, gc, segs[i].x, segs[i].y,
                           (unsigned)segs[i].w, (unsigned)segs[i].h);
        }
    }
}

static void render_vfd(void) {
    if (!g_vfd_area || !XtIsRealized(g_vfd_area)) return;
    Display *dpy = XtDisplay(g_vfd_area);
    Window   win = XtWindow(g_vfd_area);
    Dimension ww = 0, hh = 0;
    XtVaGetValues(g_vfd_area, XmNwidth, &ww, XmNheight, &hh, NULL);
    if (ww < 40 || hh < 40) return;

    if (!g_vfd_gc) {
        g_vfd_gc  = XCreateGC(dpy, win, 0, NULL);
        g_font_sm = XLoadQueryFont(dpy, "-misc-fixed-bold-r-normal--12-*-*-*-*-*-iso8859-1");
        if (!g_font_sm) g_font_sm = XLoadQueryFont(dpy, "fixed");
        g_font_md = XLoadQueryFont(dpy, "-misc-fixed-bold-r-normal--13-*-*-*-*-*-iso8859-1");
        if (!g_font_md) g_font_md = g_font_sm;
    }

    if (g_vfd_pix == None || g_vfd_w != (int)ww || g_vfd_h != (int)hh) {
        if (g_vfd_pix != None) XFreePixmap(dpy, g_vfd_pix);
        g_vfd_pix = XCreatePixmap(dpy, win, ww, hh, DefaultDepth(dpy, DefaultScreen(dpy)));
        g_vfd_w = (int)ww;
        g_vfd_h = (int)hh;
    }

    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    VfdColors vc = get_vfd_colors(dpy, cmap, g_st.theme);

    /* Outer sunken bezel + VFD glass */
    XSetForeground(dpy, g_vfd_gc, vc.bezel_dark);
    XFillRectangle(dpy, g_vfd_pix, g_vfd_gc, 0, 0, ww, hh);
    XSetForeground(dpy, g_vfd_gc, vc.glass_bg);
    XFillRectangle(dpy, g_vfd_pix, g_vfd_gc, 4, 4, ww - 8, hh - 8);

    /* 1. Top Annunciator Row */
    if (g_font_sm) XSetFont(dpy, g_vfd_gc, g_font_sm->fid);
    XSetForeground(dpy, g_vfd_gc, vc.ann_amber);
    char ann[256];
    const char *bstr = (g_st.base == BASE_HEX) ? "HEX" :
                       (g_st.base == BASE_OCT) ? "OCT" :
                       (g_st.base == BASE_BIN) ? "BIN" : "DEC";
    snprintf(ann, sizeof(ann), "[%s]  [%s]  [%s]  [%d-BIT %s]%s%s%s",
             g_st.rpn_mode ? "RPN STACK" : "ALGEBRAIC",
             g_st.deg_mode ? "DEG" : "RAD",
             bstr,
             g_st.word_bits,
             g_st.signed_word ? "SGN" : "UNS",
             g_st.shift_2nd ? "  [2nd]" : "",
             g_st.has_mem   ? "  [MEM]" : "",
             g_st.error_flag ? "  [ERROR]" : "");
    XDrawString(dpy, g_vfd_pix, g_vfd_gc, 12, 20, ann, (int)strlen(ann));

    /* 2. Upper Stack / Companion Multi-Base Readout */
    if (g_font_md) XSetFont(dpy, g_vfd_gc, g_font_md->fid);
    XSetForeground(dpy, g_vfd_gc, vc.stack_txt);
    if (g_st.rpn_mode) {
        char st_t[48], st_z[48], st_y[48], st_l[48], row1[160], row2[160];
        format_value(&g_st, g_st.stack[3], st_t, sizeof(st_t));
        format_value(&g_st, g_st.stack[2], st_z, sizeof(st_z));
        format_value(&g_st, g_st.stack[1], st_y, sizeof(st_y));
        format_value(&g_st, g_st.last_x,   st_l, sizeof(st_l));
        snprintf(row1, sizeof(row1), "T: %-20s   Z: %s", st_t, st_z);
        snprintf(row2, sizeof(row2), "Y: %-20s   LASTx: %s", st_y, st_l);
        XDrawString(dpy, g_vfd_pix, g_vfd_gc, 12, 38, row1, (int)strlen(row1));
        XDrawString(dpy, g_vfd_pix, g_vfd_gc, 12, 54, row2, (int)strlen(row2));
    } else {
        uint64_t u = double_to_u64(g_st.stack[0], g_st.word_bits);
        char row1[300], row2[200];
        snprintf(row1, sizeof(row1), "EXPR: %s", g_st.alg_expr[0] ? g_st.alg_expr : "(ready)");
        snprintf(row2, sizeof(row2), "HEX: %0*" PRIX64 "   OCT: %" PRIo64,
                 (g_st.word_bits > 32) ? 16 : (g_st.word_bits / 4), u, u);
        XDrawString(dpy, g_vfd_pix, g_vfd_gc, 12, 38, row1, (int)strlen(row1));
        XDrawString(dpy, g_vfd_pix, g_vfd_gc, 12, 54, row2, (int)strlen(row2));
    }

    /* 3. Main Glowing Vector 7-Segment X Register */
    char main_str[96];
    if (g_st.error_flag) {
        snprintf(main_str, sizeof(main_str), "%s", g_st.error_msg);
    } else if (g_st.entering && g_st.entry_buf[0] != '\0') {
        snprintf(main_str, sizeof(main_str), "%s", g_st.entry_buf);
    } else {
        format_value(&g_st, g_st.stack[0], main_str, sizeof(main_str));
    }

    int avail_w = (int)ww - 28;
    int nslots  = (avail_w / 19);
    if (nslots < 12) nslots = 12;
    if (nslots > 24) nslots = 24;
    int dw = 13, dh = 28, pitch = 18;
    if (nslots * pitch > avail_w) {
        pitch = avail_w / nslots;
        dw = pitch - 5;
        if (dw < 8) dw = 8;
    }
    int start_x = (int)ww - 14 - nslots * pitch;
    int seg_y   = 63;

    /* Count non-dot characters in main_str to right-align cleanly */
    char glyphs[32];
    bool dots[32];
    memset(glyphs, ' ', sizeof(glyphs));
    memset(dots, 0, sizeof(dots));
    int gcount = 0;
    for (int i = 0; main_str[i] != '\0' && gcount < nslots; i++) {
        if (main_str[i] == '.' && gcount > 0) {
            dots[gcount - 1] = true;
        } else {
            glyphs[gcount++] = main_str[i];
        }
    }
    int pad = nslots - gcount;
    for (int s = 0; s < nslots; s++) {
        int gx = start_x + s * pitch;
        int idx = s - pad;
        char ch = (idx >= 0 && idx < gcount) ? glyphs[idx] : ' ';
        uint8_t m = seg7_mask_for_char(ch);
        draw_seg7_digit(dpy, g_vfd_pix, g_vfd_gc, gx, seg_y, dw, dh, m, vc.seg_lit, vc.seg_ghost);
        if (idx >= 0 && idx < gcount && dots[idx]) {
            XSetForeground(dpy, g_vfd_gc, vc.seg_lit);
            XFillRectangle(dpy, g_vfd_pix, g_vfd_gc, gx + dw + 1, seg_y + dh - 3, 3, 3);
        }
    }
    /* If error or alpha chars that 7-seg approximates, also overlay crisp label on left */
    if (g_st.error_flag && g_font_md) {
        XSetForeground(dpy, g_vfd_gc, vc.ann_amber);
        XDrawString(dpy, g_vfd_pix, g_vfd_gc, 12, seg_y + 20, g_st.error_msg, (int)strlen(g_st.error_msg));
    }

    /* 4. Interactive 64-Bit Programmer LED Bit-Toggle Matrix (2 rows x 32 bits: 63..32, 31..0) */
    uint64_t bits_val = double_to_u64(g_st.stack[0], g_st.word_bits);
    int bit_top_y = 102;
    int row_gap   = 28;
    int margin_x  = 12;
    int usable_w  = (int)ww - 2 * margin_x;
    /* 32 bits + 7 nibble gaps of 4 px */
    int cell_w = (usable_w - 7 * 4) / 32;
    if (cell_w < 8) cell_w = 8;
    int total_row_w = 32 * cell_w + 7 * 4;
    int base_x = ((int)ww - total_row_w) / 2;

    if (g_font_sm) XSetFont(dpy, g_vfd_gc, g_font_sm->fid);
    for (int r = 0; r < 2; r++) {
        int high_bit = (r == 0) ? 63 : 31;
        int ry = bit_top_y + r * row_gap;
        for (int c = 0; c < 32; c++) {
            int bit_idx = high_bit - c;
            int nib_gaps = c / 4;
            int bx = base_x + c * cell_w + nib_gaps * 4;
            int bw = cell_w - 2;
            int bh = 12;
            g_bit_rects[bit_idx].x = bx;
            g_bit_rects[bit_idx].y = ry;
            g_bit_rects[bit_idx].w = bw;
            g_bit_rects[bit_idx].h = bh + 10;

            bool enabled = (bit_idx < g_st.word_bits);
            bool is_one  = enabled && (((bits_val >> bit_idx) & 1ULL) != 0);

            XSetForeground(dpy, g_vfd_gc, is_one ? vc.bit_on : vc.bit_off);
            XFillRectangle(dpy, g_vfd_pix, g_vfd_gc, bx, ry, (unsigned)bw, (unsigned)bh);
            if (enabled) {
                XSetForeground(dpy, g_vfd_gc, is_one ? vc.seg_lit : vc.seg_ghost);
                XDrawRectangle(dpy, g_vfd_pix, g_vfd_gc, bx, ry, (unsigned)bw, (unsigned)bh);
            }

            if (bit_idx % 8 == 0 || bit_idx == 63 || bit_idx == 31) {
                char blbl[8];
                snprintf(blbl, sizeof(blbl), "%d", bit_idx);
                XSetForeground(dpy, g_vfd_gc, vc.bit_txt);
                XDrawString(dpy, g_vfd_pix, g_vfd_gc, bx, ry + bh + 11, blbl, (int)strlen(blbl));
            }
        }
    }

    XCopyArea(dpy, g_vfd_pix, win, g_vfd_gc, 0, 0, ww, hh, 0, 0);
}

/* ---------------- UI Refresh Helpers ---------------- */
static void set_btn_label(Widget w, const char *txt) {
    if (!w) return;
    XmString xs = XmStringCreateLocalized((char *)txt);
    XtVaSetValues(w, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}

static void sync_ui_controls(void) {
    for (int i = 0; i < 6; i++) {
        if (g_hex_btns[i]) XtSetSensitive(g_hex_btns[i], g_st.base == BASE_HEX);
    }
    set_btn_label(g_sci_btns[0], g_st.shift_2nd ? "ASIN" : "SIN");
    set_btn_label(g_sci_btns[1], g_st.shift_2nd ? "ACOS" : "COS");
    set_btn_label(g_sci_btns[2], g_st.shift_2nd ? "ATAN" : "TAN");
    set_btn_label(g_sci_btns[3], g_st.shift_2nd ? "e^x"  : "LN");
    set_btn_label(g_sci_btns[4], g_st.shift_2nd ? "10^x" : "LOG");
    set_btn_label(g_sci_btns[5], g_st.shift_2nd ? "x^2"  : "SQRT");

    set_btn_label(g_btn_rpn,  g_st.rpn_mode ? "MODE: RPN" : "MODE: ALG");
    set_btn_label(g_btn_deg,  g_st.deg_mode ? "DEG" : "RAD");
    char bbuf[16];
    snprintf(bbuf, sizeof(bbuf), "%d-BIT", g_st.word_bits);
    set_btn_label(g_btn_bits, bbuf);
    set_btn_label(g_btn_sign, g_st.signed_word ? "SIGNED" : "UNSIGN");
    const char *tnames[] = { "VFD CYAN", "LED RED", "AMBER", "LCD" };
    set_btn_label(g_btn_theme, tnames[(int)g_st.theme % 4]);

    render_vfd();
}

/* ---------------- Motif Callbacks ---------------- */
static void on_vfd_expose(Widget w, XtPointer client, XtPointer call) {
    render_vfd();
}

static void on_vfd_click(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    if (ev->type != ButtonPress || ev->xbutton.button != Button1) return;
    int mx = ev->xbutton.x;
    int my = ev->xbutton.y;
    for (int b = 0; b < g_st.word_bits; b++) {
        RectI r = g_bit_rects[b];
        if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h) {
            commit_entry_if_needed(&g_st);
            uint64_t u = double_to_u64(g_st.stack[0], g_st.word_bits);
            u ^= (1ULL << b);
            g_st.stack[0] = u64_to_double(u, g_st.word_bits, g_st.signed_word);
            g_st.stack_lift = true;
            render_vfd();
            return;
        }
    }
}

static void copy_x_to_clipboard(void) {
    if (!g_toplevel || !XtIsRealized(g_toplevel)) return;
    char buf[96];
    format_value(&g_st, g_st.stack[0], buf, sizeof(buf));
    Display *dpy = XtDisplay(g_toplevel);
    Window   win = XtWindow(g_toplevel);
    XmString label = XmStringCreateLocalized("dtCalc");
    long item_id = 0;
    if (XmClipboardStartCopy(dpy, win, label, CurrentTime, NULL, NULL, &item_id) == XmClipboardSuccess) {
        XmClipboardCopy(dpy, win, item_id, "STRING", buf, (unsigned long)strlen(buf), 0, NULL);
        XmClipboardEndCopy(dpy, win, item_id);
    }
    XmStringFree(label);
}

static void paste_from_clipboard(void) {
    if (!g_toplevel || !XtIsRealized(g_toplevel)) return;
    Display *dpy = XtDisplay(g_toplevel);
    Window   win = XtWindow(g_toplevel);
    char buf[128];
    unsigned long out_len = 0;
    memset(buf, 0, sizeof(buf));
    if (XmClipboardRetrieve(dpy, win, "STRING", buf, sizeof(buf) - 1, &out_len, NULL) == XmClipboardSuccess && out_len > 0) {
        buf[out_len] = '\0';
        if (g_st.rpn_mode && g_st.stack_lift) rpn_push(&g_st, g_st.stack[0]);
        snprintf(g_st.entry_buf, sizeof(g_st.entry_buf), "%s", buf);
        g_st.stack[0] = parse_entry_buf(&g_st);
        g_st.entering = false;
        g_st.entry_buf[0] = '\0';
        g_st.stack_lift = true;
        render_vfd();
    }
}

static void dispatch_cmd(const char *cmd) {
    if (!cmd) return;
    if (strncmp(cmd, "D:", 2) == 0) {
        calc_digit(&g_st, cmd[2]);
    } else if (strcmp(cmd, "BKSP") == 0) {
        calc_backspace(&g_st);
    } else if (strcmp(cmd, "CHS") == 0) {
        calc_negate(&g_st);
    } else if (strcmp(cmd, "CE") == 0) {
        g_st.entering = false;
        g_st.entry_buf[0] = '\0';
        g_st.stack[0] = 0.0;
        g_st.error_flag = false;
    } else if (strcmp(cmd, "AC") == 0) {
        DisplayTheme th = g_st.theme;
        bool rpn = g_st.rpn_mode;
        bool deg = g_st.deg_mode;
        NumBase b = g_st.base;
        int wb = g_st.word_bits;
        calc_init(&g_st);
        g_st.theme = th; g_st.rpn_mode = rpn; g_st.deg_mode = deg;
        g_st.base = b; g_st.word_bits = wb;
        tape_append("--- ALL CLEAR ---");
    } else if (strcmp(cmd, "ENTER") == 0) {
        calc_enter_or_equals(&g_st);
    } else if (strcmp(cmd, "ADD") == 0) calc_binary_op(&g_st, '+', "+");
    else if (strcmp(cmd, "SUB") == 0)   calc_binary_op(&g_st, '-', "-");
    else if (strcmp(cmd, "MUL") == 0)   calc_binary_op(&g_st, '*', "*");
    else if (strcmp(cmd, "DIV") == 0)   calc_binary_op(&g_st, '/', "/");
    else if (strcmp(cmd, "MOD") == 0)   calc_binary_op(&g_st, '%', "MOD");
    else if (strcmp(cmd, "POW") == 0)   calc_binary_op(&g_st, 'P', "^");
    else if (strcmp(cmd, "AND") == 0)   calc_binary_op(&g_st, '&', "AND");
    else if (strcmp(cmd, "OR") == 0)    calc_binary_op(&g_st, '|', "OR");
    else if (strcmp(cmd, "XOR") == 0)   calc_binary_op(&g_st, '^', "XOR");
    else if (strcmp(cmd, "SHL") == 0)   calc_binary_op(&g_st, '<', "<<");
    else if (strcmp(cmd, "SHR") == 0)   calc_binary_op(&g_st, '>', ">>");
    else if (strcmp(cmd, "LP") == 0)    calc_paren(&g_st, '(');
    else if (strcmp(cmd, "RP") == 0)    calc_paren(&g_st, ')');
    else if (strcmp(cmd, "SWAP") == 0) {
        commit_entry_if_needed(&g_st);
        double t = g_st.stack[0];
        g_st.stack[0] = g_st.stack[1];
        g_st.stack[1] = t;
        g_st.stack_lift = true;
    } else if (strcmp(cmd, "RDN") == 0) {
        commit_entry_if_needed(&g_st);
        double x = g_st.stack[0];
        g_st.stack[0] = g_st.stack[1];
        g_st.stack[1] = g_st.stack[2];
        g_st.stack[2] = g_st.stack[3];
        g_st.stack[3] = x;
        g_st.stack_lift = true;
    } else if (strcmp(cmd, "DROP") == 0) {
        commit_entry_if_needed(&g_st);
        g_st.stack[0] = g_st.stack[1];
        g_st.stack[1] = g_st.stack[2];
        g_st.stack[2] = g_st.stack[3];
        g_st.stack_lift = true;
    } else if (strcmp(cmd, "LASTX") == 0) {
        commit_entry_if_needed(&g_st);
        if (g_st.rpn_mode && g_st.stack_lift) rpn_push(&g_st, g_st.stack[0]);
        g_st.stack[0] = g_st.last_x;
        g_st.stack_lift = true;
    } else if (strcmp(cmd, "STO") == 0) {
        commit_entry_if_needed(&g_st);
        g_st.mem_val = g_st.stack[0];
        g_st.has_mem = true;
    } else if (strcmp(cmd, "RCL") == 0) {
        commit_entry_if_needed(&g_st);
        if (g_st.rpn_mode && g_st.stack_lift) rpn_push(&g_st, g_st.stack[0]);
        g_st.stack[0] = g_st.mem_val;
        g_st.stack_lift = true;
    } else if (strcmp(cmd, "2ND") == 0) {
        g_st.shift_2nd = !g_st.shift_2nd;
    } else if (strcmp(cmd, "MODE") == 0) {
        commit_entry_if_needed(&g_st);
        g_st.rpn_mode = !g_st.rpn_mode;
        g_st.alg_vtop = g_st.alg_otop = 0;
        g_st.alg_expr[0] = '\0';
    } else if (strcmp(cmd, "DEG") == 0) {
        g_st.deg_mode = !g_st.deg_mode;
    } else if (strcmp(cmd, "BASE_HEX") == 0) { commit_entry_if_needed(&g_st); g_st.base = BASE_HEX; }
    else if (strcmp(cmd, "BASE_DEC") == 0)   { commit_entry_if_needed(&g_st); g_st.base = BASE_DEC; }
    else if (strcmp(cmd, "BASE_OCT") == 0)   { commit_entry_if_needed(&g_st); g_st.base = BASE_OCT; }
    else if (strcmp(cmd, "BASE_BIN") == 0)   { commit_entry_if_needed(&g_st); g_st.base = BASE_BIN; }
    else if (strcmp(cmd, "BITS") == 0) {
        commit_entry_if_needed(&g_st);
        g_st.word_bits = (g_st.word_bits == 64) ? 32 :
                         (g_st.word_bits == 32) ? 16 :
                         (g_st.word_bits == 16) ? 8  : 64;
        uint64_t u = double_to_u64(g_st.stack[0], g_st.word_bits);
        g_st.stack[0] = u64_to_double(u, g_st.word_bits, g_st.signed_word);
    } else if (strcmp(cmd, "SIGN") == 0) {
        commit_entry_if_needed(&g_st);
        uint64_t u = double_to_u64(g_st.stack[0], g_st.word_bits);
        g_st.signed_word = !g_st.signed_word;
        g_st.stack[0] = u64_to_double(u, g_st.word_bits, g_st.signed_word);
    } else if (strcmp(cmd, "THEME") == 0) {
        g_st.theme = (DisplayTheme)(((int)g_st.theme + 1) % 4);
    } else if (strcmp(cmd, "TAPE") == 0) {
        g_tape_shown = !g_tape_shown;
        if (g_tape_frame) {
            if (g_tape_shown) XtManageChild(g_tape_frame);
            else              XtUnmanageChild(g_tape_frame);
        }
    } else if (strcmp(cmd, "COPY") == 0) {
        commit_entry_if_needed(&g_st);
        copy_x_to_clipboard();
    } else if (strcmp(cmd, "PASTE") == 0) {
        paste_from_clipboard();
    } else if (strcmp(cmd, "CLRTAPE") == 0) {
        if (g_tape_text) XmTextSetString(g_tape_text, "");
    } else {
        calc_unary_or_rf(&g_st, cmd);
    }
    sync_ui_controls();
}

static void on_btn_cmd(Widget w, XtPointer client, XtPointer call) {
    dispatch_cmd((const char *)client);
}

static void on_key_press(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    if (ev->type != KeyPress) return;
    KeySym ks = NoSymbol;
    char str[16] = "";
    XLookupString(&ev->xkey, str, sizeof(str) - 1, &ks, NULL);

    if ((ev->xkey.state & ControlMask) && (ks == XK_c || ks == XK_C)) {
        dispatch_cmd("COPY");
        *cont = False;
        return;
    }
    if ((ev->xkey.state & ControlMask) && (ks == XK_v || ks == XK_V)) {
        dispatch_cmd("PASTE");
        *cont = False;
        return;
    }
    if (ks == XK_Return || ks == XK_KP_Enter || str[0] == '=') {
        dispatch_cmd("ENTER"); *cont = False; return;
    }
    if (ks == XK_BackSpace) { dispatch_cmd("BKSP"); *cont = False; return; }
    if (ks == XK_Escape)    { dispatch_cmd("CE");   *cont = False; return; }
    if (ks == XK_Delete)    { dispatch_cmd("AC");   *cont = False; return; }

    char ch = str[0];
    if ((ch >= '0' && ch <= '9') || ch == '.' ||
        (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
        char dcmd[4] = { 'D', ':', (char)toupper((unsigned char)ch), '\0' };
        dispatch_cmd(dcmd);
        *cont = False;
        return;
    }
    if (ch == '+') { dispatch_cmd("ADD"); *cont = False; return; }
    if (ch == '-') { dispatch_cmd("SUB"); *cont = False; return; }
    if (ch == '*') { dispatch_cmd("MUL"); *cont = False; return; }
    if (ch == '/') { dispatch_cmd("DIV"); *cont = False; return; }
    if (ch == '^') { dispatch_cmd("POW"); *cont = False; return; }
    if (ch == '%') { dispatch_cmd("MOD"); *cont = False; return; }
    if (ch == '(') { dispatch_cmd("LP");  *cont = False; return; }
    if (ch == ')') { dispatch_cmd("RP");  *cont = False; return; }
    if (ch == '~') { dispatch_cmd("NOT"); *cont = False; return; }
    if (ch == '&') { dispatch_cmd("AND"); *cont = False; return; }
    if (ch == '|') { dispatch_cmd("OR");  *cont = False; return; }
}

/* ---------------- Motif Keycap Styling ---------------- */
static void style_keycap(Widget btn, KeyStyle ks) {
    Display *dpy  = XtDisplay(btn);
    Screen  *scr  = XtScreen(btn);
    Colormap cmap = DefaultColormapOfScreen(scr);
    const char *bg_hex = "#525A6B";
    const char *fg_hex = "#F3F6FB";

    switch (ks) {
        case KEY_DIGIT: bg_hex = "#687285"; fg_hex = "#FFFFFF"; break;
        case KEY_HEX:   bg_hex = "#4A5C78"; fg_hex = "#E2EEFF"; break;
        case KEY_OP:    bg_hex = "#434C5E"; fg_hex = "#F4F7FC"; break;
        case KEY_SCI:   bg_hex = "#2F5B70"; fg_hex = "#E6FAFF"; break;
        case KEY_RF:    bg_hex = "#2B6056"; fg_hex = "#9EFFDF"; break;
        case KEY_BIT:   bg_hex = "#3B4150"; fg_hex = "#DCE3F0"; break;
        case KEY_SHIFT: bg_hex = "#B88420"; fg_hex = "#FFFBEA"; break;
        case KEY_ENTER: bg_hex = "#2C6A9E"; fg_hex = "#FFFFFF"; break;
        case KEY_CLEAR: bg_hex = "#C85624"; fg_hex = "#FFFFFF"; break;
    }

    Pixel bg = alloc_hex(dpy, cmap, bg_hex, BlackPixelOfScreen(scr));
    Pixel fg_c = alloc_hex(dpy, cmap, fg_hex, WhitePixelOfScreen(scr));
    Pixel fg_auto, top_sh, bot_sh, sel_px;
    XmGetColors(scr, cmap, bg, &fg_auto, &top_sh, &bot_sh, &sel_px);
    XtVaSetValues(btn,
                  XmNbackground,        bg,
                  XmNforeground,        fg_c,
                  XmNtopShadowColor,    top_sh,
                  XmNbottomShadowColor, bot_sh,
                  XmNarmColor,          sel_px,
                  XmNshadowThickness,   2,
                  XmNmarginHeight,      4,
                  XmNmarginWidth,       4,
                  NULL);
}

static Widget make_key(Widget parent, const char *label, const char *cmd,
                       KeyStyle ks, int row, int col, int colspan, int total_cols) {
    XmString xs = XmStringCreateLocalized((char *)label);
    int lp = (col * 100) / total_cols;
    int rp = ((col + colspan) * 100) / total_cols;
    Widget b = XtVaCreateManagedWidget(
        "calcKey", xmPushButtonWidgetClass, parent,
        XmNlabelString,      xs,
        XmNtopAttachment,    XmATTACH_POSITION,
        XmNtopPosition,      row * 10,
        XmNbottomAttachment, XmATTACH_POSITION,
        XmNbottomPosition,   (row + 1) * 10,
        XmNleftAttachment,   XmATTACH_POSITION,
        XmNleftPosition,     lp,
        XmNrightAttachment,  XmATTACH_POSITION,
        XmNrightPosition,    rp,
        XmNtopOffset,        2,
        XmNbottomOffset,     2,
        XmNleftOffset,       2,
        XmNrightOffset,      2,
        XmNrecomputeSize,    False,
        XmNtraversalOn,      False,
        NULL);
    XmStringFree(xs);
    style_keycap(b, ks);
    XtAddCallback(b, XmNactivateCallback, on_btn_cmd, (XtPointer)cmd);
    return b;
}

/* ---------------- Self-Test Suite ---------------- */
static int run_self_test(void) {
    CalcState s;
    calc_init(&s);

    /* 1. RPN test: (14.074 ENTER 2 * -> 28.148) */
    s.rpn_mode = true;
    const char *seq = "14.074";
    for (int i = 0; seq[i]; i++) calc_digit(&s, seq[i]);
    calc_enter_or_equals(&s);
    calc_digit(&s, '2');
    calc_binary_op(&s, '*', "*");
    if (fabs(s.stack[0] - 28.148) > 1e-6) {
        fprintf(stderr, "RPN test failed: %.9f\n", s.stack[0]);
        return 1;
    }

    /* 2. RF Wavelength test: 14.074 MHz -> ~21.30117 m */
    calc_init(&s);
    for (int i = 0; seq[i]; i++) calc_digit(&s, seq[i]);
    calc_unary_or_rf(&s, "FLAM");
    if (fabs(s.stack[0] - (299.792458 / 14.074)) > 1e-5) {
        fprintf(stderr, "RF FLAM test failed: %.9f\n", s.stack[0]);
        return 1;
    }

    /* 3. RF dBm -> Watts: 30 dBm -> 1.0 W, then W -> dBm -> 30.0 dBm */
    calc_init(&s);
    calc_digit(&s, '3'); calc_digit(&s, '0');
    calc_unary_or_rf(&s, "DBMW");
    if (fabs(s.stack[0] - 1.0) > 1e-9) {
        fprintf(stderr, "RF DBMW test failed: %.9f\n", s.stack[0]);
        return 1;
    }
    calc_unary_or_rf(&s, "WDBM");
    if (fabs(s.stack[0] - 30.0) > 1e-9) {
        fprintf(stderr, "RF WDBM test failed: %.9f\n", s.stack[0]);
        return 1;
    }

    /* 4. LC Resonance: 10 uH and 100 pF -> ~5.032921 MHz */
    calc_init(&s);
    calc_digit(&s, '1'); calc_digit(&s, '0');
    calc_enter_or_equals(&s);
    calc_digit(&s, '1'); calc_digit(&s, '0'); calc_digit(&s, '0');
    calc_unary_or_rf(&s, "LC");
    if (fabs(s.stack[0] - 5.0329212) > 1e-4) {
        fprintf(stderr, "RF LC test failed: %.9f\n", s.stack[0]);
        return 1;
    }

    /* 5. Algebraic precedence: 2 + 3 * 4 = 14 */
    calc_init(&s);
    s.rpn_mode = false;
    calc_digit(&s, '2');
    calc_binary_op(&s, '+', "+");
    calc_digit(&s, '3');
    calc_binary_op(&s, '*', "*");
    calc_digit(&s, '4');
    calc_enter_or_equals(&s);
    if (fabs(s.stack[0] - 14.0) > 1e-9) {
        fprintf(stderr, "ALG precedence test failed: %.9f\n", s.stack[0]);
        return 1;
    }

    /* 6. 16-bit HEX bitwise BSWAP: 0x1234 -> 0x3412 */
    calc_init(&s);
    s.base = BASE_HEX;
    s.word_bits = 16;
    s.signed_word = false;
    calc_digit(&s, '1'); calc_digit(&s, '2'); calc_digit(&s, '3'); calc_digit(&s, '4');
    calc_unary_or_rf(&s, "BSWP");
    if ((uint64_t)llround(s.stack[0]) != 0x3412ULL) {
        fprintf(stderr, "HEX BSWP test failed: 0x%" PRIX64 "\n", (uint64_t)llround(s.stack[0]));
        return 1;
    }

    printf("DTCALC_SELF_TEST_OK\n");
    return 0;
}

/* ---------------- Main Application Entry ---------------- */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        }
    }

    calc_init(&g_st);
    XtSetLanguageProc(NULL, NULL, NULL);

    String fallbacks[] = {
        "*fontList: -misc-fixed-bold-r-normal--13-*-*-*-*-*-iso8859-1",
        "*calcTape*fontList: -misc-fixed-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        NULL
    };

    g_toplevel = XtVaAppInitialize(
        &g_app, "DtCalc", NULL, 0, &argc, argv, fallbacks,
        XmNtitle,    "dtCalc — Workstation RPN / Scientific / RF / 64-Bit Calculator",
        XmNiconName, "dtCalc",
        XmNwidth,    880,
        XmNheight,   610,
        NULL);

    /* Set XPM window icon */
    Pixmap icon_pm = None, icon_mask = None;
    XpmAttributes xattr;
    memset(&xattr, 0, sizeof(xattr));
    if (XpmCreatePixmapFromData(XtDisplay(g_toplevel),
                                DefaultRootWindow(XtDisplay(g_toplevel)),
                                dtcalc_pm, &icon_pm, &icon_mask, &xattr) == XpmSuccess) {
        XtVaSetValues(g_toplevel, XmNiconPixmap, icon_pm, XmNiconMask, icon_mask, NULL);
    }

    Display *dpy  = XtDisplay(g_toplevel);
    Screen  *scr  = XtScreen(g_toplevel);
    Colormap cmap = DefaultColormapOfScreen(scr);
    Pixel chassis_bg = alloc_hex(dpy, cmap, "#353B47", BlackPixelOfScreen(scr));
    Pixel cream_well = alloc_hex(dpy, cmap, "#FFF7E9", WhitePixelOfScreen(scr));
    Pixel dark_ink   = alloc_hex(dpy, cmap, "#181C24", BlackPixelOfScreen(scr));

    Widget outer_form = XtVaCreateManagedWidget(
        "outerForm", xmFormWidgetClass, g_toplevel,
        XmNbackground, chassis_bg,
        NULL);

    /* Right-side Toggleable Vintage Paper-Tape Roll */
    g_tape_frame = XtVaCreateManagedWidget(
        "tapeFrame", xmFrameWidgetClass, outer_form,
        XmNshadowType,       XmSHADOW_IN,
        XmNshadowThickness,  2,
        XmNtopAttachment,    XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_FORM,
        XmNtopOffset,        6,
        XmNbottomOffset,     6,
        XmNrightOffset,      6,
        XmNwidth,            255,
        XmNbackground,       chassis_bg,
        NULL);

    Widget tape_form = XtVaCreateManagedWidget(
        "tapeForm", xmFormWidgetClass, g_tape_frame,
        XmNbackground, chassis_bg,
        NULL);

    Widget tape_hdr = XtVaCreateManagedWidget(
        "PAPER TAPE / RF LOG", xmLabelWidgetClass, tape_form,
        XmNtopAttachment,   XmATTACH_FORM,
        XmNleftAttachment,  XmATTACH_FORM,
        XmNtopOffset,       4,
        XmNleftOffset,      6,
        XmNbackground,      chassis_bg,
        XmNforeground,      alloc_hex(dpy, cmap, "#D8E0EE", WhitePixelOfScreen(scr)),
        NULL);

    Widget btn_clr_tape = XtVaCreateManagedWidget(
        "CLR", xmPushButtonWidgetClass, tape_form,
        XmNtopAttachment,   XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopOffset,       2,
        XmNrightOffset,     4,
        XmNtraversalOn,     False,
        NULL);
    style_keycap(btn_clr_tape, KEY_BIT);
    XtAddCallback(btn_clr_tape, XmNactivateCallback, on_btn_cmd, (XtPointer)"CLRTAPE");

    Widget btn_copy_x = XtVaCreateManagedWidget(
        "COPY X", xmPushButtonWidgetClass, tape_form,
        XmNtopAttachment,   XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget,     btn_clr_tape,
        XmNtopOffset,       2,
        XmNrightOffset,     4,
        XmNtraversalOn,     False,
        NULL);
    style_keycap(btn_copy_x, KEY_SCI);
    XtAddCallback(btn_copy_x, XmNactivateCallback, on_btn_cmd, (XtPointer)"COPY");

    Arg targs[12];
    int tn = 0;
    XtSetArg(targs[tn], XmNeditable,         False); tn++;
    XtSetArg(targs[tn], XmNeditMode,         XmMULTI_LINE_EDIT); tn++;
    XtSetArg(targs[tn], XmNcursorPositionVisible, False); tn++;
    XtSetArg(targs[tn], XmNbackground,       cream_well); tn++;
    XtSetArg(targs[tn], XmNforeground,       dark_ink); tn++;
    g_tape_text = XmCreateScrolledText(tape_form, "calcTape", targs, tn);
    XtVaSetValues(XtParent(g_tape_text),
                  XmNtopAttachment,    XmATTACH_WIDGET,
                  XmNtopWidget,        btn_clr_tape,
                  XmNtopOffset,        4,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment,   XmATTACH_FORM,
                  XmNrightAttachment,  XmATTACH_FORM,
                  XmNbottomOffset,     4,
                  XmNleftOffset,       4,
                  XmNrightOffset,      4,
                  NULL);
    XtManageChild(g_tape_text);
    (void)tape_hdr;

    /* Left Main Calculator Deck */
    Widget calc_form = XtVaCreateManagedWidget(
        "calcForm", xmFormWidgetClass, outer_form,
        XmNtopAttachment,    XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_WIDGET,
        XmNrightWidget,      g_tape_frame,
        XmNtopOffset,        6,
        XmNbottomOffset,     6,
        XmNleftOffset,       6,
        XmNrightOffset,      6,
        XmNbackground,       chassis_bg,
        NULL);

    /* Top VFD + 64-Bit LED Bit-Switch Bezel */
    Widget vfd_frame = XtVaCreateManagedWidget(
        "vfdFrame", xmFrameWidgetClass, calc_form,
        XmNshadowType,       XmSHADOW_IN,
        XmNshadowThickness,  3,
        XmNtopAttachment,    XmATTACH_FORM,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_FORM,
        XmNbackground,       chassis_bg,
        NULL);

    g_vfd_area = XtVaCreateManagedWidget(
        "vfdArea", xmDrawingAreaWidgetClass, vfd_frame,
        XmNwidth,      590,
        XmNheight,     158,
        XmNbackground, BlackPixelOfScreen(scr),
        NULL);
    XtAddCallback(g_vfd_area, XmNexposeCallback, on_vfd_expose, NULL);
    XtAddCallback(g_vfd_area, XmNresizeCallback, on_vfd_expose, NULL);
    XtAddEventHandler(g_vfd_area, ButtonPressMask, False, on_vfd_click, NULL);

    /* Mode Selector Bar */
    Widget mode_grid = XtVaCreateManagedWidget(
        "modeGrid", xmFormWidgetClass, calc_form,
        XmNfractionBase,     100,
        XmNtopAttachment,    XmATTACH_WIDGET,
        XmNtopWidget,        vfd_frame,
        XmNtopOffset,        4,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_FORM,
        XmNheight,           32,
        XmNbackground,       chassis_bg,
        NULL);

    g_btn_rpn     = make_key(mode_grid, "MODE: RPN", "MODE",     KEY_SHIFT, 0, 0, 2, 10);
    g_btn_deg     = make_key(mode_grid, "DEG",       "DEG",      KEY_SCI,   0, 2, 1, 10);
    g_btn_base[0] = make_key(mode_grid, "HEX",       "BASE_HEX", KEY_BIT,   0, 3, 1, 10);
    g_btn_base[1] = make_key(mode_grid, "DEC",       "BASE_DEC", KEY_BIT,   0, 4, 1, 10);
    g_btn_base[2] = make_key(mode_grid, "OCT",       "BASE_OCT", KEY_BIT,   0, 5, 1, 10);
    g_btn_base[3] = make_key(mode_grid, "BIN",       "BASE_BIN", KEY_BIT,   0, 6, 1, 10);
    g_btn_bits    = make_key(mode_grid, "64-BIT",    "BITS",     KEY_BIT,   0, 7, 1, 10);
    g_btn_sign    = make_key(mode_grid, "SIGNED",    "SIGN",     KEY_BIT,   0, 8, 1, 10);
    g_btn_theme   = make_key(mode_grid, "VFD CYAN",  "THEME",    KEY_RF,    0, 9, 1, 10);
    Widget mode_kids[] = {
        g_btn_rpn, g_btn_deg, g_btn_base[0], g_btn_base[1], g_btn_base[2],
        g_btn_base[3], g_btn_bits, g_btn_sign, g_btn_theme
    };
    for (size_t i = 0; i < sizeof(mode_kids) / sizeof(mode_kids[0]); i++) {
        XtVaSetValues(mode_kids[i], XmNtopPosition, 0, XmNbottomPosition, 100, XmNheight, 28, NULL);
    }

    /* Main 8-Column x 8-Row Tactile Keypad Matrix */
    Widget key_grid = XtVaCreateManagedWidget(
        "keyGrid", xmFormWidgetClass, calc_form,
        XmNfractionBase,     80,
        XmNtopAttachment,    XmATTACH_WIDGET,
        XmNtopWidget,        mode_grid,
        XmNtopOffset,        4,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_FORM,
        XmNbackground,       chassis_bg,
        NULL);

    /* Helper macro using fractionBase=80 (8 rows x 10 = 80, cols scaled to 80) */
    #define K(lbl, cmd, sty, r, c, cs) do { \
        Widget _b = make_key(key_grid, (lbl), (cmd), (sty), (r), (c), (cs), 8); \
        XtVaSetValues(_b, \
            XmNleftPosition, ((c) * 80) / 8, \
            XmNrightPosition, (((c) + (cs)) * 80) / 8, \
            NULL); \
    } while (0)

    /* Row 0: RF Bench & Tape Controls */
    make_key(key_grid, "2nd",    "2ND",   KEY_SHIFT, 0, 0, 1, 8);
    make_key(key_grid, "f<->WL", "FLAM",  KEY_RF,    0, 1, 1, 8);
    make_key(key_grid, "dBm->W", "DBMW",  KEY_RF,    0, 2, 1, 8);
    make_key(key_grid, "W->dBm", "WDBM",  KEY_RF,    0, 3, 1, 8);
    make_key(key_grid, "LC MHz", "LC",    KEY_RF,    0, 4, 1, 8);
    make_key(key_grid, "dB V",   "DBV",   KEY_RF,    0, 5, 1, 8);
    g_btn_tape = make_key(key_grid, "TAPE", "TAPE", KEY_OP, 0, 6, 1, 8);
    make_key(key_grid, "AC",     "AC",    KEY_CLEAR, 0, 7, 1, 8);

    /* Row 1: Scientific & Clear */
    g_sci_btns[0] = make_key(key_grid, "SIN",  "SIN",  KEY_SCI, 1, 0, 1, 8);
    g_sci_btns[1] = make_key(key_grid, "COS",  "COS",  KEY_SCI, 1, 1, 1, 8);
    g_sci_btns[2] = make_key(key_grid, "TAN",  "TAN",  KEY_SCI, 1, 2, 1, 8);
    g_sci_btns[3] = make_key(key_grid, "LN",   "LN",   KEY_SCI, 1, 3, 1, 8);
    g_sci_btns[4] = make_key(key_grid, "LOG",  "LOG",  KEY_SCI, 1, 4, 1, 8);
    g_sci_btns[5] = make_key(key_grid, "SQRT", "SQRT", KEY_SCI, 1, 5, 1, 8);
    make_key(key_grid, "CE",   "CE",   KEY_CLEAR, 1, 6, 1, 8);
    make_key(key_grid, "BKSP", "BKSP", KEY_CLEAR, 1, 7, 1, 8);

    /* Row 2: Powers, Stack & Memory */
    make_key(key_grid, "x^y",   "POW",   KEY_SCI, 2, 0, 1, 8);
    make_key(key_grid, "1/x",   "INV",   KEY_SCI, 2, 1, 1, 8);
    make_key(key_grid, "n!",    "FACT",  KEY_SCI, 2, 2, 1, 8);
    make_key(key_grid, "PI",    "PI",    KEY_SCI, 2, 3, 1, 8);
    make_key(key_grid, "x<->y", "SWAP",  KEY_OP,  2, 4, 1, 8);
    make_key(key_grid, "R dn",  "RDN",   KEY_OP,  2, 5, 1, 8);
    make_key(key_grid, "DROP",  "DROP",  KEY_OP,  2, 6, 1, 8);
    make_key(key_grid, "LASTx", "LASTX", KEY_OP,  2, 7, 1, 8);

    /* Row 3: Bitwise & Hex E-F + 7 8 9 / */
    make_key(key_grid, "AND", "AND", KEY_BIT, 3, 0, 1, 8);
    make_key(key_grid, "OR",  "OR",  KEY_BIT, 3, 1, 1, 8);
    g_hex_btns[4] = make_key(key_grid, "E", "D:E", KEY_HEX, 3, 2, 1, 8);
    g_hex_btns[5] = make_key(key_grid, "F", "D:F", KEY_HEX, 3, 3, 1, 8);
    make_key(key_grid, "7", "D:7", KEY_DIGIT, 3, 4, 1, 8);
    make_key(key_grid, "8", "D:8", KEY_DIGIT, 3, 5, 1, 8);
    make_key(key_grid, "9", "D:9", KEY_DIGIT, 3, 6, 1, 8);
    make_key(key_grid, "/", "DIV", KEY_OP,    3, 7, 1, 8);

    /* Row 4: XOR, NOT + Hex C-D + 4 5 6 * */
    make_key(key_grid, "XOR", "XOR", KEY_BIT, 4, 0, 1, 8);
    make_key(key_grid, "NOT", "NOT", KEY_BIT, 4, 1, 1, 8);
    g_hex_btns[2] = make_key(key_grid, "C", "D:C", KEY_HEX, 4, 2, 1, 8);
    g_hex_btns[3] = make_key(key_grid, "D", "D:D", KEY_HEX, 4, 3, 1, 8);
    make_key(key_grid, "4", "D:4", KEY_DIGIT, 4, 4, 1, 8);
    make_key(key_grid, "5", "D:5", KEY_DIGIT, 4, 5, 1, 8);
    make_key(key_grid, "6", "D:6", KEY_DIGIT, 4, 6, 1, 8);
    make_key(key_grid, "*", "MUL", KEY_OP,    4, 7, 1, 8);

    /* Row 5: SHL, SHR + Hex A-B + 1 2 3 - */
    make_key(key_grid, "<<", "SHL", KEY_BIT, 5, 0, 1, 8);
    make_key(key_grid, ">>", "SHR", KEY_BIT, 5, 1, 1, 8);
    g_hex_btns[0] = make_key(key_grid, "A", "D:A", KEY_HEX, 5, 2, 1, 8);
    g_hex_btns[1] = make_key(key_grid, "B", "D:B", KEY_HEX, 5, 3, 1, 8);
    make_key(key_grid, "1", "D:1", KEY_DIGIT, 5, 4, 1, 8);
    make_key(key_grid, "2", "D:2", KEY_DIGIT, 5, 5, 1, 8);
    make_key(key_grid, "3", "D:3", KEY_DIGIT, 5, 6, 1, 8);
    make_key(key_grid, "-", "SUB", KEY_OP,    5, 7, 1, 8);

    /* Row 6: ROL, ROR, STO, RCL + 0 . +/- + */
    make_key(key_grid, "ROL", "ROL", KEY_BIT,   6, 0, 1, 8);
    make_key(key_grid, "ROR", "ROR", KEY_BIT,   6, 1, 1, 8);
    make_key(key_grid, "STO", "STO", KEY_OP,    6, 2, 1, 8);
    make_key(key_grid, "RCL", "RCL", KEY_OP,    6, 3, 1, 8);
    make_key(key_grid, "0",   "D:0", KEY_DIGIT, 6, 4, 1, 8);
    make_key(key_grid, ".",   "D:.", KEY_DIGIT, 6, 5, 1, 8);
    make_key(key_grid, "+/-", "CHS", KEY_DIGIT, 6, 6, 1, 8);
    make_key(key_grid, "+",   "ADD", KEY_OP,    6, 7, 1, 8);

    /* Row 7: BSWAP, (, ), MOD, EE + wide ENTER / = */
    make_key(key_grid, "BSWP", "BSWP",  KEY_BIT,   7, 0, 1, 8);
    make_key(key_grid, "(",    "LP",    KEY_OP,    7, 1, 1, 8);
    make_key(key_grid, ")",    "RP",    KEY_OP,    7, 2, 1, 8);
    make_key(key_grid, "MOD",  "MOD",   KEY_OP,    7, 3, 1, 8);
    make_key(key_grid, "EE",   "D:E",   KEY_SCI,   7, 4, 1, 8);
    make_key(key_grid, "PASTE","PASTE", KEY_OP,    7, 5, 1, 8);
    make_key(key_grid, "ENTER / =", "ENTER", KEY_ENTER, 7, 6, 2, 8);

    /* Fix horizontal positions to fractionBase=80 across key_grid */
    Widget *kids = NULL;
    Cardinal nkids = 0;
    XtVaGetValues(key_grid, XmNchildren, &kids, XmNnumChildren, &nkids, NULL);
    for (Cardinal i = 0; i < nkids; i++) {
        int lp = 0, rp = 0;
        XtVaGetValues(kids[i], XmNleftPosition, &lp, XmNrightPosition, &rp, NULL);
        XtVaSetValues(kids[i],
                      XmNleftPosition,  (lp * 80) / 100,
                      XmNrightPosition, (rp * 80) / 100,
                      NULL);
    }

    XtAddEventHandler(g_toplevel, KeyPressMask, True, on_key_press, NULL);
    XtAddEventHandler(outer_form, KeyPressMask, True, on_key_press, NULL);
    XtAddEventHandler(g_vfd_area, KeyPressMask, True, on_key_press, NULL);

    XtRealizeWidget(g_toplevel);
    tape_append("dtCalc Ready (HP RPN / ALG / RF / 64-Bit)");
    tape_append("Click any LED bit [63..0] to toggle.");
    sync_ui_controls();

    XtAppMainLoop(g_app);
    return 0;
}
