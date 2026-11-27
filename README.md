<div align="center">

# `dt` — Modern Workstation Application Suite for CDE

**Fourteen cohesive, native-feeling desktop applications crafted for the Common Desktop Environment (CDE) on Linux & UNIX.**

<p>
  <img src="assets/screenshots/hero-collage.png" alt="dt CDE Application Suite Showcase" width="100%" />
</p>

<p>
  <a href="#quick-start--installation"><b>Quick Install</b></a> •
  <a href="#application-directory"><b>All 14 Apps</b></a> •
  <a href="#cde-palette--front-panel-architecture"><b>Palette Architecture</b></a> •
  <a href="./LICENSE"><b>MIT License</b></a>
</p>

</div>

---

## Overview

The **`dt` suite** brings essential modern capabilities — Microsoft 365 email & calendar sync, Apple Podcasts streaming & downloading, universal image viewing & MS Paint-style editing, GStreamer music/video playback with 60-second clip trimming, TLS Libera.Chat IRC, Rich Text/Markdown editing, fillable PDF forms, drag-and-drop archives, vintage HF/shortwave & NOAA/emergency radio reception, and classic Windows game management — straight to the **Common Desktop Environment (`dtwm`)** without sacrificing the crisp 3D beveled ergonomics, spatial speed, or warm ivory reading wells of 1990s UNIX workstations.

Every application lives in **its own self-contained directory** with its source code, multi-size CDE XPM icon set (`.pm`, `.l.pm`, `.m.pm`, `.s.pm`, `.t.pm`), CDE `DtDts` action/datatype definitions, Front Panel subpanel control (`.fp`), and standalone `install.sh`.

---

## Application Directory

| Icon | Folder | Purpose | CDE Panel Location |
| :---: | :--- | :--- | :--- |
| <img src="assets/icons/dtmail.png" width="32" height="32" alt="dtMail" /> | **[`dtMail/`](./dtMail/)** | **Native OSF/Motif 2.3 Microsoft 365 & Mail Client** | `Front Panel / Mail` |
| <img src="assets/icons/dtcalendar.png" width="32" height="32" alt="dtCalendar (`dtcm`)" /> | **[`dtCalendar/`](./dtCalendar/)** | **Native OSF/Motif 2.3 Calendar & Microsoft 365 Companion** | `Front Panel Date / Calendar (`Dtcm`)` |
| <img src="assets/icons/dtterm.png" width="32" height="32" alt="dtTerm" /> | **[`dtTerm/`](./dtTerm/)** | **Multi-Tab Native OSF/Motif 2.3 & `libDtTerm` Terminal Emulator** | `Utilities Subpanel` |
| <img src="assets/icons/dtpod.png" width="32" height="32" alt="dtPod" /> | **[`dtPod/`](./dtPod/)** | **Apple Podcasts Directory Browser, Subscriber & Streaming Player** | `Utilities Subpanel` |
| <img src="assets/icons/dtpaint.png" width="32" height="32" alt="dtPaint" /> | **[`dtPaint/`](./dtPaint/)** | **Classic MS Paint Ribbon Studio with Universal Image Format Support** | `Utilities Subpanel` |
| <img src="assets/icons/dtplay.png" width="32" height="32" alt="dtPlay" /> | **[`dtPlay/`](./dtPlay/)** | **VLC-Style Music & Movie Player with 60-Second Clip Studio** | `Utilities Subpanel` |
| <img src="assets/icons/dtirc.png" width="32" height="32" alt="dtIRC" /> | **[`dtIRC/`](./dtIRC/)** | **Full-Featured Libera.Chat TLS IRC Client with Cream mIRC Palette** | `Utilities Subpanel` |
| <img src="assets/icons/dtpics.png" width="32" height="32" alt="dtPics" /> | **[`dtPics/`](./dtPics/)** | **Universal Image Viewer, 1-Click Magic Color & Photo Markup Studio** | `Utilities Subpanel` |
| <img src="assets/icons/dtnotes.png" width="32" height="32" alt="dtNotes" /> | **[`dtNotes/`](./dtNotes/)** | **Multi-Tab Rich Text (`.rtf`), Markdown (`.md`) & Code Editor** | `Utilities Subpanel` |
| <img src="assets/icons/dtreader.png" width="32" height="32" alt="dtReader" /> | **[`dtReader/`](./dtReader/)** | **CDE PDF Document Viewer & Interactive AcroForm Editor** | `Utilities Subpanel` |
| <img src="assets/icons/dtzip.png" width="32" height="32" alt="dtZip" /> | **[`dtZip/`](./dtZip/)** | **Drag-and-Drop Archive Manager for CDE** | `Utilities Subpanel` |
| <img src="assets/icons/dtwine.png" width="32" height="32" alt="dtWine" /> | **[`dtWine/`](./dtWine/)** | **Classic Windows Game & Application Prefix Manager for CDE** | `Utilities Subpanel` |
| <img src="assets/icons/dtupdate.png" width="32" height="32" alt="dtUpdate" /> | **[`dtUpdate/`](./dtUpdate/)** | **All-in-One Debian (`apt-get`) & CDE Application Rebuild Manager** | `Utilities Subpanel` |
| <img src="assets/icons/dtradio.png" width="32" height="32" alt="dtRadio" /> | **[`dtRadio/`](./dtRadio/)** | **Vintage HF/Shortwave (`0–30 MHz`), Ham Repeater, NOAA, Emergency & Space Receiver** | `Radio Apps Subpanel` |

