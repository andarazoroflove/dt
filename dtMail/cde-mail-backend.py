#!/usr/bin/env python3
"""
cde-mail-backend: Zero-Config 1-Click Browser Sign-In (Microsoft 365 OAuth 2.0 + 2FA)
& Microsoft Graph v1.0 Sync Engine for CDE dtMail.

Uses WebKit2GTK (`cde-mail-webauth`) with the system's standard Microsoft 365 native
public client configuration so signing in requires ZERO manual Client ID / Tenant setup:
the user simply signs in via browser + 2FA, and their profile, folder tree (including
subfolders), and messages download automatically into a fast local SQLite3 cache.
"""
import base64
from datetime import datetime, timezone
import hashlib
import html
from html.parser import HTMLParser
import json
import mimetypes
import os
import re
import secrets
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

_OVERRIDE_DIR = os.environ.get("CDE_MAIL_DATA_DIR", "").strip()
CONFIG_DIR = os.path.join(_OVERRIDE_DIR, "config") if _OVERRIDE_DIR else os.path.expanduser("~/.config/cde-mail")
CACHE_DIR = os.path.join(_OVERRIDE_DIR, "cache") if _OVERRIDE_DIR else os.path.expanduser("~/.local/share/cde-mail")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")
TOKEN_PATH = os.path.join(CONFIG_DIR, "tokens.json")
SIG_PATH = os.path.join(CONFIG_DIR, "signature.txt")
DB_PATH = os.path.join(CACHE_DIR, "mail_cache.db")
DIRTY_STAMP_PATH = os.path.join(CACHE_DIR, "calendar_dirty.stamp")
GOTO_JSON_PATH = os.path.join(CACHE_DIR, "calendar_goto.json")

# Standard Debian desktop Microsoft 365 native OAuth2 public client defaults
DEFAULT_M365_CLIENT_ID = "20460e5d-ce91-49af-a3a5-70b6be7486d1"
DEFAULT_M365_TENANT = "common"
DEFAULT_M365_REDIRECT = "https://login.microsoftonline.com/common/oauth2/nativeclient"
DEFAULT_M365_SCOPES = (
    "openid profile email offline_access "
    "Mail.ReadWrite Mail.Send Calendars.ReadWrite MailboxSettings.Read User.Read"
)

out_lock = threading.Lock()


def emit(obj):
    line = json.dumps(obj, ensure_ascii=False)
    with out_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


class HtmlToTextParser(HTMLParser):
    """Converts Office 365 HTML email bodies into clean, readable text for Motif XmText."""
    BLOCK_TAGS = {"p", "div", "tr", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "section", "article"}

    def __init__(self):
        super().__init__()
        self.parts = []
        self.skip_depth = 0
        self.in_href = None
        self.link_text = []

    def handle_starttag(self, tag, attrs):
        t = tag.lower()
        if t in ("style", "script", "head", "title", "meta", "xml"):
            self.skip_depth += 1
            return
        if self.skip_depth:
            return
        if t == "br":
            self.parts.append("\n")
        elif t == "hr":
            self.parts.append("\n" + ("-" * 60) + "\n")
        elif t == "li":
            self.parts.append("\n  * ")
        elif t in self.BLOCK_TAGS:
            self.parts.append("\n")
        elif t in ("td", "th"):
            self.parts.append("  ")
        elif t == "a":
            href = dict(attrs).get("href", "")
            if href.startswith(("http://", "https://")):
                self.in_href = href
                self.link_text = []

    def handle_endtag(self, tag):
        t = tag.lower()
        if t in ("style", "script", "head", "title", "meta", "xml"):
            if self.skip_depth > 0:
                self.skip_depth -= 1
            return
        if self.skip_depth:
            return
        if t == "a" and self.in_href:
            vis = "".join(self.link_text).strip()
            if vis and vis != self.in_href and self.in_href not in vis:
                self.parts.append(f" <{self.in_href}>")
            self.in_href = None
            self.link_text = []
        elif t in ("p", "h1", "h2", "h3", "blockquote"):
            self.parts.append("\n")

    def handle_data(self, data):
        if self.skip_depth:
            return
        cleaned = data.replace("\r", "")
        if "\n" in cleaned:
            cleaned = re.sub(r"[ \t]*\n[ \t]*", " ", cleaned)
        self.parts.append(cleaned)
        if self.in_href is not None:
            self.link_text.append(cleaned)

    def get_text(self):
        raw = html.unescape("".join(self.parts)).replace("\xa0", " ")
        lines = [ln.rstrip() for ln in raw.splitlines()]
        out = []
        blank_run = 0
        for ln in lines:
            if not ln.strip():
                blank_run += 1
                if blank_run <= 2:
                    out.append("")
            else:
                blank_run = 0
                out.append(ln)
        return "\n".join(out).strip()


def html_to_plain(html_str):
    if not html_str:
        return ""
    p = HtmlToTextParser()
    try:
        p.feed(html_str)
        p.close()
        return p.get_text()
    except Exception:
        return re.sub(r"<[^>]+>", "", html_str)


def ensure_dirs():
    os.makedirs(CONFIG_DIR, mode=0o700, exist_ok=True)
    os.makedirs(CACHE_DIR, mode=0o700, exist_ok=True)


def load_config():
    ensure_dirs()
    default_cfg = {
        "display_name": "",
        "email": "",
        "tenant": DEFAULT_M365_TENANT,
        "client_id": "",
        "sig_enabled": True,
        "sig_skip_replies": False,
        "theme": "cde",
        "font_pt": 11,
    }
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                default_cfg.update(json.load(f))
        except Exception:
            pass
    if os.path.exists(SIG_PATH):
        try:
            with open(SIG_PATH, "r", encoding="utf-8") as f:
                default_cfg["signature"] = f.read()
        except Exception:
            default_cfg["signature"] = ""
    else:
        default_cfg["signature"] = "Best regards,"
    return default_cfg


def save_config(cfg):
    ensure_dirs()
    sig = cfg.pop("signature", None)
    if sig is not None:
        with open(SIG_PATH, "w", encoding="utf-8") as f:
            f.write(sig)
    disk_cfg = {k: v for k, v in cfg.items() if k != "signature"}
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(disk_cfg, f, indent=2)
    os.chmod(tmp, 0o600)
    os.replace(tmp, CONFIG_PATH)
    if sig is not None:
        cfg["signature"] = sig


