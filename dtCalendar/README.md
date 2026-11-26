# <img src="../assets/icons/dtcalendar.png" width="36" height="36" align="absmiddle" alt="dtCalendar (`dtcm`) icon" /> dtCalendar (`dtcm`) — Native OSF/Motif 2.3 Calendar & Microsoft 365 Companion

> **CDE Front Panel Placement:** `Front Panel Date / Calendar (`Dtcm`)`  
> **Technology Stack:** C11 • OSF/Motif (`libXm` / `libDtWidget`) • Python 3 Graph Calendar Engine

A drop-in native OSF/Motif replacement for stock CDE `dtcm` that shares OAuth 2.0 credentials seamlessly with `dtMail`. Displays interactive Month, Week, and Day schedules, supports creating and editing events with attendees, handles incoming `.ics` meeting invitations, and syncs bi-directionally with Microsoft 365.

![dtCalendar (`dtcm`) Screenshot](../assets/screenshots/dtcalendar.png)

## Key Features

- **Front Panel `Dtcm` Integration** — Launches directly when clicking the CDE Front Panel date/calendar icon.
- **Shared Microsoft 365 Sync** — Reuses `dtMail`'s OAuth2 token store (`~/.config/cde-mail/`) with file-locked automatic refresh.
- **Full Appointment & Attendee Editor** — Create, update, delete, and RSVP (`Accept`, `Tentative`, `Decline`) to meetings.
- **`.ics` File Import** — Double-click any `.ics` invitation in `dtfile` to import or inspect it directly.

## Installation

```bash
cd dtCalendar && make && ./install.sh
```

This installs the executable into `~/.local/bin/`, copies the multi-resolution CDE XPM icons (`.pm`, `.l.pm`, `.m.pm`, `.s.pm`, `.t.pm`) into `~/.dt/icons/`, registers the CDE `ACTION` and file data types in `~/.dt/types/`, adds the Front Panel control to `~/.dt/types/fp_dynamic/`, and reloads `dtaction ReloadActions`.

## Running

```bash
~/.local/bin/dtcm
```

---

[← Back to the `dt` CDE Application Suite](../README.md)
