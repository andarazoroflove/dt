#!/usr/bin/env python3
"""
cde-mail-webauth: Dedicated WebKit2GTK Browser Sign-In Window for Microsoft 365 (2FA / SSO).

Intercepts `https://login.microsoftonline.com/common/oauth2/nativeclient?code=...` across
navigation actions, HTTP 302 response redirects, load-changed, and URI notifications.
"""
import json
import sys
import urllib.parse

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.1")
from gi.repository import Gtk, WebKit2


def main():
    if len(sys.argv) < 3:
        print(json.dumps({"error": "Usage: cde-mail-webauth <auth_url> <redirect_prefix>"}))
        return 1

    auth_url = sys.argv[1]
    redirect_prefix = sys.argv[2].lower()
    result = {"code": None, "state": None, "error": "Sign-in window closed before completion."}

    win = Gtk.Window(title="Sign in to Microsoft 365 — dtMail")
    win.set_default_size(520, 680)
    win.set_position(Gtk.WindowPosition.CENTER)

    ctx = WebKit2.WebContext.get_default()
    cm = ctx.get_cookie_manager()
    cm.set_accept_policy(WebKit2.CookieAcceptPolicy.ALWAYS)

    webview = WebKit2.WebView.new_with_context(ctx)
    settings = webview.get_settings()
    settings.set_enable_javascript(True)

    done = [False]

    def check_uri(uri):
        if done[0] or not uri:
            return False
        low = uri.lower()
        if low.startswith(redirect_prefix) or "/oauth2/nativeclient" in low:
            parsed = urllib.parse.urlparse(uri)
            qs = urllib.parse.parse_qs(parsed.query)
            if "code" in qs and qs["code"]:
                done[0] = True
                result["code"] = qs["code"][0]
                result["state"] = (qs.get("state") or [""])[0]
                result["error"] = None
                Gtk.main_quit()
                return True
            if "error" in qs:
                done[0] = True
                desc = (qs.get("error_description") or qs["error"])[0]
                result["error"] = desc
                Gtk.main_quit()
                return True
        return False

    def on_decide_policy(wv, decision, decision_type):
        if decision_type in (
            WebKit2.PolicyDecisionType.NAVIGATION_ACTION,
            WebKit2.PolicyDecisionType.NEW_WINDOW_ACTION,
        ):
            nav_act = decision.get_navigation_action()
            req = nav_act.get_request()
            uri = req.get_uri()
            if check_uri(uri):
                decision.ignore()
                return True
            if decision_type == WebKit2.PolicyDecisionType.NEW_WINDOW_ACTION and uri:
                wv.load_uri(uri)
                decision.ignore()
                return True
        elif decision_type == WebKit2.PolicyDecisionType.RESPONSE:
            resp = decision.get_response()
            if resp and check_uri(resp.get_uri()):
                decision.ignore()
                return True
        return False

    def on_load_changed(wv, _load_event):
        check_uri(wv.get_uri())

    def on_uri_changed(wv, _pspec):
        check_uri(wv.get_uri())

    def on_destroy(*_args):
        if not done[0]:
            Gtk.main_quit()

    webview.connect("decide-policy", on_decide_policy)
    webview.connect("load-changed", on_load_changed)
    webview.connect("notify::uri", on_uri_changed)
    win.connect("destroy", on_destroy)

    win.add(webview)
    win.show_all()
    webview.load_uri(auth_url)
    Gtk.main()

    sys.stdout.write(json.dumps(result) + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