def load_tokens():
    if not os.path.exists(TOKEN_PATH):
        return {}
    try:
        with open(TOKEN_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_tokens(tok):
    ensure_dirs()
    tmp = TOKEN_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(tok, f, indent=2)
    os.chmod(tmp, 0o600)
    os.replace(tmp, TOKEN_PATH)


def init_db():
    ensure_dirs()
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS folders (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            well_known TEXT DEFAULT '',
            total_count INTEGER DEFAULT 0,
            unread_count INTEGER DEFAULT 0,
            sort_order INTEGER DEFAULT 100
        );
        CREATE TABLE IF NOT EXISTS messages (
            id TEXT PRIMARY KEY,
            folder_id TEXT NOT NULL,
            from_name TEXT DEFAULT '',
            from_addr TEXT DEFAULT '',
            to_addrs TEXT DEFAULT '',
            cc_addrs TEXT DEFAULT '',
            subject TEXT DEFAULT '',
            date_ts INTEGER DEFAULT 0,
            date_disp TEXT DEFAULT '',
            is_read INTEGER DEFAULT 1,
            importance TEXT DEFAULT 'normal',
            has_attachments INTEGER DEFAULT 0,
            size_bytes INTEGER DEFAULT 0,
            body_text TEXT DEFAULT '',
            body_html TEXT DEFAULT '',
            body_loaded INTEGER DEFAULT 0,
            attachments_json TEXT DEFAULT '[]',
            meeting_json TEXT DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_msg_folder_date ON messages(folder_id, date_ts DESC);
    """)
    msg_cols = {r[1] for r in conn.execute("PRAGMA table_info(messages)").fetchall()}
    if "meeting_json" not in msg_cols:
        conn.execute("ALTER TABLE messages ADD COLUMN meeting_json TEXT DEFAULT ''")
    cur = conn.execute("SELECT COUNT(*) FROM folders")
    if cur.fetchone()[0] == 0:
        seed_folders = [
            ("local_inbox", "Inbox", "inbox", 1, 1, 1),
            ("local_outbox", "Outbox", "outbox", 0, 0, 2),
            ("local_sent", "Sent Items", "sentitems", 0, 0, 3),
            ("local_drafts", "Drafts", "drafts", 0, 0, 4),
            ("local_deleted", "Deleted Items", "deleteditems", 0, 0, 5),
            ("local_junk", "Junk Email", "junkemail", 0, 0, 6),
            ("local_archive", "Archive", "archive", 0, 0, 7),
        ]
        conn.executemany(
            "INSERT INTO folders(id, name, well_known, total_count, unread_count, sort_order) VALUES (?,?,?,?,?,?)",
            seed_folders,
        )
        now_ts = int(time.time())
        welcome_body = (
            "Welcome to dtMail — Common Desktop Environment Edition!\n"
            "============================================================================\n\n"
            "To connect your Work Office 365 mailbox:\n"
            "  * Simply click 'Sign In' (or 'Send/Recv') on the toolbar above!\n"
            "  * Sign in with your work email and 2FA in the Microsoft browser window.\n"
            "  * Your folders and emails will automatically download right here.\n\n"
            "Tip: Click 'Signature' on the toolbar anytime to edit your outgoing signature."
        )
        conn.execute(
            """INSERT INTO messages(id, folder_id, from_name, from_addr, to_addrs, subject,
               date_ts, date_disp, is_read, importance, has_attachments, size_bytes,
               body_text, body_loaded)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                "welcome_oe_cde_1",
                "local_inbox",
                "dtMail",
                "welcome@outlook-express.cde",
                "you@office365",
                "Welcome! Click 'Sign In' above to connect your Office 365 mailbox",
                now_ts,
                datetime.now().strftime("%I:%M %p").lstrip("0"),
                0,
                "high",
                0,
                len(welcome_body),
                welcome_body,
                1,
            ),
        )
        conn.commit()
    conn.close()


def format_date_short(iso_str):
    if not iso_str:
        return int(time.time()), ""
    try:
        s = iso_str.replace("Z", "+00:00")
        dt = datetime.fromisoformat(s).astimezone()
        now = datetime.now().astimezone()
        if dt.date() == now.date():
            return int(dt.timestamp()), dt.strftime("%I:%M %p").lstrip("0")
        elif dt.year == now.year:
            return int(dt.timestamp()), dt.strftime("%b %d  %I:%M %p")
        else:
            return int(dt.timestamp()), dt.strftime("%m/%d/%Y")
    except Exception:
        return int(time.time()), iso_str[:16]


def http_json(method, url, headers=None, data_bytes=None, form_dict=None, timeout=30):
    hdrs = dict(headers or {})
    if form_dict is not None:
        data_bytes = urllib.parse.urlencode(form_dict).encode("utf-8")
        hdrs["Content-Type"] = "application/x-www-form-urlencoded"
    req = urllib.request.Request(url, data=data_bytes, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            if not raw:
                return resp.status, {}
            return resp.status, json.loads(raw.decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace")
        try:
            err_obj = json.loads(raw)
        except Exception:
            err_obj = {"error_description": raw or str(e)}
        return e.code, err_obj
    except Exception as e:
        return 0, {"error_description": str(e)}


def open_url_in_browser(url):
    for cmd in (["/usr/bin/xdg-open", url], ["/usr/bin/microsoft-edge-stable", url], ["/usr/bin/firefox-esr", url]):
        if os.path.exists(cmd[0]):
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                return True
            except Exception:
                continue
    return False


def find_webauth_helper():
    p1 = os.path.expanduser("~/.local/bin/cde-mail-webauth")
    if os.path.exists(p1):
        return p1
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "cde-mail-webauth.py")


class OAuthManager:
    def __init__(self):
        self.login_lock = threading.Lock()

    def effective_client_and_tenant(self, cfg, tok=None):
        custom_cid = (cfg.get("client_id") or (tok or {}).get("client_id") or "").strip()
        if not custom_cid or custom_cid == DEFAULT_M365_CLIENT_ID:
            # Built-in multi-tenant Microsoft 365 public client MUST always use
            # authority 'common' and exact redirect 'https://login.microsoftonline.com/common/oauth2/nativeclient'
            return DEFAULT_M365_CLIENT_ID, DEFAULT_M365_TENANT, DEFAULT_M365_REDIRECT
        ten = (cfg.get("tenant") or (tok or {}).get("tenant") or "").strip() or DEFAULT_M365_TENANT
        redirect = (tok or {}).get("redirect_uri") or DEFAULT_M365_REDIRECT
        return custom_cid, ten, redirect

    def get_valid_access_token(self, cfg):
        tok = load_tokens()
        if not tok or not tok.get("access_token"):
            return None, "Not signed in — click 'Sign In' on the toolbar."
        if tok.get("expires_at", 0) > time.time() + 90:
            return tok["access_token"], None

        refresh_tok = tok.get("refresh_token")
        cid, ten, _ = self.effective_client_and_tenant(cfg, tok)
        if not refresh_tok:
            return None, "Session expired — click 'Sign In' to reconnect."

        token_url = f"https://login.microsoftonline.com/{urllib.parse.quote(ten)}/oauth2/v2.0/token"
        code, resp = http_json("POST", token_url, form_dict={
            "client_id": cid,
            "grant_type": "refresh_token",
            "refresh_token": refresh_tok,
            "scope": DEFAULT_M365_SCOPES,
        })
        if code == 200 and "access_token" in resp:
            tok["access_token"] = resp["access_token"]
            if "refresh_token" in resp:
                tok["refresh_token"] = resp["refresh_token"]
            tok["expires_at"] = time.time() + int(resp.get("expires_in", 3600))
            save_tokens(tok)
            return tok["access_token"], None
        desc = resp.get("error_description") or resp.get("error") or "Token refresh failed"
        return None, f"OAuth2 refresh error: {desc.splitlines()[0][:140]}"

    def start_one_click_browser_login(self, cfg, on_complete):
        t = threading.Thread(target=self._run_webauth_login, args=(dict(cfg), on_complete), daemon=True)
        t.start()

    def _run_webauth_login(self, cfg, on_complete):
        if not self.login_lock.acquire(blocking=False):
            emit({"event": "auth_status", "status": "Browser sign-in window is already open..."})
            return
        try:
            cid, ten, redirect_uri = self.effective_client_and_tenant(cfg)
            verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode("ascii")
            challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode("ascii")).digest()).rstrip(b"=").decode("ascii")
            state_tok = secrets.token_hex(12)

            params = {
                "client_id": cid,
                "response_type": "code",
                "redirect_uri": redirect_uri,
                "response_mode": "query",
                "scope": DEFAULT_M365_SCOPES,
                "code_challenge": challenge,
                "code_challenge_method": "S256",
                "state": state_tok,
                "prompt": "select_account",
            }
            if cfg.get("email"):
                params["login_hint"] = cfg["email"].strip()

            auth_url = (
                f"https://login.microsoftonline.com/{urllib.parse.quote(ten)}/oauth2/v2.0/authorize?"
                + urllib.parse.urlencode(params)
            )
            emit({"event": "auth_status", "status": "Complete your Microsoft 365 + 2FA sign-in in the browser window..."})

            helper = find_webauth_helper()
            proc = subprocess.run(
                ["/usr/bin/python3", helper, auth_url, redirect_uri],
                capture_output=True,
                text=True,
                timeout=600,
            )
            out_lines = [ln.strip() for ln in (proc.stdout or "").splitlines() if ln.strip()]
            if not out_lines:
                err_txt = (proc.stderr or "").strip().splitlines()[-1:] or ["Browser sign-in cancelled."]
                emit({"event": "auth_error", "message": err_txt[0][:180]})
                return

            res = json.loads(out_lines[-1])
            if res.get("error"):
                # Don't pop a scary error dialog if user merely closed the window manually
                if "closed before completion" in res["error"]:
                    emit({"event": "status", "status": "Sign-in cancelled."})
                else:
                    emit({"event": "auth_error", "message": str(res["error"]).splitlines()[0][:200]})
                return

            auth_code = res.get("code")
            if not auth_code:
                emit({"event": "status", "status": "Sign-in cancelled."})
                return

            emit({"event": "auth_status", "status": "2FA verified! Exchanging token & downloading your folders..."})
            token_url = f"https://login.microsoftonline.com/{urllib.parse.quote(ten)}/oauth2/v2.0/token"
            code_http, tok_resp = http_json("POST", token_url, form_dict={
                "client_id": cid,
                "grant_type": "authorization_code",
                "code": auth_code,
                "redirect_uri": redirect_uri,
                "code_verifier": verifier,
                "scope": DEFAULT_M365_SCOPES,
            })
            if code_http != 200 or "access_token" not in tok_resp:
                desc = tok_resp.get("error_description") or tok_resp.get("error") or "Token exchange failed"
                emit({"event": "auth_error", "message": desc.splitlines()[0][:200]})
                return

            tok_data = {
                "access_token": tok_resp["access_token"],
                "refresh_token": tok_resp.get("refresh_token", ""),
                "expires_at": time.time() + int(tok_resp.get("expires_in", 3600)),
                "client_id": cid,
                "tenant": ten,
                "redirect_uri": redirect_uri,
            }

            # Extract profile from id_token + /v1.0/me
            id_tok = tok_resp.get("id_token", "")
            if id_tok and "." in id_tok:
                try:
                    seg = id_tok.split(".")[1]
                    seg += "=" * (-len(seg) % 4)
                    claims = json.loads(base64.urlsafe_b64decode(seg).decode("utf-8"))
                    upn = claims.get("preferred_username") or claims.get("email") or claims.get("upn") or ""
                    nm = claims.get("name") or ""
                    if upn:
                        cfg["email"] = upn
                    if nm and not cfg.get("display_name"):
                        cfg["display_name"] = nm
                except Exception:
                    pass

            c_me, me = http_json(
                "GET",
                "https://graph.microsoft.com/v1.0/me?$select=displayName,mail,userPrincipalName",
                headers={"Authorization": f"Bearer {tok_resp['access_token']}", "Accept": "application/json"},
            )
            if c_me == 200:
                addr = me.get("mail") or me.get("userPrincipalName") or ""
                if addr:
                    cfg["email"] = addr
                if me.get("displayName") and not cfg.get("display_name"):
                    cfg["display_name"] = me["displayName"]

            save_config(cfg)
            save_tokens(tok_data)
            emit({
                "event": "auth_ok",
                "email": cfg.get("email", ""),
                "display_name": cfg.get("display_name", ""),
                "message": f"Signed in as {cfg.get('email') or 'Office 365'} — syncing folders & mail...",
            })
            if on_complete:
                on_complete()
        except Exception as e:
            emit({"event": "auth_error", "message": str(e)[:180]})
        finally:
            self.login_lock.release()


