#!/usr/bin/env python3
"""
cde-calendar-backend: Native Office 365 Microsoft Graph Calendar Engine for CDE dtcm.

Shares the exact same OAuth 2.0 connection (`~/.config/cde-mail/tokens.json` and
`~/.config/cde-mail/config.json`) as CDE dtMail (`cde-mail`), supporting:
- Expanded `calendarView` sync with organizer, attendees, and `responseStatus`
- Creating & updating events with invited attendees (`POST`/`PATCH /v1.0/me/events`)
- Replying to incoming meeting invitations (`Accept`, `Tentative`, `Decline` + comment)
- `.ics` invitation file parsing for pre-filling/importing external invites
- Live companion synchronization with `dtMail` via `calendar_dirty.stamp` and `calendar_goto.json`
"""
import base64
from datetime import date, datetime, time as dtime, timedelta, timezone
import fcntl
import hashlib
import json
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
LOCK_PATH = os.path.join("/tmp", f"cde-m365-tok-{os.getuid()}.lock")
CAL_DB_PATH = os.path.join(CACHE_DIR, "calendar_cache.db")
DIRTY_STAMP_PATH = os.path.join(CACHE_DIR, "calendar_dirty.stamp")
GOTO_JSON_PATH = os.path.join(CACHE_DIR, "calendar_goto.json")

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


def ensure_dirs():
    os.makedirs(CONFIG_DIR, mode=0o700, exist_ok=True)
    os.makedirs(CACHE_DIR, mode=0o700, exist_ok=True)


def load_config():
    ensure_dirs()
    cfg = {
        "display_name": "",
        "email": "",
        "tenant": DEFAULT_M365_TENANT,
        "client_id": "",
        "mailbox_tz": "UTC",
    }
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                cfg.update(json.load(f))
        except Exception:
            pass
    return cfg


def save_config_fields(updates):
    ensure_dirs()
    cur = {}
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                cur = json.load(f)
        except Exception:
            cur = {}
    cur.update(updates)
    tmp = CONFIG_PATH + ".cal.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cur, f, indent=2)
    os.chmod(tmp, 0o600)
    os.replace(tmp, CONFIG_PATH)


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
    tmp = TOKEN_PATH + ".cal.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(tok, f, indent=2)
    os.chmod(tmp, 0o600)
    os.replace(tmp, TOKEN_PATH)


def token_has_calendar_scope(access_token):
    if not access_token or "." not in access_token:
        return False
    try:
        seg = access_token.split(".")[1]
        seg += "=" * (-len(seg) % 4)
        claims = json.loads(base64.urlsafe_b64decode(seg).decode("utf-8", errors="replace"))
        scp = (claims.get("scp") or "").lower()
        return "calendars.readwrite" in scp or "calendars.read" in scp
    except Exception:
        return True


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


def find_webauth_helper():
    p1 = os.path.expanduser("~/.local/bin/cde-mail-webauth")
    if os.path.exists(p1):
        return p1
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "cde-mail-webauth.py")


