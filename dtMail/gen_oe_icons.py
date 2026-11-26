#!/usr/bin/env python3
"""
Converts the exact user-provided Outlook Express logo (dist/outlook_express_logo.png)
into transparent multi-size CDE XPM icons (~/.dt/icons/outlook_express.{pm,l.pm,m.pm,s.pm,t.pm})
and generates cde_oe_icons.h with the embedded 32x32 logo + classic OE toolbar icons.
"""
from collections import deque
import os
from PIL import Image

# Printable single-byte XPM characters (excluding '"' and '\\')
XPM_CHARS = (
    "@#$%&*+=-;:,.<>1234567890"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!?/~^|()[]{}"
)

TOOLBAR_PALETTE = {
    '.': None,
    'k': '#101828',
    'n': '#0B2E6B',
    'b': '#185ABD',
    'c': '#2B88D8',
    'a': '#5CC8FF',
    'w': '#FFFFFF',
    'e': '#FFF8EC',
    's': '#D6D9E2',
    'm': '#9DA5B8',
    'g': '#16A34A',
    'l': '#4ADE80',
    'r': '#DC2626',
    'p': '#FCA5A5',
    'y': '#F59E0B',
    'u': '#FDE68A',
    't': '#64748B',
}


def knockout_exterior_white(img_rgba):
    """Flood-fills exterior white background from image borders to transparent,
    keeping the white envelope interior 100% opaque."""
    im = img_rgba.convert("RGBA")
    w, h = im.size
    px = im.load()
    visited = [[False] * w for _ in range(h)]
    q = deque()

    def is_bg(r, g, b):
        # Exterior white/near-white background without strong blue saturation
        return r >= 236 and g >= 236 and b >= 236 and (max(r, g, b) - min(r, g, b)) <= 18

    for x in range(w):
        for y in (0, h - 1):
            r, g, b, _ = px[x, y]
            if is_bg(r, g, b):
                visited[y][x] = True
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if not visited[y][x]:
                r, g, b, _ = px[x, y]
                if is_bg(r, g, b):
                    visited[y][x] = True
                    q.append((x, y))

    while q:
        cx, cy = q.popleft()
        px[cx, cy] = (0, 0, 0, 0)
        for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
            if 0 <= nx < w and 0 <= ny < h and not visited[ny][nx]:
                r, g, b, _ = px[nx, ny]
                if is_bg(r, g, b):
                    visited[ny][nx] = True
                    q.append((nx, ny))

    # Clean 1px white anti-aliased fringe touching transparent background
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            touches_trans = any(
                0 <= nx < w and 0 <= ny < h and px[nx, ny][3] == 0
                for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
            )
            if touches_trans and r > 220 and g > 220 and b > 220 and (b - r) < 20:
                px[x, y] = (0, 0, 0, 0)

    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    cw, ch = im.size
    side = max(cw, ch) + 4
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - cw) // 2, (side - ch) // 2), im)
    return sq


def rgba_to_xpm(var_name, sq_rgba, size, max_colors=60):
    # Composite onto neutral mid-blue-gray before resizing RGB to prevent dark/white fringe,
    # while resizing the alpha mask separately for crisp silhouette edges!
    alpha_resized = sq_rgba.getchannel("A").resize((size, size), Image.Resampling.LANCZOS)

    matte = Image.new("RGBA", sq_rgba.size, (174, 178, 195, 255))
    comp = Image.alpha_composite(matte, sq_rgba).convert("RGB")
    rgb_resized = comp.resize((size, size), Image.Resampling.LANCZOS)

    quant = rgb_resized.quantize(colors=min(max_colors, len(XPM_CHARS) - 2), method=Image.Quantize.MEDIANCUT)
    pal_raw = quant.getpalette()
    q_px = quant.load()
    a_px = alpha_resized.load()

    trans_char = " "
    used_indices = {}
    next_ci = 0
    rows_str = []
    has_trans = False

    for y in range(size):
        row_chars = []
        for x in range(size):
            if a_px[x, y] < 115:
                row_chars.append(trans_char)
                has_trans = True
            else:
                idx = q_px[x, y]
                if idx not in used_indices:
                    used_indices[idx] = XPM_CHARS[next_ci]
                    next_ci += 1
                row_chars.append(used_indices[idx])
        rows_str.append("".join(row_chars))

    ncolors = len(used_indices) + (1 if has_trans else 0)
    lines = [
        "/* XPM */",
        f"static char *{var_name}[] __attribute__((unused)) = {{",
        f'"{size} {size} {ncolors} 1",',
    ]
    if has_trans:
        lines.append(f'"{trans_char} c None",')
    for idx, ch in used_indices.items():
        r = pal_raw[idx * 3]
        g = pal_raw[idx * 3 + 1]
        b = pal_raw[idx * 3 + 2]
        lines.append(f'"{ch} c #{r:02X}{g:02X}{b:02X}",')
    for i, rstr in enumerate(rows_str):
        comma = "," if i + 1 < size else ""
        lines.append(f'"{rstr}"{comma}')
    lines.append("};")
    return "\n".join(lines) + "\n", rgb_resized, alpha_resized