WELL_KNOWN_ORDER = {
    "inbox": 1,
    "outbox": 2,
    "sentitems": 3,
    "sent": 3,
    "drafts": 4,
    "deleteditems": 5,
    "trash": 5,
    "junkemail": 6,
    "junk": 6,
    "archive": 7,
}


class MailEngine:
    def __init__(self):
        self.oauth = OAuthManager()
        self.sync_lock = threading.Lock()

    def _fetch_all_graph_folders(self, hdrs):
        """Fetches top-level mailFolders and one level of childFolders so custom subfolders appear automatically."""
        results = []
        url = "https://graph.microsoft.com/v1.0/me/mailFolders?$top=100&$select=id,displayName,totalItemCount,unreadItemCount,childFolderCount"
        while url:
            c, data = http_json("GET", url, headers=hdrs)
            if c != 200:
                return c, data, []
            for f in data.get("value", []):
                results.append((f, 0))
                if int(f.get("childFolderCount", 0)) > 0:
                    curl = (
                        f"https://graph.microsoft.com/v1.0/me/mailFolders/{urllib.parse.quote(f['id'])}/childFolders"
                        "?$top=50&$select=id,displayName,totalItemCount,unreadItemCount"
                    )
                    cc, cdata = http_json("GET", curl, headers=hdrs)
                    if cc == 200:
                        for sub in cdata.get("value", []):
                            results.append((sub, 1))
            url = data.get("@odata.nextLink")
        return 200, {}, results

    def sync_graph(self, cfg, token, active_folder_id=None):
        hdrs = {
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
            "Prefer": 'outlook.body-content-type="html"',
        }
        if not cfg.get("email"):
            c, me = http_json("GET", "https://graph.microsoft.com/v1.0/me?$select=displayName,mail,userPrincipalName", headers=hdrs)
            if c == 200:
                cfg["email"] = me.get("mail") or me.get("userPrincipalName") or ""
                if not cfg.get("display_name") and me.get("displayName"):
                    cfg["display_name"] = me["displayName"]
                save_config(cfg)

        c, err_data, folder_tuples = self._fetch_all_graph_folders(hdrs)
        if c != 200:
            err = err_data.get("error", {})
            msg = err.get("message") if isinstance(err, dict) else err_data.get("error_description", "Graph sync error")
            return False, f"Graph error ({c}): {str(msg)[:160]}"

        conn = sqlite3.connect(DB_PATH)
        conn.execute("DELETE FROM messages WHERE id = 'welcome_oe_cde_1'")
        conn.execute("DELETE FROM folders WHERE id LIKE 'local_%'")

        remote_fids = set()
        order_counter = 20
        for f, depth in folder_tuples:
            fid = f["id"]
            remote_fids.add(fid)
            raw_name = f.get("displayName", "Folder")
            disp_name = f"  |- {raw_name}" if depth > 0 else raw_name
            key = raw_name.lower().replace(" ", "") if depth == 0 else f"sub_{order_counter}"
            if depth == 0 and key in WELL_KNOWN_ORDER:
                sord = WELL_KNOWN_ORDER[key] * 10
            else:
                order_counter += 1
                sord = 100 + order_counter
            conn.execute(
                """INSERT INTO folders(id, name, well_known, total_count, unread_count, sort_order)
                   VALUES (?,?,?,?,?,?)
                   ON CONFLICT(id) DO UPDATE SET
                     name=excluded.name, total_count=excluded.total_count,
                     unread_count=excluded.unread_count, sort_order=excluded.sort_order""",
                (fid, disp_name, key, int(f.get("totalItemCount", 0)), int(f.get("unreadItemCount", 0)), sord),
            )
        conn.commit()
        all_f = conn.execute("SELECT id, well_known, total_count FROM folders ORDER BY sort_order ASC").fetchall()
        conn.close()

        # Sync Inbox + Sent + Drafts + Deleted + active folder + first non-empty custom folders
        to_sync = []
        for fid, wk, tot in all_f:
            if wk in ("inbox", "sentitems", "drafts", "deleteditems") or fid == active_folder_id:
                to_sync.append(fid)
            elif tot > 0 and len(to_sync) < 8:
                to_sync.append(fid)

        for fid in to_sync:
            self._sync_graph_folder_messages(fid, hdrs)

        return True, f"Synchronized with Office 365 ({cfg.get('email', 'Connected')})"

    def _sync_graph_folder_messages(self, folder_id, hdrs):
        q = (
            f"https://graph.microsoft.com/v1.0/me/mailFolders/{urllib.parse.quote(folder_id)}/messages"
            "?$top=60&$orderby=receivedDateTime%20desc"
            "&$select=id,subject,from,toRecipients,ccRecipients,receivedDateTime,sentDateTime,isRead,importance,hasAttachments,bodyPreview"
        )
        c, mdata = http_json("GET", q, headers=hdrs)
        if c != 200:
            return
        items = mdata.get("value", [])
        conn = sqlite3.connect(DB_PATH)
        for m in items:
            mid = m["id"]
            fr = (m.get("from") or {}).get("emailAddress") or {}
            from_name = fr.get("name") or fr.get("address") or "(Unknown)"
            from_addr = fr.get("address") or ""
            tos = ", ".join(
                (r.get("emailAddress") or {}).get("address", "")
                for r in (m.get("toRecipients") or [])
                if (r.get("emailAddress") or {}).get("address")
            )
            ccs = ", ".join(
                (r.get("emailAddress") or {}).get("address", "")
                for r in (m.get("ccRecipients") or [])
                if (r.get("emailAddress") or {}).get("address")
            )
            dt_raw = m.get("receivedDateTime") or m.get("sentDateTime") or ""
            ts, disp = format_date_short(dt_raw)
            subj = m.get("subject") or "(No Subject)"
            is_read = 1 if m.get("isRead", True) else 0
            imp = (m.get("importance") or "normal").lower()
            has_att = 1 if m.get("hasAttachments") else 0
            preview = m.get("bodyPreview") or ""

            conn.execute(
                """INSERT INTO messages(id, folder_id, from_name, from_addr, to_addrs, cc_addrs,
                   subject, date_ts, date_disp, is_read, importance, has_attachments, body_text)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
                   ON CONFLICT(id) DO UPDATE SET
                     folder_id=excluded.folder_id, is_read=excluded.is_read,
                     has_attachments=excluded.has_attachments, subject=excluded.subject""",
                (mid, folder_id, from_name, from_addr, tos, ccs, subj, ts, disp, is_read, imp, has_att, preview),
            )
        conn.commit()
        conn.close()

    def fetch_graph_message_body(self, msg_id, cfg):
        token, err = self.oauth.get_valid_access_token(cfg)
        if not token:
            return False, err
        hdrs = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
        url = (
            f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}"
            "?$select=id,body,hasAttachments,isRead"
        )
        c, data = http_json("GET", url, headers=hdrs)
        if c != 200:
            return False, f"Failed to load message ({c})"
        body = data.get("body") or {}
        ctype = (body.get("contentType") or "text").lower()
        content = body.get("content") or ""
        if ctype == "html":
            plain = html_to_plain(content)
            raw_html = content
        else:
            plain = content
            raw_html = ""

        att_list = []
        ics_saved_path = ""
        if data.get("hasAttachments") or ("cid:" in content.lower()):
            ca, adata = http_json(
                "GET",
                f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/attachments",
                headers=hdrs,
            )
            if ca == 200:
                for a in adata.get("value", []):
                    cid_val = (a.get("contentId") or "").strip().strip("<>")
                    cbytes = a.get("contentBytes")
                    amime = a.get("contentType") or "application/octet-stream"
                    aname = a.get("name", "attachment")
                    if cid_val and cbytes and raw_html:
                        data_uri = f"data:{amime};base64,{cbytes}"
                        raw_html = re.sub(
                            r'cid:' + re.escape(cid_val),
                            lambda _m, u=data_uri: u,
                            raw_html,
                            flags=re.IGNORECASE,
                        )
                    if cbytes and (aname.lower().endswith(".ics") or "text/calendar" in amime.lower()):
                        try:
                            mh = hashlib.sha1(msg_id.encode("utf-8")).hexdigest()[:12]
                            ics_p = os.path.join(CACHE_DIR, f"invite_{mh}.ics")
                            with open(ics_p, "wb") as f_ics:
                                f_ics.write(base64.b64decode(cbytes))
                            ics_saved_path = ics_p
                        except Exception:
                            pass
                    if not a.get("isInline"):
                        att_list.append({"id": a["id"], "name": aname, "size": int(a.get("size", 0))})

        # Detect Office 365 eventMessageRequest / eventMessage or .ics calendar invitation
        meeting_info = {"is_meeting": False}
        odata_t = (data.get("@odata.type") or "").lower()
        if "eventmessage" in odata_t:
            cm_full, mfull = http_json(
                "GET",
                f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}",
                headers=hdrs,
            )
            ce_ev, ev_obj = http_json(
                "GET",
                f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/microsoft.graph.eventMessage/event"
                "?$select=id,subject,start,end,isAllDay,location,responseStatus,isOrganizer",
                headers=hdrs,
            )
            src_m = mfull if cm_full == 200 and isinstance(mfull, dict) else data
            ev_d = ev_obj if ce_ev == 200 and isinstance(ev_obj, dict) else {}
            mtype = (src_m.get("meetingMessageType") or "").strip()
            ev_id = ev_d.get("id") or ""
            is_org = bool(ev_d.get("isOrganizer", False))
            raw_resp = ((ev_d.get("responseStatus") or {}).get("response") or "").strip()
            loc_s = (
                ((ev_d.get("location") or {}).get("displayName"))
                or ((src_m.get("location") or {}).get("displayName"))
                or ""
            ).strip()
            s_obj = ev_d.get("start") or src_m.get("startDateTime") or {}
            e_obj = ev_d.get("end") or src_m.get("endDateTime") or {}
            all_day = bool(ev_d.get("isAllDay") or src_m.get("isAllDay"))
            date_iso, when_disp = format_meeting_window(s_obj, e_obj, all_day)

            st_map = {
                "notResponded": "Needs Reply",
                "none": "Needs Reply",
                "": "Needs Reply",
                "accepted": "Accepted",
                "tentativelyAccepted": "Tentative",
                "declined": "Declined",
                "organizer": "Organizer",
            }
            if mtype == "meetingCancelled":
                st_lbl = "Cancelled"
                can_rsvp = False
            elif mtype in ("meetingAccepted", "meetingTenativelyAccepted", "meetingDeclined"):
                st_lbl = mtype.replace("meeting", "")
                can_rsvp = False
            else:
                st_lbl = st_map.get(raw_resp, raw_resp or "Needs Reply")
                can_rsvp = (not is_org) and (mtype in ("meetingRequest", "") or bool(ev_id))

            meeting_info = {
                "is_meeting": True,
                "can_rsvp": can_rsvp,
                "meeting_type": mtype,
                "event_id": ev_id,
                "date": date_iso,
                "when_disp": when_disp,
                "location": loc_s,
                "status_label": st_lbl,
                "ics_path": ics_saved_path,
            }
        elif ics_saved_path:
            ics_meta = parse_ics_summary(ics_saved_path)
            if ics_meta:
                meeting_info = {
                    "is_meeting": True,
                    "can_rsvp": False,
                    "meeting_type": "icsAttachment",
                    "event_id": "",
                    "date": ics_meta.get("date", ""),
                    "when_disp": ics_meta.get("when_disp", "Calendar .ics Event"),
                    "location": ics_meta.get("location", ""),
                    "status_label": ".ics Invite",
                    "ics_path": ics_saved_path,
                }

        conn = sqlite3.connect(DB_PATH)
        conn.execute(
            "UPDATE messages SET body_text=?, body_html=?, body_loaded=1, is_read=1, attachments_json=?, meeting_json=? WHERE id=?",
            (plain, raw_html, json.dumps(att_list), json.dumps(meeting_info), msg_id),
        )
        conn.commit()
        conn.close()

        if not data.get("isRead", True):
            threading.Thread(
                target=http_json,
                args=(
                    "PATCH",
                    f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}",
                    {"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
                    json.dumps({"isRead": True}).encode("utf-8"),
                ),
                daemon=True,
            ).start()
        return True, None

    def send_mail(self, cfg, payload):
        token, err = self.oauth.get_valid_access_token(cfg)
        if not token:
            return False, err

        to_raw = [x.strip() for x in re.split(r"[,;]+", payload.get("to", "")) if x.strip()]
        cc_raw = [x.strip() for x in re.split(r"[,;]+", payload.get("cc", "")) if x.strip()]
        bcc_raw = [x.strip() for x in re.split(r"[,;]+", payload.get("bcc", "")) if x.strip()]
        if not to_raw:
            return False, "Please enter at least one recipient in the To: field."

        msg_obj = {
            "subject": payload.get("subject", ""),
            "importance": payload.get("priority", "normal").lower() if payload.get("priority", "normal").lower() in ("low", "normal", "high") else "normal",
            "body": {"contentType": "Text", "content": payload.get("body", "")},
            "toRecipients": [{"emailAddress": {"address": a}} for a in to_raw],
        }
        if cc_raw:
            msg_obj["ccRecipients"] = [{"emailAddress": {"address": a}} for a in cc_raw]
        if bcc_raw:
            msg_obj["bccRecipients"] = [{"emailAddress": {"address": a}} for a in bcc_raw]

        att_paths = payload.get("attachments") or []
        if att_paths:
            g_atts = []
            for p in att_paths:
                if os.path.isfile(p):
                    with open(p, "rb") as af:
                        b64 = base64.b64encode(af.read()).decode("ascii")
                    ctype = mimetypes.guess_type(p)[0] or "application/octet-stream"
                    g_atts.append({
                        "@odata.type": "#microsoft.graph.fileAttachment",
                        "name": os.path.basename(p),
                        "contentType": ctype,
                        "contentBytes": b64,
                    })
            if g_atts:
                msg_obj["attachments"] = g_atts

        req_body = json.dumps({"message": msg_obj, "saveToSentItems": True}).encode("utf-8")
        c, resp = http_json(
            "POST",
            "https://graph.microsoft.com/v1.0/me/sendMail",
            headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
            data_bytes=req_body,
        )
        if c in (200, 202):
            return True, "Message sent!"
        err_msg = (resp.get("error") or {}).get("message") if isinstance(resp.get("error"), dict) else resp.get("error_description", "Send failed")
        return False, f"Send failed ({c}): {str(err_msg)[:160]}"

    def set_message_read(self, cfg, msg_id, is_read: bool):
        conn = sqlite3.connect(DB_PATH)
        conn.execute("UPDATE messages SET is_read=? WHERE id=?", (1 if is_read else 0, msg_id))
        conn.commit()
        conn.close()

        if msg_id != "welcome_oe_cde_1":
            def _patch_graph():
                token, _ = self.oauth.get_valid_access_token(cfg)
                if token:
                    http_json(
                        "PATCH",
                        f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}",
                        {"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
                        json.dumps({"isRead": bool(is_read)}).encode("utf-8"),
                    )
            threading.Thread(target=_patch_graph, daemon=True).start()
        return True, ("Message marked as read." if is_read else "Message marked as unread.")

    def delete_message(self, cfg, msg_id):
        conn = sqlite3.connect(DB_PATH)
        row = conn.execute("SELECT folder_id FROM messages WHERE id=?", (msg_id,)).fetchone()
        conn.execute("DELETE FROM messages WHERE id=?", (msg_id,))
        if row:
            conn.execute("UPDATE folders SET total_count = MAX(0, total_count - 1) WHERE id=?", (row[0],))
        conn.commit()
        conn.close()

        if msg_id == "welcome_oe_cde_1":
            return True, "Deleted welcome message"

        token, _ = self.oauth.get_valid_access_token(cfg)
        if token:
            http_json(
                "POST",
                f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/move",
                headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
                data_bytes=json.dumps({"destinationId": "deleteditems"}).encode("utf-8"),
            )
        return True, "Message moved to Deleted Items"

    def open_or_save_single_attachment(self, cfg, msg_id, att_id, att_name, dest_dir, do_open=True):
        os.makedirs(dest_dir, exist_ok=True)
        safe_name = os.path.basename(att_name or "attachment.bin") or "attachment.bin"
        outp = os.path.join(dest_dir, safe_name)

        # Download from Graph unless already identical or local welcome message
        token, err = self.oauth.get_valid_access_token(cfg)
        if not token:
            return False, err

        hdrs = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
        wrote = False
        if att_id:
            c, aobj = http_json(
                "GET",
                f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/attachments/{urllib.parse.quote(att_id)}",
                headers=hdrs,
            )
            if c == 200 and isinstance(aobj, dict) and "contentBytes" in aobj:
                with open(outp, "wb") as f:
                    f.write(base64.b64decode(aobj["contentBytes"]))
                wrote = True

        if not wrote:
            c, adata = http_json(
                "GET",
                f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/attachments",
                headers=hdrs,
            )
            if c != 200:
                return False, f"Attachment download failed ({c})"
            for a in adata.get("value", []):
                if a.get("isInline") or "contentBytes" not in a:
                    continue
                if (att_id and a.get("id") == att_id) or (a.get("name") == att_name):
                    with open(outp, "wb") as f:
                        f.write(base64.b64decode(a["contentBytes"]))
                    wrote = True
                    break

        if not wrote:
            return False, f"Could not download attachment '{safe_name}'."

        if do_open:
            launch_cde_utility_for_file(outp)
            return True, f"Saved '{safe_name}' to {dest_dir} and opened."
        return True, f"Saved '{safe_name}' to {dest_dir}"

    def save_attachments(self, cfg, msg_id, dest_dir):
        os.makedirs(dest_dir, exist_ok=True)
        token, err = self.oauth.get_valid_access_token(cfg)
        if not token:
            return False, err
        c, adata = http_json(
            "GET",
            f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/attachments",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        if c != 200:
            return False, f"Attachment download failed ({c})"
        saved = []
        for a in adata.get("value", []):
            if a.get("isInline") or "contentBytes" not in a:
                continue
            fname = os.path.basename(a.get("name") or "attachment.bin")
            outp = os.path.join(dest_dir, fname)
            with open(outp, "wb") as f:
                f.write(base64.b64decode(a["contentBytes"]))
            saved.append(outp)
        if not saved:
            return False, "No downloadable file attachments on this message."
        return True, f"Saved {len(saved)} attachment(s) to {dest_dir}"

    def respond_to_invite(self, cfg, msg_id: str, event_id: str, action: str):
        act_map = {
            "accept": ("accept", "Accepted", "Accepted meeting invitation (reply sent to organizer)."),
            "tentative": ("tentativelyAccept", "Tentative", "Tentatively accepted invitation (reply sent to organizer)."),
            "decline": ("decline", "Declined", "Declined meeting invitation (reply sent to organizer)."),
        }
        if action not in act_map:
            return False, f"Unknown RSVP action: {action}", ""
        endpoint, st_lbl, ok_msg = act_map[action]

        token, err = self.oauth.get_valid_access_token(cfg)
        if not token:
            return False, err or "Not signed in", ""

        hdrs = {"Authorization": f"Bearer {token}", "Content-Type": "application/json"}
        body_bytes = json.dumps({"sendResponse": True}).encode("utf-8")

        urls = []
        if msg_id and msg_id != "welcome_oe_cde_1":
            urls.append(f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/microsoft.graph.eventMessageRequest/{endpoint}")
            urls.append(f"https://graph.microsoft.com/v1.0/me/messages/{urllib.parse.quote(msg_id)}/{endpoint}")
        if event_id:
            urls.append(f"https://graph.microsoft.com/v1.0/me/events/{urllib.parse.quote(event_id)}/{endpoint}")

        succeeded = False
        last_err = "Invitation response failed"
        for u in urls:
            c, resp = http_json("POST", u, headers=hdrs, data_bytes=body_bytes)
            if c in (200, 202, 204):
                succeeded = True
                break
            if isinstance(resp, dict):
                last_err = ((resp.get("error") or {}).get("message") if isinstance(resp.get("error"), dict) else resp.get("error_description")) or f"HTTP {c}"

        if not succeeded:
            return False, f"RSVP failed: {str(last_err)[:150]}", ""

        # Update cached meeting status & signal companion dtcm to refresh immediately!
        bar_lbl = f"Calendar [{st_lbl}]: Reply sent"
        try:
            conn = sqlite3.connect(DB_PATH)
            row = conn.execute("SELECT meeting_json FROM messages WHERE id=?", (msg_id,)).fetchone()
            if row and row[0]:
                mobj = json.loads(row[0])
                mobj["status_label"] = st_lbl
                when_s = mobj.get("when_disp") or "Meeting Invitation"
                loc_s = mobj.get("location") or ""
                bar_lbl = f"Calendar [{st_lbl}]: {when_s}" + (f"  ({loc_s})" if loc_s else "")
                conn.execute("UPDATE messages SET meeting_json=? WHERE id=?", (json.dumps(mobj), msg_id))
                conn.commit()
            conn.close()
        except Exception:
            pass

        try:
            ensure_dirs()
            with open(DIRTY_STAMP_PATH, "w", encoding="utf-8") as sf:
                sf.write(str(time.time()))
        except Exception:
            pass

        return True, ok_msg, bar_lbl


def format_meeting_window(start_obj, end_obj, all_day: bool = False):
    s_raw = (start_obj or {}).get("dateTime") or ""
    e_raw = (end_obj or {}).get("dateTime") or ""
    if not s_raw:
        return "", ""
    if all_day:
        d_iso = s_raw[:10]
        return d_iso, f"{d_iso} (All Day)"
    try:
        s_clean = s_raw.split(".")[0].replace("Z", "")
        sdt = datetime.fromisoformat(s_clean).replace(tzinfo=timezone.utc).astimezone()
        d_iso = sdt.date().isoformat()
        sh = sdt.hour % 12 or 12
        s_ampm = "AM" if sdt.hour < 12 else "PM"
        s_str = f"{sdt.strftime('%a %Y-%m-%d')}, {sh}:{sdt.minute:02d} {s_ampm}"
        if e_raw:
            e_clean = e_raw.split(".")[0].replace("Z", "")
            edt = datetime.fromisoformat(e_clean).replace(tzinfo=timezone.utc).astimezone()
            eh = edt.hour % 12 or 12
            e_ampm = "AM" if edt.hour < 12 else "PM"
            return d_iso, f"{s_str} - {eh}:{edt.minute:02d} {e_ampm}"
        return d_iso, s_str
    except Exception:
        return s_raw[:10], s_raw[:16]


def parse_ics_summary(ics_path: str):
    try:
        raw = open(ics_path, "r", encoding="utf-8", errors="replace").read()
        unfolded = re.sub(r"\r?\n[ \t]", "", raw)
        in_ev = False
        props = {}
        for ln in unfolded.splitlines():
            u = ln.strip().upper()
            if u == "BEGIN:VEVENT":
                in_ev = True
                continue
            if u == "END:VEVENT":
                break
            if in_ev and ":" in ln:
                k, v = ln.split(":", 1)
                kb = k.split(";", 1)[0].upper()
                if kb not in props:
                    props[kb] = v.replace("\\,", ",").strip()
        s_val = props.get("DTSTART", "")
        loc = props.get("LOCATION", "")
        if len(s_val) >= 8 and s_val[:8].isdigit():
            d_iso = f"{s_val[0:4]}-{s_val[4:6]}-{s_val[6:8]}"
            if "T" in s_val and len(s_val) >= 15:
                clean = s_val.rstrip("Z")[:15]
                dt = datetime.strptime(clean, "%Y%m%dT%H%M%S")
                if s_val.endswith("Z"):
                    dt = dt.replace(tzinfo=timezone.utc).astimezone()
                h12 = dt.hour % 12 or 12
                ap = "AM" if dt.hour < 12 else "PM"
                return {"date": dt.date().isoformat(), "when_disp": f"{dt.strftime('%a %Y-%m-%d')}, {h12}:{dt.minute:02d} {ap}", "location": loc}
            return {"date": d_iso, "when_disp": f"{d_iso} (All Day)", "location": loc}
    except Exception:
        pass
    return None


def open_in_calendar_companion(date_str: str = "", ics_path: str = ""):
    ensure_dirs()
    lbin = os.path.expanduser("~/.local/bin")
    dtcm_bin = os.path.join(lbin, "dtcm")
    if not os.path.isfile(dtcm_bin):
        dtcm_bin = os.path.join(lbin, "cde-calendar")

    # Check if dtcm / cde-calendar is already running for this user
    uid_s = str(os.getuid())
    r1 = subprocess.run(["pgrep", "-u", uid_s, "-x", "dtcm"], capture_output=True)
    r2 = subprocess.run(["pgrep", "-u", uid_s, "-x", "cde-calendar"], capture_output=True)
    if r1.returncode == 0 or r2.returncode == 0:
        payload = {"date": date_str or datetime.now().strftime("%Y-%m-%d"), "ics_path": ics_path or ""}
        tmp_g = GOTO_JSON_PATH + ".tmp"
        with open(tmp_g, "w", encoding="utf-8") as gf:
            json.dump(payload, gf)
        os.replace(tmp_g, GOTO_JSON_PATH)
        return "Switched to Calendar (dtcm)."

    cmd = [dtcm_bin]
    if ics_path and os.path.isfile(ics_path):
        cmd.extend(["--import-ics", ics_path])
    elif date_str:
        cmd.extend(["--date", date_str])
    env = os.environ.copy()
    env["PATH"] = f"/usr/dt/bin:{lbin}:{env.get('PATH', '')}"
    subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    return "Launched Calendar (dtcm)."


def launch_cde_utility_for_file(file_path: str):
    low = file_path.lower()
    lbin = os.path.expanduser("~/.local/bin")
    archive_exts = (
        ".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tbz",
        ".tar.xz", ".txz", ".tar.zst", ".tzst", ".7z", ".rar", ".gz", ".bz2", ".xz", ".z",
    )
    note_exts = (".rtf", ".txt", ".md", ".markdown", ".log", ".ini", ".cfg", ".conf", ".csv")

    pic_exts = (
        ".png", ".jpg", ".jpeg", ".jpe", ".jfif", ".bmp", ".dib", ".gif",
        ".heic", ".heif", ".avif", ".jxl", ".webp", ".tif", ".tiff",
        ".svg", ".svgz", ".ico", ".icns", ".ppm", ".pgm", ".pbm", ".pnm",
        ".xpm", ".xbm", ".tga", ".targa", ".pcx", ".qoi", ".dds", ".psd",
    )
    if low.endswith(".ics"):
        open_in_calendar_companion(ics_path=file_path)
        return
    if low.endswith(pic_exts) and os.path.isfile(os.path.join(lbin, "dtpics")):
        cmd = [os.path.join(lbin, "dtpics"), file_path]
    elif low.endswith(".pdf") and os.path.isfile(os.path.join(lbin, "dtreader")):
        cmd = [os.path.join(lbin, "dtreader"), file_path]
    elif low.endswith(archive_exts) and os.path.isfile(os.path.join(lbin, "dtzip")):
        cmd = [os.path.join(lbin, "dtzip"), file_path]
    elif low.endswith(note_exts) and os.path.isfile(os.path.join(lbin, "dtnotes")):
        cmd = [os.path.join(lbin, "dtnotes"), file_path]
    elif os.path.isfile("/usr/dt/bin/dtaction"):
        cmd = ["/usr/dt/bin/dtaction", "Open", file_path]
    else:
        cmd = ["xdg-open", file_path]

    env = os.environ.copy()
    env["PATH"] = f"/usr/dt/bin:{lbin}:{env.get('PATH', '')}"
    subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def snapshot_folders():
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        """SELECT f.id, f.name, f.well_known,
                  (SELECT COUNT(*) FROM messages m WHERE m.folder_id=f.id AND m.is_read=0) AS local_unread,
                  f.unread_count,
                  (SELECT COUNT(*) FROM messages m WHERE m.folder_id=f.id) AS local_total,
                  f.total_count
           FROM folders f ORDER BY f.sort_order ASC, f.name COLLATE NOCASE ASC"""
    ).fetchall()
    conn.close()
    res = []
    for fid, name, wk, lunread, funread, ltot, ftot in rows:
        unread = lunread if ltot > 0 else funread
        total = max(ltot, ftot)
        res.append({"id": fid, "name": name, "well_known": wk, "unread": unread, "total": total})
    return res


def snapshot_messages(folder_id):
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        """SELECT id, from_name, from_addr, to_addrs, cc_addrs, subject, date_disp,
                  is_read, importance, has_attachments
           FROM messages WHERE folder_id=? ORDER BY date_ts DESC""",
        (folder_id,),
    ).fetchall()
    conn.close()
    return [
        {
            "id": r[0], "from_name": r[1], "from_addr": r[2], "to": r[3], "cc": r[4],
            "subject": r[5], "date": r[6], "unread": (r[7] == 0),
            "importance": r[8], "att": bool(r[9]),
        }
        for r in rows
    ]


