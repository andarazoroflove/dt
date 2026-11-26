# dtTerm

**Native Multi-Tab OSF/Motif 2.3 & CDE `libDtTerm` Terminal Emulator**

`dtTerm` upgrades CDE's classic `dtterm` with a 3D beveled multi-tab strip while continuing to embed genuine CDE `DtTerm` (`DtCreateTerm`) widgets for authentic terminal emulation, scrollbars, fonts, and palette resources.

## Highlights

- **Multi-Tab Deck (`+ New Tab` / `Ctrl+Shift+T`)** — Open additional tabs in one click or shortcut, automatically inheriting the active tab's working directory (`/proc/<pid>/cwd`), with per-tab `[x]` close buttons, middle-click close, right-click **Rename / Duplicate / Close Other Tabs**, `Ctrl+Tab` / `Ctrl+PgUp` / `Ctrl+PgDn`, and `Alt+1`..`Alt+9`.
- **Smart Clipboard Built In** — `Ctrl+C` copies selected text to `CLIPBOARD` and clears the highlight (so a second `Ctrl+C` sends `SIGINT`), or sends normal `^C` when nothing is selected; `Ctrl+V` pastes directly from `CLIPBOARD`.
- **Dynamic CDE Palette & Retro Themes** — Automatically binds to your active CDE palette (`Crimson.dp`, `Broica.dp`, etc.) with instant switching between **CDE Palette**, **Dark Workstation**, **Green Phosphor**, and **Amber Phosphor**, plus `[A-]` / `[A+]` (`Ctrl+-` / `Ctrl++`) font scaling.

## Build & Install

```bash
cd dtTerm && ./install.sh
```