---

## CDE Palette & Front Panel Architecture

1. **Dynamic OSF/Motif Palette Synchronization:** At launch, every app queries `xrdb` / Xrm for the active CDE `ColorPalette` (such as `Crimson.dp`, `Broica.dp`, `Alpine.dp`, or `Default.dp`) from `~/.dt/palettes` or `/usr/dt/palettes`, binding its 3D beveled chrome (`#AEB2C3`), selection highlight, warm cream sunken reading wells (`#FFF7E9`), and subtle vintage **greenbar** alternating list rows (`#EEF4DB`) automatically.
2. **Authentic Multi-Size XPM Icons:** Each folder ships `48×48` (`.l.pm` / `.pm`), `32×32` (`.m.pm`), `24×24` (`.s.pm`), and `16×16` (`.t.pm`) transparent X11 Pixmap icons for crisp rendering across the Front Panel, subpanels, `dtfile`, and minimized `dtwm` desktop icons.
3. **Scoped `DtDts` File Associations:** File double-click actions (`dtPics` for images, `dtPlay` for audio/video, `dtReader` for PDFs, `dtNotes` for RTF/Markdown/text, `dtZip` for archives, `dtCalendar` for `.ics` invites) register cleanly inside `~/.dt/types/`.

---

## Quick Start & Installation

### System Dependencies (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install -y \
  build-essential libmotif-dev libx11-dev libxt-dev libxpm-dev \
  python3 python3-gi python3-gi-cairo gir1.2-gtk-3.0 gir1.2-gdkpixbuf-2.0 \
  gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  gir1.2-webkit2-4.1 gir1.2-gtksource-4 gir1.2-evince-3.0 \
  python3-pil python3-requests imagemagick p7zip-full
```

*(Note: `dtMail`, `dtCalendar`, and `dtTerm` also link against `/usr/dt/include` and `/usr/dt/lib` from a standard CDE installation.)*

### Install All 14 Applications

```bash
git clone https://github.com/andarazoroflove/dt.git
cd dt
./install-all.sh
```

Or install any individual application from its own directory:

```bash
cd dtPod && ./install.sh
```

After installing, reload the CDE Workspace Manager (**Workspace Menu -> Restart Workspace Manager**) so the Front Panel **Utilities**, **Mail**, and **Radio Apps** subpanels pick up all new icons.

---

## <img src="assets/icons/dtmail.png" width="40" height="40" align="absmiddle" alt="dtMail icon" /> dtMail

**Folder:** [`./dtMail/`](./dtMail/) &nbsp;•&nbsp; **Front Panel:** `Mail Subpanel` &nbsp;•&nbsp; **Stack:** C11 • OSF/Motif (`libXm` / `libDtWidget`) • Python 3 Graph Sync • WebKit2GTK Reading Pane

A classic three-pane workstation mail client written in C and OSF/Motif 2.3 that binds dynamically to your active CDE color palette. Features zero-config Microsoft 365 browser sign-in (OAuth 2.0 + 2FA), an SQLite3 local message cache, vintage greenbar alternating message rows, an embedded HTML reading pane via X11 window reparenting, attachment chips, and 1-click RSVP integration with `dtCalendar`.

<p align="center">
  <img src="assets/screenshots/dtmail.png" alt="dtMail running under CDE dtwm" width="880" />
</p>

### Highlights
- **Dynamic CDE Palette Binding** — Reads `*ColorPalette` (`Crimson.dp`, `Broica.dp`, etc.) for 3D beveled Motif chrome, warm ivory wells (`#FFF7E9`), and subtle greenbar row shading (`#EEF4DB`), plus an optional Dark Motif toggle.
- **Zero-Config Microsoft 365 OAuth 2.0 + 2FA** — Sign in via WebKit2GTK (`cde-mail-webauth`) and sync folders, subfolders, and messages over Microsoft Graph v1.0.
- **Embedded HTML Reading Pane** — Safely renders HTML emails (JavaScript disabled) inside the bottom-right Motif `XmDrawingArea` with synchronized `Ctrl++` / `Ctrl+-` font scaling.
- **Meeting Invitation Banner** — Detects `.ics` and Exchange calendar invites with inline **Accept / Tentative / Decline** actions synchronized directly with `dtCalendar`.