def write_reading_pane_html(msg_dict, theme="cde"):
    """Writes a self-contained HTML document for the embedded WebKit2 Reading Pane."""
    ensure_dirs()
    dark = (theme == "dark")
    bg = "#15181E" if dark else "#FFF7E9"
    fg = "#EDF2F7" if dark else "#000000"
    link_col = "#63B3ED" if dark else "#185ABD"

    base_css = f"""<style>
      html {{ background: {bg}; color: {fg}; }}
      body {{
        margin: 10px 14px;
        font-family: "DejaVu Sans", "Liberation Sans", sans-serif;
        font-size: 11pt;
        line-height: 1.45;
        word-wrap: break-word;
        overflow-wrap: anywhere;
      }}
      a {{ color: {link_col}; }}
      img {{ max-width: 100%; height: auto; }}
      pre {{ white-space: pre-wrap; font-family: "DejaVu Sans Mono", monospace; }}
      blockquote {{ border-left: 3px solid #718BA5; margin: 6px 0; padding-left: 10px; }}
    </style>"""

    raw_html = (msg_dict.get("raw_html") or "").strip()
    if raw_html:
        if re.search(r"<head[^>]*>", raw_html, flags=re.IGNORECASE):
            doc = re.sub(r"(<head[^>]*>)", r"\1" + base_css, raw_html, count=1, flags=re.IGNORECASE)
        elif re.search(r"<html[^>]*>", raw_html, flags=re.IGNORECASE):
            doc = re.sub(r"(<html[^>]*>)", r"\1<head><meta charset='utf-8'/>" + base_css + "</head>", raw_html, count=1, flags=re.IGNORECASE)
        else:
            doc = f"<!DOCTYPE html><html><head><meta charset='utf-8'/>{base_css}</head><body>{raw_html}</body></html>"
    else:
        esc = html.escape(msg_dict.get("body") or "")
        esc = re.sub(
            r"(https?://[^\s<>\"']+)",
            r'<a href="\1">\1</a>',
            esc,
        )
        doc = (
            f"<!DOCTYPE html><html><head><meta charset='utf-8'/>{base_css}</head>"
            f"<body><div style='white-space:pre-wrap;'>{esc}</div></body></html>"
        )

    out_path = os.path.join(CACHE_DIR, "reading_pane.html")
    tmp_path = out_path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8", errors="replace") as f:
        f.write(doc)
    os.chmod(tmp_path, 0o600)
    os.replace(tmp_path, out_path)
    return out_path