def draw_toolbar_icon(kind):
    w, h = 20, 20
    g = [['.' for _ in range(w)] for _ in range(h)]

    def set_px(x, y, ch):
        if 0 <= x < w and 0 <= y < h:
            g[y][x] = ch

    def rect(x0, y0, x1, y1, border, fill=None):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if x in (x0, x1) or y in (y0, y1):
                    set_px(x, y, border)
                elif fill is not None:
                    set_px(x, y, fill)

    def envelope(x0, y0, x1, y1):
        rect(x0, y0, x1, y1, 'k', 'e')
        midx = (x0 + x1) // 2
        for x in range(x0 + 1, x1):
            fy = y0 + 1 + ((x - x0) if x <= midx else (x1 - x)) * (y1 - y0 - 2) // max(1, (midx - x0))
            set_px(x, min(y1 - 1, fy), 'm')
            if y0 + 1 < fy:
                for yy in range(y0 + 1, min(y1 - 1, fy)):
                    set_px(x, yy, 'w')

    if kind == 'new_mail':
        envelope(1, 5, 14, 15)
        for i in range(7):
            set_px(11 + i, 9 - i, 'y')
            set_px(12 + i, 9 - i, 'u')
            set_px(11 + i, 10 - i, 'k')
        set_px(10, 10, 'k')
    elif kind == 'reply':
        envelope(5, 6, 18, 16)
        for x in range(3, 11):
            set_px(x, 4, 'b'); set_px(x, 5, 'c')
        for d in range(1, 4):
            set_px(2 + d, 4 - d, 'b'); set_px(2 + d, 5 + d, 'b'); set_px(3 + d, 4 - d, 'a')
        set_px(2, 4, 'b'); set_px(2, 5, 'b')
    elif kind == 'reply_all':
        envelope(6, 7, 18, 16)
        for off, col in [(0, 'b'), (4, 'c')]:
            for x in range(2 + off, 9 + off):
                set_px(x, 4, col); set_px(x, 5, col)
            for d in range(1, 4):
                set_px(1 + off + d, 4 - d, col); set_px(1 + off + d, 5 + d, col)
            set_px(1 + off, 4, col)
    elif kind == 'forward':
        envelope(1, 6, 14, 16)
        for x in range(9, 17):
            set_px(x, 4, 'g'); set_px(x, 5, 'l')
        for d in range(1, 4):
            set_px(17 - d, 4 - d, 'g'); set_px(17 - d, 5 + d, 'g')
        set_px(17, 4, 'g'); set_px(17, 5, 'g')
    elif kind == 'send_recv':
        for y in range(2, 15):
            set_px(5, y, 'g'); set_px(6, y, 'l')
        for d in range(4):
            for xx in range(5 - (3 - d), 7 + (3 - d)):
                set_px(xx, 12 + d, 'g')
        for y in range(5, 18):
            set_px(13, y, 'b'); set_px(14, y, 'a')
        for d in range(4):
            for xx in range(13 - d, 15 + d):
                set_px(xx, 4 + d, 'b')
    elif kind == 'delete':
        for i in range(3, 17):
            for t in (-1, 0, 1):
                set_px(i + t, i, 'r')
                set_px((19 - i) + t, i, 'r')
            set_px(i, i - 1, 'p')
    elif kind == 'signature':
        rect(2, 2, 17, 17, 'k', 'e')
        for x in range(4, 15):
            set_px(x, 5, 'm'); set_px(x, 8, 'm')
        for x, y in [(4, 14), (5, 12), (6, 14), (8, 13), (9, 11), (10, 14), (12, 13), (14, 12), (15, 14)]:
            set_px(x, y, 'b'); set_px(x, y - 1, 'c')
    elif kind == 'accounts':
        # Browser globe + key icon for 1-click Sign In
        for y in range(2, 17):
            for x in range(2, 17):
                d2 = (x - 9.5) ** 2 + (y - 9.5) ** 2
                if 36 <= d2 <= 52:
                    set_px(x, y, 'n')
                elif d2 < 36:
                    if x in (9, 10) or y in (9, 10) or abs(x - 9.5) == 4:
                        set_px(x, y, 'a')
                    else:
                        set_px(x, y, 'b')
        for x in range(11, 18):
            set_px(x, 15, 'y'); set_px(x, 16, 'u')
    elif kind == 'send_now':
        envelope(1, 5, 13, 15)
        for x in range(12, 19):
            set_px(x, 9, 'b'); set_px(x, 10, 'a')
        set_px(16, 7, 'b'); set_px(17, 8, 'b')
        set_px(16, 12, 'b'); set_px(17, 11, 'b')
    elif kind == 'attach':
        for y in range(4, 15):
            set_px(6, y, 'k'); set_px(13, y, 'k')
        for y in range(6, 14):
            set_px(9, y, 't')
        for x in range(7, 13):
            set_px(x, 3, 'k'); set_px(x, 16, 'k')
        for x in range(7, 10):
            set_px(x, 14, 't')
        for x in range(9, 13):
            set_px(x, 5, 't')
    elif kind == 'calendar':
        rect(2, 3, 17, 17, 'k', 'e')
        for y in range(4, 7):
            for x in range(3, 17):
                set_px(x, y, 'r' if y == 4 else 'b')
        for rx in (5, 6, 13, 14):
            set_px(rx, 2, 'k'); set_px(rx, 3, 'w'); set_px(rx, 4, 'w')
        for gy in (9, 12, 15):
            for gx in (5, 8, 11, 14):
                set_px(gx, gy, 'b' if (gx == 11 and gy == 12) else 't')
                set_px(gx + 1, gy, 'b' if (gx == 11 and gy == 12) else 't')

    used = []
    for row in g:
        for ch in row:
            if ch not in used:
                used.append(ch)
    lines = ["/* XPM */", f"static char *xpm_tb_{kind}[] __attribute__((unused)) = {{", f'"20 20 {len(used)} 1",']
    for ch in used:
        col = TOOLBAR_PALETTE[ch]
        lines.append(f'"{ch} c None",' if col is None else f'"{ch} c {col}",')
    for idx, row in enumerate(g):
        lines.append(f'"{"".join(row)}"' + ("," if idx + 1 < h else ""))
    lines.append("};\n")
    return "\n".join(lines)


