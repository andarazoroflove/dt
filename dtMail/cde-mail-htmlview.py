#!/usr/bin/env python3
"""
cde-mail-htmlview: Embedded X11 WebKit2GTK HTML Reading Pane for CDE Outlook Express.

Reparents a borderless WebKit2.WebView directly inside the Motif Reading Pane's sunken
XmDrawingArea X11 window (`XReparentWindow`), so HTML emails (tables, fonts, colors,
inline CID images) render natively inside the bottom-right Reading Pane — just like
classic Outlook Express's embedded Trident reading pane.

Also forwards Ctrl+/Ctrl-/Ctrl+0 key presses to the parent Motif client so all panes
scale their fonts in unison.
"""
import ctypes
import ctypes.util
import json
import os
import subprocess
import sys
import threading

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("GdkX11", "3.0")
gi.require_version("WebKit2", "4.1")
from gi.repository import Gdk, GdkX11, GLib, Gtk, WebKit2


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def open_external(url):
    for browser in ("/usr/bin/xdg-open", "/usr/bin/microsoft-edge-stable", "/usr/bin/firefox-esr"):
        if os.path.exists(browser):
            try:
                subprocess.Popen([browser, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                return
            except Exception:
                pass


def reparent_into_motif(child_xid, parent_xid, w, h):
    x11_path = ctypes.util.find_library("X11") or "libX11.so.6"
    libX11 = ctypes.CDLL(x11_path)
    libX11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    libX11.XOpenDisplay.restype = ctypes.c_void_p
    libX11.XReparentWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int]
    libX11.XMoveResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
    libX11.XMapRaised.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    libX11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

    dpy = libX11.XOpenDisplay(None)
    if not dpy:
        return False
    libX11.XReparentWindow(dpy, child_xid, parent_xid, 0, 0)
    libX11.XMoveResizeWindow(dpy, child_xid, 0, 0, max(w, 10), max(h, 10))
    libX11.XMapRaised(dpy, child_xid)
    libX11.XSync(dpy, 0)
    return True


def main():
    if len(sys.argv) < 2:
        return 1
    parent_xid = int(sys.argv[1], 0)
    init_w = int(sys.argv[2]) if len(sys.argv) >= 4 else 680
    init_h = int(sys.argv[3]) if len(sys.argv) >= 4 else 320

    win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
    win.set_decorated(False)
    win.set_skip_taskbar_hint(True)
    win.set_skip_pager_hint(True)
    win.set_default_size(max(init_w, 50), max(init_h, 50))

    ctx = WebKit2.WebContext.get_default()
    webview = WebKit2.WebView.new_with_context(ctx)
    settings = webview.get_settings()
    settings.set_enable_javascript(False)  # Safe email HTML rendering (no tracking scripts!)
    settings.set_enable_page_cache(False)
    settings.set_default_font_size(15)     # ~11pt at 96dpi baseline
    settings.set_default_monospace_font_size(14)

    # Match CDE Crimson.dp warm ivory paper background (#FFF7E9) by default
    bg_rgba = Gdk.RGBA()
    bg_rgba.parse("#FFF7E9")
    webview.set_background_color(bg_rgba)

    state = {"allow_load_uri": ""}

    def set_theme_bg(hex_col):
        c = Gdk.RGBA()
        if c.parse(hex_col):
            webview.set_background_color(c)

    def apply_font_pt(pt):
        # 11pt is zoom 1.0 so inline Outlook style="font-size:11pt" scales in exact lockstep!
        zoom = max(0.6, min(2.6, float(pt) / 11.0))
        webview.set_zoom_level(zoom)

    def on_decide_policy(wv, decision, decision_type):
        if decision_type in (
            WebKit2.PolicyDecisionType.NAVIGATION_ACTION,
            WebKit2.PolicyDecisionType.NEW_WINDOW_ACTION,
        ):
            nav = decision.get_navigation_action()
            uri = nav.get_request().get_uri() or ""
            nav_type = nav.get_navigation_type()
            if uri.startswith(("about:", "data:")):
                return False
            if uri.startswith("file://") and uri == state["allow_load_uri"]:
                return False
            if nav_type == WebKit2.NavigationType.LINK_CLICKED or decision_type == WebKit2.PolicyDecisionType.NEW_WINDOW_ACTION:
                if uri.lower().startswith("mailto:"):
                    addr = uri[7:].split("?", 1)[0]
                    emit({"event": "mailto", "to": addr})
                elif uri.startswith(("http://", "https://")):
                    open_external(uri)
                decision.ignore()
                return True
        return False

    def on_key_press(_widget, event):
        ctrl = bool(event.state & Gdk.ModifierType.CONTROL_MASK)
        if ctrl:
            kv = event.keyval
            if kv in (Gdk.KEY_plus, Gdk.KEY_equal, Gdk.KEY_KP_Add):
                emit({"event": "font_delta", "delta": 1})
                return True
            if kv in (Gdk.KEY_minus, Gdk.KEY_underscore, Gdk.KEY_KP_Subtract):
                emit({"event": "font_delta", "delta": -1})
                return True
            if kv in (Gdk.KEY_0, Gdk.KEY_KP_0):
                emit({"event": "font_delta", "delta": 0})
                return True
        return False

    webview.connect("decide-policy", on_decide_policy)
    webview.connect("key-press-event", on_key_press)
    win.connect("key-press-event", on_key_press)

    win.add(webview)
    win.realize()
    win.show_all()

    gdk_win = win.get_window()
    child_xid = gdk_win.get_xid()
    reparent_into_motif(child_xid, parent_xid, init_w, init_h)

    webview.load_html(
        "<!DOCTYPE html><html><body style='background:#FFF7E9;color:#000;font-family:\"DejaVu Sans\",sans-serif;font-size:11pt;margin:12px;'></body></html>",
        "about:blank",
    )
    emit({"event": "ready", "child_xid": child_xid})

    def handle_cmd(cmd):
        op = cmd.get("op")
        if op == "resize":
            w = max(20, int(cmd.get("w", 100)))
            h = max(20, int(cmd.get("h", 100)))
            win.resize(w, h)
        elif op == "load_file":
            fpath = cmd.get("path", "")
            pt = int(cmd.get("font_pt", 11))
            dark = bool(cmd.get("dark", False))
            set_theme_bg("#15181E" if dark else "#FFF7E9")
            apply_font_pt(pt)
            if fpath and os.path.exists(fpath):
                furi = "file://" + fpath
                state["allow_load_uri"] = furi
                webview.load_uri(furi)
            else:
                bg = "#15181E" if dark else "#FFF7E9"
                fg = "#EDF2F7" if dark else "#000000"
                webview.load_html(
                    f"<!DOCTYPE html><html><body style='background:{bg};color:{fg};'></body></html>",
                    "about:blank",
                )
        elif op == "set_font_pt":
            apply_font_pt(int(cmd.get("font_pt", 11)))
        elif op == "set_theme":
            dark = bool(cmd.get("dark", False))
            set_theme_bg("#15181E" if dark else "#FFF7E9")
        elif op == "quit":
            Gtk.main_quit()
        return False

    def stdin_reader():
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                cmd = json.loads(raw)
                GLib.idle_add(handle_cmd, cmd)
                if cmd.get("op") == "quit":
                    break
            except Exception:
                pass
        GLib.idle_add(Gtk.main_quit)

    threading.Thread(target=stdin_reader, daemon=True).start()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