def get_cached_message(msg_id, engine=None, cfg=None):
    conn = sqlite3.connect(DB_PATH)
    r = conn.execute(
        """SELECT id, folder_id, from_name, from_addr, to_addrs, cc_addrs, subject,
                  date_disp, is_read, importance, has_attachments, body_text,
                  body_html, body_loaded, attachments_json, meeting_json
           FROM messages WHERE id=?""",
        (msg_id,),
    ).fetchone()
    was_unread = bool(r and r[8] == 0)
    if was_unread:
        conn.execute("UPDATE messages SET is_read=1 WHERE id=?", (msg_id,))
        conn.commit()
    conn.close()
    if was_unread and engine is not None and cfg is not None and r[13]:
        # Message body was already cached locally (e.g. previously marked unread), sync isRead=True to Graph
        engine.set_message_read(cfg, msg_id, True)
    if not r:
        return None
    try:
        atts = json.loads(r[14] or "[]")
    except Exception:
        atts = []
    raw_mjson = r[15] or ""
    try:
        mtg = json.loads(raw_mjson) if raw_mjson else {"is_meeting": False}
    except Exception:
        mtg = {"is_meeting": False}
    return {
        "id": r[0], "folder_id": r[1], "from_name": r[2], "from_addr": r[3],
        "to": r[4], "cc": r[5], "subject": r[6], "date": r[7],
        "unread": (r[8] == 0), "importance": r[9], "att": bool(r[10]),
        "body": r[11] or "", "has_html": bool(r[12]), "body_loaded": bool(r[13]),
        "attachments": atts, "meeting": mtg, "meeting_checked": bool(raw_mjson),
        "raw_html": r[12] or "",
    }