```bash
cd dtMail && make && ./install.sh
```
---

## <img src="assets/icons/dtcalendar.png" width="40" height="40" align="absmiddle" alt="dtCalendar (`dtcm`) icon" /> dtCalendar (`dtcm`)

**Folder:** [`./dtCalendar/`](./dtCalendar/) &nbsp;•&nbsp; **Front Panel:** `Front Panel Date / Calendar (`Dtcm`)` &nbsp;•&nbsp; **Stack:** C11 • OSF/Motif (`libXm` / `libDtWidget`) • Python 3 Graph Calendar Engine

A drop-in native OSF/Motif replacement for stock CDE `dtcm` that shares OAuth 2.0 credentials seamlessly with `dtMail`. Displays interactive Month, Week, and Day schedules, supports creating and editing events with attendees, handles incoming `.ics` meeting invitations, and syncs bi-directionally with Microsoft 365.

<p align="center">
  <img src="assets/screenshots/dtcalendar.png" alt="dtCalendar (`dtcm`) running under CDE dtwm" width="880" />
</p>

### Highlights
- **Front Panel `Dtcm` Integration** — Launches directly when clicking the CDE Front Panel date/calendar icon.
- **Shared Microsoft 365 Sync** — Reuses `dtMail`'s OAuth2 token store (`~/.config/cde-mail/`) with file-locked automatic refresh.
- **Full Appointment & Attendee Editor** — Create, update, delete, and RSVP (`Accept`, `Tentative`, `Decline`) to meetings.
- **`.ics` File Import** — Double-click any `.ics` invitation in `dtfile` to import or inspect it directly.

```bash
cd dtCalendar && make && ./install.sh
```
---

## <img src="assets/icons/dtterm.png" width="40" height="40" align="absmiddle" alt="dtTerm icon" /> dtTerm