def init_db():
    ensure_dirs()
    conn = sqlite3.connect(CAL_DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS events (
            id TEXT PRIMARY KEY,
            series_master_id TEXT DEFAULT '',
            subject TEXT DEFAULT '',
            location TEXT DEFAULT '',
            organizer TEXT DEFAULT '',
            organizer_email TEXT DEFAULT '',
            is_organizer INTEGER DEFAULT 1,
            response_requested INTEGER DEFAULT 0,
            response_status TEXT DEFAULT 'organizer',
            attendees_str TEXT DEFAULT '',
            body_preview TEXT DEFAULT '',
            start_date TEXT NOT NULL,      -- YYYY-MM-DD local
            end_date TEXT NOT NULL,        -- YYYY-MM-DD local (inclusive)
            start_min INTEGER DEFAULT 0,   -- minutes from midnight local (0..1439)
            end_min INTEGER DEFAULT 60,
            start_disp TEXT DEFAULT '',    -- e.g. '9:00a' or 'All Day'
            time_range TEXT DEFAULT '',    -- e.g. '9:00 AM - 10:00 AM'
            is_all_day INTEGER DEFAULT 0,
            show_as TEXT DEFAULT 'busy',
            start_ts INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_ev_dates ON events(start_date, end_date, start_min);
    """)
    existing_cols = {r[1] for r in conn.execute("PRAGMA table_info(events)").fetchall()}
    migrations = [
        ("organizer_email", "TEXT DEFAULT ''"),
        ("is_organizer", "INTEGER DEFAULT 1"),
        ("response_requested", "INTEGER DEFAULT 0"),
        ("response_status", "TEXT DEFAULT 'organizer'"),
        ("attendees_str", "TEXT DEFAULT ''"),
    ]
    for col_name, col_def in migrations:
        if col_name not in existing_cols:
            conn.execute(f"ALTER TABLE events ADD COLUMN {col_name} {col_def}")
    conn.commit()
    conn.close()


class SharedOAuthCalendar:
    def __init__(self):
        self.login_lock = threading.Lock()

    def effective_params(self, cfg, tok=None):
        custom_cid = (cfg.get("client_id") or (tok or {}).get("client_id") or "").strip()
        if not custom_cid or custom_cid == DEFAULT_M365_CLIENT_ID:
            return DEFAULT_M365_CLIENT_ID, DEFAULT_M365_TENANT, DEFAULT_M365_REDIRECT
        ten = (cfg.get("tenant") or (tok or {}).get("tenant") or "").strip() or DEFAULT_M365_TENANT
        redirect = (tok or {}).get("redirect_uri") or DEFAULT_M365_REDIRECT
        return custom_cid, ten, redirect

    def get_calendar_token(self, cfg, allow_interactive=False):
        lock_f = open(LOCK_PATH, "w")
        try:
            fcntl.flock(lock_f, fcntl.LOCK_EX)
            tok = load_tokens()
            if not tok:
                return None, "Not signed in — click Browse -> Connect Office 365 (or sign in via dtMail)."

            acc = tok.get("access_token", "")
            if acc and tok.get("expires_at", 0) > time.time() + 90 and token_has_calendar_scope(acc):
                return acc, None

            refresh_tok = tok.get("refresh_token", "")
            cid, ten, _ = self.effective_params(cfg, tok)
            if refresh_tok:
                token_url = f"https://login.microsoftonline.com/{urllib.parse.quote(ten)}/oauth2/v2.0/token"
                code, resp = http_json("POST", token_url, form_dict={
                    "client_id": cid,
                    "grant_type": "refresh_token",
                    "refresh_token": refresh_tok,
                    "scope": DEFAULT_M365_SCOPES,
                })
                if code == 200 and "access_token" in resp:
                    new_acc = resp["access_token"]
                    tok["access_token"] = new_acc
                    if "refresh_token" in resp:
                        tok["refresh_token"] = resp["refresh_token"]
                    tok["expires_at"] = time.time() + int(resp.get("expires_in", 3600))
                    save_tokens(tok)
                    if token_has_calendar_scope(new_acc):
                        return new_acc, None
        finally:
            fcntl.flock(lock_f, fcntl.LOCK_UN)
            lock_f.close()

        if allow_interactive:
            return self.run_interactive_consent(cfg)
        return None, "NEEDS_CALENDAR_CONSENT"

    def run_interactive_consent(self, cfg):
        if not self.login_lock.acquire(blocking=False):
            return None, "Sign-in window already open..."
        try:
            cid, ten, redirect_uri = self.effective_params(cfg, load_tokens())
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
            }
            if cfg.get("email"):
                params["login_hint"] = cfg["email"].strip()

            auth_url = (
                f"https://login.microsoftonline.com/{urllib.parse.quote(ten)}/oauth2/v2.0/authorize?"
                + urllib.parse.urlencode(params)
            )
            emit({"event": "status", "status": "Granting Calendar access for your Office 365 connection..."})
            proc = subprocess.run(
                ["/usr/bin/python3", find_webauth_helper(), auth_url, redirect_uri],
                capture_output=True,
                text=True,
                timeout=600,
            )
            lines = [ln.strip() for ln in (proc.stdout or "").splitlines() if ln.strip()]
            if not lines:
                return None, "Calendar authorization cancelled."
            res = json.loads(lines[-1])
            if not res.get("code"):
                return None, res.get("error") or "Calendar authorization cancelled."

            token_url = f"https://login.microsoftonline.com/{urllib.parse.quote(ten)}/oauth2/v2.0/token"
            c_http, tok_resp = http_json("POST", token_url, form_dict={
                "client_id": cid,
                "grant_type": "authorization_code",
                "code": res["code"],
                "redirect_uri": redirect_uri,
                "code_verifier": verifier,
                "scope": DEFAULT_M365_SCOPES,
            })
            if c_http != 200 or "access_token" not in tok_resp:
                desc = tok_resp.get("error_description") or tok_resp.get("error") or "Token exchange failed"
                return None, desc.splitlines()[0][:160]

            tok_data = {
                "access_token": tok_resp["access_token"],
                "refresh_token": tok_resp.get("refresh_token", ""),
                "expires_at": time.time() + int(tok_resp.get("expires_in", 3600)),
                "client_id": cid,
                "tenant": ten,
                "redirect_uri": redirect_uri,
            }
            save_tokens(tok_data)

            hdrs = {"Authorization": f"Bearer {tok_resp['access_token']}", "Accept": "application/json"}
            cm, me = http_json("GET", "https://graph.microsoft.com/v1.0/me?$select=displayName,mail,userPrincipalName", headers=hdrs)
            updates = {}
            if cm == 200:
                if me.get("mail") or me.get("userPrincipalName"):
                    updates["email"] = me.get("mail") or me.get("userPrincipalName")
                if me.get("displayName"):
                    updates["display_name"] = me.get("displayName")
            cs, mb = http_json("GET", "https://graph.microsoft.com/v1.0/me/mailboxSettings", headers=hdrs)
            if cs == 200 and mb.get("timeZone"):
                updates["mailbox_tz"] = mb["timeZone"]
            if updates:
                save_config_fields(updates)

            return tok_resp["access_token"], None
        except Exception as e:
            return None, str(e)[:160]
        finally:
            self.login_lock.release()


def fmt_ampm_short(dt_local):
    h = dt_local.hour
    m = dt_local.minute
    ampm = "a" if h < 12 else "p"
    h12 = h % 12 or 12
    if m == 0:
        return f"{h12}{ampm}"
    return f"{h12}:{m:02d}{ampm}"


def fmt_ampm_long(dt_local):
    h = dt_local.hour
    m = dt_local.minute
    ampm = "AM" if h < 12 else "PM"
    h12 = h % 12 or 12
    return f"{h12}:{m:02d} {ampm}"


def parse_graph_dt_utc(dt_obj):
    raw = (dt_obj or {}).get("dateTime") or ""
    raw_clean = raw.split(".")[0].replace("Z", "")
    dt_utc = datetime.fromisoformat(raw_clean).replace(tzinfo=timezone.utc)
    return dt_utc.astimezone()


def parse_ics_file(ics_path: str) -> dict | None:
    """Extract VEVENT fields from an .ics invitation file for pre-filling dtcm."""
    if not ics_path or not os.path.isfile(ics_path):
        return None
    try:
        raw = open(ics_path, "r", encoding="utf-8", errors="replace").read()
        # Unfold RFC 5545 continuation lines
        unfolded = re.sub(r"\r?\n[ \t]", "", raw)
        in_ev = False
        props = {}
        attendees = []
        for line in unfolded.splitlines():
            if line.strip().upper() == "BEGIN:VEVENT":
                in_ev = True
                continue
            if line.strip().upper() == "END:VEVENT":
                break
            if not in_ev or ":" not in line:
                continue
            k_part, val = line.split(":", 1)
            k_base = k_part.split(";", 1)[0].upper()
            val = val.replace("\\n", "\n").replace("\\,", ",").replace("\\;", ";").strip()
            if k_base == "ATTENDEE":
                m = re.search(r"mailto:([^\s;>]+)", val, flags=re.IGNORECASE)
                if m:
                    attendees.append(m.group(1))
            elif k_base not in props:
                props[k_base] = (k_part, val)

        if not props:
            return None

        subj = props.get("SUMMARY", ("", "Imported Event"))[1]
        loc = props.get("LOCATION", ("", ""))[1]
        desc = props.get("DESCRIPTION", ("", ""))[1]
        s_meta, s_val = props.get("DTSTART", ("", ""))
        e_meta, e_val = props.get("DTEND", ("", ""))

        all_day = ("VALUE=DATE" in s_meta.upper()) or (len(s_val) == 8 and s_val.isdigit())
        today_s = date.today().isoformat()
        if all_day and len(s_val) >= 8:
            d_s = f"{s_val[0:4]}-{s_val[4:6]}-{s_val[6:8]}"
            return {
                "date": d_s,
                "start_min": 540,
                "end_min": 600,
                "all_day": True,
                "subject": subj,
                "location": loc,
                "notes": desc[:500],
                "attendees": ", ".join(attendees[:10]),
            }

        def _parse_ics_dt(v: str):
            clean = v.rstrip("Z")
            if len(clean) >= 15 and "T" in clean:
                dt = datetime.strptime(clean[:15], "%Y%m%dT%H%M%S")
                if v.endswith("Z"):
                    dt = dt.replace(tzinfo=timezone.utc).astimezone()
                return dt
            return None

        sdt = _parse_ics_dt(s_val)
        edt = _parse_ics_dt(e_val) if e_val else None
        if sdt:
            s_min = sdt.hour * 60 + sdt.minute
            e_min = (edt.hour * 60 + edt.minute) if edt else min(1439, s_min + 60)
            return {
                "date": sdt.date().isoformat(),
                "start_min": s_min,
                "end_min": e_min if e_min > s_min else min(1439, s_min + 60),
                "all_day": False,
                "subject": subj,
                "location": loc,
                "notes": desc[:500],
                "attendees": ", ".join(attendees[:10]),
            }
        return {
            "date": today_s,
            "start_min": 540,
            "end_min": 600,
            "all_day": False,
            "subject": subj,
            "location": loc,
            "notes": desc[:500],
            "attendees": ", ".join(attendees[:10]),
        }
    except Exception:
        return None


class CalendarSyncEngine:
    def __init__(self):
        self.oauth = SharedOAuthCalendar()
        self.sync_lock = threading.Lock()

    def sync_range(self, start_d: date, end_d: date, allow_interactive=False):
        if not self.sync_lock.acquire(blocking=False):
            return False, "Sync already in progress..."
        try:
            cfg = load_config()
            token, err = self.oauth.get_calendar_token(cfg, allow_interactive=allow_interactive)
            if err == "NEEDS_CALENDAR_CONSENT":
                token, err = self.oauth.run_interactive_consent(cfg)
            if not token:
                return False, err or "Not signed in"

            hdrs = {"Authorization": f"Bearer {token}", "Accept": "application/json"}

            if not cfg.get("email"):
                cm, me = http_json("GET", "https://graph.microsoft.com/v1.0/me?$select=displayName,mail,userPrincipalName", headers=hdrs)
                if cm == 200:
                    addr = me.get("mail") or me.get("userPrincipalName") or ""
                    if addr:
                        save_config_fields({"email": addr})
                        cfg["email"] = addr

            my_email = (cfg.get("email") or "").strip().lower()
            start_iso = f"{start_d.isoformat()}T00:00:00Z"
            end_iso = f"{(end_d + timedelta(days=1)).isoformat()}T23:59:59Z"
            url = (
                "https://graph.microsoft.com/v1.0/me/calendarView"
                f"?startDateTime={urllib.parse.quote(start_iso)}&endDateTime={urllib.parse.quote(end_iso)}"
                "&$top=250&$orderby=start/dateTime"
                "&$select=id,seriesMasterId,subject,location,organizer,attendees,isOrganizer,responseRequested,responseStatus,bodyPreview,start,end,isAllDay,showAs,isCancelled"
            )

            all_items = []
            while url:
                c, data = http_json("GET", url, headers=hdrs)
                if c in (401, 403) and not allow_interactive:
                    token2, err2 = self.oauth.run_interactive_consent(cfg)
                    if not token2:
                        return False, err2 or f"Calendar permission denied ({c})"
                    hdrs["Authorization"] = f"Bearer {token2}"
                    c, data = http_json("GET", url, headers=hdrs)
                if c != 200:
                    msg = (data.get("error") or {}).get("message") if isinstance(data.get("error"), dict) else data.get("error_description", "Calendar sync failed")
                    return False, f"Graph Calendar ({c}): {str(msg)[:150]}"
                all_items.extend(data.get("value", []))
                url = data.get("@odata.nextLink")

            conn = sqlite3.connect(CAL_DB_PATH)
            conn.execute(
                "DELETE FROM events WHERE start_date >= ? AND start_date <= ?",
                (start_d.isoformat(), end_d.isoformat()),
            )

            pending_cnt = 0
            for ev in all_items:
                if ev.get("isCancelled"):
                    continue
                eid = ev["id"]
                smid = ev.get("seriesMasterId") or ""
                subj = (ev.get("subject") or "(No Title)").strip()
                loc = ((ev.get("location") or {}).get("displayName") or "").strip()
                org_ea = (ev.get("organizer") or {}).get("emailAddress") or {}
                org_name = (org_ea.get("name") or "").strip()
                org_addr = (org_ea.get("address") or "").strip()
                org_disp = org_name or org_addr

                is_org_flag = bool(ev.get("isOrganizer", True))
                if my_email and org_addr and org_addr.lower() == my_email:
                    is_org_flag = True

                resp_req = 1 if ev.get("responseRequested", True) else 0
                raw_resp = ((ev.get("responseStatus") or {}).get("response") or "").strip()
                if is_org_flag:
                    resp_status = "organizer"
                elif raw_resp in ("notResponded", "none", ""):
                    resp_status = "notResponded"
                    pending_cnt += 1
                elif raw_resp == "tentativelyAccepted":
                    resp_status = "tentativelyAccepted"
                elif raw_resp == "accepted":
                    resp_status = "accepted"
                elif raw_resp == "declined":
                    resp_status = "declined"
                else:
                    resp_status = raw_resp

                att_addrs = []
                for att in (ev.get("attendees") or []):
                    aea = (att.get("emailAddress") or {}).get("address") or ""
                    aea = aea.strip()
                    if aea and aea.lower() != my_email:
                        att_addrs.append(aea)
                attendees_s = ", ".join(att_addrs)

                bprev = (ev.get("bodyPreview") or "").strip()
                all_day = 1 if ev.get("isAllDay") else 0
                show_as = (ev.get("showAs") or "busy").lower()

                if all_day:
                    s_raw = ((ev.get("start") or {}).get("dateTime") or "")[:10]
                    e_raw = ((ev.get("end") or {}).get("dateTime") or "")[:10]
                    try:
                        sd = date.fromisoformat(s_raw)
                        ed_excl = date.fromisoformat(e_raw)
                        ed = max(sd, ed_excl - timedelta(days=1))
                    except Exception:
                        continue
                    s_date_s = sd.isoformat()
                    e_date_s = ed.isoformat()
                    s_min = 0
                    e_min = 1440
                    s_disp = "All Day"
                    trange = "All Day Event"
                    s_ts = int(datetime.combine(sd, dtime(0, 0)).timestamp())
                else:
                    try:
                        sdt = parse_graph_dt_utc(ev.get("start"))
                        edt = parse_graph_dt_utc(ev.get("end"))
                    except Exception:
                        continue
                    s_date_s = sdt.date().isoformat()
                    e_date_s = edt.date().isoformat()
                    if edt.hour == 0 and edt.minute == 0 and edt.date() > sdt.date():
                        e_date_s = (edt.date() - timedelta(days=1)).isoformat()
                    s_min = sdt.hour * 60 + sdt.minute
                    e_min = edt.hour * 60 + edt.minute
                    if e_min <= s_min and s_date_s == e_date_s:
                        e_min = min(1440, s_min + 30)
                    s_disp = fmt_ampm_short(sdt)
                    trange = f"{fmt_ampm_long(sdt)} - {fmt_ampm_long(edt)}"
                    s_ts = int(sdt.timestamp())

                conn.execute(
                    """INSERT OR REPLACE INTO events(
                         id, series_master_id, subject, location, organizer, organizer_email,
                         is_organizer, response_requested, response_status, attendees_str,
                         body_preview, start_date, end_date, start_min, end_min,
                         start_disp, time_range, is_all_day, show_as, start_ts
                       ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                    (
                        eid, smid, subj, loc, org_disp, org_addr,
                        1 if is_org_flag else 0, resp_req, resp_status, attendees_s,
                        bprev, s_date_s, e_date_s, s_min, e_min,
                        s_disp, trange, all_day, show_as, s_ts,
                    ),
                )

            conn.commit()
            conn.close()
            extra = f", {pending_cnt} pending invite(s)" if pending_cnt > 0 else ""
            return True, f"Calendar : {cfg.get('email') or 'Office 365'} ({len(all_items)} appointment(s) synced{extra})"
        finally:
            self.sync_lock.release()

    def create_or_update_event(self, cmd, is_update=False):
        cfg = load_config()
        token, err = self.oauth.get_calendar_token(cfg, allow_interactive=True)
        if not token:
            return False, err or "Not signed in"

        date_str = (cmd.get("date") or "").strip()
        try:
            d_obj = date.fromisoformat(date_str)
        except Exception:
            return False, "Invalid Date (use YYYY-MM-DD, e.g. 2026-11-26)."

        subj = (cmd.get("subject") or "").strip()
        if not subj:
            return False, "Please enter an appointment title in 'What:'."
        loc = (cmd.get("location") or "").strip()
        notes = (cmd.get("notes") or "").strip()
        attendees_raw = (cmd.get("attendees") or "").strip()
        all_day = bool(cmd.get("all_day", False))

        if all_day:
            hdrs = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
            mb_tz = cfg.get("mailbox_tz") or "UTC"
            cs, mb = http_json("GET", "https://graph.microsoft.com/v1.0/me/mailboxSettings", headers=hdrs)
            if cs == 200 and mb.get("timeZone"):
                mb_tz = mb["timeZone"]
                save_config_fields({"mailbox_tz": mb_tz})
            next_d = d_obj + timedelta(days=1)
            body_obj = {
                "subject": subj,
                "isAllDay": True,
                "start": {"dateTime": f"{d_obj.isoformat()}T00:00:00", "timeZone": mb_tz},
                "end": {"dateTime": f"{next_d.isoformat()}T00:00:00", "timeZone": mb_tz},
            }
        else:
            s_min = max(0, min(1439, int(cmd.get("start_min", 540))))
            e_min = max(0, min(1439, int(cmd.get("end_min", s_min + 60))))
            if e_min <= s_min:
                e_min = min(1439, s_min + 30)
            local_tz = datetime.now().astimezone().tzinfo
            sdt_loc = datetime(d_obj.year, d_obj.month, d_obj.day, s_min // 60, s_min % 60, tzinfo=local_tz)
            edt_loc = datetime(d_obj.year, d_obj.month, d_obj.day, e_min // 60, e_min % 60, tzinfo=local_tz)
            sdt_utc = sdt_loc.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
            edt_utc = edt_loc.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
            body_obj = {
                "subject": subj,
                "isAllDay": False,
                "start": {"dateTime": sdt_utc, "timeZone": "UTC"},
                "end": {"dateTime": edt_utc, "timeZone": "UTC"},
            }

        if loc:
            body_obj["location"] = {"displayName": loc}
        elif is_update:
            body_obj["location"] = {"displayName": ""}

        if notes or is_update:
            body_obj["body"] = {"contentType": "Text", "content": notes}

        att_emails = []
        if attendees_raw:
            for tok_a in re.split(r"[,;]+", attendees_raw):
                addr = tok_a.strip()
                m = re.search(r"<([^>]+)>", addr)
                if m:
                    addr = m.group(1).strip()
                if "@" in addr:
                    att_emails.append(addr)
        if att_emails:
            body_obj["attendees"] = [
                {"emailAddress": {"address": a}, "type": "required"}
                for a in att_emails
            ]
        elif is_update and "attendees" in cmd:
            body_obj["attendees"] = []

        hdrs_post = {"Authorization": f"Bearer {token}", "Content-Type": "application/json"}
        if is_update:
            ev_id = (cmd.get("id") or "").strip()
            if not ev_id:
                return False, "Select an appointment in the list first to Change it."
            c, resp = http_json(
                "PATCH",
                f"https://graph.microsoft.com/v1.0/me/events/{urllib.parse.quote(ev_id)}",
                headers=hdrs_post,
                data_bytes=json.dumps(body_obj).encode("utf-8"),
            )
            if c in (200, 201):
                if att_emails:
                    return True, f"Appointment updated & update sent to {len(att_emails)} attendee(s)."
                return True, "Appointment updated on Office 365 Calendar."
        else:
            c, resp = http_json(
                "POST",
                "https://graph.microsoft.com/v1.0/me/events",
                headers=hdrs_post,
                data_bytes=json.dumps(body_obj).encode("utf-8"),
            )
            if c in (200, 201):
                if att_emails:
                    return True, f"Event created & invitations sent to {len(att_emails)} attendee(s)!"
                return True, "Appointment scheduled on Office 365 Calendar."

        err_msg = (resp.get("error") or {}).get("message") if isinstance(resp.get("error"), dict) else resp.get("error_description", "Calendar save failed")
        return False, f"Calendar error ({c}): {str(err_msg)[:150]}"

    def respond_to_event(self, ev_id: str, action: str, comment: str = "", send_response: bool = True):
        if not ev_id:
            return False, "Select an invitation first."
        act_map = {
            "accept": ("accept", "accepted", "Accepted invitation"),
            "tentative": ("tentativelyAccept", "tentativelyAccepted", "Tentatively accepted invitation"),
            "tentativelyAccept": ("tentativelyAccept", "tentativelyAccepted", "Tentatively accepted invitation"),
            "decline": ("decline", "declined", "Declined invitation"),
        }
        if action not in act_map:
            return False, f"Unknown RSVP action: {action}"
        endpoint, new_status, human_lbl = act_map[action]

        cfg = load_config()
        token, err = self.oauth.get_calendar_token(cfg, allow_interactive=True)
        if not token:
            return False, err or "Not signed in"

        payload = {"sendResponse": bool(send_response)}
        if comment and comment.strip():
            payload["comment"] = comment.strip()

        c, resp = http_json(
            "POST",
            f"https://graph.microsoft.com/v1.0/me/events/{urllib.parse.quote(ev_id)}/{endpoint}",
            headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
            data_bytes=json.dumps(payload).encode("utf-8"),
        )
        if c not in (200, 202, 204):
            err_msg = (resp.get("error") or {}).get("message") if isinstance(resp.get("error"), dict) else resp.get("error_description", "Invitation response failed")
            return False, f"RSVP error ({c}): {str(err_msg)[:150]}"

        conn = sqlite3.connect(CAL_DB_PATH)
        if new_status == "declined":
            conn.execute("DELETE FROM events WHERE id=?", (ev_id,))
        else:
            conn.execute("UPDATE events SET response_status=? WHERE id=?", (new_status, ev_id))
        conn.commit()
        conn.close()

        suffix = " (reply sent to organizer)." if send_response else " (no email sent)."
        return True, human_lbl + suffix

    def delete_event(self, ev_id):
        if not ev_id:
            return False, "No appointment selected."
        conn = sqlite3.connect(CAL_DB_PATH)
        conn.execute("DELETE FROM events WHERE id=?", (ev_id,))
        conn.commit()
        conn.close()

        cfg = load_config()
        token, _ = self.oauth.get_calendar_token(cfg, allow_interactive=False)
        if token:
            http_json(
                "DELETE",
                f"https://graph.microsoft.com/v1.0/me/events/{urllib.parse.quote(ev_id)}",
                headers={"Authorization": f"Bearer {token}"},
            )
        return True, "Appointment deleted from Office 365 Calendar."


def query_events(start_d: date, end_d: date):
    conn = sqlite3.connect(CAL_DB_PATH)
    rows = conn.execute(
        """SELECT id, subject, location, organizer, organizer_email,
                  is_organizer, response_requested, response_status, attendees_str,
                  body_preview, start_date, end_date, start_min, end_min,
                  start_disp, time_range, is_all_day, show_as
           FROM events
           WHERE end_date >= ? AND start_date <= ?
           ORDER BY start_date ASC, is_all_day DESC, start_min ASC""",
        (start_d.isoformat(), end_d.isoformat()),
    ).fetchall()
    conn.close()
    return [
        {
            "id": r[0], "subject": r[1], "location": r[2],
            "organizer": r[3], "organizer_email": r[4],
            "is_organizer": bool(r[5]), "response_requested": bool(r[6]),
            "response_status": r[7] or "organizer", "attendees": r[8] or "",
            "notes": r[9], "start_date": r[10], "end_date": r[11],
            "start_min": r[12], "end_min": r[13], "start_disp": r[14],
            "time_range": r[15], "all_day": bool(r[16]), "show_as": r[17],
        }
        for r in rows
    ]


def window_for_anchor(year: int, month: int, full_year: bool = False):
    if full_year:
        return date(year, 1, 1), date(year, 12, 31)
    anchor = date(year, month, 15)
    return anchor - timedelta(days=50), anchor + timedelta(days=65)


def main():
    init_db()
    engine = CalendarSyncEngine()
    cfg = load_config()
    today = date.today()
    w_start, w_end = window_for_anchor(today.year, today.month)

    emit({
        "event": "init",
        "email": cfg.get("email", ""),
        "signed_in": bool(load_tokens().get("access_token")),
        "events": query_events(w_start, w_end),
    })

    state = {"year": today.year, "month": today.month, "full_year": False, "running": True}

    def bg_sync(y, m, fy=False, interactive=False, quiet_status=False):
        s_d, e_d = window_for_anchor(y, m, fy)
        if not quiet_status:
            emit({"event": "status", "status": "Synchronizing Office 365 Calendar..."})
        ok, st = engine.sync_range(s_d, e_d, allow_interactive=interactive)
        c_now = load_config()
        emit({
            "event": "events_updated",
            "ok": ok,
            "email": c_now.get("email", ""),
            "signed_in": bool(load_tokens().get("access_token")),
            "status": st,
            "events": query_events(*window_for_anchor(state["year"], state["month"], state["full_year"])),
        })

    # Companion IPC watcher: detects RSVP actions & "Open in Calendar" requests from dtMail!
    def companion_watcher():
        last_dirty_mtime = os.path.getmtime(DIRTY_STAMP_PATH) if os.path.exists(DIRTY_STAMP_PATH) else 0.0
        while state["running"]:
            time.sleep(1.5)
            try:
                if os.path.exists(GOTO_JSON_PATH):
                    raw_g = open(GOTO_JSON_PATH, "r", encoding="utf-8").read()
                    os.remove(GOTO_JSON_PATH)
                    g_obj = json.loads(raw_g)
                    if g_obj.get("ics_path"):
                        parsed = parse_ics_file(g_obj["ics_path"])
                        if parsed:
                            emit({"event": "prefill_editor", **parsed})
                    elif g_obj.get("date"):
                        d_s = g_obj["date"][:10]
                        dt_p = date.fromisoformat(d_s)
                        state["year"], state["month"] = dt_p.year, dt_p.month
                        emit({"event": "goto_date", "year": dt_p.year, "month": dt_p.month, "day": dt_p.day})
                        bg_sync(dt_p.year, dt_p.month, state["full_year"], False, True)

                if os.path.exists(DIRTY_STAMP_PATH):
                    mt = os.path.getmtime(DIRTY_STAMP_PATH)
                    if mt > last_dirty_mtime:
                        last_dirty_mtime = mt
                        if load_tokens().get("access_token"):
                            bg_sync(state["year"], state["month"], state["full_year"], False, True)
            except Exception:
                pass

    threading.Thread(target=companion_watcher, daemon=True).start()

    if load_tokens().get("access_token"):
        threading.Thread(target=bg_sync, args=(today.year, today.month, False, False), daemon=True).start()

    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            cmd = json.loads(raw)
        except Exception:
            continue

        op = cmd.get("op")
        if op == "set_window":
            y = int(cmd.get("year", state["year"]))
            m = int(cmd.get("month", state["month"]))
            fy = bool(cmd.get("full_year", False))
            state["year"], state["month"], state["full_year"] = y, m, fy
            s_d, e_d = window_for_anchor(y, m, fy)
            emit({
                "event": "events_updated",
                "ok": True,
                "email": load_config().get("email", ""),
                "signed_in": bool(load_tokens().get("access_token")),
                "status": "",
                "events": query_events(s_d, e_d),
            })
            if load_tokens().get("access_token"):
                threading.Thread(target=bg_sync, args=(y, m, fy, False), daemon=True).start()

        elif op == "sync":
            threading.Thread(
                target=bg_sync,
                args=(state["year"], state["month"], state["full_year"], True),
                daemon=True,
            ).start()

        elif op == "connect":
            def _conn():
                tok, err = engine.oauth.run_interactive_consent(load_config())
                if tok:
                    bg_sync(state["year"], state["month"], state["full_year"], False)
                elif err:
                    emit({"event": "status", "status": err, "error": True})
            threading.Thread(target=_conn, daemon=True).start()

        elif op in ("insert_event", "update_event"):
            is_upd = (op == "update_event")
            def _mut(c=cmd, u=is_upd):
                emit({"event": "status", "status": "Saving appointment to Office 365..."})
                ok, st = engine.create_or_update_event(c, is_update=u)
                if ok:
                    bg_sync(state["year"], state["month"], state["full_year"], False)
                emit({"event": "mutation_result", "ok": ok, "status": st})
            threading.Thread(target=_mut, daemon=True).start()

        elif op == "rsvp_event":
            def _rsvp(c=cmd):
                act = c.get("action", "accept")
                emit({"event": "status", "status": f"Sending '{act}' response to Office 365..."})
                ok, st = engine.respond_to_event(
                    c.get("id", ""),
                    act,
                    c.get("comment", ""),
                    bool(c.get("send_response", True)),
                )
                s_d, e_d = window_for_anchor(state["year"], state["month"], state["full_year"])
                emit({
                    "event": "events_updated",
                    "ok": ok,
                    "email": load_config().get("email", ""),
                    "signed_in": bool(load_tokens().get("access_token")),
                    "status": st,
                    "events": query_events(s_d, e_d),
                })
                if ok:
                    bg_sync(state["year"], state["month"], state["full_year"], False, True)
            threading.Thread(target=_rsvp, daemon=True).start()

        elif op == "import_ics":
            ics_p = cmd.get("path", "")
            parsed = parse_ics_file(ics_p)
            if parsed:
                emit({"event": "prefill_editor", **parsed})
            else:
                emit({"event": "status", "status": f"Could not parse calendar file: {ics_p}"})

        elif op == "delete_event":
            eid = cmd.get("id", "")
            def _del():
                ok, st = engine.delete_event(eid)
                s_d, e_d = window_for_anchor(state["year"], state["month"], state["full_year"])
                emit({
                    "event": "events_updated",
                    "ok": ok,
                    "email": load_config().get("email", ""),
                    "signed_in": bool(load_tokens().get("access_token")),
                    "status": st,
                    "events": query_events(s_d, e_d),
                })
            threading.Thread(target=_del, daemon=True).start()

        elif op == "quit":
            state["running"] = False
            break


if __name__ == "__main__":
    main()