def main():
    init_db()
    engine = MailEngine()
    cfg = load_config()
    tok = load_tokens()

    folders = snapshot_folders()
    first_fid = folders[0]["id"] if folders else ""
    emit({
        "event": "init",
        "config": cfg,
        "signed_in": bool(tok.get("access_token")),
        "folders": folders,
        "initial_folder": first_fid,
        "messages": snapshot_messages(first_fid) if first_fid else [],
    })

    def run_bg_sync(selected_folder=None):
        if not engine.sync_lock.acquire(blocking=False):
            return
        try:
            c = load_config()
            token, err = engine.oauth.get_valid_access_token(c)
            if not token:
                emit({"event": "sync_done", "ok": False, "status": err})
                return
            emit({"event": "sync_progress", "status": "Synchronizing folders & mail with Office 365..."})
            ok, st = engine.sync_graph(c, token, active_folder_id=selected_folder)
            flist = snapshot_folders()
            target_f = selected_folder
            if not target_f or not any(f["id"] == target_f for f in flist):
                target_f = flist[0]["id"] if flist else ""
            emit({
                "event": "sync_done",
                "ok": ok,
                "status": st,
                "config": load_config(),
                "folders": flist,
                "folder_id": target_f,
                "messages": snapshot_messages(target_f) if target_f else [],
            })
        finally:
            engine.sync_lock.release()

    if tok.get("access_token"):
        threading.Thread(target=run_bg_sync, daemon=True).start()

    for raw_line in sys.stdin:
        raw_line = raw_line.strip()
        if not raw_line:
            continue
        try:
            cmd = json.loads(raw_line)
        except Exception:
            continue

        op = cmd.get("op")
        if op == "select_folder":
            fid = cmd.get("folder_id", "")
            msgs = snapshot_messages(fid)
            emit({"event": "folder_messages", "folder_id": fid, "messages": msgs, "folders": snapshot_folders()})
            if not fid.startswith("local_") and load_tokens().get("access_token"):
                def _lazy_folder(f_id=fid):
                    c = load_config()
                    tk, _ = engine.oauth.get_valid_access_token(c)
                    if tk:
                        engine._sync_graph_folder_messages(f_id, {"Authorization": f"Bearer {tk}", "Accept": "application/json"})
                        emit({"event": "folder_messages", "folder_id": f_id, "messages": snapshot_messages(f_id), "folders": snapshot_folders()})
                threading.Thread(target=_lazy_folder, daemon=True).start()

        elif op == "open_message":
            mid = cmd.get("id", "")
            c_cur = load_config()
            m = get_cached_message(mid, engine=engine, cfg=c_cur)
            if m:
                hp = write_reading_pane_html(m, c_cur.get("theme", "cde"))
                payload = {k: v for k, v in m.items() if k != "raw_html"}
                payload["html_path"] = hp
                emit({"event": "message_detail", "message": payload, "folders": snapshot_folders()})
                if (not m["body_loaded"] or not m.get("meeting_checked") or (m["att"] and not m["attachments"])) and mid != "welcome_oe_cde_1":
                    def _load_full(m_id=mid):
                        cfg_now = load_config()
                        ok, err = engine.fetch_graph_message_body(m_id, cfg_now)
                        m2 = get_cached_message(m_id)
                        if m2:
                            hp2 = write_reading_pane_html(m2, cfg_now.get("theme", "cde"))
                            p2 = {k: v for k, v in m2.items() if k != "raw_html"}
                            p2["html_path"] = hp2
                            emit({"event": "message_detail", "message": p2, "folders": snapshot_folders()})
                        elif err:
                            emit({"event": "status", "status": err})
                    threading.Thread(target=_load_full, daemon=True).start()

        elif op == "open_html_browser":
            mid = cmd.get("id", "")
            m = get_cached_message(mid)
            if m and m.get("raw_html"):
                hp = os.path.join(CACHE_DIR, "preview_message.html")
                with open(hp, "w", encoding="utf-8") as f:
                    f.write(m["raw_html"])
                open_url_in_browser(f"file://{hp}")
                emit({"event": "status", "status": "Opened HTML message in web browser"})

        elif op == "open_attachment":
            mid = cmd.get("id", "")
            att_id = cmd.get("att_id", "")
            att_name = cmd.get("att_name", "attachment.bin")
            do_open = bool(cmd.get("open", True))
            dest = cmd.get("dest_dir") or os.path.expanduser("~/Downloads")
            def _do_att(m_id=mid, a_id=att_id, a_name=att_name, d_dir=dest, opn=do_open):
                ok, st = engine.open_or_save_single_attachment(load_config(), m_id, a_id, a_name, d_dir, opn)
                emit({"event": "status", "status": st, "alert": (not ok)})
            threading.Thread(target=_do_att, daemon=True).start()

        elif op == "save_attachments":
            mid = cmd.get("id", "")
            dest = cmd.get("dest_dir") or os.path.expanduser("~/Downloads")
            threading.Thread(
                target=lambda: emit({"event": "status", "status": engine.save_attachments(load_config(), mid, dest)[1], "alert": True}),
                daemon=True,
            ).start()

        elif op == "sync":
            if not load_tokens().get("access_token"):
                engine.oauth.start_one_click_browser_login(load_config(), lambda: threading.Thread(target=run_bg_sync, daemon=True).start())
            else:
                threading.Thread(target=run_bg_sync, args=(cmd.get("folder_id"),), daemon=True).start()

        elif op == "login_browser":
            new_cfg = load_config()
            for k in ("display_name", "email", "tenant", "client_id"):
                if k in cmd:
                    new_cfg[k] = cmd[k]
            save_config(new_cfg)
            engine.oauth.start_one_click_browser_login(new_cfg, lambda: threading.Thread(target=run_bg_sync, daemon=True).start())

        elif op == "save_config":
            new_cfg = load_config()
            for k in ("display_name", "email", "tenant", "client_id", "sig_enabled", "sig_skip_replies", "signature", "theme", "font_pt", "split_promille"):
                if k in cmd:
                    new_cfg[k] = cmd[k]
            save_config(new_cfg)
            emit({"event": "config_saved", "config": new_cfg, "status": "Settings saved."})

        elif op == "sign_out":
            if os.path.exists(TOKEN_PATH):
                os.remove(TOKEN_PATH)
            emit({"event": "signed_out", "status": "Signed out of Office 365."})

        elif op == "send_mail":
            def _do_send(p=cmd):
                emit({"event": "status", "status": "Sending message via Office 365..."})
                ok, st = engine.send_mail(load_config(), p)
                emit({"event": "send_result", "ok": ok, "status": st, "req_id": p.get("req_id", 0)})
                if ok:
                    threading.Thread(target=run_bg_sync, daemon=True).start()
            threading.Thread(target=_do_send, daemon=True).start()

        elif op == "set_read":
            mid = cmd.get("id", "")
            is_read = bool(cmd.get("read", False))
            if mid:
                _, st = engine.set_message_read(load_config(), mid, is_read)
                emit({
                    "event": "read_state_changed",
                    "id": mid,
                    "unread": (not is_read),
                    "folders": snapshot_folders(),
                    "status": st,
                })

        elif op == "delete_msg":
            mid = cmd.get("id", "")
            fid = cmd.get("folder_id", "")
            def _do_del():
                _, st = engine.delete_message(load_config(), mid)
                emit({
                    "event": "folder_messages",
                    "folder_id": fid,
                    "messages": snapshot_messages(fid),
                    "folders": snapshot_folders(),
                    "status": st,
                })
            threading.Thread(target=_do_del, daemon=True).start()

        elif op == "rsvp_invite":
            mid = cmd.get("id", "")
            ev_id = cmd.get("event_id", "")
            act = cmd.get("action", "accept")
            fid = cmd.get("folder_id", "")
            def _do_rsvp(m_id=mid, e_id=ev_id, a=act, f_id=fid):
                emit({"event": "status", "status": f"Sending '{a}' reply to organizer..."})
                ok, st, bar_lbl = engine.respond_to_invite(load_config(), m_id, e_id, a)
                emit({"event": "rsvp_done", "ok": ok, "status": st, "bar_label": bar_lbl})
                if ok and f_id and not f_id.startswith("local_"):
                    c_now = load_config()
                    tk, _ = engine.oauth.get_valid_access_token(c_now)
                    if tk:
                        engine._sync_graph_folder_messages(f_id, {"Authorization": f"Bearer {tk}", "Accept": "application/json"})
                        emit({"event": "folder_messages", "folder_id": f_id, "messages": snapshot_messages(f_id), "folders": snapshot_folders(), "status": st})
            threading.Thread(target=_do_rsvp, daemon=True).start()

        elif op == "open_calendar":
            d_str = cmd.get("date", "")
            ics_p = cmd.get("ics_path", "")
            st = open_in_calendar_companion(d_str, ics_p)
            emit({"event": "status", "status": st})

        elif op == "quit":
            break


if __name__ == "__main__":
    main()