def main():
    dist_dir = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.dirname(dist_dir)
    logo_png = os.path.join(dist_dir, "outlook_express_logo.png")

    orig = Image.open(logo_png)
    clean_sq = knockout_exterior_white(orig)

    icon_dir = os.path.expanduser("~/.dt/icons")
    os.makedirs(icon_dir, exist_ok=True)

    sizes = [
        ("outlook_express.pm", "outlook_express_pm", 48, 64),
        ("outlook_express.l.pm", "outlook_express_l", 48, 64),
        ("outlook_express.m.pm", "outlook_express_m", 32, 56),
        ("outlook_express.s.pm", "outlook_express_s", 24, 48),
        ("outlook_express.t.pm", "outlook_express_t", 16, 36),
    ]
    xpm_32_str = ""
    xpm_20_str, _, _ = rgba_to_xpm("xpm_oe_logo_20", clean_sq, 20, max_colors=44)
    preview_sheet = Image.new("RGBA", (140, 64), (174, 178, 195, 255))
    px_off = 8
    for fname, vname, sz, nmax in sizes:
        xpm_str, rgb_im, a_im = rgba_to_xpm(vname, clean_sq, sz, max_colors=nmax)
        with open(os.path.join(icon_dir, fname), "w", encoding="utf-8") as f:
            f.write(xpm_str)
        if sz == 32:
            xpm_32_str, _, _ = rgba_to_xpm("xpm_oe_logo_32", clean_sq, 32, max_colors=56)
        if fname in ("outlook_express.l.pm", "outlook_express.m.pm", "outlook_express.t.pm"):
            rgba_prev = rgb_im.copy().convert("RGBA")
            rgba_prev.putalpha(a_im.point(lambda p: 255 if p >= 115 else 0))
            preview_sheet.paste(rgba_prev, (px_off, (64 - sz) // 2), rgba_prev)
            px_off += sz + 12

    prev_out = os.environ.get("OE_ICON_PREVIEW", "")
    if prev_out:
        preview_sheet.save(prev_out)

    hdr_parts = [
        "#ifndef CDE_OE_ICONS_H",
        "#define CDE_OE_ICONS_H",
        "",
        xpm_32_str,
        xpm_20_str,
    ]
    for tb in [
        "new_mail", "reply", "reply_all", "forward",
        "send_recv", "delete", "signature", "accounts",
        "send_now", "attach", "calendar",
    ]:
        hdr_parts.append(draw_toolbar_icon(tb))
    hdr_parts.append("#endif /* CDE_OE_ICONS_H */\n")

    with open(os.path.join(src_dir, "cde_oe_icons.h"), "w", encoding="utf-8") as f:
        f.write("\n".join(hdr_parts))
    print("Successfully converted attached Outlook Express logo to CDE XPM icons!")


if __name__ == "__main__":
    main()