**Folder:** [`./dtTerm/`](./dtTerm/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** C11 • OSF/Motif (`libXm`) • CDE `libDtTerm` (`DtCreateTerm`)

A multi-tab upgrade to CDE's classic `dtterm` that embeds genuine `DtCreateTerm` terminal widgets inside a 3D beveled Motif tab deck. Preserves authentic CDE terminal emulation, scrollbars, fonts, and palette resources while adding 1-click tab management and built-in smart clipboard shortcuts.

### Highlights
- **Multi-Tab Motif Deck (`+ New Tab` / `Ctrl+Shift+T`)** — Open additional tabs that automatically inherit the active tab's working directory (`/proc/<pid>/cwd`), switch with `Ctrl+Tab` / `Alt+1`–`Alt+9`, and right-click tabs to **Duplicate**, **Rename**, or **Close Other Tabs**.
- **Built-in Smart Clipboard** — `Ctrl+C` copies selected text to `CLIPBOARD` and clears the highlight (or sends normal `^C` `SIGINT` when nothing is selected); `Ctrl+V` pastes from `CLIPBOARD`.
- **Dynamic CDE Palette & Retro Themes** — Syncs with your active CDE palette (`Crimson.dp`, `Broica.dp`, etc.) and switches instantly between **CDE Palette**, **Dark Workstation**, **Green Phosphor**, and **Amber Phosphor**, plus `[A-]`/`[A+]` (`Ctrl+-`/`Ctrl++`) font zoom.

```bash
cd dtTerm && ./install.sh
```
---

## <img src="assets/icons/dtpod.png" width="40" height="40" align="absmiddle" alt="dtPod icon" /> dtPod

**Folder:** [`./dtPod/`](./dtPod/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • GStreamer 1.0 • Cairo • Public iTunes API

A full-featured Apple Podcasts browser, subscription manager, and audio player requiring zero login or API keys. Search by show title, artist/creator, or browse the Apple Top 25 chart, subscribe to favorite shows, inspect high-resolution show and episode artwork, read formatted show notes, stream instantly, or right-click any episode to download it for offline listening.

<p align="center">
  <img src="assets/screenshots/dtpod.png" alt="dtPod running under CDE dtwm" width="880" />
</p>

### Highlights
- **Public Apple Podcasts API + RSS Engine** — Search shows, hosts, and artists or load Top Charts with zero account login.
- **Subscriptions & OPML Support** — Subscribe/unsubscribe in one click (`~/.config/dtpod/subscriptions.json`), plus OPML import and export.
- **High-Res Cover Artwork Cache** — Asynchronous disk/memory artwork cache rendering thumbnails in show lists and a `136×136` sunken Motif header frame.
- **Right-Click `Save Episode As...`** — Download episodes in the background with live progress, or stream directly with `-15s`/`+30s` skip and `0.75×`–`2.0×` playback speed.

```bash
cd dtPod && ./install.sh
```
---

## <img src="assets/icons/dtpaint.png" width="40" height="40" align="absmiddle" alt="dtPaint icon" /> dtPaint

**Folder:** [`./dtPaint/`](./dtPaint/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • Cairo • Pillow • GdkPixbuf • ImageMagick

A faithful CDE clone of classic MS Paint upgraded to open and save every modern and vintage image format supported by `dtPics`. Combines the iconic MS Paint Ribbon workflow — selection modes, brushes, 20 geometric shapes, flood fill, Color 1 / Color 2 mouse buttons, and draggable canvas resize handles — with authentic CDE beveled chrome and custom vector tool icons.

<p align="center">
  <img src="assets/screenshots/dtpaint.png" alt="dtPaint running under CDE dtwm" width="880" />
</p>

### Highlights
- **Universal Image I/O** — Opens and exports `.png`, `.jpg`, `.bmp`, `.gif`, `.heic`, `.avif`, `.jxl`, `.webp`, `.tiff`, `.svg`, `.ico`, `.icns`, `.ppm`, `.xpm`, `.tga`, and `.psd`.
- **Full MS Paint Toolkit** — Rectangular & Free-Form Lasso selection (with **Transparent Selection** background keying), Cut/Copy/Paste floating selections, Pencil, Flood Fill, Text Box, Eraser (Color 2), Eyedropper, and Magnifier.
- **6 Brushes & 20 Geometry Shapes** — Round, Square, Calligraphy `/` & `\`, Airbrush Spray, and Marker brushes; Line, 2-step bendable Bezier Curve, Oval, Rounded Rect, Polygons, 4-way Arrows, 4/5/6-Point Stars, Speech Callout, Heart, and Lightning Bolt.
- **Seamless Workflow** — Integrated directly with `dtPics` and CDE Screenshot via **Open with dtPaint**, plus 30-step Undo/Redo.

```bash
cd dtPaint && ./install.sh
```
---

## <img src="assets/icons/dtplay.png" width="40" height="40" align="absmiddle" alt="dtPlay icon" /> dtPlay

**Folder:** [`./dtPlay/`](./dtPlay/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • GStreamer 1.0 (`playbin` + `gtksink` + `x264enc`)

A streamlined CDE music and movie player supporting `.wav`, `.mp3`, `.m4a`, `.m4v`, `.flac`, `.mp4`, `.mov`, `.avi`, `.mpeg`, `.mkv`, `.webm`, `.ogg`, and `.opus`. Opens to `~/Music` in `dtMail`'s signature greenbar directory browser (with `..` parent folder navigation), switches to a native video or retro 20-band spectrum stage on double-click, goes borderless fullscreen, and includes a 1-click **Make a Clip** snippet studio.

<p align="center">
  <img src="assets/screenshots/dtplay.png" alt="dtPlay running under CDE dtwm" width="880" />
</p>

### Highlights
- **Greenbar `~/Music` Browser** — Browse `~/Music` or navigate anywhere via `..` and quick folder shortcuts; double-click to play and click **`◀ Back to List`** anytime.
- **Borderless Full Screen (`F11` / `F`)** — Uses `_MOTIF_WM_HINTS` decoration removal + fullscreen state for edge-to-edge cinema playback in `dtwm` with auto-revealing bottom controls.
- **`✂ Make a Clip` (60s Snippet Studio)** — Captures up to 60 seconds of audio or video starting from the moment you press the button and opens it in a second **dtPlay Clip Studio** window to trim `Start`/`End` further or **Save Clip As**.
- **Default CDE Media Handler** — Registers `DtDts` and XDG MIME associations for double-clicked audio and video files.

```bash
cd dtPlay && ./install.sh
```
---

## <img src="assets/icons/dtirc.png" width="40" height="40" align="absmiddle" alt="dtIRC icon" /> dtIRC

**Folder:** [`./dtIRC/`](./dtIRC/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • Python `ssl` Socket Engine • RFC 1459 / IRCv3 SASL

A full-featured CDE IRC client pre-wired to connect to `irc.libera.chat:6697` over TLS. Features saved NickServ credentials (`0600` permissions) with SASL PLAIN and customizable `IDENTIFY` commands, a left-hand ranked channel user list with right-click Operator actions, a directly editable top channel topic bar, cream-adapted mIRC colors, and instant font zoom buttons.

<p align="center">
  <img src="assets/screenshots/dtirc.png" alt="dtIRC running under CDE dtwm" width="880" />
</p>

### Highlights
- **Libera.Chat TLS & Saved NickServ Auth** — Supports both IRCv3 `SASL PLAIN` during connection and saved `/msg NickServ IDENTIFY` templates + a 1-click **`✔ Identify`** toolbar button.
- **Left-Hand Ranked User List & Right-Click Op Menu** — Sorts `~`, `&`, `@`, `%`, `+` ranks on the left; right-click any nick for **Op/Deop**, **Voice/Devoice**, **Half-Op**, **Kick**, **Ban (`*!*@host`)**, **Kick + Ban**, **Quiet/Unquiet (`+q`/`-q`)**, **WHOIS**, **PM**, and **CTCP**.
- **Top Editable Channel Topic Bar** — View and update the active channel's `TOPIC` directly from the top entry bar.
- **Cream-Refactored mIRC Colors & `[A−]`/`[A＋]` Zoom** — Full mIRC `00`–`99` color/bold/italic/underline parser tuned for high contrast on CDE's `#FFF7E9` cream background, plus 1-click text scaling (`8pt`–`26pt`) and complete `/command` support.

```bash
cd dtIRC && ./install.sh
```
---

## <img src="assets/icons/dtpics.png" width="40" height="40" align="absmiddle" alt="dtPics icon" /> dtPics

**Folder:** [`./dtPics/`](./dtPics/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • Cairo • Pillow • GdkPixbuf • ImageMagick

A fast CDE image viewer and quick photo editor supporting modern mobile and web formats (HEIC/HEIF, AVIF, JPEG-XL, WebP, animated GIF, SVG, PSD, TIFF, PNG, JPEG, XPM). Includes rule-of-thirds cropping, rotation/skew straightening, 1-click Magic Color auto-enhancement, callout arrows/boxes/text stamps, and 1-click handoff to `dtPaint`.

<p align="center">
  <img src="assets/screenshots/dtpics.png" alt="dtPics running under CDE dtwm" width="880" />
</p>

### Highlights
- **Broad Format Decoding** — Natively views static and animated `.gif`/`.webp`, iPhone `.heic`, `.avif`, `.jxl`, `.svg`, `.psd`, `.ico`, `.xpm`, and standard raster formats.
- **1-Click Magic Color & Manual Levels** — Outlier-safe percentile white balance, adaptive gamma shadow lift, vibrancy, and unsharp clarity.
- **Quick Callouts & Geometry** — Interactive Crop with rule-of-thirds guides, Skew/Straighten preview dialog, Pen, Highlighter, Arrow, Box, and outlined Text overlays.
- **Direct `Open with dtPaint` Button** — Jump straight from viewing/cropping into full pixel editing in `dtPaint`.

```bash
cd dtPics && ./install.sh
```
---

## <img src="assets/icons/dtnotes.png" width="40" height="40" align="absmiddle" alt="dtNotes icon" /> dtNotes

**Folder:** [`./dtNotes/`](./dtNotes/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 • GtkSourceView 4 • WebKit2GTK Live Markdown Preview

A modern replacement for stock `dtpad` styled for CDE. Supports multi-tab editing, native Rich Text Format (`.rtf`) reading and writing with bold/italic/underline/font colors, side-by-side Markdown (`.md`) live preview, and syntax-highlighted plain text and source files.

<p align="center">
  <img src="assets/screenshots/dtnotes.png" alt="dtNotes running under CDE dtwm" width="880" />
</p>

### Highlights
- **Native `.rtf` Serializer & Parser** — Edit formatted Rich Text documents without heavyweight office suites.
- **Live Markdown Split Preview** — Toggle a live CDE-themed WebKit2 preview pane while writing `.md` documentation.
- **GtkSourceView 4 Engine** — Line numbers, undo/redo, find & replace, and syntax highlighting in warm CDE cream wells.

```bash
cd dtNotes && ./install.sh
```
---

## <img src="assets/icons/dtreader.png" width="40" height="40" align="absmiddle" alt="dtReader icon" /> dtReader

**Folder:** [`./dtReader/`](./dtReader/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 • EvinceDocument 3.0 & EvinceView 3.0 (Poppler)

A crisp, fast PDF viewer and fillable form editor for CDE powered by Poppler/Evince. Supports page thumbnails and document outlines, full-text search, rotation, printing, and direct on-page editing and atomic saving of interactive PDF AcroForm fields.

<p align="center">
  <img src="assets/screenshots/dtreader.png" alt="dtReader running under CDE dtwm" width="880" />
</p>

### Highlights
- **Interactive Fillable PDF Forms** — Click directly into PDF text fields, checkboxes, radio buttons, and dropdowns and save cleanly (`Ctrl+S` / `Ctrl+Shift+S`).
- **Sidebar Thumbnails & Outline Index** — Navigate long technical manuals and specifications effortlessly.
- **Native Print & Search** — Integrated GTK/CUPS printing and instant highlight search.

```bash
cd dtReader && ./install.sh
```
---

## <img src="assets/icons/dtzip.png" width="40" height="40" align="absmiddle" alt="dtZip icon" /> dtZip

**Folder:** [`./dtZip/`](./dtZip/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • `zipfile` / `tarfile` / `7z`

A classic UNIX/CDE archive utility supporting drag-and-drop extraction and creation of `.zip`, `.tar.gz`/`.tgz`, `.tar.bz2`, `.tar.xz`, `.tar.zst`, `.tar`, `.7z`, `.rar`, `.iso`, `.gz`, `.bz2`, and `.xz` archives, including `Extract Here` integration in `dtfile`.

<p align="center">
  <img src="assets/screenshots/dtzip.png" alt="dtZip running under CDE dtwm" width="880" />
</p>

### Highlights
- **Universal Archive Inspection & Extraction** — Browse contents, sizes, ratios, and timestamps before extracting.
- **Drag-and-Drop Archive Creation** — Drop files or folders onto `dtZip` to package `.zip`, `.tar.gz`, `.tar.xz`, or `.7z` archives.
- **CDE File Manager Integration** — Adds `Open` and `Extract Here (dtZip)` actions to archive data types.

```bash
cd dtZip && ./install.sh
```
---

## <img src="assets/icons/dtwine.png" width="40" height="40" align="absmiddle" alt="dtWine icon" /> dtWine

**Folder:** [`./dtWine/`](./dtWine/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • Wine • Pure-Python PE/NE/ICO Icon Extractor

Manages classic Windows 95/98/XP/7/10 applications and offline GOG installers on CDE. Automatically configures compatible OpenGL WineD3D prefixes, extracts 32-bit PE and vintage 16-bit NE executable icons into multi-size CDE `.pm` XPM icons, and registers launchers directly into CDE's **Games** or **Personal Applications** Front Panel subpanels.

<p align="center">
  <img src="assets/screenshots/dtwine.png" alt="dtWine running under CDE dtwm" width="880" />
</p>

### Highlights
- **GOG & Classic Installer Wizard** — Installs `.exe`, `.msi`, `.bat`, or portable folders into clean Wine prefixes while filtering out `Uninstall`/`Setup`/`Readme` clutter.
- **Pure-Python PE32 / PE32+ / 16-Bit NE Icon Extractor** — Converts embedded Windows DIB icons + 1-bit AND masks into native CDE `.l.pm`/`.m.pm`/`.s.pm`/`.t.pm` icons.
- **Per-App Windows Version & Virtual Desktop** — Switch `win95`..`win10`, `640x480`..`1280x720` virtual desktops, Winetricks runtimes, `winecfg`, and `regedit` in one click.

```bash
cd dtWine && ./install.sh
```
---

## <img src="assets/icons/dtupdate.png" width="40" height="40" align="absmiddle" alt="dtUpdate icon" /> dtUpdate

**Folder:** [`./dtUpdate/`](./dtUpdate/) &nbsp;•&nbsp; **Front Panel:** `Utilities Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • APT (`Status-Fd`) • ELF `ldd` / `dpkg -S` Dependency Analyzer

An all-in-one workstation update manager for Debian (`apt-get`) and custom CDE applications. Prompts securely for `sudo`, runs `apt-get update`, inspects every installed CDE C/Motif binary and Python utility to build a targeted rebuild plan **only when needed**, asks for explicit **Yes / No** confirmation, downloads and installs updates with a live progress bar, recompiles affected CDE binaries atomically, restarts `dtwm`, and shows a full post-update overview.

<p align="center">
  <img src="assets/screenshots/dtupdate.png" alt="dtUpdate running under CDE dtwm" width="880" />
</p>

### Highlights
- **In-Memory `sudo` Auth & `apt-get update`** — Prompts for your `sudo` password (kept strictly in session memory) and runs `apt-get update` + `apt-get -s dist-upgrade`.
- **Smart CDE Rebuild Planner (Only When Needed)** — Maps `ldd -r` shared library dependencies via `dpkg -S` and checks local source `mtime`s so unaffected CDE apps are never needlessly rebuilt.
- **Explicit `[✔ Yes]` / `[✖ No]` Plan Approval** — Presents itemized greenbar tables of pending Debian package upgrades and planned CDE binary rebuilds before modifying the system.
- **Live Progress Bar, Atomic Rebuilds & `dtwm` Restart** — Streams APT download/install percentages via `APT::Status-Fd`, atomically installs rebuilt Motif binaries, restarts the CDE Workspace Manager (`dtwm`), and displays a **Finished Overview** changelog.

```bash
cd dtUpdate && ./install.sh
```
---

## <img src="assets/icons/dtradio.png" width="40" height="40" align="absmiddle" alt="dtRadio icon" /> dtRadio

**Folder:** [`./dtRadio/`](./dtRadio/) &nbsp;•&nbsp; **Front Panel:** `Radio Apps Subpanel` &nbsp;•&nbsp; **Stack:** Python 3 • GTK 3 (CDE Motif CSS) • Cairo • GStreamer 1.0 DSP • KiwiSDR WebSocket Engine • RTL-SDR (`rtl_fm`)

A tabletop 1980s/1990s HF communications receiver styled for CDE. Combines live `0–30 MHz` off-air HF/Shortwave reception via the public KiwiSDR network with curated shortwave relays, linked amateur repeaters, NOAA Weather Radio, emergency feeds, space/mission audio, and plug-and-play USB RTL-SDR dongle support.

### Highlights
- **Vintage Receiver Faceplate** — Warm backlit ballistic analog **S-Meter**, glowing cyan-green **Vacuum Fluorescent Display (VFD)** + vernier slide-rule scale, and weighted spun-aluminum **VFO flywheel**.
- **Real-Time GStreamer Audio DSP Knobs** — Interactive rotary knobs and switches for **AF Gain**, **RF Gain**, **Squelch**, **Clarifier (`±2.50 kHz` RIT/Pitch)**, **IF Bandpass (`300 Hz` CW – `12 kHz` Wide)**, **Tone**, **Noise Blanker (`NB` + `NB Level`)**, and **ANL**.
- **Direct Emergency & NOAA Buttons + Hold-to-Save Memories** — Dedicated safety-orange **`FEMA / EMERG`** and NOAA-blue **`NOAA WX`** buttons, curated non-music category dropdowns, 12 press-and-hold memory presets (`M01`–`M12`), 1-click `.wav` aircheck recording, and plug-and-play **RTL-SDR** support.

```bash
cd dtRadio && ./install.sh
```


---

## License

Released under the [MIT License](./LICENSE).
